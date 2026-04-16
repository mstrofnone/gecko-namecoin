# gecko-namecoin

Native Namecoin (`.bit`) domain resolution and DANE-TLSA certificate validation for Firefox, built directly into Gecko.

This is a patch series against [mozilla-central](https://github.com/mozilla/gecko-dev) (Firefox source) that adds:

- **DNS resolution** of `.bit` domains via the Namecoin blockchain (ElectrumX JSON-RPC over WebSocket)
- **DANE-TLSA certificate validation** so Firefox can establish HTTPS connections to `.bit` sites without traditional Certificate Authorities
- **Two TLS authentication methods** compatible with [ncgencert](https://github.com/namecoin/ncgencert):
  - **Method 1 (DANE-TA hash):** TLSA record published in the blockchain — standard DANE
  - **Method 2 (Address signature):** Domain owner signs a message with their Namecoin wallet — no TLSA in blockchain needed
- **From-scratch secp256k1 ECDSA** because Firefox/NSS does not support this curve (see [Why secp256k1 from scratch?](#why-secp256k1-from-scratch))

## Table of Contents

- [How .bit DNS Resolution Works](#how-bit-dns-resolution-works)
- [TLS Certificate Validation](#tls-certificate-validation)
  - [Background: Why .bit Sites Need Special TLS](#background-why-bit-sites-need-special-tls)
  - [How ncgencert Works](#how-ncgencert-works)
  - [Method 1: DANE-TA Hash in Blockchain](#method-1-dane-ta-hash-in-blockchain)
  - [Method 2: Namecoin Address Signature (ncgencert Option 2)](#method-2-namecoin-address-signature-ncgencert-option-2)
  - [Decision Flow: Which Method Gets Used?](#decision-flow-which-method-gets-used)
- [Why secp256k1 from scratch?](#why-secp256k1-from-scratch)
- [Architecture Diagram](#architecture-diagram)
- [Files and Their Roles](#files-and-their-roles)
- [Commit History](#commit-history)
- [Building](#building)
- [Configuration](#configuration)
- [Security Model](#security-model)
- [Known Limitations](#known-limitations)
- [References](#references)

## How .bit DNS Resolution Works

When a user navigates to `https://example.bit`, Firefox's DNS resolver detects the `.bit` TLD and hands off to `nsNamecoinResolver`:

1. **Compute scripthash**: The domain name `example` is prefixed with `d/` (Namecoin's domain namespace), then encoded as a `NAME_UPDATE` script (`OP_NAME_UPDATE` opcode `0x53`), SHA-256 hashed, and byte-reversed to produce the ElectrumX scripthash.

2. **Query ElectrumX**: Using raw NSPR sockets (not Gecko's WebSocket — see [Implementation Notes](#implementation-notes)), the resolver connects to an ElectrumX server over WebSocket and sends:
   - `blockchain.scripthash.get_history` → returns transaction hashes for the name
   - `blockchain.transaction.get` → returns the raw transaction hex

3. **Decode NAME_UPDATE**: The transaction's scriptSig is parsed to extract the name (`d/example`) and its JSON value. The `DecodeNameScriptWithAddress` function also extracts the **owner Namecoin address** from the transaction's scriptPubKey (P2PKH output) — this is critical for Method 2 TLS.

4. **Parse name value**: The JSON value is parsed for `ip`, `ip6`, `tls`, `map`, and `alias` fields. TLSA records and the owner address are stored in a static cache for the TLS validation stage.

5. **Return IP**: The resolved IP address is fed back to `nsHostResolver` and the TCP connection proceeds normally.

## TLS Certificate Validation

### Background: Why .bit Sites Need Special TLS

Traditional HTTPS relies on Certificate Authorities (CAs) — organizations like Let's Encrypt or DigiCert that vouch for a domain's identity. But CAs only issue certificates for ICANN domains (`.com`, `.org`, etc.). Nobody will issue a CA-signed certificate for `example.bit`.

This means when Firefox connects to a `.bit` site over HTTPS, the standard certificate verification (`AuthCertificate`) always fails — the cert is signed by an unknown CA. Without intervention, the user sees an error page.

**DANE (DNS-Based Authentication of Named Entities)** solves this by letting the domain owner publish their certificate's fingerprint directly in DNS. For Namecoin, "DNS" is the blockchain itself. The [ncgencert](https://github.com/namecoin/ncgencert) tool generates the certificates and the blockchain records.

### How ncgencert Works

[ncgencert](https://github.com/namecoin/ncgencert) generates a certificate chain with a unique structure:

```
AIA Parent CA (self-signed, NOT sent to browser)
    │
    ├── Its SPKI (public key hash) is stapled into the Domain CA's
    │   issuer serialNumber as a "pubb64" field
    │
    ▼
Domain CA (sent in chain.pem, signs the EE cert)
    │  issuer serialNumber contains JSON:
    │  {"pubb64":"<AIA Parent CA SPKI base64>", "sigs":{...}}
    │
    ▼
End-Entity cert (sent in chain.pem, presented to browser)
```

Key detail: the **AIA Parent CA** is never sent to the browser. Only its SPKI (SubjectPublicKeyInfo) is embedded ("stapled") in the Domain CA cert's `issuer` distinguished name, specifically in the `serialNumber` AVA (Attribute Value Assertion). This is read from `aCert->issuer`, NOT from the cert's own integer serial number.

ncgencert produces:
- `chain.pem` — EE cert + Domain CA cert (what the HTTPS server serves)
- `key.pem` — EE private key
- `namecoin.json` — TLSA record: `[2, 1, 1, "<SHA-256 of AIA Parent CA SPKI>"]`
- `caAIAMessage.txt` — Message to sign for Method 2 (address signature)

### Method 1: DANE-TA Hash in Blockchain

**Commit:** [`d283685`](../../commit/d283685) (Phase 2 DANE + AIA stapling)

This is the standard DANE approach defined in [RFC 6698](https://tools.ietf.org/html/rfc6698). The domain owner publishes a TLSA record in the Namecoin blockchain:

```json
{
  "ip": "1.2.3.4",
  "map": {
    "*": {
      "tls": [[2, 1, 1, "ajSshY0WuR1zn0ieD3tOchvJ1VQczNfi5mgyaLNurMc="]]
    }
  }
}
```

The TLSA record `[2, 1, 1, "<hash>"]` means:
- `2` = DANE-TA (Trust Anchor) — a CA cert in the chain must match
- `1` = Selector: SubjectPublicKeyInfo (SPKI)
- `1` = Matching type: SHA-256

**Step by step:**

1. Domain owner runs `ncgencert --host example.bit`
2. Puts the TLSA record from `namecoin.json` into the Namecoin blockchain value
3. Configures HTTPS server with `chain.pem` + `key.pem`
4. User navigates to `https://example.bit` in Firefox
5. Firefox resolves DNS via blockchain → caches TLSA records
6. Firefox connects over TLS → server presents chain (EE + Domain CA)
7. `AuthCertificate` fails (no trusted CA) → DANE hook fires
8. `NmcValidateDane()` retrieves cached TLSA records
9. For each chain cert, tries direct SPKI match (DANE-TA usage 2)
10. If no direct match, calls `NmcDaneExtractStapledSpki()` to extract the `pubb64` from the Domain CA's **issuer** serialNumber
11. SHA-256 of the extracted SPKI is compared to the TLSA hash
12. **Match → connection allowed**. Mismatch → hard fail (no click-through).

**Code path:**
```
SSLServerCertVerificationJob::Run()
  → AuthCertificate() fails
  → IsNamecoinHost() + namecoin enabled?
  → GetStoredNameValue() → has TLSA records
  → NmcValidateDane()
    → GetTlsaForPort() → records found
    → DANE-TA: walk chain certs
      → NmcDaneMatchSingleRecord() (direct SPKI match)
      → NmcDaneExtractStapledSpki() (pubb64 from issuer DN)
      → SHA-256 compare → match!
  → Dispatch(success)
```

### Method 2: Namecoin Address Signature (ncgencert Option 2)

**Commits:**
- [`f5b6d6d`](../../commit/f5b6d6d) — secp256k1 ECDSA, RIPEMD-160, Base58Check, owner address extraction
- [`d31f7db`](../../commit/d31f7db) — Integration of sig path into NmcDaneValidator

This method **conserves blockchain space** — no TLSA record is needed. Instead, the domain owner proves they authorized the TLS certificate by signing a message with the same Namecoin private key that controls the domain name.

**Setup:**

1. Run `ncgencert --host example.bit` → get `caAIAMessage.txt`:
   ```
   Namecoin X.509 Stapled Certification: {"address":"FILL IN","domain":"example.bit","x509pub":"MFkw..."}
   ```
2. Fill in your Namecoin address and sign with:
   ```
   namecoin-cli signmessage <address> "<message>"
   ```
3. Re-run ncgencert with `-grandparent-key caAIAKey.pem -sigs sigs.json`
4. The signature gets embedded in the Domain CA cert's issuer serialNumber `sigs` field

**Step by step in Firefox:**

1. User navigates to `https://example.bit`
2. Firefox resolves DNS via blockchain:
   - `DecodeNameScriptWithAddress()` extracts the **owner Namecoin address** from the `NAME_UPDATE` transaction's P2PKH scriptPubKey
   - No TLSA records in the name value (or the `tls` field is empty)
3. Firefox connects over TLS → `AuthCertificate` fails → DANE hook fires
4. `NmcValidateDane()` finds no TLSA records, but has an owner address
5. Walks the cert chain looking for `sigs` in the issuer serialNumber:
   - Parses the issuer DN's serialNumber AVA for JSON
   - Extracts `pubb64` (AIA Parent CA SPKI) and `sigs` (map of address → base64 signature)
6. Finds the signature for the owner address
7. Reconstructs the signed message:
   ```
   Namecoin X.509 Stapled Certification: {"address":"<owner>","domain":"example.bit","x509pub":"<pubb64>"}
   ```
8. Calls `NmcVerifyMessageSignature(ownerAddress, message, sigBase64)`:
   - Decodes the 65-byte Bitcoin `signmessage` format (1 recovery byte + 32-byte R + 32-byte S)
   - Hashes the message with Bitcoin's message prefix (`"\x18Bitcoin Signed Message:\n"` + varint + double SHA-256)
   - Performs **secp256k1 ECDSA public key recovery** from (r, s, hash, recovery_flag)
   - Derives the Namecoin address from the recovered public key: SHA-256 → RIPEMD-160 → version byte `0x34` → Base58Check
   - Compares the derived address to the owner address from the blockchain
9. **Match → connection allowed**. Invalid signature → hard fail.

**Code path:**
```
SSLServerCertVerificationJob::Run()
  → AuthCertificate() fails
  → IsNamecoinHost() + namecoin enabled?
  → GetStoredNameValue() → no TLSA records, but has ownerAddress
  → NmcValidateDane(nameValue, cert, chain, host, port, ownerAddress)
    → records empty + ownerAddress not empty
    → Walk chain certs → parse issuer serialNumber → find pubb64 + sigs
    → Reconstruct message, extract sig for owner address
    → NmcVerifyMessageSignature()  [NmcSigVerify.cpp]
      → base64 decode → extract (recid, r, s)
      → bitcoin_message_hash() → double SHA-256
      → ecdsa_recover() → secp256k1 point recovery
      → pubkey_to_namecoin_address() → SHA-256 → RIPEMD-160 → Base58Check
      → Compare to owner address → match!
  → Dispatch(success)
```

### Decision Flow: Which Method Gets Used?

```
NmcValidateDane() called
    │
    ├─ TLSA records exist for this port?
    │   ├─ YES → Method 1 (DANE-TA hash match)
    │   │   ├─ Match → SUCCESS
    │   │   └─ No match → HARD FAIL (blockchain says specific cert, this isn't it)
    │   │
    │   └─ NO → Owner address available?
    │       ├─ YES → Method 2 (address signature)
    │       │   ├─ Valid sig → SUCCESS
    │       │   └─ Invalid/missing sig → HARD FAIL
    │       │
    │       └─ NO → NMC_DANE_NO_RECORD (fall through to standard error page)
    │
    └─ (no name value cached) → Standard cert error
```

**Important:** If TLSA records exist, Method 1 is always used (even if sigs are also present). Method 2 is only tried when TLSA records are absent.

## Why secp256k1 from scratch?

Firefox uses [NSS (Network Security Services)](https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS) for all cryptography. NSS has the OID for secp256k1 (`SEC_OID_SECG_EC_SECP256K1`) registered in its OID tables, but the low-level crypto library (**freebl**) marks it as **unsupported** — the actual curve arithmetic is not implemented.

This is because secp256k1 is a [Koblitz curve](https://en.wikipedia.org/wiki/Koblitz_curve) used almost exclusively by Bitcoin, Namecoin, and other cryptocurrencies. Standard TLS and PKI use NIST curves (P-256, P-384, P-521), so there has been no reason for NSS to implement secp256k1.

Attempting to use NSS functions like `PK11_Verify` or `VFY_VerifyDigestDirect` with secp256k1 keys will fail with `SEC_ERROR_UNSUPPORTED_KEYALG`.

Our implementation in [`NmcSigVerify.cpp`](netwerk/dns/NmcSigVerify.cpp) (~780 lines) provides:

| Component | Description |
|-----------|-------------|
| **U256** | 256-bit unsigned integer (4×uint64_t, little-endian limbs) |
| **Field arithmetic** | `u256_add`, `u256_sub`, `u256_mul_mod` (double-and-add), `u256_modinv` (Fermat's little theorem) |
| **EC points** | Jacobian projective coordinates (`JPoint`): `jpoint_double`, `jpoint_add`, `jpoint_to_affine`, `point_mul` |
| **lift_x** | Compute Y from X on secp256k1 (easy: `p ≡ 3 mod 4` → `y = x^((p+1)/4) mod p`) |
| **ecdsa_recover** | Public key recovery from `(r, s, hash, recovery_id)` — Bitcoin's `signmessage` encodes a recovery flag so the pubkey can be recovered without knowing it |
| **RIPEMD-160** | Standalone implementation — NSS also lacks `HASH_AlgRIPEMD160` |
| **Base58Check** | Namecoin address encoding (version byte `0x34` for P2PKH, `0x0d` for P2SH) |
| **Bitcoin message hash** | `"\x18Bitcoin Signed Message:\n"` prefix + varint length + double SHA-256 |

**Timing note:** The implementation uses double-and-add for scalar multiplication, which is not constant-time. This is acceptable for signature **verification** (all inputs are public), but should NOT be used for key generation or signing.

Also implemented standalone: **RIPEMD-160**. NSS's `hasht.h` only exposes SHA family + MD2/MD5. There is no `HASH_AlgRIPEMD160` enum value, so `HASH_HashBuf` cannot compute RIPEMD-160. We provide a full standalone implementation.

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                     Firefox Browser                          │
│                                                              │
│  ┌──────────────────┐     ┌────────────────────────────┐    │
│  │ nsHostResolver    │────▶│ nsNamecoinResolver         │    │
│  │ (DNS entry point) │     │ • ElectrumX WebSocket pool │    │
│  └──────────────────┘     │ • NAME_UPDATE decoder      │    │
│          │                │ • scripthash computation    │    │
│          │                │ • owner address extraction  │    │
│          ▼                │ • static name-value cache   │    │
│  TCP connection to IP     └────────────────────────────┘    │
│          │                                                   │
│          ▼                                                   │
│  ┌──────────────────────────────────────────────────────┐   │
│  │ SSLServerCertVerification                             │   │
│  │ • AuthCertificate() → FAILS for .bit (no CA trust)   │   │
│  │ • DANE hook: if .bit + namecoin enabled               │   │
│  └───────────────┬───────────────────────────────────────┘   │
│                  │                                            │
│                  ▼                                            │
│  ┌──────────────────────────────────────────────────────┐   │
│  │ NmcDaneValidator                                      │   │
│  │ • GetTlsaForPort() — port-specific record lookup     │   │
│  │ • Method 1: DANE-TA hash match (TLSA records exist)  │   │
│  │   └─ NmcDaneExtractStapledSpki() → SHA-256 compare   │   │
│  │ • Method 2: ncgencert sig (no TLSA, owner address)   │   │
│  │   └─ Extract sigs from issuer DN → verify signature  │   │
│  └───────────────┬───────────────────────────────────────┘   │
│                  │ (Method 2 only)                            │
│                  ▼                                            │
│  ┌──────────────────────────────────────────────────────┐   │
│  │ NmcSigVerify                                          │   │
│  │ • secp256k1 EC arithmetic (from scratch — not in NSS) │   │
│  │ • ECDSA public key recovery                           │   │
│  │ • RIPEMD-160 (standalone — not in NSS)                │   │
│  │ • Base58Check Namecoin address derivation              │   │
│  │ • Bitcoin message hash format                          │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## Files and Their Roles

### New files (added by this project)

| File | Lines | Role |
|------|-------|------|
| `netwerk/dns/nsNamecoinResolver.cpp` | ~2080 | Main resolver: ElectrumX WebSocket pool (NSPR raw sockets), JSON parser, NAME_UPDATE decoder, scripthash computation, `DecodeNameScriptWithAddress`, static name-value cache |
| `netwerk/dns/nsNamecoinResolver.h` | | Data types (`NamecoinTLSARecord`, `NamecoinNameValue`, `NamecoinResolveResult`), class declaration, cache types |
| `netwerk/dns/nsNamecoinErrors.h` | | `NMC_LOG`/`NMC_ERR` macros, `HexEncode`/`HexDecode`, opcodes |
| `netwerk/dns/NmcDaneValidator.cpp` | ~880 | DANE-TLSA validation: `NmcValidateDane()`, DANE-TA chain walk, stapled SPKI extraction, ncgencert sig path integration, validation result cache |
| `netwerk/dns/NmcDaneValidator.h` | | `NmcDaneValidateResult` enum, `NmcDaneCache`, function declarations |
| `netwerk/dns/NmcSigVerify.cpp` | ~780 | secp256k1 ECDSA from scratch: U256 bigint, EC point operations, `ecdsa_recover`, RIPEMD-160, Base58Check, Bitcoin message hashing |
| `netwerk/dns/NmcSigVerify.h` | | `NmcVerifyMessageSignature()`, `NmcRecoverSigningAddress()` |

### Modified files (from mozilla-central)

| File | What changed |
|------|-------------|
| `netwerk/dns/nsHostResolver.cpp` | `.bit` TLD check → `nsNamecoinResolver::ResolveSync()`, `StoreNameValue()` after resolution, `mEffectiveTRRMode` fix |
| `netwerk/dns/moz.build` | Build system: add new `.cpp` files |
| `security/manager/ssl/SSLServerCertVerification.cpp` | DANE hook after `AuthCertificate` failure for `.bit` domains, built chain population for DANE success |
| `security/manager/ssl/moz.build` | Build system integration |
| `modules/libpref/init/StaticPrefList.yaml` | `network.namecoin.*` preferences |

## Commit History

Commits are ordered so that each TLS method can be reviewed independently:

### Phase 1: DNS Resolution
| Commit | Description |
|--------|-------------|
| [`0b2347c`](../../commit/0b2347c) | Initial: native .bit DNS resolution and TLS/DANE validation |
| [`1dff953`](../../commit/1dff953) | Fix: deadlock in NameLookup + debug output |
| [`c2b84d2`](../../commit/c2b84d2) | NSPR WebSocket rewrite + 7 crash fixes |
| [`fbeb02f`](../../commit/fbeb02f) | Populate DNS host record from Namecoin resolution |

### Method 1: DANE-TA Hash Verification
| Commit | Description |
|--------|-------------|
| [`3ecfef3`](../../commit/3ecfef3) | Fix `NmcDaneExtractStapledSpki` to read issuer DN serialNumber AVA |
| [`d283685`](../../commit/d283685) | **Phase 2 DANE**: SSLServerCertVerification hook, static name-value cache, wildcard TLSA, AIA stapling |

### Method 2: secp256k1 Address Signature Verification
| Commit | Description |
|--------|-------------|
| [`f5b6d6d`](../../commit/f5b6d6d) | **secp256k1 ECDSA** from scratch + RIPEMD-160 + Base58Check + owner address extraction from NAME_UPDATE |
| [`d31f7db`](../../commit/d31f7db) | **ncgencert Option 2 integration**: wire sig path into NmcDaneValidator (no-TLSA fallback) |

### Bug Fixes & Misc
| Commit | Description |
|--------|-------------|
| [`b1a1314`](../../commit/b1a1314) | Fix: populate `builtChainBytesArray` for DANE success dispatch (crash #9) |
| [`8d82081`](../../commit/8d82081) | Three-stage enablement gating (Nightly → Beta → Release) |
| [`9a13dfc`](../../commit/9a13dfc) | `about:namecoin`: wire JS to real XPCOM resolver via JSActor IPC |

## Building

```bash
# Clone mozilla-central
git clone https://github.com/mozilla/gecko-dev.git mozilla-central
cd mozilla-central

# Add this repo as a remote
git remote add gecko-namecoin https://github.com/mstrofnone/gecko-namecoin.git
git fetch gecko-namecoin

# Cherry-pick or merge the namecoin commits
git cherry-pick 0b2347c^..HEAD  # all commits from gecko-namecoin/main

# Build Firefox (incremental ~20s, full ~15min)
./mach build

# Run with Namecoin enabled
./mach run --setpref "network.namecoin.enabled=true"

# Run with verbose Namecoin logging
MOZ_LOG="nsNamecoinResolver:5,NmcDaneValidator:5,NmcSigVerify:5" \
  ./mach run --setpref "network.namecoin.enabled=true"
```

## Configuration

All preferences are in `about:config`:

| Preference | Default | Description |
|-----------|---------|-------------|
| `network.namecoin.enabled` | `false` | Enable `.bit` domain resolution |
| `network.namecoin.electrumx_servers` | (empty) | Comma-separated ElectrumX WebSocket URLs |
| `network.namecoin.cache_ttl_seconds` | `3600` | DNS cache TTL for resolved names |
| `network.namecoin.max_alias_hops` | `5` | Maximum alias chain depth |
| `network.namecoin.connection_timeout_ms` | `10000` | ElectrumX connection timeout |
| `network.namecoin.require_tls` | `true` | Require HTTPS when TLSA records exist |

## Security Model

| Aspect | Trust basis |
|--------|------------|
| **DNS resolution** | Namecoin blockchain via ElectrumX. The blockchain is authoritative. Name expiry enforced (36000 blocks ≈ 250 days). |
| **Method 1 (DANE-TA)** | TLSA hash in blockchain is the trust anchor. If records exist but the server cert doesn't match → **hard fail** (no click-through override). |
| **Method 2 (Address sig)** | The Namecoin address controlling the domain must have signed the CA key authorization. Signature verified via secp256k1 ECDSA recovery. Invalid sig → **hard fail**. |
| **No TLSA + no sig** | Falls through to standard error handling (user sees cert error page). |

**Hard fail** means the user cannot click "Accept the Risk and Continue" — the blockchain explicitly specifies which certificate is trusted, and the presented one doesn't match.

## Implementation Notes

These are hard-won lessons from 9 crashes across 15 development sessions:

1. **NSPR sockets, NOT `nsIWebSocketChannel`**: Gecko's WebSocket requires main-thread creation + admission DNS lookup → deadlock when called from DNS resolver threads. NSPR `PR_OpenTCPSocket` + `PR_Connect` works on any thread.

2. **`StaticPrefs`, NOT `Preferences::GetBool`**: `GetBool` calls `InitStaticMembers` which calls `IsInServoTraversal` → null deref crash on socket thread. `StaticPrefs` uses atomic reads, safe from any thread.

3. **`mEffectiveTRRMode`**: Must be set to `TRR_DISABLED_MODE` BEFORE `CompleteLookupLocked`. Default `TRR_DEFAULT_MODE` hits `MOZ_ASSERT_UNREACHABLE` in `ResolveComplete()`.

4. **`builtChainBytesArray`**: Must be non-empty when dispatching success. When `AuthCertificate` fails (our case), clone `mPeerCertChain` into it before `Dispatch()`.

5. **Issuer DN, not subject**: `NmcDaneExtractStapledSpki` reads from `aCert->issuer` (the cert that signed this cert), NOT from `aCert->subject`.

6. **ncgencert `namecoin.json`**: Contains SHA-256 of AIA Parent CA SPKI — the cert that is NOT in `chain.pem`. Only its SPKI is stapled.

7. **Full `mach build` required**: `mach build netwerk/dns/` only recompiles `.o` files but does NOT relink XUL. Always use `mach build` for binary changes.

8. **`OP_NAME_UPDATE` (0x53)**, NOT `OP_NAME_SHOW`: Wrong opcode = empty history from ElectrumX (silent failure).

9. **Namecoin address version byte**: `0x34` (P2PKH), `0x0d` (P2SH). Bitcoin uses `0x00`/`0x05`.

## Known Limitations

- **Dehydrated certificates**: Namecoin supports compact cert representations stored in the blockchain that get "rehydrated" into full X.509 certs. Not yet implemented.
- **AIA fetching**: When a server only sends the EE cert, Firefox may try to fetch the intermediate via `http://aia.x--nmc.bit/aia?...` — which is itself a `.bit` domain. Not yet handled.
- **Constant-time crypto**: secp256k1 scalar multiplication uses double-and-add (timing leaks). Acceptable for verification only.
- **UI indicators**: The lock icon doesn't yet indicate "Namecoin DANE validated" — it shows as a normal connection or cert error.
- **ElectrumX consensus**: Currently uses single-server queries with failover. Multi-server consensus verification not yet implemented.

## References

- [ncgencert](https://github.com/namecoin/ncgencert) — Generate Namecoin TLS certificates
- [RFC 6698](https://tools.ietf.org/html/rfc6698) — DANE-TLSA protocol
- [Namecoin d/ namespace spec](https://wiki.namecoin.org/Domain_Name_Specification)
- [Namecoin dehydrated certificates](https://github.com/namecoin/proposals/blob/master/ifa-0003.md)
- [secp256k1 curve parameters](https://www.secg.org/sec2-v2.pdf) — SEC 2, section 2.4.1
- [Bitcoin signmessage format](https://bitcoin.stackexchange.com/questions/3337/what-are-the-parts-of-a-bitcoin-transaction-input-script)
