# Phase 3: UI Integration — Implementation Notes

## Overview

Phase 3 adds user-facing UI for the Namecoin .bit integration. This includes
a diagnostic page, address bar security indicators, certificate viewer
integration, localization strings, and settings UI.

## What Was Implemented

### 1. `about:namecoin` Diagnostic Page

**Files:** `src/about-namecoin/about_namecoin.html`, `src/about-namecoin/aboutNamecoin.js`

A full diagnostic page matching Firefox's Proton design language:

```
┌─────────────────────────────────────────────────────────┐
│ [N] Namecoin Diagnostics                                │
│ about:namecoin — Monitor .bit resolution & connectivity │
│                                                         │
│ ● Namecoin (.bit) resolution is enabled                 │
│                                                         │
│ ┌─ ElectrumX Servers ───────── [2/3 connected] ──────┐  │
│ │  Height: 824,155  Connected: 2/3  Latency: 78 ms   │  │
│ │                                                     │  │
│ │  Server URL          Status     Latency  Height     │  │
│ │  wss://electrumx…    ● Connected  62 ms  824,155    │  │
│ │  wss://nmc.obeli…    ● Connected  94 ms  824,152    │  │
│ │  ws://127.0.0.1:…    ○ Disconnected  —     —        │  │
│ └─────────────────────────────────────────────────────┘  │
│                                                         │
│ ┌─ Recent Resolutions ────────── [20 lookups] ───────┐  │
│ │  Domain       Result          Time  Cache  When     │  │
│ │  testls.bit   185.45.12.88    82ms  MISS   14:32    │  │
│ │  wiki.bit     94.23.204.17     4ms  HIT    14:31    │  │
│ │  nf.bit       2a01:4f8:…     156ms  MISS   14:30    │  │
│ │  nx.bit       ⚠ EXPIRED      211ms  MISS   14:29    │  │
│ │  …                                                  │  │
│ └─────────────────────────────────────────────────────┘  │
│                                                         │
│ ┌─ TLS / DANE Cache ──────────── [4 entries] ────────┐  │
│ │  Domain       Fingerprint      Match      Usage     │  │
│ │  testls.bit   a3:f2:…:33:44   SHA-256    DANE-EE   │  │
│ │  bitcoin.bit  d1:e4:…:ba:98   SHA-256    DANE-EE   │  │
│ │  …                                                  │  │
│ └─────────────────────────────────────────────────────┘  │
│                                                         │
│ ┌─ Name Lookup Tool ─────────────────────────────────┐  │
│ │  [ example.bit              ] [ Resolve ]           │  │
│ │                                                     │  │
│ │  Raw JSON Value:                                    │  │
│ │  ┌─────────────────────────────────────────────┐    │  │
│ │  │ { "ip": "185.45.12.88", "tls": { … } }     │    │  │
│ │  └─────────────────────────────────────────────┘    │  │
│ │                                                     │  │
│ │  Parsed Result:                                     │  │
│ │    Domain:       testls.bit                         │  │
│ │    IPv4:         185.45.12.88                       │  │
│ │    Block Height: 821,455                            │  │
│ │    TX Hash:      d7a8fbb307d7…                      │  │
│ │    Has TLSA:     Yes                                │  │
│ └─────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

**Features:**
- Dark/light mode aware via `prefers-color-scheme`
- Responsive layout (mobile-friendly)
- No external dependencies — pure HTML/CSS/JS
- Mock data with realistic structures (marked with `// REAL IMPL:` comments)
- Live name lookup tool with error handling
- Auto-refresh capability (commented out for prototype)

### 2. Address Bar Security Indicators

**File:** `patches/0010-namecoin-address-bar-indicator.patch`

Three CSS classes applied to `#identity-box`:

| Class | Color | Icon | Meaning |
|-------|-------|------|---------|
| `nmc-secure` | Teal (#00a78e) | Lock | DANE-TLSA validated cert |
| `nmc-warning` | Amber (#c27400) | Warning | HTTP-only or no TLSA |
| `nmc-error` | Red (#d70022) | Broken lock | TLSA validation failed |

```
Address bar states:

  🔒 testls.bit                    ← nmc-secure (teal lock)
  ⚠️  example.bit                   ← nmc-warning (amber)
  🔴 compromised.bit               ← nmc-error (red broken lock)
```

Includes:
- `UrlbarNamecoinSecurity.sys.mjs` — security state detection module
- CSS with dark mode variants
- Fluent l10n IDs for all tooltip/label strings
- Identity popup integration points

### 3. Certificate Viewer Changes

**File:** `patches/0011-namecoin-cert-viewer.patch`

When viewing a certificate for a `.bit` domain with DANE validation:

```
┌── Certificate Details ─────────────────────────────────┐
│                                                        │
│  … standard certificate fields …                       │
│                                                        │
│  ━━━ Namecoin Blockchain Verification ━━━              │
│                                                        │
│  ✓ Certificate matches blockchain TLSA record          │
│                                                        │
│  Trust Source:    Namecoin Blockchain (DANE-TLSA)       │
│  Block Height:   Block 821,455                         │
│  Transaction:    d7a8fbb307d7…37c9e592 (view ↗)       │
│  Name Expiry:    Block 857,455 (approx. 2026-08-15)    │
│                                                        │
│  DANE-TLSA Record:                                     │
│    Usage:     3 — DANE-EE (End Entity)                 │
│    Selector:  1 — SubjectPublicKeyInfo                 │
│    Matching:  1 — SHA-256                              │
└────────────────────────────────────────────────────────┘
```

Includes:
- `CertDecoder.buildNamecoinInfoItems()` — structured data builder
- `InfoGroup._renderNamecoinSection()` — DOM renderer
- `TransportSecurityInfo.h` — `NamecoinDANEInfo` struct
- `nsNSSCertHelper.cpp` — integration point (commented)
- Explorer link to `namecha.in/tx/{hash}`

### 4. Fluent Localization Strings

**File:** `src/namecoin-ui.ftl`

Complete l10n coverage for all UI surfaces:
- **Address bar:** 9 strings (tooltips, labels, identity popup)
- **Certificate viewer:** 17 strings (trust source, TLSA details, status)
- **about:namecoin:** 30+ strings (all page labels, status messages, empty states)
- **Settings:** 12 strings (toggles, descriptions, buttons)
- **DANE errors:** 6 strings (mismatch, expired, no TLSA, fallback, weak match, self-signed)

### 5. Settings UI

**File:** `patches/0012-namecoin-settings.patch`

Added to Privacy & Security → Security section:

```
┌── Namecoin (.bit) Domains ────────────────────────────┐
│                                                       │
│  Resolve .bit domains directly from the Namecoin      │
│  blockchain using ElectrumX servers. Experimental.    │
│                                                       │
│  ☑ Enable Namecoin (.bit) domain resolution           │
│                                                       │
│  ElectrumX Servers:                                   │
│  ┌─────────────────────────────────────────────┐      │
│  │ wss://electrumx-nmc.example.org:50004       │      │
│  │ wss://nmc.obelisk.xyz:50004                 │      │
│  └─────────────────────────────────────────────┘      │
│                    [Add Server] [Restore Defaults]    │
│                                                       │
│  ☑ Require HTTPS for .bit when TLSA records exist     │
│                                                       │
│  🔗 Open Namecoin Diagnostics                         │
└───────────────────────────────────────────────────────┘
```

### 6. about:namecoin Registration

**File:** `patches/0009-namecoin-about-page.patch`

Registers `about:namecoin` via `AboutRedirector.cpp` with:
- `ALLOW_SCRIPT` — page needs JavaScript
- `IS_SECURE_CHROME_UI` — runs in parent process with chrome privileges
- JAR manifest entries for the HTML and JS files

## How to Test

### about:namecoin Page
1. Open `src/about-namecoin/about_namecoin.html` directly in Firefox
2. The prototype uses mock data — all panels will populate with realistic test data
3. Try the name lookup tool: type `testls.bit` or `bitcoin.bit` for mock results
4. Test dark mode: toggle system appearance or use DevTools
5. Test responsive: resize window below 600px

### Patches (Review Only)
The patch files are realistic diffs showing exact integration points in the
Firefox source tree. They cannot be applied to a vanilla Firefox checkout
without the Phase 1 and Phase 2 changes in place.

To review:
```bash
# Read any patch
cat patches/0010-namecoin-address-bar-indicator.patch

# Check all patches parse as valid diffs
for p in patches/00{09,10,11,12}*.patch; do
  echo "=== $p ==="
  grep -c '^[+-]' "$p"
done
```

### Fluent Strings
```bash
# Verify all l10n IDs referenced in patches exist in the FTL file
grep 'data-l10n-id=' patches/001*.patch | \
  sed 's/.*data-l10n-id="\([^"]*\)".*/\1/' | sort -u | \
  while read id; do
    grep -q "^$id " src/namecoin-ui.ftl && echo "✓ $id" || echo "✗ $id MISSING"
  done
```

## Known Gaps

1. **Full XUL/React integration:** The about:namecoin page uses standard HTML
   rather than XUL or the React-based about:certificate infrastructure. Full
   integration requires deeper Gecko build system changes.

2. **XPCOM bindings:** The `aboutNamecoin.js` uses mock data. Real integration
   requires `nsINamecoinResolver` XPCOM interface definition (`.idl` file) and
   registration so `Cc["@mozilla.org/network/namecoin-resolver;1"]` works.

3. **Identity popup panel:** The address bar patch shows integration points
   but doesn't include the full XUL changes needed for the identity popup
   panel (the dropdown when you click the lock icon). This requires modifying
   `browser/base/content/browser-siteIdentity.js`.

4. **Certificate viewer React components:** The cert viewer patch shows the
   data flow but the actual React component changes (about:certificate uses
   a custom element + lit-html pattern) need adaptation for the build system.

5. **Preference observer:** The settings UI writes prefs directly but doesn't
   set up a `nsIPrefBranch` observer to reactively update the resolver when
   settings change mid-session. The resolver would need to watch for pref
   changes and reconnect to new servers.

6. **Fenix/Android settings:** Desktop settings only in this phase. Android
   settings (Kotlin activity in Fenix) are tracked in Phase 4.

7. **Automated tests:** No mochitests or browser chrome tests are included.
   Production patches would need:
   - `browser/components/urlbar/tests/browser/browser_namecoin_indicator.js`
   - `toolkit/components/certviewer/tests/browser/browser_namecoin_cert.js`
   - `browser/components/preferences/tests/browser/browser_namecoin_settings.js`

## File Inventory

| File | Type | Description |
|------|------|-------------|
| `src/about-namecoin/about_namecoin.html` | HTML | Diagnostic page |
| `src/about-namecoin/aboutNamecoin.js` | JS | Page logic with mock data |
| `src/namecoin-ui.ftl` | FTL | All UI localization strings |
| `patches/0009-namecoin-about-page.patch` | Patch | about: page registration |
| `patches/0010-namecoin-address-bar-indicator.patch` | Patch | URL bar security indicators |
| `patches/0011-namecoin-cert-viewer.patch` | Patch | Certificate viewer DANE info |
| `patches/0012-namecoin-settings.patch` | Patch | Privacy & Security settings |
| `docs/PHASE3.md` | Docs | This file |
