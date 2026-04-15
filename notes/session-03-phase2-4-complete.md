# Firefox Native Namecoin Integration — Session 5 Handoff Notes
# =============================================================
# Written: 2026-04-16 08:18 AEST
# Purpose: Let a new session pick up where we left off.
# Previous: notes/session-02-phase1-complete.md (Phase 1 completion), notes/session-01-phase1-skeleton.md (Phase 1 skeleton),
#           SPEC.md (full spec), PLAN.md (3-tier plan)
# Repo: https://github.com/mstrofnone/gecko-namecoin (or the browse workspace)


================================================================================
WHAT WAS COMPLETED THIS SESSION
================================================================================

Starting point: notes/session-02-phase1-complete.md described Phase 1 as fully implemented with Phases
2-4 remaining. This session implemented ALL THREE remaining phases (2, 3, 4).

PHASE 2 — DANE-TLSA VALIDATION:

  [x] NmcDaneValidator.h/cpp — full DANE-TLSA validator (~580+232 lines)
      - NmcValidateDane() entry point
      - Usage types: 2 (DANE-TA) and 3 (DANE-EE)
      - Selectors: 0=full DER cert, 1=SubjectPublicKeyInfo
      - Matching types: 0=exact, 1=SHA-256, 2=SHA-512 via NSS HASH_HashBuf
      - Port-specific TLSA via `_tcp._<port>` map traversal
      - NmcDaneCache with TTL expiry, keyed on `domain:cert_fingerprint_sha256`

  [x] patches/0006 — AuthCertificateCallback integration
      - DANE validation as fallback after CA verification fails
      - Hooks into security/manager/ssl/nsNSSCallbacks.cpp

  [x] patches/0007 — Name constraints
      - Block public CAs from issuing certificates for .bit domains
      - Pre-check in AuthCertificateCallback before DANE validation
      - Consistent with how .onion was handled in Tor Browser

  [x] patches/0008 — HTTPS upgrade
      - require_tls flag from Namecoin d/ value
      - Channel redirect from HTTP → HTTPS when flag is set

  [x] docs/PHASE2.md

PHASE 3 — UI INTEGRATION:

  [x] src/about-namecoin/about_namecoin.html + aboutNamecoin.js
      - Full diagnostic page with:
        * Server status display
        * Resolution log viewer
        * DANE cache inspector
        * Live name lookup tool
      - Dark/light mode support, no external dependencies

  [x] src/namecoin-ui.ftl — Fluent l10n strings for all UI surfaces

  [x] patches/0009 — about:namecoin chrome registration

  [x] patches/0010 — Address bar security indicators
      - nmc-secure / nmc-warning / nmc-error states
      - Custom lock icon for DANE-validated connections

  [x] patches/0011 — Certificate viewer integration
      - "Namecoin Blockchain (DANE-TLSA)" as trust source in cert viewer

  [x] patches/0012 — Settings UI
      - Enable/disable toggle in Privacy & Security
      - Server list editor
      - HTTPS upgrade toggle
      - Link to about:namecoin diagnostics page

  [x] docs/PHASE3.md

PHASE 4 — ANDROID / GECKOVIEW:

  [x] src/android/NmcConnectionManager.h/cpp — battery-aware WS lifecycle
      - Foreground/background/network-change transitions
      - 5-minute background idle timer, lazy connect when backgrounded
      - nsIObserverService observers:
        * geckoview:activity-state-changed
        * network:link-status-changed
        * network:offline-status-changed

  [x] src/android/NmcGeckoViewSettings.h/cpp — XPCOM → prefs bridge

  [x] src/android/NamecoinSettings.kt — GeckoView embedding API

  [x] src/android/FenixNamecoinSettings.kt — Fenix NamecoinSettingsFragment

  [x] src/android/moz.build

  [x] patches/0013 — GeckoRuntimeSettings.Builder API + JNI bridge

  [x] patches/0014 — NmcConnectionManager lifecycle wiring

  [x] patches/0015 — battery_aware mode with #ifdef ANDROID guards

  [x] docs/PHASE4.md


================================================================================
GIT HISTORY (this session)
================================================================================

  3c4edb4  Phase 3: UI integration — about:namecoin, address bar indicators, cert viewer, settings
  91ca666  Phase 2: DANE-TLSA validation, name constraints, HTTPS upgrade
  756e86e  Phase 4: Android/GeckoView — battery-aware connections, Fenix settings, GeckoView API

Remote: https://github.com/mstrofnone/gecko-namecoin


================================================================================
CURRENT FILE LAYOUT
================================================================================

gecko-namecoin/
├── README.md                                       — overview + quick test instructions
├── docs/
│   ├── BUGZILLA.md                                 — ready-to-file Bugzilla templates
│   ├── PHASE1.md                                   — Phase 1 scope, build instructions
│   ├── PHASE2.md                                   — Phase 2 DANE-TLSA spec + implementation notes
│   ├── PHASE3.md                                   — Phase 3 UI integration spec
│   └── PHASE4.md                                   — Phase 4 Android/GeckoView spec
├── src/
│   ├── nsNamecoinResolver.h                        — C++ header (~200 lines)
│   ├── nsNamecoinResolver.cpp                      — C++ implementation (~1300 lines)
│   ├── nsNamecoinErrors.h                          — Namecoin-specific error codes (module 77)
│   ├── netError-namecoin.ftl                       — Fluent l10n for error pages
│   ├── namecoin-ui.ftl                             — Fluent l10n for all UI surfaces
│   ├── NmcDaneValidator.h                          — DANE-TLSA validator header (~232 lines)
│   ├── NmcDaneValidator.cpp                        — DANE-TLSA validator impl (~580 lines)
│   ├── about-namecoin/
│   │   ├── about_namecoin.html                     — diagnostic page (dark/light mode)
│   │   └── aboutNamecoin.js                        — diagnostic page logic
│   └── android/
│       ├── moz.build                               — Android-specific build config
│       ├── NmcConnectionManager.h                  — battery-aware WS lifecycle header
│       ├── NmcConnectionManager.cpp                — battery-aware WS lifecycle impl
│       ├── NmcGeckoViewSettings.h                  — XPCOM → prefs bridge header
│       ├── NmcGeckoViewSettings.cpp                — XPCOM → prefs bridge impl
│       ├── NamecoinSettings.kt                     — GeckoView embedding API
│       └── FenixNamecoinSettings.kt                — Fenix NamecoinSettingsFragment
├── patches/
│   ├── 0001-etld-bit.patch                         — Add "bit" to effective_tld_names.dat
│   ├── 0002-namecoin-prefs.patch                   — Register network.namecoin.* prefs
│   ├── 0003-nsNamecoinResolver-new-files.patch     — moz.build additions
│   ├── 0004-nsHostResolver-hook.patch              — Hook .bit into nsHostResolver
│   ├── 0005-namecoin-error-pages.patch             — Error page integration template
│   ├── 0006-namecoin-dane-validation.patch         — AuthCertificateCallback DANE fallback
│   ├── 0007-namecoin-name-constraints.patch        — Block public CAs for .bit
│   ├── 0008-namecoin-https-upgrade.patch           — require_tls channel redirect
│   ├── 0009-namecoin-about-page.patch              — about:namecoin chrome registration
│   ├── 0010-namecoin-address-bar-indicator.patch   — Security indicators (nmc-secure/warning/error)
│   ├── 0011-namecoin-cert-viewer.patch             — "Namecoin Blockchain (DANE-TLSA)" trust source
│   ├── 0012-namecoin-settings.patch                — Settings UI in Privacy & Security
│   ├── 0013-geckoview-namecoin-api.patch           — GeckoRuntimeSettings.Builder + JNI bridge
│   ├── 0014-android-connection-lifecycle.patch     — NmcConnectionManager lifecycle wiring
│   └── 0015-android-websocket-battery.patch        — battery_aware mode + #ifdef ANDROID guards
├── tests/
│   └── test_namecoin_resolver.js                   — xpcshell + Node.js test suite
└── tools/
    ├── test-resolution.mjs                         — live ElectrumX test
    └── build-patch-series.sh                       — apply patches to mozilla-central


================================================================================
WORKSPACE CONTEXT FILES (outside gecko-namecoin/)
================================================================================

  PLAN.md            — original three-tier integration plan
  SPEC.md           — Tier 3 native implementation spec (the big document)
  notes/session-01-phase1-skeleton.md           — session handoff notes (Phase 1 skeleton)
  notes/session-02-phase1-complete.md           — session handoff notes (Phase 1 completion)
  notes/session-03-phase2-4-complete.md           — THIS FILE
  tls-namecoin-ext/      — JS browser extension (reference implementation)
  "TLS Namecoin Resolver/" — Safari/macOS extension wrapper (Xcode project)
  namecoin.txt           — full Namecoin GitHub organization catalog
  extension*.txt         — extension development notes (1-10)
  howto-bit-website.txt  — guide for setting up a .bit website
  browser.txt            — browser integration research notes
  electrumx.txt          — ElectrumX protocol documentation


================================================================================
WHAT'S LEFT TO DO
================================================================================

PHASE 1 — FINAL VALIDATION (still pending from session 4):

  [ ] Build and test in a real Firefox Nightly
      - Clone mozilla-central (hg or git)
      - Apply patches: ./gecko-namecoin/tools/build-patch-series.sh <tree>
      - Copy src/ files to netwerk/dns/
      - ./mach build
      - Navigate to testls.bit — should resolve to 107.152.38.155
      - Test expired name, non-JSON name (d/bitcoin), subdomain (sub1.testls.bit)
      - Verify error pages show Namecoin-specific messages

  [ ] File Bugzilla meta-bug
      - Use docs/BUGZILLA.md content (copy-paste ready)
      - File meta-bug first, get bug number
      - File Phase 1 sub-bug, reference meta-bug
      - CC necko-reviewers and jeremyrand

  [ ] Phabricator submission
      - moz-phab submit on the patch series
      - Expect multiple review rounds, especially on:
        * nsHostResolver hook (thread safety scrutiny)
        * eTLD patch (maintenance burden discussion)
        * Error code module number (77 — may conflict, check nsNetErrorCodes.h)

PHASE 2 — INTEGRATION TESTING:

  [ ] Test DANE-TLSA against a real .bit site with TLSA records
      - d/testls has tls field — use as primary test target
  [ ] Verify cert cache TTL behaviour
      - NmcDaneCache should evict expired entries correctly
  [ ] Verify name constraint rejection
      - Use openssl s_client + CA-signed .bit cert to confirm rejection

PHASE 3 — INTEGRATION:

  [ ] Wire about:namecoin JS to real XPCOM resolver
      - Currently uses mock data
      - Requires a privileged JS module and IPC message
  [ ] Test address bar indicator CSS in real Firefox build
  [ ] Verify cert viewer patch integrates cleanly
      - Check against current browser/components/certviewer/ in mozilla-central

PHASE 4 — ANDROID TESTING:

  [ ] Build GeckoView AAR with patches applied
      - Test on physical Android device
  [ ] Test Wi-Fi ↔ LTE transition handling
      - Verify NmcConnectionManager reconnects correctly
  [ ] Test battery/backgrounding behaviour on real device
      - 5-min idle timer, lazy reconnect on foreground
  [ ] Integrate FenixNamecoinSettings into Fenix PR
      - Target: mozilla-mobile/firefox-android repo

PHASE 5 — ENABLEMENT (not started):

  [ ] about:config only (Nightly) — evaluate stability
  [ ] Settings toggle (Beta)
  [ ] Default-off with discovery UI (Release)


================================================================================
ARCHITECTURE DECISIONS LOG
================================================================================

1. JSON PARSER: Wrote a minimal recursive-descent parser (~200 lines) instead of
   using mozilla::dom::JSON. Reason: DOM objects require SpiderMonkey context
   which isn't available on the netwerk background resolver threads. The parser
   handles all Namecoin d/ value shapes. Phase 2 may revisit if threading changes.

2. CONNECTION POOLING: Full replacement of the original nsElectrumXListener.
   The old one-shot pattern (open WS → send → receive → close) was replaced
   with nsElectrumXPooledConnection (persistent per server) + pool manager.
   This changed a LOT of code in the transport layer — if you need to debug
   WebSocket issues, the pool classes are the place to look.

3. CROSS-VALIDATION: Off by default because there's really only one reliable
   public ElectrumX WS server right now (testls.space:50003). The feature is
   ready for when more servers come online.

4. ERROR CODES: Module 77 was chosen for Namecoin errors. This needs to be
   verified against the actual mozilla-central nsNetErrorCodes.h to ensure no
   conflict. The module number may need to change during review.

5. SUB-AGENT WORKFLOW: The three MEDIUM priority items (session 4) were
   implemented in parallel by three separate Opus sub-agents. They each modified
   different parts of nsNamecoinResolver.cpp. Check for subtle merge conflicts.

6. DANE VALIDATOR: Standalone NmcDaneValidator rather than inline in
   nsNSSCallbacks.cpp. Reason: testability and isolation from NSS internals.
   The validator has no direct NSS dependency beyond HASH_HashBuf, making it
   easier to unit test and maintain across NSS version changes.

7. NAME CONSTRAINTS: Implemented as a pre-check in AuthCertificateCallback
   before DANE validation. This is consistent with how .onion was handled in
   Tor Browser — reject CA-signed certs for the namespace before attempting
   any alternative validation path.

8. ANDROID LIFECYCLE: C++ observer pattern (nsIObserverService) rather than
   JNI callbacks for NmcConnectionManager. Keeps Android-specific code
   contained to NmcConnectionManager and avoids threading issues at the
   JNI boundary. The observer topics (geckoview:activity-state-changed,
   network notifications) are already dispatched on the main thread.

9. GECKOVIEW API: Thin XPCOM bridge (NmcGeckoViewSettings) rather than direct
   pref exposure. Allows future API evolution without breaking the embedding
   contract. Settings flow: Kotlin → JNI → NmcGeckoViewSettings → prefs.


================================================================================
POTENTIAL ISSUES / THINGS TO WATCH
================================================================================

1. NmcDaneCache is NOT thread-safe by itself — callers must hold appropriate
   locks. This is documented in the header but easy to miss. The
   AuthCertificateCallback integration (patch 0006) handles this correctly,
   but any new callers must be careful.

2. The about:namecoin JS currently uses MOCK DATA. Wiring to real XPCOM
   requires a privileged JS module (ChromeUtils.importESModule) and an IPC
   message to query the resolver from the parent process. This is the biggest
   remaining integration gap for Phase 3.

3. Error code module 77 still needs verification against nsNetErrorCodes.h
   in mozilla-central. If there's a conflict, the module number and all error
   code constants in nsNamecoinErrors.h will need updating.

4. Phase 3 agent timed out at the commit step — files were captured and
   committed by the orchestrating session, but the agent's git history is
   inconsistent with the remote. The files themselves are correct; only the
   commit authorship metadata differs from what the agent intended.

5. The eTLD patch (0001) WILL break every time Mozilla updates
   effective_tld_names.dat from the upstream PSL. The NAMECOIN_INTEGRATION
   comment markers make this grep-able, but it's a known maintenance burden.

6. The ElectrumX server situation remains fragile. testls.space:50003 is still
   the only reliable public WS server. For production, users should run their
   own ElectrumX instance.


================================================================================
END — PICK UP FROM HERE
================================================================================
