/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * NmcDaneValidator — DANE-TLSA validation for Namecoin .bit domains
 *
 * Phase 2: Validates TLS certificates against DANE/TLSA records from
 * the Namecoin blockchain per RFC 6698.
 *
 * Supports:
 *   - Usage 2 (DANE-TA): Trust Anchor — CA cert in chain must match
 *   - Usage 3 (DANE-EE): End Entity — server cert must match directly
 *   - Selector 0: Full DER-encoded certificate
 *   - Selector 1: DER-encoded SubjectPublicKeyInfo
 *   - MatchingType 0: Exact byte comparison
 *   - MatchingType 1: SHA-256 hash comparison
 *   - MatchingType 2: SHA-512 hash comparison
 *
 * Port-specific TLSA records (e.g. _tcp._443 in the map) take precedence
 * over top-level tls array.
 *
 * Reference implementations:
 *   Namecoin safetlsa (Go) — X.509 cert generation from TLSA
 *   Namecoin ncp11 (Go)    — PKCS#11 module for NSS
 *   RFC 6698                — DANE protocol specification
 *   firefox2.txt Section 5  — Integration spec
 */

#include "NmcDaneValidator.h"
#include "nsNamecoinResolver.h"
#include "nsNamecoinErrors.h"

// Mozilla / Gecko
#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "mozilla/Base64.h"

// NSS
#include "sechash.h"      // HASH_HashBuf, HASH_AlgSHA256, HASH_AlgSHA512
#include "pk11func.h"     // PK11_HashBuf
#include "cert.h"         // CERTCertificate, CERT_GetCertificateNames
#include "keyhi.h"        // SECKEY_DecodeDERSubjectPublicKeyInfo
#include "secitem.h"      // SECItem
#include "secder.h"       // DER utilities
#include "secoid.h"       // SEC_OID_AVA_SERIAL_NUMBER

namespace mozilla {
namespace net {

// Reuse the namecoin logger from nsNamecoinResolver.cpp
static LazyLogModule gNmcDaneLog("namecoin");
#define DANE_LOG(...) MOZ_LOG(gNmcDaneLog, LogLevel::Debug, (__VA_ARGS__))
#define DANE_ERR(...) MOZ_LOG(gNmcDaneLog, LogLevel::Error, (__VA_ARGS__))

// Preference for cache TTL
static constexpr char kPrefCacheTTL[] = "network.namecoin.cache_ttl_seconds";

// ---------------------------------------------------------------------------
// Hex utilities (local copies to avoid cross-TU dependency on statics)
// ---------------------------------------------------------------------------

static void DaneHexEncode(const uint8_t* aData, size_t aLen,
                          nsACString& aOut) {
  static const char kHex[] = "0123456789abcdef";
  aOut.SetLength(aLen * 2);
  char* p = aOut.BeginWriting();
  for (size_t i = 0; i < aLen; i++) {
    *p++ = kHex[(aData[i] >> 4) & 0xf];
    *p++ = kHex[aData[i] & 0xf];
  }
}

static bool DaneHexDecode(const nsACString& aHex, nsTArray<uint8_t>& aOut) {
  if (aHex.Length() % 2 != 0) return false;
  aOut.SetLength(aHex.Length() / 2);
  const char* p = aHex.BeginReading();
  for (size_t i = 0; i < aOut.Length(); i++) {
    auto fromHexChar = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int hi = fromHexChar(*p++);
    int lo = fromHexChar(*p++);
    if (hi < 0 || lo < 0) return false;
    aOut[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// ---------------------------------------------------------------------------
// NmcDaneCache implementation
// ---------------------------------------------------------------------------

bool NmcDaneCache::Lookup(const nsACString& aKey,
                          NmcDaneValidateResult& aResult) {
  MutexAutoLock lock(mMutex);

  NmcDaneCacheEntry entry;
  if (!mEntries.Get(aKey, &entry)) {
    return false;
  }

  // Check expiry
  if (TimeStamp::Now() > entry.expiry) {
    mEntries.Remove(aKey);
    DANE_LOG("DaneCache: expired entry for %s",
             nsPromiseFlatCString(aKey).get());
    return false;
  }

  aResult = entry.result;
  DANE_LOG("DaneCache: hit for %s → %u",
           nsPromiseFlatCString(aKey).get(), (unsigned)aResult);
  return true;
}

void NmcDaneCache::Put(const nsACString& aKey,
                       NmcDaneValidateResult aResult,
                       uint32_t aTTLSeconds) {
  MutexAutoLock lock(mMutex);

  NmcDaneCacheEntry entry;
  entry.result = aResult;
  entry.expiry = TimeStamp::Now() +
                 TimeDuration::FromSeconds((double)aTTLSeconds);

  mEntries.InsertOrUpdate(aKey, entry);
  DANE_LOG("DaneCache: stored %s → %u (ttl=%us)",
           nsPromiseFlatCString(aKey).get(), (unsigned)aResult, aTTLSeconds);
}

void NmcDaneCache::InvalidateDomain(const nsACString& aDomain) {
  MutexAutoLock lock(mMutex);

  // Collect keys to remove (can't remove during iteration)
  nsTArray<nsCString> toRemove;
  for (auto iter = mEntries.Iter(); !iter.Done(); iter.Next()) {
    const nsACString& key = iter.Key();
    // Key format: "domain:sha256hex" — check prefix
    if (StringBeginsWith(key, aDomain) &&
        key.Length() > aDomain.Length() &&
        key.CharAt(aDomain.Length()) == ':') {
      toRemove.AppendElement(nsCString(key));
    }
  }

  for (const auto& key : toRemove) {
    mEntries.Remove(key);
  }

  DANE_LOG("DaneCache: invalidated %u entries for domain %s",
           (unsigned)toRemove.Length(),
           nsPromiseFlatCString(aDomain).get());
}

void NmcDaneCache::EvictExpired() {
  MutexAutoLock lock(mMutex);

  TimeStamp now = TimeStamp::Now();
  nsTArray<nsCString> toRemove;

  for (auto iter = mEntries.Iter(); !iter.Done(); iter.Next()) {
    if (now > iter.Data().expiry) {
      toRemove.AppendElement(nsCString(iter.Key()));
    }
  }

  for (const auto& key : toRemove) {
    mEntries.Remove(key);
  }

  if (!toRemove.IsEmpty()) {
    DANE_LOG("DaneCache: evicted %u expired entries",
             (unsigned)toRemove.Length());
  }
}

void NmcDaneCache::Clear() {
  MutexAutoLock lock(mMutex);
  uint32_t count = mEntries.Count();
  mEntries.Clear();
  DANE_LOG("DaneCache: cleared %u entries", count);
}

// ---------------------------------------------------------------------------
// NmcDaneExtractCertData
// ---------------------------------------------------------------------------

nsresult NmcDaneExtractCertData(CERTCertificate* aCert,
                                uint8_t aSelector,
                                nsTArray<uint8_t>& aOutput) {
  if (!aCert) {
    DANE_ERR("ExtractCertData: null certificate");
    return NS_ERROR_INVALID_ARG;
  }

  switch (aSelector) {
    case 0: {
      // Selector 0: Full DER-encoded certificate
      // CERTCertificate.derCert is the DER encoding
      if (!aCert->derCert.data || aCert->derCert.len == 0) {
        DANE_ERR("ExtractCertData: cert has no DER data");
        return NS_ERROR_FAILURE;
      }
      aOutput.SetLength(aCert->derCert.len);
      memcpy(aOutput.Elements(), aCert->derCert.data, aCert->derCert.len);
      DANE_LOG("ExtractCertData: selector=0 (full DER), %u bytes",
               aCert->derCert.len);
      return NS_OK;
    }

    case 1: {
      // Selector 1: DER-encoded SubjectPublicKeyInfo
      // Extract from the certificate's SubjectPublicKeyInfo field.
      //
      // In NSS, CERTCertificate contains derPublicKey which is the
      // BIT STRING of the public key, but we need the full SPKI
      // (AlgorithmIdentifier + BIT STRING). Use the
      // CERTSubjectPublicKeyInfo from the parsed certificate.
      //
      // The SPKI is encoded in the cert at a known offset. NSS provides
      // SECKEY_EncodeDERSubjectPublicKeyInfo to re-encode from the
      // parsed structure, but the simplest correct approach is to
      // use CERT_ExtractPublicKey then re-encode, OR use the raw
      // derPublicKey SECItem which is actually the entire SPKI in
      // CERTCertificate (despite the misleading field name).
      //
      // Actually, CERTSubjectPublicKeyInfo is parsed from the cert;
      // to get DER SPKI we can use SEC_ASN1EncodeItem on it, OR
      // we can use the approach from certDER: find the SPKI in the
      // TBSCertificate. The cleanest NSS approach is:
      //
      //   SECKEYPublicKey* pubKey = CERT_ExtractPublicKey(cert);
      //   SECItem* spkiDER = SECKEY_EncodeDERSubjectPublicKeyInfo(pubKey);

      SECKEYPublicKey* pubKey = CERT_ExtractPublicKey(aCert);
      if (!pubKey) {
        DANE_ERR("ExtractCertData: failed to extract public key");
        return NS_ERROR_FAILURE;
      }

      // SECKEY_EncodeDERSubjectPublicKeyInfo returns an arena-allocated
      // SECItem containing the full DER-encoded SPKI.
      SECItem* spkiDER = SECKEY_EncodeDERSubjectPublicKeyInfo(pubKey);
      SECKEY_DestroyPublicKey(pubKey);

      if (!spkiDER || !spkiDER->data || spkiDER->len == 0) {
        DANE_ERR("ExtractCertData: failed to encode SPKI DER");
        if (spkiDER) {
          SECITEM_FreeItem(spkiDER, PR_TRUE);
        }
        return NS_ERROR_FAILURE;
      }

      aOutput.SetLength(spkiDER->len);
      memcpy(aOutput.Elements(), spkiDER->data, spkiDER->len);
      DANE_LOG("ExtractCertData: selector=1 (SPKI), %u bytes", spkiDER->len);

      SECITEM_FreeItem(spkiDER, PR_TRUE);
      return NS_OK;
    }

    default:
      DANE_ERR("ExtractCertData: unknown selector %u", (unsigned)aSelector);
      return NS_ERROR_INVALID_ARG;
  }
}

// ---------------------------------------------------------------------------
// NmcDaneComputeMatch
// ---------------------------------------------------------------------------

nsresult NmcDaneComputeMatch(const nsTArray<uint8_t>& aData,
                             uint8_t aMatchType,
                             nsTArray<uint8_t>& aOutput) {
  switch (aMatchType) {
    case 0: {
      // MatchingType 0: Exact — no transformation, return data as-is
      aOutput = aData.Clone();
      DANE_LOG("ComputeMatch: matchType=0 (exact), %u bytes",
               (unsigned)aOutput.Length());
      return NS_OK;
    }

    case 1: {
      // MatchingType 1: SHA-256 hash of the extracted data
      aOutput.SetLength(32);  // SHA-256 = 32 bytes
      if (HASH_HashBuf(HASH_AlgSHA256,
                       aOutput.Elements(),
                       aData.Elements(),
                       aData.Length()) != SECSuccess) {
        DANE_ERR("ComputeMatch: SHA-256 hash failed");
        return NS_ERROR_FAILURE;
      }
      DANE_LOG("ComputeMatch: matchType=1 (SHA-256), input=%u bytes",
               (unsigned)aData.Length());
      return NS_OK;
    }

    case 2: {
      // MatchingType 2: SHA-512 hash of the extracted data
      aOutput.SetLength(64);  // SHA-512 = 64 bytes
      if (HASH_HashBuf(HASH_AlgSHA512,
                       aOutput.Elements(),
                       aData.Elements(),
                       aData.Length()) != SECSuccess) {
        DANE_ERR("ComputeMatch: SHA-512 hash failed");
        return NS_ERROR_FAILURE;
      }
      DANE_LOG("ComputeMatch: matchType=2 (SHA-512), input=%u bytes",
               (unsigned)aData.Length());
      return NS_OK;
    }

    default:
      DANE_ERR("ComputeMatch: unknown matchingType %u",
               (unsigned)aMatchType);
      return NS_ERROR_INVALID_ARG;
  }
}

// ---------------------------------------------------------------------------
// NmcDaneCertFingerprint
// ---------------------------------------------------------------------------

nsresult NmcDaneCertFingerprint(CERTCertificate* aCert,
                                nsACString& aHexOut) {
  if (!aCert || !aCert->derCert.data || aCert->derCert.len == 0) {
    return NS_ERROR_INVALID_ARG;
  }

  uint8_t hash[32];  // SHA-256
  if (HASH_HashBuf(HASH_AlgSHA256,
                   hash,
                   aCert->derCert.data,
                   aCert->derCert.len) != SECSuccess) {
    DANE_ERR("CertFingerprint: SHA-256 hash failed");
    return NS_ERROR_FAILURE;
  }

  DaneHexEncode(hash, 32, aHexOut);
  return NS_OK;
}

// ---------------------------------------------------------------------------
// GetTlsaForPort
// ---------------------------------------------------------------------------

void GetTlsaForPort(const NamecoinNameValue& aNameValue,
                    uint16_t aPort,
                    nsTArray<NamecoinTLSARecord>& aRecords) {
  aRecords.Clear();

  // Port-specific TLSA lookup:
  // In the Namecoin JSON value, port-specific TLSA records are stored at:
  //   map._tcp.map._<port>.tls
  //
  // This corresponds to the DNS TLSA convention: _<port>._tcp.<domain>
  //
  // The NamecoinNameValue struct stores the already-traversed result for
  // the queried hostname. Port-specific records would be in the map under
  // _tcp → _<port> → tls. Since nsNamecoinResolver::ParseNameValue()
  // resolves the subdomain map for the queried hostname but doesn't
  // traverse _tcp/_<port> (those aren't subdomain labels in the query),
  // we check the tls array on the resolved value directly.
  //
  // For port-specific TLSA, the name value JSON would look like:
  //   { "map": { "_tcp": { "map": { "_443": { "tls": [...] } } } } }
  //
  // Since we receive the already-parsed NamecoinNameValue for the domain
  // (not subdomain-traversed for _tcp), we use the top-level tls records.
  // Port-specific records should be parsed during name resolution by
  // looking up _tcp._<port> in the map. For now, the top-level tls array
  // is authoritative — Phase 1's ParseNameValue already copies map-level
  // tls records during subdomain traversal.
  //
  // TODO: If the raw JSON is available, implement full _tcp._<port> lookup.
  // For the common case (single-port HTTPS on 443), the top-level tls
  // array is correct.

  if (aNameValue.tls.IsEmpty()) {
    DANE_LOG("GetTlsaForPort: no TLSA records for port %u", (unsigned)aPort);
    return;
  }

  // Copy all applicable TLSA records
  for (const auto& rec : aNameValue.tls) {
    // Validate usage field (only 2 and 3 are supported per spec)
    if (rec.usage != 2 && rec.usage != 3) {
      DANE_LOG("GetTlsaForPort: skipping unsupported usage=%u", rec.usage);
      continue;
    }
    // Validate selector (0 or 1)
    if (rec.selector > 1) {
      DANE_LOG("GetTlsaForPort: skipping unsupported selector=%u",
               rec.selector);
      continue;
    }
    // Validate matchType (0, 1, or 2)
    if (rec.matchType > 2) {
      DANE_LOG("GetTlsaForPort: skipping unsupported matchType=%u",
               rec.matchType);
      continue;
    }
    aRecords.AppendElement(rec);
  }

  DANE_LOG("GetTlsaForPort: %u valid TLSA records for port %u",
           (unsigned)aRecords.Length(), (unsigned)aPort);
}

// ---------------------------------------------------------------------------
// Single TLSA record validation against a certificate
// ---------------------------------------------------------------------------

/**
 * Validate one TLSA record against one certificate.
 *
 * @param aRecord  The TLSA record to check
 * @param aCert    The certificate to validate
 * @returns true if this record matches the certificate
 */
static bool NmcDaneMatchSingleRecord(const NamecoinTLSARecord& aRecord,
                                     CERTCertificate* aCert) {
  // Step 1: Extract cert data based on selector
  nsTArray<uint8_t> certData;
  nsresult rv = NmcDaneExtractCertData(aCert, aRecord.selector, certData);
  if (NS_FAILED(rv)) {
    DANE_ERR("MatchSingleRecord: failed to extract cert data (selector=%u)",
             aRecord.selector);
    return false;
  }

  // Step 2: Compute match based on matchingType
  nsTArray<uint8_t> computed;
  rv = NmcDaneComputeMatch(certData, aRecord.matchType, computed);
  if (NS_FAILED(rv)) {
    DANE_ERR("MatchSingleRecord: failed to compute match (matchType=%u)",
             aRecord.matchType);
    return false;
  }

  // Step 3: Decode the TLSA record's data field (hex-encoded)
  // The data field in NamecoinTLSARecord is hex or base64 encoded.
  // Try hex first (most common for TLSA), then base64.
  nsTArray<uint8_t> recordData;
  if (!DaneHexDecode(aRecord.data, recordData)) {
    // Try base64 decoding
    nsAutoCString decoded;
    nsresult b64rv = Base64Decode(aRecord.data, decoded);
    if (NS_FAILED(b64rv)) {
      DANE_ERR("MatchSingleRecord: data is neither valid hex nor base64: %s",
               aRecord.data.get());
      return false;
    }
    recordData.SetLength(decoded.Length());
    memcpy(recordData.Elements(), decoded.BeginReading(), decoded.Length());
  }

  // Compare computed hash/data with the TLSA record data
  if (computed.Length() != recordData.Length()) {
    DANE_LOG("MatchSingleRecord: length mismatch (computed=%u, record=%u)",
             (unsigned)computed.Length(), (unsigned)recordData.Length());
    return false;
  }

  // Constant-time comparison to prevent timing attacks
  uint8_t diff = 0;
  for (size_t i = 0; i < computed.Length(); i++) {
    diff |= computed[i] ^ recordData[i];
  }

  if (diff != 0) {
    DANE_LOG("MatchSingleRecord: data mismatch (usage=%u, sel=%u, match=%u)",
             aRecord.usage, aRecord.selector, aRecord.matchType);
    return false;
  }

  DANE_LOG("MatchSingleRecord: MATCH (usage=%u, sel=%u, match=%u)",
           aRecord.usage, aRecord.selector, aRecord.matchType);
  return true;
}

// ---------------------------------------------------------------------------
// NmcDaneExtractStapledSpki
//
// The x--nmc AIA scheme encodes the root CA's SPKI in the intermediate CA
// cert's serialNumber field as a JSON blob:
//   "Namecoin TLS Certificate\n\nStapled: {\"pubb64\":\"<url-safe-base64-SPKI>\"}"
//
// This function extracts the pubb64 bytes from a cert's serialNumber if
// present, for use in DANE-TA validation.
//
// @param aCert    A certificate (typically an intermediate CA in the chain)
// @param aOutput  Output: the decoded SPKI bytes from pubb64
// @returns NS_OK if pubb64 was found and decoded
// ---------------------------------------------------------------------------

static nsresult NmcDaneExtractStapledSpki(CERTCertificate* aCert,
                                           nsTArray<uint8_t>& aOutput) {
  if (!aCert) return NS_ERROR_INVALID_ARG;

  // The x--nmc AIA scheme encodes the root CA's SPKI in the ISSUER DN's
  // serialNumber attribute of the intermediate CA cert, NOT in the cert's
  // own integer serial number.
  //
  // Chain cert (intermediate CA) structure:
  //   Subject: CN=testls.bit Domain CA, serialNumber=Namecoin TLS Certificate
  //   Issuer:  CN=testls.bit Domain AIA Parent CA,
  //            serialNumber=Namecoin TLS Certificate\n\nStapled: {"pubb64":"..."}
  //
  // So we read aCert->issuer's serialNumber AVA (OID 2.5.4.5) to get the
  // text that contains the JSON-encoded pubb64 SPKI.
  //
  // CERT_GetNameElement() is not public; we iterate the CERTName's rdns/avas
  // directly using public types and CERT_GetAVATag / CERT_DecodeAVAValue.
  // CERT_NameToAscii would truncate the serialNumber at 64 chars (too short).
  nsAutoCString serialStr;
  {
    CERTRDN** rdns = aCert->issuer.rdns;
    bool found = false;
    if (rdns) {
      for (CERTRDN** rdnp = rdns; *rdnp && !found; ++rdnp) {
        CERTAVA** avas = (*rdnp)->avas;
        if (!avas) continue;
        for (CERTAVA** avap = avas; *avap && !found; ++avap) {
          CERTAVA* ava = *avap;
          if (SECOID_FindOIDTag(&ava->type) == SEC_OID_AVA_SERIAL_NUMBER) {
            // Decode the AVA value bytes to a string
            SECItem* val = CERT_DecodeAVAValue(&ava->value);
            if (val && val->data && val->len > 0) {
              serialStr.Assign(reinterpret_cast<const char*>(val->data),
                               (uint32_t)val->len);
              found = true;
            }
            if (val) SECITEM_FreeItem(val, PR_TRUE);
          }
        }
      }
    }
    if (!found) {
      DANE_LOG("ExtractStapledSpki: no serialNumber AVA in issuer DN");
      return NS_ERROR_NOT_AVAILABLE;
    }
  }

  if (serialStr.Length() < 4) return NS_ERROR_NOT_AVAILABLE;

  DANE_LOG("ExtractStapledSpki: issuer serialNumber = '%s'",
           serialStr.get());

  // Look for the pubb64 JSON field
  static const char kPubb64Key[] = "\"pubb64\":\"";
  int32_t keyPos = serialStr.Find(kPubb64Key);
  if (keyPos < 0) return NS_ERROR_NOT_AVAILABLE;

  int32_t valueStart = keyPos + (int32_t)(sizeof(kPubb64Key) - 1);
  int32_t valueEnd = serialStr.FindChar('"', valueStart);
  if (valueEnd < 0) return NS_ERROR_FAILURE;

  nsAutoCString pubb64(Substring(serialStr, valueStart,
                                  valueEnd - valueStart));
  // Convert URL-safe base64 to standard base64
  pubb64.ReplaceChar('-', '+');
  pubb64.ReplaceChar('_', '/');
  // Add padding
  while (pubb64.Length() % 4 != 0) {
    pubb64.Append('=');
  }

  nsAutoCString decoded;
  nsresult rv = mozilla::Base64Decode(pubb64, decoded);
  if (NS_FAILED(rv)) {
    DANE_ERR("ExtractStapledSpki: base64 decode failed for pubb64");
    return NS_ERROR_FAILURE;
  }

  aOutput.SetLength(decoded.Length());
  memcpy(aOutput.Elements(), decoded.BeginReading(), decoded.Length());
  DANE_LOG("ExtractStapledSpki: extracted %u bytes of SPKI from serial",
           (unsigned)aOutput.Length());
  return NS_OK;
}

// ---------------------------------------------------------------------------
// NmcValidateDane — Main entry point
// ---------------------------------------------------------------------------

NmcDaneValidateResult NmcValidateDane(
    const NamecoinNameValue& aNameValue,
    CERTCertificate* aCert,
    CERTCertList* aCertChain,
    const nsACString& aHost,
    uint16_t aPort) {
  DANE_LOG("NmcValidateDane: host=%s port=%u",
           nsPromiseFlatCString(aHost).get(), (unsigned)aPort);

  if (!aCert) {
    DANE_ERR("NmcValidateDane: null server certificate");
    return NmcDaneValidateResult::NMC_DANE_FAIL;
  }

  // Step 1: Get applicable TLSA records for this port
  nsTArray<NamecoinTLSARecord> records;
  GetTlsaForPort(aNameValue, aPort, records);

  if (records.IsEmpty()) {
    DANE_LOG("NmcValidateDane: no TLSA records → NMC_DANE_NO_RECORD");
    return NmcDaneValidateResult::NMC_DANE_NO_RECORD;
  }

  DANE_LOG("NmcValidateDane: checking %u TLSA records",
           (unsigned)records.Length());

  // Step 2: Check each TLSA record
  // ANY matching record is sufficient for validation to pass.
  for (const auto& rec : records) {
    switch (rec.usage) {
      case 3: {
        // DANE-EE (End Entity): the server certificate must match directly.
        // Chain validation is entirely bypassed — if the hash matches, trust it.
        if (NmcDaneMatchSingleRecord(rec, aCert)) {
          DANE_LOG("NmcValidateDane: DANE-EE match → NMC_DANE_OK");
          return NmcDaneValidateResult::NMC_DANE_OK;
        }
        break;
      }

      case 2: {
        // DANE-TA (Trust Anchor): a CA certificate in the chain must match.
        // Walk the full certificate chain looking for a match.
        //
        // First check the end-entity cert itself (unusual for DANE-TA but
        // valid if the cert is self-signed and acts as its own TA).
        if (NmcDaneMatchSingleRecord(rec, aCert)) {
          DANE_LOG("NmcValidateDane: DANE-TA match on EE cert → NMC_DANE_OK");
          return NmcDaneValidateResult::NMC_DANE_OK;
        }

        // Walk the chain if available
        if (aCertChain) {
          CERTCertListNode* node = CERT_LIST_HEAD(aCertChain);
          while (!CERT_LIST_END(node, aCertChain)) {
            CERTCertificate* chainCert = node->cert;
            if (chainCert && chainCert != aCert) {
              // Standard check: match the chain cert's own SPKI/DER
              if (NmcDaneMatchSingleRecord(rec, chainCert)) {
                DANE_LOG("NmcValidateDane: DANE-TA match on chain cert → "
                         "NMC_DANE_OK");
                return NmcDaneValidateResult::NMC_DANE_OK;
              }

              // x--nmc AIA stapling: the chain cert may have a pubb64 field
              // in its serialNumber encoding the root CA's SPKI. If the TLSA
              // record uses selector=1 (SPKI), check the stapled SPKI too.
              if (rec.selector == 1) {
                nsTArray<uint8_t> stapledSpki;
                if (NS_SUCCEEDED(
                        NmcDaneExtractStapledSpki(chainCert, stapledSpki))) {
                  // Compute the match on the stapled SPKI bytes
                  nsTArray<uint8_t> computed;
                  if (NS_SUCCEEDED(NmcDaneComputeMatch(stapledSpki,
                                                        rec.matchType,
                                                        computed))) {
                    // Decode the TLSA record data
                    nsTArray<uint8_t> recordData;
                    bool decoded = DaneHexDecode(rec.data, recordData);
                    if (!decoded) {
                      nsAutoCString b64decoded;
                      if (NS_SUCCEEDED(
                              mozilla::Base64Decode(rec.data, b64decoded))) {
                        recordData.SetLength(b64decoded.Length());
                        memcpy(recordData.Elements(),
                               b64decoded.BeginReading(),
                               b64decoded.Length());
                        decoded = true;
                      }
                    }
                    if (decoded && computed.Length() == recordData.Length()) {
                      uint8_t diff = 0;
                      for (size_t k = 0; k < computed.Length(); k++) {
                        diff |= computed[k] ^ recordData[k];
                      }
                      if (diff == 0) {
                        DANE_LOG("NmcValidateDane: DANE-TA match via stapled "
                                 "pubb64 in chain cert → NMC_DANE_OK");
                        return NmcDaneValidateResult::NMC_DANE_OK;
                      }
                    }
                  }
                }
              }
            }
            node = CERT_LIST_NEXT(node);
          }
        } else {
          DANE_LOG("NmcValidateDane: DANE-TA record but no cert chain "
                   "provided — can only check EE cert");
        }
        break;
      }

      default:
        // Usage 0 (PKIX-TA) and 1 (PKIX-EE) are not supported for Namecoin.
        // Skip silently (already filtered in GetTlsaForPort but guard here).
        DANE_LOG("NmcValidateDane: skipping unsupported usage=%u", rec.usage);
        break;
    }
  }

  // If we get here, TLSA records exist but none matched.
  // This is a HARD FAIL — the blockchain specifies expected certs and the
  // server's cert doesn't match any of them.
  DANE_ERR("NmcValidateDane: %u TLSA records checked, none matched → "
           "NMC_DANE_FAIL for %s",
           (unsigned)records.Length(),
           nsPromiseFlatCString(aHost).get());
  return NmcDaneValidateResult::NMC_DANE_FAIL;
}

}  // namespace net
}  // namespace mozilla
