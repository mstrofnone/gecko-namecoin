# gecko-namecoin — Firefox Native Namecoin Integration

Native `.bit` domain resolution and TLS/DANE validation built directly into
Gecko. No extensions or external daemons required — the browser resolves
Namecoin names via ElectrumX WebSocket and validates certificates against
blockchain TLSA records, all behind an `about:config` flag.

## What's Here

```
.
├── README.md
├── src/
│   ├── nsNamecoinResolver.h        ← Core resolver class (C++ header)
│   ├── nsNamecoinResolver.cpp      ← Core resolver implementation
│   ├── NmcDaneValidator.h          ← DANE-TLSA certificate validator (header)
│   ├── NmcDaneValidator.cpp        ← DANE-TLSA certificate validator
│   ├── nsNamecoinErrors.h          ← Error code definitions
│   ├── netError-namecoin.ftl       ← Fluent strings for error pages
│   ├── namecoin-ui.ftl             ← Fluent strings for all UI surfaces
│   ├── about-namecoin/
│   │   ├── about_namecoin.html     ← Diagnostic page (about:namecoin)
│   │   └── aboutNamecoin.js        ← Diagnostic page logic
│   └── android/
│       ├── NmcConnectionManager.h  ← Battery-aware WS lifecycle (header)
│       ├── NmcConnectionManager.cpp← Battery-aware WS lifecycle
│       ├── NmcGeckoViewSettings.h  ← GeckoView settings bridge (header)
│       ├── NmcGeckoViewSettings.cpp← GeckoView settings bridge
│       ├── NamecoinSettings.kt     ← GeckoView Kotlin settings API
│       ├── FenixNamecoinSettings.kt← Fenix settings fragment
│       └── moz.build               ← Android-conditional build config
├── patches/
│   ├── 0001-etld-bit.patch                    ← Add "bit" to effective_tld_names.dat
│   ├── 0002-namecoin-prefs.patch              ← Register network.namecoin.* prefs
│   ├── 0003-nsNamecoinResolver-new-files.patch← moz.build additions for resolver
│   ├── 0004-nsHostResolver-hook.patch         ← Route .bit queries to resolver
│   ├── 0005-namecoin-error-pages.patch        ← Custom error pages for .bit failures
│   ├── 0006-namecoin-dane-validation.patch    ← DANE-TLSA cert override in NSS callbacks
│   ├── 0007-namecoin-name-constraints.patch   ← Block public CAs for .bit domains
│   ├── 0008-namecoin-https-upgrade.patch      ← Force HTTPS when TLSA records exist
│   ├── 0009-namecoin-about-page.patch         ← Register about:namecoin chrome page
│   ├── 0010-namecoin-address-bar-indicator.patch ← URL bar security indicators
│   ├── 0011-namecoin-cert-viewer.patch        ← Cert viewer Namecoin trust source
│   ├── 0012-namecoin-settings.patch           ← Privacy & Security settings panel
│   ├── 0013-geckoview-namecoin-api.patch      ← GeckoRuntimeSettings Java API + JNI
│   ├── 0014-android-connection-lifecycle.patch ← Wire NmcConnectionManager into resolver
│   └── 0015-android-websocket-battery.patch   ← Battery-aware pool mode (#ifdef ANDROID)
├── docs/
│   ├── PHASE1.md           ← Phase 1 scope and build instructions
│   ├── PHASE2.md           ← Phase 2 DANE-TLSA implementation notes
│   ├── PHASE3.md           ← Phase 3 UI integration notes
│   ├── PHASE4.md           ← Phase 4 Android/GeckoView notes
│   └── BUGZILLA.md         ← Bugzilla meta-bug + sub-bug templates
├── tests/
│   └── test_namecoin_resolver.js  ← xpcshell test suite (also runnable in Node.js)
└── tools/
    ├── test-resolution.mjs    ← Node.js live ElectrumX test
    └── build-patch-series.sh  ← Apply patches to a Firefox source tree
```

## Quick Test (No Firefox Build Required)

```bash
node tools/test-resolution.mjs
```

This runs the full ElectrumX resolution pipeline in JavaScript (mirroring the
C++ implementation) against a live ElectrumX server. It verifies:
- Scripthash computation (SHA-256 of OP_NAME_UPDATE index script)
- NAME_UPDATE script decoder
- Live resolution of `d/testls` → IP address

## Applying to Firefox Source

```bash
# Clone Firefox (one-time, ~10GB)
hg clone https://hg.mozilla.org/mozilla-central
# or: git clone https://github.com/mozilla/gecko-dev

# Apply our patch series
./tools/build-patch-series.sh /path/to/mozilla-central

# Build and test
cd /path/to/mozilla-central
./mach build
./mach run
# In about:config: set network.namecoin.enabled = true
# Navigate to testls.bit
```

## Phases Overview

| Phase | What | Status |
|-------|------|--------|
| 1 | DNS resolution (.bit → HTTP, about:config flag) | ✅ Implemented |
| 2 | TLS / DANE-TLSA cert validation | ✅ Implemented |
| 3 | UI (address bar indicators, cert viewer, about:namecoin, settings) | ✅ Implemented |
| 4 | Android GeckoView + Fenix settings | ✅ Implemented |
| 5 | Enablement (Nightly → Beta → Release) | ⬜ Not started |

All phases are behind the `network.namecoin.enabled` about:config flag (default `false`).

## What Each Phase Adds

### Phase 1 — DNS Resolution

Core `.bit` domain resolution via ElectrumX WebSocket. The `nsNamecoinResolver`
C++ class computes scripthashes, decodes NAME_UPDATE transactions, and resolves
Namecoin name values to IP addresses. Includes connection pooling with health
checks and failover, multi-server cross-validation, DNS result caching with
TTL based on name expiry, and custom error pages for `.bit`-specific failures
(expired names, no servers, resolution errors).

### Phase 2 — DANE-TLSA Certificate Validation

`NmcDaneValidator` implements RFC 6698 DANE-TLSA validation for `.bit` TLS
connections. Supports Usage 2 (DANE-TA) and Usage 3 (DANE-EE) with selectors
for full cert or SPKI, and SHA-256/SHA-512 matching types. Name constraints
block public CAs from issuing `.bit` certificates — only DANE-validated certs
are accepted, creating a hard separation between public PKI and the Namecoin
namespace. Domains with TLSA records are automatically upgraded to HTTPS.

### Phase 3 — User Interface

`about:namecoin` diagnostic page shows ElectrumX server connectivity, recent
resolutions, DANE cache entries, and a live name lookup tool. Address bar
security indicators display teal (DANE-validated), amber (HTTP-only/no TLSA),
or red (validation failed). The certificate viewer shows Namecoin trust source
details including block height, transaction hash, and TLSA record parameters.
A settings panel in Privacy & Security allows toggling resolution, managing
ElectrumX servers, and requiring HTTPS.

### Phase 4 — Android / GeckoView

Adapts the resolver for mobile. `NmcConnectionManager` adds battery-aware
WebSocket lifecycle: connections close after 5 minutes in background, reconnect
lazily on foreground, and drop immediately on network changes. The GeckoView
settings API (`NamecoinSettings.kt`) lets any embedding app configure Namecoin
resolution programmatically. `FenixNamecoinSettings.kt` provides a settings
fragment in Fenix's Privacy & Security panel with toggles for resolution,
HTTPS enforcement, and server configuration.

## Key Correctness Notes

1. **Scripthash must use OP_NAME_UPDATE (0x53)**, NOT OP_NAME_SHOW (0xd1).
   Using the wrong opcode returns empty history — a silent failure with no error.

2. **Name expiry threshold**: 36,000 blocks ≈ 250 days. Expired names must NOT
   be resolved (they may be re-registered by someone else at any time).

3. **ElectrumX requires WebSocket** (ws:// or wss://). The testls.space server
   uses self-signed TLS on port 50004 (browsers reject it); use ws:// port 50003
   or a wss:// server with a valid CA-signed certificate.

4. **.bit must NOT fall through to system DNS** if all ElectrumX servers fail.
   .bit is not a real DNS TLD — system DNS will NXDOMAIN. Fail with a clear
   error instead.

## Reference

- JS reference impl: https://github.com/mstrofnone/tls-namecoin-ext (electrumx-ws.js)
- Namecoin PKCS#11 tool: https://github.com/namecoin/ncp11
- Tor Browser integration (lessons): https://gitlab.torproject.org/tpo/applications/tor-browser/-/issues/30558
- Namecoin spec: https://www.namecoin.org/docs/name-owners/
- DANE RFC 6698: https://datatracker.ietf.org/doc/html/rfc6698
