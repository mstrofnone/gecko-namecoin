/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsNamecoinResolver_h__
#define nsNamecoinResolver_h__

/**
 * nsNamecoinResolver — Native Namecoin (.bit) domain resolver for Gecko
 *
 * Resolves .bit domain names by querying the Namecoin blockchain via the
 * ElectrumX JSON-RPC-over-WebSocket protocol. Integrated into nsHostResolver
 * as an alternate resolution path when network.namecoin.enabled is true.
 *
 * Architecture:
 *   nsHostResolver::ResolveHost()
 *     └─ if TLD == "bit" && network.namecoin.enabled
 *          └─ nsNamecoinResolver::Resolve(host, callback)
 *               ├─ Compute scripthash (OP_NAME_UPDATE index script + SHA-256)
 *               ├─ WebSocket → ElectrumX: scripthash.get_history
 *               ├─ WebSocket → ElectrumX: transaction.get (verbose)
 *               ├─ Decode NAME_UPDATE output → JSON value
 *               ├─ Parse ip/ip6/map/alias/ns/translate fields
 *               └─ Return nsHostRecord with A/AAAA results
 *
 * Reference implementation: tls-namecoin-ext/background/electrumx-ws.js
 *
 * Phase 1 scope (this file):
 *   - DNS resolution only (A/AAAA records from Namecoin blockchain)
 *   - No TLS/DANE validation (Phase 2)
 *   - No UI indicators (Phase 3)
 *
 * Controlled by about:config:
 *   network.namecoin.enabled            — boolean, default false
 *   network.namecoin.electrumx_servers  — string, comma-separated wss:// URLs
 *   network.namecoin.cache_ttl_seconds  — integer, default 3600
 *   network.namecoin.max_alias_hops     — integer, default 5
 *   network.namecoin.connection_timeout_ms — integer, default 10000
 */

#include "nscore.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsISupports.h"
#include "mozilla/RefPtr.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Mutex.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/net/DNS.h"
#include "nsITimer.h"
#include "mozilla/Monitor.h"
#include "nsTHashMap.h"
#include "NmcDaneValidator.h"

namespace mozilla {
namespace net {

// Forward declarations — defined in nsNamecoinResolver.cpp
class nsElectrumXConnectionPool;

// Forward declaration — Phase 2 DANE validation cache
class NmcDaneCache;

// ---------------------------------------------------------------------------
// Parsed Namecoin name value (d/ namespace JSON)
// ---------------------------------------------------------------------------

struct NamecoinTLSARecord {
  uint8_t usage;        // 2=DANE-TA, 3=DANE-EE
  uint8_t selector;     // 0=full cert, 1=SPKI
  uint8_t matchType;    // 0=exact, 1=SHA-256, 2=SHA-512
  nsCString data;       // hex or base64 encoded cert/hash

  NamecoinTLSARecord() = default;
  NamecoinTLSARecord(const NamecoinTLSARecord&) = default;
  NamecoinTLSARecord& operator=(const NamecoinTLSARecord&) = default;
  NamecoinTLSARecord(NamecoinTLSARecord&&) = default;
  NamecoinTLSARecord& operator=(NamecoinTLSARecord&&) = default;
};

struct NamecoinNameValue {
  nsCString ip;         // IPv4 (A record)
  nsCString ip6;        // IPv6 (AAAA record)
  nsTArray<nsCString> ns;  // NS delegation targets
  nsTArray<NamecoinTLSARecord> tls;  // DANE-TLSA records
  nsCString alias;      // CNAME-like, follow chain
  nsCString translate;  // Map to standard domain
  nsCString tor;        // .onion address

  // map: subdomain → NamecoinNameValue (simplified as nested JSON for now)
  // Full map traversal implemented in Resolve() directly
  bool hasMap = false;

  NamecoinNameValue() = default;
  NamecoinNameValue(NamecoinNameValue&&) = default;
  NamecoinNameValue& operator=(NamecoinNameValue&&) = default;

  // Deep copy via nsTArray::Clone()
  NamecoinNameValue(const NamecoinNameValue& aOther)
      : ip(aOther.ip),
        ip6(aOther.ip6),
        ns(aOther.ns.Clone()),
        tls(aOther.tls.Clone()),
        alias(aOther.alias),
        translate(aOther.translate),
        tor(aOther.tor),
        hasMap(aOther.hasMap) {}

  NamecoinNameValue& operator=(const NamecoinNameValue& aOther) {
    if (this != &aOther) {
      ip = aOther.ip;
      ip6 = aOther.ip6;
      ns = aOther.ns.Clone();
      tls = aOther.tls.Clone();
      alias = aOther.alias;
      translate = aOther.translate;
      tor = aOther.tor;
      hasMap = aOther.hasMap;
    }
    return *this;
  }
};

// ---------------------------------------------------------------------------
// Resolution result
// ---------------------------------------------------------------------------

struct NamecoinResolveResult {
  bool resolved = false;
  bool expired = false;
  nsCString error;

  // Resolved addresses
  nsTArray<NetAddr> addresses;

  // Metadata
  uint32_t blockHeight = 0;
  uint32_t updateHeight = 0;
  nsCString txHash;

  // Full value (for TLSA/DANE validation)
  NamecoinNameValue nameValue;

  // HTTPS-only flag: set when TLSA records exist and require_tls pref is true
  bool httpsOnly = false;

  // Suggested cache TTL in seconds
  uint32_t ttlSeconds = 3600;
};

// ---------------------------------------------------------------------------
// Callback interface
// ---------------------------------------------------------------------------

class nsNamecoinResolveCallback {
 public:
  virtual void OnNamecoinResolved(const NamecoinResolveResult& aResult) = 0;
  virtual ~nsNamecoinResolveCallback() = default;
};

// ---------------------------------------------------------------------------
// nsNamecoinResolver
// ---------------------------------------------------------------------------

class nsNamecoinResolver final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(nsNamecoinResolver)

  nsNamecoinResolver();

  /**
   * Initialize from preferences. Call once at startup.
   * Reads network.namecoin.* prefs and sets up server list.
   */
  nsresult Init();

  /**
   * Returns true if Namecoin resolution is enabled and configured.
   * Check this before calling Resolve().
   */
  bool IsEnabled() const;

  /**
   * Resolve a hostname asynchronously.
   *
   * @param aHostname  The full hostname (e.g. "example.bit", "www.example.bit")
   *                   Must end in ".bit".
   * @param aCallback  Called on the calling thread when resolution completes.
   *
   * Resolution is performed on a background thread. The callback is invoked
   * on the main thread (or the calling thread if already off-main-thread).
   */
  nsresult Resolve(const nsACString& aHostname,
                   nsNamecoinResolveCallback* aCallback);

  /**
   * Synchronous resolve (blocks calling thread — for testing only).
   * Do NOT call from main thread in production builds.
   */
  nsresult ResolveSync(const nsACString& aHostname,
                       NamecoinResolveResult& aResult);

  /**
   * Test connectivity to configured ElectrumX servers.
   * Returns the first responsive server's block height, or -1 on failure.
   */
  int32_t TestConnectivity();

  /**
   * Shutdown: cancel pending requests, close WebSocket connections.
   */
  void Shutdown();

  // ---- Static helpers (exposed for testing) --------------------------------

  /**
   * Compute the ElectrumX scripthash for a Namecoin name lookup.
   *
   * Builds the canonical name-index script:
   *   OP_NAME_UPDATE (0x53) + pushdata(name) + pushdata("") +
   *   OP_2DROP (0x6d) + OP_DROP (0x75) + OP_RETURN (0x6a)
   * Then: SHA-256 → reverse bytes → hex string.
   *
   * @param aName     e.g. "d/example"
   * @param aScripthash  Output: reversed SHA-256 hex of the index script
   */
  static nsresult ComputeScripthash(const nsACString& aName,
                                    nsACString& aScripthash);

  /**
   * Decode a Namecoin NAME_UPDATE output script.
   *
   * Script format:
   *   0x53 (OP_NAME_UPDATE)
   *   + pushdata(name_bytes)
   *   + pushdata(value_bytes)
   *   + OP_2DROP + OP_DROP + <address script>
   *
   * @param aScriptHex  Hex-encoded script bytes
   * @param aName       Output: decoded name string (e.g. "d/example")
   * @param aValue      Output: decoded value string (JSON)
   * @returns true if successful, false if not a NAME_UPDATE script
   */
  static bool DecodeNameScript(const nsACString& aScriptHex,
                                nsACString& aName,
                                nsACString& aValue);

  /**
   * Parse the Namecoin d/ name value JSON into a NamecoinNameValue struct.
   *
   * @param aValueJson   UTF-8 JSON string
   * @param aHostname    Full hostname being resolved (for subdomain map lookup)
   * @param aResult      Output struct
   * @returns NS_OK on success
   */
  static nsresult ParseNameValue(const nsACString& aValueJson,
                                 const nsACString& aHostname,
                                 NamecoinNameValue& aResult);

  /**
   * Returns true if the hostname ends with ".bit" (case-insensitive).
   */
  static bool IsNamecoinHost(const nsACString& aHostname);

  /**
   * Get the DANE validation cache (Phase 2).
   * Returns nullptr if resolver is not initialized.
   */
  NmcDaneCache* GetDaneCache() const;

  // ---- Global name value cache (Phase 2 DANE hook-in) ---------------------
  //
  // After DNS resolution, the resolved NamecoinNameValue (including TLSA
  // records) is stored here keyed by hostname so that the cert verifier
  // (SSLServerCertVerification.cpp) can retrieve it during TLS handshake.
  // Thread-safe via internal static mutex.

  /**
   * Store a resolved name value for a hostname.
   * Called from nsHostResolver after successful .bit DNS resolution.
   *
   * @param aHostname  The resolved hostname (e.g. "example.bit")
   * @param aValue     The parsed name value (containing TLSA records etc.)
   * @param aTTLSeconds  Cache TTL
   */
  static void StoreNameValue(const nsACString& aHostname,
                              const NamecoinNameValue& aValue,
                              uint32_t aTTLSeconds);

  /**
   * Retrieve a stored name value for a hostname.
   * Called from SSLServerCertVerification to get TLSA records.
   *
   * @param aHostname  The hostname to look up
   * @param aValue     Output: the stored name value
   * @returns true if found and not expired
   */
  static bool GetStoredNameValue(const nsACString& aHostname,
                                  NamecoinNameValue& aValue);

  /**
   * Get TLSA records applicable for a specific port.
   * Checks port-specific records (_tcp._<port>) before top-level tls array.
   *
   * @param aNameValue  Parsed Namecoin name value
   * @param aPort       TCP port number (e.g. 443)
   * @param aRecords    Output: applicable TLSA records
   */
  static void GetTlsaForPort(const NamecoinNameValue& aNameValue,
                             uint16_t aPort,
                             nsTArray<NamecoinTLSARecord>& aRecords);

 private:
  ~nsNamecoinResolver();

  // Internal resolution pipeline (runs on background thread)
  nsresult ResolveInternal(const nsACString& aHostname,
                            NamecoinResolveResult& aResult);

  // ElectrumX WebSocket helpers
  nsresult ElectrumXRequest(const nsCString& aServer,
                             const nsCString& aMethod,
                             const nsCString& aParamsJson,
                             nsCString& aResultJson);

  nsresult ElectrumXRequestAny(const nsCString& aMethod,
                                const nsCString& aParamsJson,
                                nsCString& aResultJson,
                                nsCString& aUsedServer);

  nsresult ElectrumXRequestValidated(const nsCString& aMethod,
                                      const nsCString& aParamsJson,
                                      nsCString& aResultJson,
                                      nsCString& aUsedServer);

  nsresult GetCurrentBlockHeight(uint32_t& aHeight);

  // Alias chain resolution (max hops controlled by pref)
  nsresult ResolveAlias(const nsACString& aAlias,
                        NamecoinNameValue& aResult,
                        int aHopsRemaining);

  // Internal state
  mutable Mutex mMutex;
  bool mEnabled = false;
  bool mShuttingDown = false;
  nsTArray<nsCString> mServers;  // Ordered list of ElectrumX servers
  uint32_t mCacheTTLSeconds = 3600;
  uint32_t mMaxAliasHops = 5;
  uint32_t mConnectionTimeoutMs = 10000;
  bool mQueryMultipleServers = false;
  bool mRequireTLS = true;  // Phase 2: force HTTPS when TLSA records exist

  // WebSocket connection pool (reuses connections across ElectrumX requests)
  RefPtr<nsElectrumXConnectionPool> mConnectionPool;

  // Phase 2: DANE-TLSA validation cache
  // Caches cert validation results to avoid repeated crypto operations.
  // Key: "domain:cert_sha256_hex", TTL from network.namecoin.cache_ttl_seconds
  UniquePtr<NmcDaneCache> mDaneCache;

  // ---- Static name-value cache for DANE hook-in ---------------------------
  struct NameValueCacheEntry {
    NamecoinNameValue value;
    mozilla::TimeStamp expiry;

    NameValueCacheEntry() = default;
    NameValueCacheEntry(const NameValueCacheEntry&) = default;
    NameValueCacheEntry& operator=(const NameValueCacheEntry&) = default;
    NameValueCacheEntry(NameValueCacheEntry&&) = default;
    NameValueCacheEntry& operator=(NameValueCacheEntry&&) = default;
  };
  static mozilla::StaticMutex sNameValueCacheMutex;
  static nsTHashMap<nsCStringHashKey, NameValueCacheEntry> sNameValueCache;
};

}  // namespace net
}  // namespace mozilla

#endif  // nsNamecoinResolver_h__
