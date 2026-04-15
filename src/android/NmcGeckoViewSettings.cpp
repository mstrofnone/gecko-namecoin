/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * NmcGeckoViewSettings — Bridges GeckoView runtime settings to Namecoin prefs.
 *
 * This is a thin layer: each setter writes to the underlying Gecko preference
 * via nsIPrefBranch, and each getter reads it back. The nsNamecoinResolver
 * already watches these preferences, so changes take effect immediately.
 *
 * Preference keys (must match nsNamecoinResolver.cpp):
 *   network.namecoin.enabled
 *   network.namecoin.electrumx_servers   (comma-separated wss:// URLs)
 *   network.namecoin.require_https
 */

#include "NmcGeckoViewSettings.h"

#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "nsIPrefBranch.h"
#include "nsServiceManagerUtils.h"
#include "nsCCharSeparatedTokenizer.h"

namespace mozilla {
namespace net {

static LazyLogModule gNmcSettingsLog("namecoin");
#define NMC_SETTINGS_LOG(...) \
  MOZ_LOG(gNmcSettingsLog, LogLevel::Debug, (__VA_ARGS__))

// Preference keys
static constexpr char kPrefEnabled[]     = "network.namecoin.enabled";
static constexpr char kPrefServers[]     = "network.namecoin.electrumx_servers";
static constexpr char kPrefRequireHttps[] = "network.namecoin.require_https";

// ---------------------------------------------------------------------------
// NS_IMPL
// ---------------------------------------------------------------------------

NS_IMPL_ISUPPORTS(NmcGeckoViewSettings, nsINmcGeckoViewSettings)

// ---------------------------------------------------------------------------
// Enabled
// ---------------------------------------------------------------------------

NS_IMETHODIMP
NmcGeckoViewSettings::SetNamecoinEnabled(bool aEnabled) {
  NMC_SETTINGS_LOG("NmcGeckoViewSettings::SetNamecoinEnabled(%s)",
                   aEnabled ? "true" : "false");
  return Preferences::SetBool(kPrefEnabled, aEnabled);
}

NS_IMETHODIMP
NmcGeckoViewSettings::GetNamecoinEnabled(bool* aEnabled) {
  NS_ENSURE_ARG_POINTER(aEnabled);
  *aEnabled = Preferences::GetBool(kPrefEnabled, false);
  return NS_OK;
}

// ---------------------------------------------------------------------------
// ElectrumX servers
// ---------------------------------------------------------------------------

NS_IMETHODIMP
NmcGeckoViewSettings::SetElectrumXServers(
    const nsTArray<nsCString>& aServers) {
  // Join array into comma-separated string for the pref
  nsAutoCString joined;
  for (uint32_t i = 0; i < aServers.Length(); i++) {
    if (i > 0) joined.Append(',');
    joined.Append(aServers[i]);
  }

  NMC_SETTINGS_LOG("NmcGeckoViewSettings::SetElectrumXServers: %s",
                   joined.get());
  return Preferences::SetCString(kPrefServers, joined);
}

NS_IMETHODIMP
NmcGeckoViewSettings::GetElectrumXServers(nsTArray<nsCString>& aServers) {
  aServers.Clear();

  nsAutoCString serversPref;
  Preferences::GetCString(kPrefServers, serversPref);

  if (serversPref.IsEmpty()) {
    return NS_OK;
  }

  nsCCharSeparatedTokenizer tokenizer(serversPref, ',');
  while (tokenizer.hasMoreTokens()) {
    nsAutoCString server(tokenizer.nextToken());
    server.Trim(" \t");
    if (!server.IsEmpty()) {
      aServers.AppendElement(server);
    }
  }

  return NS_OK;
}

// ---------------------------------------------------------------------------
// Require HTTPS
// ---------------------------------------------------------------------------

NS_IMETHODIMP
NmcGeckoViewSettings::SetRequireHttps(bool aRequire) {
  NMC_SETTINGS_LOG("NmcGeckoViewSettings::SetRequireHttps(%s)",
                   aRequire ? "true" : "false");
  return Preferences::SetBool(kPrefRequireHttps, aRequire);
}

NS_IMETHODIMP
NmcGeckoViewSettings::GetRequireHttps(bool* aRequire) {
  NS_ENSURE_ARG_POINTER(aRequire);
  *aRequire = Preferences::GetBool(kPrefRequireHttps, true);
  return NS_OK;
}

}  // namespace net
}  // namespace mozilla
