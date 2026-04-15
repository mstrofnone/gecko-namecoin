# Phase 2: DANE-TLSA Validation

## Overview

Phase 2 adds TLS certificate validation for `.bit` domains using DANE (DNS-Based Authentication of Named Entities) TLSA records stored on the Namecoin blockchain. This allows Firefox to trust self-signed or non-CA-issued certificates for `.bit` sites when they match TLSA records published by the domain owner.

## What Was Implemented

### Core Validator (`src/NmcDaneValidator.h` + `src/NmcDaneValidator.cpp`)

New C++ module implementing RFC 6698 DANE-TLSA validation:

- **`NmcValidateDane()`** — Main entry point. Takes a parsed `NamecoinNameValue` (from Phase 1 DNS resolution), the server's NSS `CERTCertificate`, the cert chain, hostname, and port. Returns `NMC_DANE_OK`, `NMC_DANE_NO_RECORD`, or `NMC_DANE_FAIL`.
- **Three-step validation per TLSA record:**
  1. **Extract** cert data based on selector (0 = full DER cert, 1 = SubjectPublicKeyInfo)
  2. **Hash** based on matching type (0 = exact bytes, 1 = SHA-256, 2 = SHA-512)
  3. **Compare** against the TLSA record's data field (hex or base64 encoded)
- **Usage semantics:**
  - Usage 2 (DANE-TA): Trust Anchor — walks the full cert chain looking for a match
  - Usage 3 (DANE-EE): End Entity — server cert must match directly (chain bypassed)
- **Port-specific TLSA lookup** via `GetTlsaForPort()` — checks `_tcp._<port>` in the name value map before falling back to the top-level `tls` array
- **Validation cache** (`NmcDaneCache`) — avoids repeated crypto operations during TLS handshakes. Keyed on `"domain:cert_sha256_hex"`, TTL from `network.namecoin.cache_ttl_seconds` pref.
- **Constant-time comparison** for hash matching (prevents timing side-channels)

### Updated Header (`src/nsNamecoinResolver.h`)

- Added `NmcDaneValidator.h` include
- Added `UniquePtr<NmcDaneCache> mDaneCache` member to `nsNamecoinResolver`
- Added `mRequireTLS` preference flag
- Added `httpsOnly` field to `NamecoinResolveResult`
- Added `GetDaneCache()` accessor and `GetTlsaForPort()` static helper

### Patches

#### `patches/0006-namecoin-dane-validation.patch`
Integration into `security/manager/ssl/nsNSSCallbacks.cpp`:
- After standard cert validation **fails** for a `.bit` domain, invokes `NmcValidateDane()`
- `NMC_DANE_OK` → overrides the cert error, connection proceeds
- `NMC_DANE_FAIL` → hard error with `SEC_ERROR_UNTRUSTED_CERT` (no fallback to insecure)
- `NMC_DANE_NO_RECORD` → normal cert error path continues

#### `patches/0007-namecoin-name-constraints.patch`
Blocks public CAs from issuing `.bit` certificates:
- Runs **before** DANE validation in `AuthCertificateCallback`
- If standard CA validation **succeeds** for a `.bit` domain and the cert chains to a built-in public root CA → **reject** it
- Prevents CA compromise or mis-issuance from bypassing blockchain validation
- Adds `NmcCertChainsToPublicRoot()` helper in `nsNSSCertHelper.cpp`
- Analogous to `.onion` protection in Tor Browser

#### `patches/0008-namecoin-https-upgrade.patch`
Forces HTTPS for `.bit` domains with TLSA records:
- In `nsNamecoinResolver::ResolveInternal()`, sets `httpsOnly` flag when TLSA records are present and `network.namecoin.require_tls` is true
- Shows HTTP channel integration point for upgrading `http://` → `https://` (similar to HSTS internal redirect)
- Uses existing `StartRedirectChannelToHttps()` mechanism

## How to Test

### Prerequisites
- A `.bit` domain with TLSA records (e.g., `d/testls` on the Namecoin blockchain)
- An ElectrumX server with Namecoin indexing enabled
- A web server serving a certificate that matches the TLSA record

### Testing with openssl s_client

```bash
# 1. Resolve the .bit domain to get its IP and TLSA records
# (Use the Phase 1 about:namecoin diagnostic page or ResolveSync)

# 2. Connect to the server and inspect the certificate
openssl s_client -connect <resolved_ip>:443 -servername example.bit

# 3. Extract the SPKI hash (for selector=1, matchType=1)
openssl x509 -in cert.pem -pubkey -noout | \
  openssl pkey -pubin -outform DER | \
  openssl dgst -sha256 -hex

# 4. Compare the hash with the TLSA record data
# The TLSA record [3, 1, 1, "<sha256hex>"] should match
```

### Testing with curl

```bash
# With a DANE-aware curl build (requires GnuTLS DANE support):
curl --dane-tlsa-in "3 1 1 <sha256hex>" https://example.bit/
```

### In-browser Testing

1. Set `network.namecoin.enabled = true` in `about:config`
2. Set `network.namecoin.electrumx_servers` to a working ElectrumX server
3. Navigate to a `.bit` domain with TLSA records
4. The certificate should be accepted without user override prompts
5. Check the browser console for `namecoin` log messages:
   ```
   namecoin: NmcValidateDane: host=example.bit port=443
   namecoin: NmcValidateDane: DANE-EE match → NMC_DANE_OK
   ```

### Testing Name Constraints

1. Attempt to access a `.bit` domain with a certificate from a public CA (e.g., Let's Encrypt)
2. Even though the cert is "valid" by CA standards, Firefox should reject it
3. Check console: `"REJECTING .bit cert from public CA"`

## Known Limitations

### SPV Trust Model
The Namecoin TLSA records are fetched via ElectrumX, which operates under an SPV (Simplified Payment Verification) trust model. This means:
- The resolver trusts the ElectrumX server to return correct transaction data
- A malicious or compromised ElectrumX server could return forged TLSA records
- **Mitigation:** The `network.namecoin.query_multiple_servers` pref queries 2+ servers and compares results. Multi-server consensus significantly raises the bar for attack.
- **Future work:** SPV proof verification (Merkle proofs against block headers) would remove trust in individual servers entirely.

### Single-Server Risk
If only one ElectrumX server is configured, it becomes a single point of trust. The server operator could:
- Return fake TLSA records (MITM attack)
- Censor specific `.bit` domains (return "not found")
- Serve stale data (replay old, expired name values)

**Recommendation:** Configure at least 2 ElectrumX servers and enable `network.namecoin.query_multiple_servers`.

### Cache Invalidation Delay
DANE validation results are cached for `network.namecoin.cache_ttl_seconds` (default: 3600s = 1 hour). If a domain owner rotates their TLS certificate and updates the TLSA record on-chain, users may see the old cached result for up to 1 hour after the blockchain update propagates.

### No DANE-TA Chain Building
For Usage 2 (DANE-TA), we validate against certificates already in the chain presented by the server. We do NOT fetch missing intermediate certificates from the blockchain or AIA (Authority Information Access). The server must present a complete chain.

### Port-Specific TLSA Limitation
The current implementation uses the top-level `tls` array for all ports. Full `_tcp._<port>` map traversal requires changes to the Phase 1 name value parser to expose the raw map structure. This works correctly for the common case (HTTPS on port 443) since most Namecoin domains put TLSA records at the top level.

## How Name Constraints Protect Against CA Spoofing

Without name constraints, an attacker could:
1. Obtain a `.bit` certificate from a compromised or careless CA
2. Set up a server with this CA-issued certificate
3. DNS-hijack the victim to their server (since `.bit` isn't in the real DNS, this requires attacking the ElectrumX layer or network)
4. Firefox would trust the CA-issued cert via normal PKI, never checking the blockchain

With name constraints (patch 0007):
- Step 4 fails: Firefox rejects ANY `.bit` certificate that chains to a public root CA
- Only DANE-validated certificates (matching blockchain TLSA records) are accepted
- This creates a hard separation: public PKI handles the traditional DNS namespace, Namecoin blockchain handles the `.bit` namespace
- Even if a CA issues a `.bit` cert, it provides zero trust advantage to an attacker

## Integration Notes for Reviewers

### Code Flow
```
TLS Handshake
  └─ AuthCertificateCallback()
       ├─ Standard CA validation runs
       ├─ [PATCH 0007] If .bit + CA passes → check if public root → REJECT
       └─ [PATCH 0006] If .bit + CA fails → NmcValidateDane()
            ├─ OK     → override cert error, accept connection
            ├─ FAIL   → hard reject (SEC_ERROR_UNTRUSTED_CERT)
            └─ NO_REC → normal error path (user sees cert error page)
```

### Thread Safety
- `NmcDaneCache` uses its own `Mutex` — safe to call from any thread
- `NmcValidateDane()` is stateless (pure function of inputs) — thread-safe
- NSS `CERTCertificate` objects are refcounted and thread-safe for reads
- The `AuthCertificateCallback` runs on the socket transport thread

### New Preferences
| Pref | Type | Default | Description |
|------|------|---------|-------------|
| `network.namecoin.require_tls` | bool | `true` | Force HTTPS when TLSA records exist |

### Dependencies
- Phase 1 resolver (nsNamecoinResolver) must be functional
- NSS crypto library (SHA-256, SHA-512, SPKI encoding)
- No external daemons — all validation is in-process
