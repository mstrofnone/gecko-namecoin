# Bugzilla Filing Guide — Namecoin .bit Integration

This document provides ready-to-file Bugzilla content for the Namecoin native
integration work. File in order: meta-bug first, then sub-bugs as each phase
is ready for review.

---

## Meta-Bug (file first)

**Component:** Core :: Networking: DNS
**Type:** Task (enhancement)
**Priority:** P3 (experimental / opt-in)
**Keywords:** dev-doc-needed, sec-audit-needed

**Title:**
```
[meta] Support for Namecoin (.bit) decentralized domain resolution
```

**Description:**
```
This is the tracking meta-bug for native Namecoin (.bit) DNS resolution
in Gecko/Firefox.

Namecoin is a blockchain-based naming system that predates ICANN's jurisdiction.
.bit domains are registered on-chain and resolve to IP addresses, NS records, or
TLS fingerprints stored in the Namecoin blockchain. No certificate authority
is involved.

This integration adds native .bit resolution to Firefox, enabled behind an
about:config preference (network.namecoin.enabled = false by default).

Resolution uses the ElectrumX JSON-RPC-over-WebSocket protocol to query the
Namecoin blockchain without running a full node. The implementation mirrors the
existing tls-namecoin browser extension (https://github.com/hzrd149/nostrudel/pull/352)
but as a native Gecko component in netwerk/.

Architecture (four phases):

  Phase 1 [this bug, see bug XXXXX]: DNS resolution (A/AAAA records)
    - nsNamecoinResolver in netwerk/dns/
    - Hook in nsHostResolver for TLD == "bit"
    - Controlled by network.namecoin.enabled (about:config only, Nightly)

  Phase 2 [bug XXXXX]: TLS/DANE-TLSA certificate validation
    - TLSA record parsing from Namecoin values
    - Integration in security/manager/ssl/nsNSSCallbacks.cpp
    - AuthCertificateCallback: DANE-EE as fallback for .bit
    - Block public CAs from issuing for .bit (name constraints)

  Phase 3 [bug XXXXX]: UI indicators
    - Address bar lock icon: green = DANE-validated, yellow = HTTP-only
    - Certificate viewer: "Namecoin Blockchain" trust source
    - about:namecoin diagnostic page
    - Settings UI in Privacy & Security

  Phase 4 [bug XXXXX]: Android/GeckoView
    - Battery-aware WS connection management
    - Fenix Settings Kotlin activity
    - All C++ changes auto-apply to GeckoView

Prior art / references:
  - ncp11 PKCS#11 approach: Tor Browser issue #33568 (JeremyRand)
  - eTLD maintenance: Tor Browser issue #33807
  - ElectrumX protocol docs: https://electrumx.readthedocs.io/en/latest/
  - Existing JS extension: tls-namecoin-ext (in-tree reference)

CC: necko-reviewers, JeremyRand (Namecoin project lead, Bugzilla account: jeremyrand)
```

**Blocks:** (leave empty — sub-bugs will block this)
**See Also:** https://github.com/namecoin/namecoin-core

---

## Sub-Bug 1 — Phase 1: DNS Resolution

**Component:** Core :: Networking: DNS
**Type:** Enhancement
**Priority:** P3

**Title:**
```
[namecoin] Phase 1: Native .bit DNS resolution via ElectrumX WebSocket
```

**Description:**
```
Implements native Namecoin (.bit) DNS resolution in nsHostResolver.

Changes:
  1. netwerk/dns/nsNamecoinResolver.h/.cpp — New resolver class
     - Connects to ElectrumX servers via nsIWebSocketChannel
     - Computes ElectrumX scripthash (OP_NAME_UPDATE index script + SHA-256)
     - Queries blockchain.scripthash.get_history for the name
     - Fetches the most recent NAME_UPDATE transaction
     - Decodes the output script to extract the JSON value
     - Parses ip/ip6/map/alias/ns/translate fields
     - Checks name expiry (36,000 block window)
     - Returns NetAddr array to the nsHostResolver callback

  2. netwerk/dns/nsHostResolver.cpp — Hook for TLD == "bit"
     - Before querying system DNS, check IsNamecoinHost()
     - If true && network.namecoin.enabled, dispatch to nsNamecoinResolver
     - .bit MUST NOT fall through to system DNS (would give misleading NXDOMAIN)

  3. modules/libpref/init/StaticPrefList.yaml — 5 new prefs:
     network.namecoin.enabled            (bool, default: false)
     network.namecoin.electrumx_servers  (string, default: ws://electrumx.testls.space:50003)
     network.namecoin.cache_ttl_seconds  (uint32, default: 3600)
     network.namecoin.max_alias_hops     (uint32, default: 5)
     network.namecoin.connection_timeout_ms (uint32, default: 10000)

  4. netwerk/dns/moz.build — add nsNamecoinResolver.cpp/.h

  5. netwerk/test/unit/test_namecoin_resolver.js — xpcshell tests

Key implementation decisions:
  - C++ (not Rust FFI): netwerk layer is C++; nsHostRecord/NetAddr/nsIWebSocketChannel
    are all C++ interfaces. Rust FFI would add complexity for no gain.
  - nsIWebSocketChannel: uses Gecko's existing WS impl, respects proxy settings
    and content process sandboxing.
  - SHA-256 via NSS HASH_HashBuf: same crypto lib Firefox already uses everywhere.
  - Monitor-based blocking for Phase 1: acceptable since nsHostResolver dispatches
    resolution to a background thread pool (MUST NOT run on main thread).
  - Phase 2 should convert to async resolution to avoid tying up pool threads
    on slow ElectrumX responses.

CRITICAL: Uses OP_NAME_UPDATE (0x53) NOT OP_NAME_SHOW (0xd1). Wrong opcode gives
silent empty history from ElectrumX — verified from extension development.

Test vectors (verified against electrumx.testls.space):
  d/testls  scripthash → b519574e96740a4b3627674a0708e71a73e654a95117fc828b8e177a0579ab42
  d/bitcoin scripthash → bdd490728e7f1cbea1836919db5e932cce651a82f5a13aa18a5267c979c95d3c

Patch series:
  0001-etld-bit.patch         — Add "bit" to effective_tld_names.dat
  0002-namecoin-prefs.patch   — Register network.namecoin.* prefs
  0003-nsNamecoinResolver.patch — New source files + moz.build
  0004-nsHostResolver-hook.patch — Hook .bit into nsHostResolver

Blocks: [meta-bug]
```

**Attachment:** Upload the patch series (0001–0004) as separate attachments with
the `phabricator-patch` keyword. Request review from `necko-reviewers`.

---

## Sub-Bug 2 — Phase 2: TLS/DANE-TLSA

**Component:** Core :: Security: PSM
**Type:** Enhancement
**Priority:** P3

**Title:**
```
[namecoin] Phase 2: DANE-TLSA certificate validation for .bit domains
```

**Description:**
```
Depends on: [Phase 1 bug]

Adds DANE-TLSA certificate validation for .bit domains using TLSA records
stored in the Namecoin blockchain (value.tls field).

Changes:
  - security/manager/ssl/nsNSSCallbacks.cpp
    - AuthCertificateCallback: if host ends in .bit, fetch TLSA from
      nsNamecoinResolver (already resolved as part of DNS phase)
    - DANE-EE (usage=3): compare server cert SPKI hash against value.tls entry
    - DANE-TA (usage=2): verify cert chain against TA cert in value.tls
    - Standard CA validation disabled for .bit (not in CT logs)
  - Name constraints: prevent public CAs from issuing for .bit
    (add to certdb name constraints list)
  - Phase 1 nsNamecoinResolver already stores TLSA records in
    NamecoinNameValue::tls (nsTArray<NamecoinTLSARecord>) for consumption here.

Reference: ncp11 PKCS#11 approach (Tor Browser #33568) works without patches,
but native DANE integration is cleaner for upstreaming.

Spec: SPEC.md Section 5

Blocks: [meta-bug]
```

---

## Sub-Bug 3 — Phase 3: UI Indicators

**Component:** Firefox :: Address Bar
**Type:** Enhancement
**Priority:** P4 (after Phase 2)

**Title:**
```
[namecoin] Phase 3: Address bar and certificate viewer UI for .bit domains
```

**Description:**
```
Depends on: [Phase 2 bug]

Adds user-visible indicators for .bit domain navigation:
  - Address bar: Namecoin lock icon
    - Green: DANE-TLSA validated (blockchain-trusted cert)
    - Yellow/info: HTTP only (no TLS)
    - Red: cert mismatch or blockchain error
  - Certificate viewer panel: "Namecoin Blockchain" as trust anchor source
  - about:namecoin: diagnostic page showing:
    - Resolver status (enabled/disabled)
    - ElectrumX server health (latency, block height, last seen)
    - Cache statistics
    - Resolved name details (block height, expiry, value hash)
  - Settings UI: Privacy & Security → Decentralized DNS → Namecoin toggle

Spec: SPEC.md Section 6

Blocks: [meta-bug]
```

---

## Sub-Bug 4 — Phase 4: Android / GeckoView

**Component:** GeckoView :: General
**Type:** Enhancement
**Priority:** P4 (after Phase 3)

**Title:**
```
[namecoin] Phase 4: Android/GeckoView support for .bit resolution
```

**Description:**
```
Depends on: [Phase 1 bug]

All C++ changes from Phase 1 automatically apply to GeckoView (Android).
This bug covers Android-specific adaptations:
  - Battery-aware WebSocket management:
    Close WS connections when app is backgrounded
    Reconnect on foreground resume
  - Fenix Settings: Kotlin activity to expose network.namecoin.enabled
    as a user-visible toggle in the Advanced/Privacy settings
  - GeckoView API: expose NamecoinResolverDelegate for embedding apps
    (optional — allows embedding apps to intercept .bit resolution)
  - Testing: GeckoView test suite additions

Blocks: [meta-bug]
```

---

## Notes for Filing

1. **Order**: File meta-bug first, get the bug number, then file Phase 1.
   Reference the meta-bug number in all sub-bugs' "Blocks:" field.

2. **CC list**: Always CC `necko-reviewers` and `jeremyrand` (Namecoin lead).

3. **Phabricator**: When ready for review, use `moz-phab submit` on the patch
   series. Set the revision title to match the bug title exactly.

4. **Phase 1 review expectations**:
   - eTLD patch is trivial and will likely land without much discussion
   - Prefs patch: reviewers may want names to follow new StaticPrefs conventions
   - New source files: expect C++ style comments (nsHostResolver uses very
     specific patterns — see existing code for examples)
   - nsHostResolver hook: this is the most sensitive change, expect multiple
     review rounds on thread safety

5. **eTLD maintenance note**: The effective_tld_names.dat patch will need
   re-applying every time Mozilla syncs from the upstream Public Suffix List.
   Add a comment to the bug explaining the NAMECOIN_INTEGRATION markers in
   the patch so future maintainers know how to grep for it.
