/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NmcGeckoViewSettings_h__
#define NmcGeckoViewSettings_h__

/**
 * NmcGeckoViewSettings — XPCOM component bridging GeckoView runtime settings
 *                         to the Namecoin network.namecoin.* preferences.
 *
 * GeckoView embedding apps (Fenix, Focus, custom browsers) use
 * GeckoRuntimeSettings to configure Gecko behavior at runtime. This component
 * is the C++ side of the bridge — it reads/writes the standard Gecko
 * preferences that nsNamecoinResolver consumes.
 *
 * Kotlin side: NamecoinSettings.kt provides the embedding API.
 * Java side: GeckoRuntimeSettings patch adds builder/getter/setter methods
 *            that call through JNI to this component.
 *
 * Preferences bridged:
 *   network.namecoin.enabled            → setNamecoinEnabled / getNamecoinEnabled
 *   network.namecoin.electrumx_servers  → setElectrumXServers / getElectrumXServers
 *   network.namecoin.require_https      → setRequireHttps / getRequireHttps
 */

#include "nscore.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsISupports.h"
#include "mozilla/RefPtr.h"

// XPCOM interface UUID for nsINmcGeckoViewSettings
// {a7e3f1d2-4b8c-4e9a-b1d6-8f2c3a5e7b90}
#define NS_INMCGECKOVIEWSETTINGS_IID                     \
  {                                                      \
    0xa7e3f1d2, 0x4b8c, 0x4e9a, {                       \
      0xb1, 0xd6, 0x8f, 0x2c, 0x3a, 0x5e, 0x7b, 0x90   \
    }                                                    \
  }

class nsINmcGeckoViewSettings : public nsISupports {
 public:
  NS_DECLARE_STATIC_IID_ACCESSOR(NS_INMCGECKOVIEWSETTINGS_IID)

  NS_IMETHOD SetNamecoinEnabled(bool aEnabled) = 0;
  NS_IMETHOD GetNamecoinEnabled(bool* aEnabled) = 0;

  NS_IMETHOD SetElectrumXServers(
      const nsTArray<nsCString>& aServers) = 0;
  NS_IMETHOD GetElectrumXServers(
      nsTArray<nsCString>& aServers) = 0;

  NS_IMETHOD SetRequireHttps(bool aRequire) = 0;
  NS_IMETHOD GetRequireHttps(bool* aRequire) = 0;
};

NS_DEFINE_STATIC_IID_ACCESSOR(nsINmcGeckoViewSettings,
                               NS_INMCGECKOVIEWSETTINGS_IID)

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

namespace mozilla {
namespace net {

// {c4d5e6f7-8a9b-0c1d-2e3f-4a5b6c7d8e9f}
#define NMC_GECKOVIEWSETTINGS_CID                        \
  {                                                      \
    0xc4d5e6f7, 0x8a9b, 0x0c1d, {                       \
      0x2e, 0x3f, 0x4a, 0x5b, 0x6c, 0x7d, 0x8e, 0x9f   \
    }                                                    \
  }

#define NMC_GECKOVIEWSETTINGS_CONTRACTID \
  "@mozilla.org/network/nmc-geckoview-settings;1"

class NmcGeckoViewSettings final : public nsINmcGeckoViewSettings {
 public:
  NS_DECL_ISUPPORTS

  NmcGeckoViewSettings() = default;

  NS_IMETHOD SetNamecoinEnabled(bool aEnabled) override;
  NS_IMETHOD GetNamecoinEnabled(bool* aEnabled) override;

  NS_IMETHOD SetElectrumXServers(
      const nsTArray<nsCString>& aServers) override;
  NS_IMETHOD GetElectrumXServers(
      nsTArray<nsCString>& aServers) override;

  NS_IMETHOD SetRequireHttps(bool aRequire) override;
  NS_IMETHOD GetRequireHttps(bool* aRequire) override;

 private:
  ~NmcGeckoViewSettings() = default;
};

}  // namespace net
}  // namespace mozilla

#endif  // NmcGeckoViewSettings_h__
