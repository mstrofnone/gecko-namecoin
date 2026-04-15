/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * NmcConnectionManager — Battery-aware WebSocket lifecycle for Android.
 *
 * See NmcConnectionManager.h for architecture overview.
 *
 * Observer topics we listen on:
 *   "geckoview:activity-state-changed"  — data = "foreground" | "background"
 *   "network:offline-status-changed"    — data = "online" | "offline"
 *   "network:link-status-changed"       — data = "changed" (network switch)
 *
 * The Java/Kotlin side is responsible for firing these via:
 *   EventDispatcher.getInstance().dispatch("GeckoView:ActivityState", ...)
 * which GeckoView's native bridge translates to the observer topic.
 */

#ifdef ANDROID

#include "NmcConnectionManager.h"
#include "nsNamecoinResolver.h"

#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "nsIObserverService.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"

namespace mozilla {
namespace net {

static LazyLogModule gNmcConnLog("namecoin");
#define NMC_CONN_LOG(...) \
  MOZ_LOG(gNmcConnLog, LogLevel::Debug, (__VA_ARGS__))
#define NMC_CONN_ERR(...) \
  MOZ_LOG(gNmcConnLog, LogLevel::Error, (__VA_ARGS__))

// Observer topic strings
static constexpr char kTopicActivityState[] =
    "geckoview:activity-state-changed";
static constexpr char kTopicOfflineStatus[] =
    "network:offline-status-changed";
static constexpr char kTopicLinkStatus[] =
    "network:link-status-changed";

// ---------------------------------------------------------------------------
// NS_IMPL / Constructor / Destructor
// ---------------------------------------------------------------------------

NS_IMPL_ISUPPORTS(NmcConnectionManager, nsIObserver, nsITimerCallback,
                   nsINamed)

NmcConnectionManager::NmcConnectionManager(nsNamecoinResolver* aResolver)
    : mResolver(aResolver),
      mMutex("NmcConnectionManager::mMutex") {
  MOZ_ASSERT(aResolver);
}

NmcConnectionManager::~NmcConnectionManager() { Shutdown(); }

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------

nsresult NmcConnectionManager::Init() {
  MOZ_ASSERT(NS_IsMainThread());

  MutexAutoLock lock(mMutex);
  if (mInitialized || mShutdown) {
    return NS_OK;
  }

  nsresult rv;
  nsCOMPtr<nsIObserverService> obs =
      do_GetService("@mozilla.org/observer-service;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = obs->AddObserver(this, kTopicActivityState, false);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = obs->AddObserver(this, kTopicOfflineStatus, false);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = obs->AddObserver(this, kTopicLinkStatus, false);
  NS_ENSURE_SUCCESS(rv, rv);

  mInitialized = true;
  NMC_CONN_LOG("NmcConnectionManager initialized");
  return NS_OK;
}

void NmcConnectionManager::Shutdown() {
  MutexAutoLock lock(mMutex);
  if (mShutdown) return;
  mShutdown = true;

  CancelBackgroundIdleTimer();

  // Unregister observers (best-effort — may fail during XPCOM shutdown)
  nsCOMPtr<nsIObserverService> obs =
      do_GetService("@mozilla.org/observer-service;1");
  if (obs) {
    obs->RemoveObserver(this, kTopicActivityState);
    obs->RemoveObserver(this, kTopicOfflineStatus);
    obs->RemoveObserver(this, kTopicLinkStatus);
  }

  NMC_CONN_LOG("NmcConnectionManager shut down");
}

// ---------------------------------------------------------------------------
// nsIObserver
// ---------------------------------------------------------------------------

NS_IMETHODIMP
NmcConnectionManager::Observe(nsISupports* aSubject, const char* aTopic,
                               const char16_t* aData) {
  if (!strcmp(aTopic, kTopicActivityState)) {
    // Data: u"foreground" or u"background"
    NS_ConvertUTF16toUTF8 state(aData);
    if (state.EqualsLiteral("foreground")) {
      OnAppForegrounded();
    } else if (state.EqualsLiteral("background")) {
      OnAppBackgrounded();
    } else {
      NMC_CONN_LOG("NmcConnectionManager: unknown activity state: %s",
                   state.get());
    }
  } else if (!strcmp(aTopic, kTopicOfflineStatus)) {
    NS_ConvertUTF16toUTF8 status(aData);
    if (status.EqualsLiteral("offline")) {
      NMC_CONN_LOG("NmcConnectionManager: device went offline");
      OnNetworkChanged(NmcNetworkType::None);
    } else {
      NMC_CONN_LOG("NmcConnectionManager: device came online");
      OnNetworkChanged(NmcNetworkType::Unknown);
    }
  } else if (!strcmp(aTopic, kTopicLinkStatus)) {
    // Network interface changed (e.g. Wi-Fi → cellular).
    // We don't know the new type from this notification alone, but we
    // should drop connections to force reconnect on the new interface.
    NMC_CONN_LOG("NmcConnectionManager: link status changed, reconnecting");
    OnNetworkChanged(NmcNetworkType::Unknown);
  }

  return NS_OK;
}

// ---------------------------------------------------------------------------
// nsITimerCallback (background idle timer)
// ---------------------------------------------------------------------------

NS_IMETHODIMP
NmcConnectionManager::Notify(nsITimer* aTimer) {
  MutexAutoLock lock(mMutex);
  if (mShutdown) return NS_OK;

  if (!mAppBackgrounded) {
    // Race: app was foregrounded between timer arm and fire. Ignore.
    NMC_CONN_LOG("NmcConnectionManager: idle timer fired but app is "
                 "foregrounded — ignoring");
    return NS_OK;
  }

  NMC_CONN_LOG("NmcConnectionManager: background idle timeout — closing "
               "all WS connections");
  // Release lock before touching the resolver (avoids lock ordering issues)
  lock.Unlock();
  CloseAllConnections();
  return NS_OK;
}

// ---------------------------------------------------------------------------
// nsINamed
// ---------------------------------------------------------------------------

NS_IMETHODIMP
NmcConnectionManager::GetName(nsACString& aName) {
  aName.AssignLiteral("NmcConnectionManager");
  return NS_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle events
// ---------------------------------------------------------------------------

void NmcConnectionManager::OnAppForegrounded() {
  MutexAutoLock lock(mMutex);
  if (mShutdown) return;

  bool wasBackgrounded = mAppBackgrounded;
  mAppBackgrounded = false;
  CancelBackgroundIdleTimer();

  if (wasBackgrounded) {
    NMC_CONN_LOG("NmcConnectionManager: app foregrounded — connections "
                 "will be created on demand");
  }
  // We do NOT eagerly reconnect. Connections are created on demand when
  // the next .bit resolve occurs. This avoids unnecessary network traffic
  // if the user doesn't navigate to a .bit domain.
}

void NmcConnectionManager::OnAppBackgrounded() {
  MutexAutoLock lock(mMutex);
  if (mShutdown) return;

  mAppBackgrounded = true;
  NMC_CONN_LOG("NmcConnectionManager: app backgrounded — starting idle "
               "timer (%u ms)", kBackgroundIdleTimeoutMs);

  // Start the idle timer. If no .bit lookup occurs within 5 minutes,
  // we close all connections to conserve battery.
  StartBackgroundIdleTimer();
}

void NmcConnectionManager::OnNetworkChanged(NmcNetworkType aNewType) {
  {
    MutexAutoLock lock(mMutex);
    if (mShutdown) return;

    NmcNetworkType oldType = mCurrentNetworkType;
    mCurrentNetworkType = aNewType;

    if (aNewType == NmcNetworkType::None) {
      NMC_CONN_LOG("NmcConnectionManager: network lost — closing connections");
    } else if (oldType != aNewType) {
      NMC_CONN_LOG("NmcConnectionManager: network changed (%u → %u) — "
                   "dropping connections to reconnect on new interface",
                   (unsigned)oldType, (unsigned)aNewType);
    }
  }

  // Drop all connections. The pool will lazily reconnect when the next
  // resolve request arrives on the new network.
  CloseAllConnections();
}

bool NmcConnectionManager::ShouldResolveOnDemand() const {
  MutexAutoLock lock(mMutex);
  return mAppBackgrounded;
}

void NmcConnectionManager::NotifyLookupActivity() {
  MutexAutoLock lock(mMutex);
  if (mShutdown || !mAppBackgrounded) return;

  // Reset the idle timer — we're still actively resolving .bit domains.
  NMC_CONN_LOG("NmcConnectionManager: .bit lookup activity — resetting "
               "idle timer");
  CancelBackgroundIdleTimer();
  StartBackgroundIdleTimer();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void NmcConnectionManager::StartBackgroundIdleTimer() {
  // Must be called with mMutex held.
  CancelBackgroundIdleTimer();

  // Timer must be created on the main thread.
  if (NS_IsMainThread()) {
    NS_NewTimerWithCallback(getter_AddRefs(mBackgroundIdleTimer), this,
                            kBackgroundIdleTimeoutMs,
                            nsITimer::TYPE_ONE_SHOT);
  } else {
    RefPtr<NmcConnectionManager> self = this;
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "NmcConnectionManager::StartBackgroundIdleTimer",
        [self]() {
          MutexAutoLock lock(self->mMutex);
          if (self->mShutdown || !self->mAppBackgrounded) return;
          NS_NewTimerWithCallback(
              getter_AddRefs(self->mBackgroundIdleTimer), self,
              kBackgroundIdleTimeoutMs, nsITimer::TYPE_ONE_SHOT);
        }));
  }
}

void NmcConnectionManager::CancelBackgroundIdleTimer() {
  // Must be called with mMutex held.
  if (mBackgroundIdleTimer) {
    mBackgroundIdleTimer->Cancel();
    mBackgroundIdleTimer = nullptr;
  }
}

void NmcConnectionManager::CloseAllConnections() {
  // Delegate to the resolver's Shutdown-and-reinit or direct pool access.
  // For now we call Shutdown() which closes all WS connections, then the
  // resolver can re-Init() on next resolve.
  //
  // NOTE: A production implementation would expose a
  // nsNamecoinResolver::CloseIdleConnections() method that only closes
  // the pool without clearing the server list. For Phase 4 we use the
  // existing Shutdown()/Init() pair.
  if (mResolver) {
    NMC_CONN_LOG("NmcConnectionManager: closing all resolver connections");
    mResolver->Shutdown();
    // Re-initialize so the resolver is ready for the next lookup
    mResolver->Init();
  }
}

}  // namespace net
}  // namespace mozilla

#endif  // ANDROID
