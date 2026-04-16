/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NmcDaneValidator_h__
#define NmcDaneValidator_h__

/**
 * NmcDaneValidator — DANE-TLSA validation for Namecoin .bit domains
 *
 * Validates TLS certificates against DANE/TLSA records stored in the
 * Namecoin blockchain, per RFC 6698 and the Namecoin d/ namespace spec.
 *
 * Phase 2 component — consumes NamecoinTLSARecord data populated by
 * nsNamecoinResolver (Phase 1) and validates server certificates during
 * the TLS handshake.
 *
 * Architecture:
 *   AuthCertificateCallback (nsNSSCallbacks.cpp)
 *     └─ if domain is .bit AND standard CA validation fails
 *          └─ NmcValidateDane(nameValue, cert, host, port)
 *               ├─ Look up port-specific TLSA: _tcp._<port> in map
 *               ├─ Fall back to top-level tls array
 *               ├─ For each TLSA record:
 *               │    ├─ Extract cert data (selector: 0=DER, 1=SPKI)
 *               │    ├─ Compute match (matchType: 0=exact, 1=SHA-256, 2=SHA-512)
 *               │    └─ Check usage (2=DANE-TA chain, 3=DANE-EE end-entity)
 *               └─ Return OK/FAIL/NO_RECORD
 *
 * Reference:
 *   RFC 6698 — The DNS-Based Authentication of Named Entities (DANE)
 *   firefox2.txt Section 5 — TLS/NSS Integration
 *   Namecoin safetlsa / ncp11 (Go reference implementations)
 *
 * Controlled by about:config:
 *   network.namecoin.cache_ttl_seconds  — TTL for DANE cache entries
 *   network.namecoin.require_tls        — force HTTPS when TLSA exists
 */

#include "nscore.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsTHashMap.h"
#include "mozilla/Mutex.h"
#include "mozilla/TimeStamp.h"

// NSS
#include "cert.h"     // CERTCertificate, CERTCertList
#include "seccomon.h"

// Forward declarations from nsNamecoinResolver.h
namespace mozilla {
namespace net {
struct NamecoinNameValue;
struct NamecoinTLSARecord;
}  // namespace net
}  // namespace mozilla

namespace mozilla {
namespace net {

// ---------------------------------------------------------------------------
// Validation result enum
// ---------------------------------------------------------------------------

enum class NmcDaneValidateResult : uint8_t {
  /**
   * NMC_DANE_OK — At least one TLSA record matched the server certificate.
   * The connection should be allowed to proceed.
   */
  NMC_DANE_OK = 0,

  /**
   * NMC_DANE_NO_RECORD — No TLSA records found for this domain/port.
   * Caller should proceed with the normal certificate error path
   * (standard CA validation result stands).
   */
  NMC_DANE_NO_RECORD = 1,

  /**
   * NMC_DANE_FAIL — TLSA records exist but none matched the server cert.
   * This is a hard failure: the certificate MUST be rejected.
   * Do NOT fall through to insecure — the blockchain explicitly specifies
   * what certs are allowed, and this one isn't.
   */
  NMC_DANE_FAIL = 2,
};

// ---------------------------------------------------------------------------
// DANE validation cache
// ---------------------------------------------------------------------------

struct NmcDaneCacheEntry {
  NmcDaneValidateResult result;
  TimeStamp expiry;
};

/**
 * NmcDaneCache — Caches DANE validation results to avoid repeated
 * blockchain queries and crypto operations during TLS handshakes.
 *
 * Key: "domain:cert_fingerprint_sha256_hex"
 * TTL: from network.namecoin.cache_ttl_seconds preference
 *
 * Thread-safe via internal mutex.
 */
class NmcDaneCache final {
 public:
  NmcDaneCache() : mMutex("NmcDaneCache::mMutex") {}

  /**
   * Look up a cached validation result.
   * @param aKey     Cache key: "domain:sha256hex"
   * @param aResult  Output: cached result if found and not expired
   * @returns true if a valid (non-expired) entry was found
   */
  bool Lookup(const nsACString& aKey, NmcDaneValidateResult& aResult);

  /**
   * Store a validation result in the cache.
   * @param aKey       Cache key: "domain:sha256hex"
   * @param aResult    Validation result to cache
   * @param aTTLSeconds  Time-to-live in seconds
   */
  void Put(const nsACString& aKey, NmcDaneValidateResult aResult,
           uint32_t aTTLSeconds);

  /**
   * Remove all entries for a given domain prefix.
   * Used when a name value changes or expires.
   */
  void InvalidateDomain(const nsACString& aDomain);

  /**
   * Remove all expired entries. Called periodically.
   */
  void EvictExpired();

  /**
   * Clear the entire cache.
   */
  void Clear();

 private:
  Mutex mMutex;
  nsTHashMap<nsCStringHashKey, NmcDaneCacheEntry> mEntries;
};

// ---------------------------------------------------------------------------
// Main validation entry point
// ---------------------------------------------------------------------------

/**
 * Validate a server certificate against DANE-TLSA records from the
 * Namecoin blockchain.
 *
 * @param aNameValue   Parsed Namecoin name value (contains tls array and map)
 * @param aCert        Server certificate (NSS CERTCertificate)
 * @param aCertChain   Full certificate chain (may be null for EE-only checks)
 * @param aHost        Hostname being connected to (e.g. "example.bit")
 * @param aPort        TCP port (usually 443)
 * @returns NmcDaneValidateResult
 */
NmcDaneValidateResult NmcValidateDane(
    const NamecoinNameValue& aNameValue,
    CERTCertificate* aCert,
    CERTCertList* aCertChain,
    const nsACString& aHost,
    uint16_t aPort);

// ---------------------------------------------------------------------------
// TLSA record lookup helpers
// ---------------------------------------------------------------------------

/**
 * Get the TLSA records applicable for a given port.
 *
 * Checks for port-specific records at _tcp._<port> in the name value's
 * map before falling back to the top-level tls array.
 *
 * @param aNameValue  Parsed Namecoin name value
 * @param aPort       TCP port number
 * @param aRecords    Output: applicable TLSA records
 */
void GetTlsaForPort(const NamecoinNameValue& aNameValue,
                    uint16_t aPort,
                    nsTArray<NamecoinTLSARecord>& aRecords);

// ---------------------------------------------------------------------------
// Internal helpers (exposed for testing)
// ---------------------------------------------------------------------------

/**
 * Extract cert data based on TLSA selector field.
 *
 * @param aCert      NSS certificate
 * @param aSelector  0 = full DER certificate, 1 = SubjectPublicKeyInfo DER
 * @param aOutput    Output: extracted bytes
 * @returns NS_OK on success
 */
nsresult NmcDaneExtractCertData(CERTCertificate* aCert,
                                uint8_t aSelector,
                                nsTArray<uint8_t>& aOutput);

/**
 * Compute a DANE matching hash.
 *
 * @param aData         Input data bytes
 * @param aMatchType    0 = exact (no hash), 1 = SHA-256, 2 = SHA-512
 * @param aOutput       Output: hash result (or copy of input for exact)
 * @returns NS_OK on success
 */
nsresult NmcDaneComputeMatch(const nsTArray<uint8_t>& aData,
                             uint8_t aMatchType,
                             nsTArray<uint8_t>& aOutput);

/**
 * Compute SHA-256 fingerprint of a DER-encoded certificate.
 * Used for cache key generation.
 *
 * @param aCert    NSS certificate
 * @param aHexOut  Output: lowercase hex-encoded SHA-256 fingerprint
 * @returns NS_OK on success
 */
nsresult NmcDaneCertFingerprint(CERTCertificate* aCert,
                                nsACString& aHexOut);

}  // namespace net
}  // namespace mozilla

#endif  // NmcDaneValidator_h__
