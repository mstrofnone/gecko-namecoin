/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NmcConnectionManager_h__
#define NmcConnectionManager_h__

/**
 * NmcConnectionManager — Battery-aware WebSocket connection management
 *                        for Namecoin resolution on Android/GeckoView.
 *
 * Wraps nsNamecoinResolver's ElectrumX connection pool with Android lifecycle
 * awareness. Listens for app foreground/background transitions and network
 * changes via nsIObserverService, and manages the connection pool accordingly:
 *
 *   - On background: close idle WebSocket connections immediately; start a
 *     5-minute idle timer — if no .bit lookup occurs before it fires, close
 *     ALL remaining connections.
 *   - On foreground: cancel the idle timer; new connections created on demand.
 *   - On network change (Wi-Fi ↔ LTE): drop all connections and let the
 *     pool reconnect on the new network on next resolve.
 *
 * The Java/Kotlin side bridges Android's ActivityLifecycleCallbacks into
 * Gecko observer notifications:
 *   - "geckoview:activity-state-changed" with data "foreground" / "background"
 *   - "network:offline-status-changed" (standard Gecko notification)
 *
 * This class is Android-only (#ifdef ANDROID).
 */

#ifdef ANDROID

#include "nscore.h"
#include "nsIObserver.h"
#include "nsISupports.h"
#include "nsITimer.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Mutex.h"

namespace mozilla {
namespace net {

class nsNamecoinResolver;

/**
 * Network type reported by the Java bridge. Used to detect transitions
 * between connection types that require dropping and reconnecting.
 */
enum class NmcNetworkType : uint8_t {
  Unknown = 0,
  WiFi,
  Cellular,
  None
};

class NmcConnectionManager final : public nsIObserver,
                                    public nsITimerCallback,
                                    public nsINamed {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER
  NS_DECL_NSITIMERCALLBACK
  NS_DECL_NSINAMED

  /**
   * Create a connection manager wrapping the given resolver.
   * The resolver's connection pool will be managed by this object.
   *
   * @param aResolver  The nsNamecoinResolver instance whose pool we manage.
   */
  explicit NmcConnectionManager(nsNamecoinResolver* aResolver);

  /**
   * Register as an observer for lifecycle and network notifications.
   * Call once after construction, from the main thread.
   */
  nsresult Init();

  /**
   * Unregister observers and cancel timers. Safe to call multiple times.
   */
  void Shutdown();

  // ---- Lifecycle events (called from observer or directly) ----------------

  /**
   * App has moved to the foreground. Cancel any pending idle shutdown
   * and allow connections to be created on demand.
   */
  void OnAppForegrounded();

  /**
   * App has moved to the background. Close idle connections immediately
   * and start the background idle timer.
   */
  void OnAppBackgrounded();

  /**
   * Network type changed (e.g. Wi-Fi → LTE). Drop all connections so
   * the pool reconnects on the new network.
   */
  void OnNetworkChanged(NmcNetworkType aNewType);

  /**
   * Returns true when the app is backgrounded. When true, the resolver
   * should only create connections on demand (lazy connect) and avoid
   * keepalive/preconnect behavior.
   */
  bool ShouldResolveOnDemand() const;

  /**
   * Notify the manager that a .bit lookup is in progress. Resets the
   * background idle timer to prevent premature connection teardown.
   */
  void NotifyLookupActivity();

 private:
  ~NmcConnectionManager();

  void StartBackgroundIdleTimer();  // 5 min timer
  void CancelBackgroundIdleTimer();
  void CloseAllConnections();

  // The resolver we manage. Raw pointer — the resolver owns us (or we share
  // the same parent scope), so it always outlives us.
  nsNamecoinResolver* mResolver;

  // State (guarded by mMutex)
  mutable Mutex mMutex;
  bool mInitialized = false;
  bool mShutdown = false;
  bool mAppBackgrounded = false;
  NmcNetworkType mCurrentNetworkType = NmcNetworkType::Unknown;

  // Background idle timer: fires 5 minutes after the last .bit lookup
  // while the app is backgrounded.
  nsCOMPtr<nsITimer> mBackgroundIdleTimer;
  static constexpr uint32_t kBackgroundIdleTimeoutMs = 5 * 60 * 1000;  // 5 min
};

}  // namespace net
}  // namespace mozilla

#endif  // ANDROID

#endif  // NmcConnectionManager_h__
