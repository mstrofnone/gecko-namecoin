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
#include "mozilla/Mutex.h"
#include "mozilla/net/DNS.h"
#include "nsITimer.h"
#include "mozilla/Monitor.h"

namespace mozilla {
namespace net {

// Forward declaration — defined in nsNamecoinResolver.cpp
class nsElectrumXConnectionPool;

// ---------------------------------------------------------------------------
// Parsed Namecoin name value (d/ namespace JSON)
// ---------------------------------------------------------------------------

struct NamecoinTLSARecord {
  uint8_t usage;        // 2=DANE-TA, 3=DANE-EE
  uint8_t selector;     // 0=full cert, 1=SPKI
  uint8_t matchType;    // 0=exact, 1=SHA-256, 2=SHA-512
  nsCString data;       // hex or base64 encoded cert/hash
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

  // Full value (for TLSA etc. — used by Phase 2)
  NamecoinNameValue nameValue;

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

  // WebSocket connection pool (reuses connections across ElectrumX requests)
  RefPtr<nsElectrumXConnectionPool> mConnectionPool;
};

}  // namespace net
}  // namespace mozilla

#endif  // nsNamecoinResolver_h__
