# Firefox Native Namecoin Integration — Session 4 Handoff Notes
# =============================================================
# Written: 2026-04-16 07:55 AEST
# Purpose: Let a new session pick up where we left off.
# Previous: notes/session-01-phase1-skeleton.md (Phase 1 skeleton), SPEC.md (full spec), PLAN.md (3-tier plan)
# Repo: https://github.com/mstrofnone/gecko-namecoin (or the browse workspace)


================================================================================
WHAT WAS COMPLETED THIS SESSION
================================================================================

Starting point: notes/session-01-phase1-skeleton.md described the Phase 1 skeleton with 4 HIGH PRIORITY
and 3 MEDIUM PRIORITY items remaining. ALL SEVEN are now done.

HIGH PRIORITY (all completed):

  [x] Full ParseNameValue() implementation
      - Built a ~200-line minimal recursive-descent JSON parser (NmcJsonValue)
        in an anonymous namespace inside nsNamecoinResolver.cpp
      - Chosen over mozilla::dom::JSON because DOM objects aren't available
        on background resolver threads (netwerk layer)
      - Handles: objects, arrays, strings (with \uXXXX escapes), numbers,
        booleans, null — covers all Namecoin d/ value shapes
      - Subdomain map traversal: www.sub.example.bit → root.map["sub"]["map"]["www"]
      - Wildcard fallback at any level: value.map["*"]
      - NS delegation: value.ns array of strings
      - TLSA stub: value.tls tuples stored in NamecoinNameValue::tls for Phase 2
      - Non-JSON values ("reserved", plain strings) → graceful no-IP return
      - Helper functions: NmcPopulateNameValue(), NmcExtractSubdomainLabels()

  [x] Fix ElectrumXRequest send timing
      - Moved SendMsg() from post-AsyncOpen() into OnStart callback
      - nsElectrumXListener now stores mRequestToSend + mChannel at construction
      - NOTE: This was subsequently superseded by the connection pooling rewrite
        (see below) which replaced nsElectrumXListener entirely

  [x] xpcshell test suite (tests/test_namecoin_resolver.js)
      - Dual-mode: runs as xpcshell test OR standalone Node.js
      - 5 test groups, 15+ individual test cases:
        1. ComputeScripthash — known-good vectors (d/testls, d/bitcoin, d/example)
        2. DecodeNameScript — round-trip including PUSHDATA1 names
        3. IsNamecoinHost — edge cases (bare TLD, leading dot, case-insensitive)
        4. ParseNameValue — 15 cases: apex, subdomains, wildcards, NS, TLSA,
           alias, translate, non-JSON, malformed JSON, empty object, missing
           map keys with apex fallback, real d/testls shape
        5. Mock ElectrumX integration (Node.js only, uses ws package)

  [x] Bugzilla meta-bug templates (docs/BUGZILLA.md)
      - Meta-bug + 4 sub-bugs (Phase 1-4) with full descriptions
      - CC lists (necko-reviewers, JeremyRand)
      - Phabricator workflow notes
      - eTLD maintenance warnings

MEDIUM PRIORITY (all completed via parallel Opus sub-agents):

  [x] Connection pooling (nsElectrumXConnectionPool)
      - Two new classes replace the old nsElectrumXListener:
        * nsElectrumXPooledConnection — persistent WS per server, serialized
          callers via Monitor, lazy connect, idle timer for cleanup
        * nsElectrumXConnectionPool — keyed by server URL, thread-safe via
          Mutex, auto-replaces dead connections
      - A single .bit resolve now uses 1 WS connection instead of 3
      - Idle connections closed after cache_ttl_seconds (configurable)
      - Pool shutdown wired into nsNamecoinResolver::Shutdown()
      - OLD nsElectrumXListener was FULLY REMOVED (no longer exists)

  [x] Multi-server cross-validation (ElectrumXRequestValidated)
      - New pref: network.namecoin.query_multiple_servers (bool, default false)
      - When enabled: queries ALL configured servers, extracts sorted tx_hash
        fingerprints, groups by agreement, majority wins
      - Disagreements logged with NMC_ERR including per-server details
      - Wired into ResolveInternal() for blockchain.scripthash.get_history only
        (the critical query — tx.get is less sensitive since hash is known)
      - When disabled: zero behavior change, delegates to ElectrumXRequestAny()

  [x] Error pages for .bit failures
      - New file: src/nsNamecoinErrors.h (module 77 error codes)
        * NS_ERROR_NAMECOIN_NOT_FOUND (0x804D0001)
        * NS_ERROR_NAMECOIN_EXPIRED (0x804D0002)
        * NS_ERROR_NAMECOIN_SERVERS_UNREACHABLE (0x804D0003)
        * NS_ERROR_NAMECOIN_NO_ADDRESS (0x804D0004)
      - New file: src/netError-namecoin.ftl (Fluent localization strings)
      - New patch: patches/0005-namecoin-error-pages.patch (integration template)
      - ResolveInternal() now returns specific error codes at all 5 failure sites


================================================================================
GIT HISTORY (this session)
================================================================================

  557625f  Implement WebSocket connection pooling for ElectrumX requests
  23a7202  Add custom error pages for .bit resolution failures
  20758b5  feat: multi-server cross-validation for ElectrumX history queries
  856c30b  gecko-namecoin: Phase 1 completion
  05cdcf1  notes/session-01-phase1-skeleton.md: session handoff notes for Phase 1 gecko-namecoin work
  3501136  gecko-namecoin: Phase 1 patch series — nsNamecoinResolver + ...

Remote: https://github.com/mstrofnone/gecko-namecoin (if push succeeded)


================================================================================
CURRENT FILE LAYOUT
================================================================================

gecko-namecoin/
├── README.md                                    — overview + quick test instructions
├── docs/
│   ├── PHASE1.md                                — scope, build instructions
│   └── BUGZILLA.md                              — ready-to-file Bugzilla templates
├── src/
│   ├── nsNamecoinResolver.h                     — C++ header (~200 lines)
│   ├── nsNamecoinResolver.cpp                   — C++ implementation (~1300 lines)
│   ├── nsNamecoinErrors.h                       — Namecoin-specific error codes
│   └── netError-namecoin.ftl                    — Fluent localization strings
├── patches/
│   ├── 0001-etld-bit.patch                      — Add "bit" to effective_tld_names.dat
│   ├── 0002-namecoin-prefs.patch                — Register network.namecoin.* prefs
│   ├── 0003-nsNamecoinResolver-new-files.patch  — moz.build additions
│   ├── 0004-nsHostResolver-hook.patch           — Hook .bit into nsHostResolver
│   └── 0005-namecoin-error-pages.patch          — Error page integration template
├── tests/
│   └── test_namecoin_resolver.js                — xpcshell + Node.js test suite
└── tools/
    ├── test-resolution.mjs                      — live ElectrumX test
    └── build-patch-series.sh                    — apply patches to mozilla-central


================================================================================
WORKSPACE CONTEXT FILES (outside gecko-namecoin/)
================================================================================

  PLAN.md            — original three-tier integration plan
  SPEC.md           — Tier 3 native implementation spec (the big document)
  notes/session-01-phase1-skeleton.md           — previous session handoff notes (Phase 1 skeleton)
  notes/session-02-phase1-complete.md           — THIS FILE
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

PHASE 1 — FINAL VALIDATION:

  [ ] Build and test in a real Firefox Nightly
      - Clone mozilla-central (hg or git)
      - Apply patches: ./gecko-namecoin/tools/build-patch-series.sh <tree>
      - Copy src/ files to netwerk/dns/
      - ./mach build
      - In about:config: set network.namecoin.enabled = true
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

PHASE 2 — TLS/DANE-TLSA:

  [ ] Implementation spec is in SPEC.md Section 5
  [ ] Key integration point: security/manager/ssl/nsNSSCallbacks.cpp
  [ ] AuthCertificateCallback: DANE-EE as fallback for .bit
  [ ] TLSA records already stored in NamecoinNameValue::tls by Phase 1
  [ ] Must add .bit name constraints to block public CAs
  [ ] Reference: ncp11 (PKCS#11 approach) from JeremyRand

PHASE 3 — UI:
  [ ] Address bar lock icon (green=DANE, yellow=HTTP)
  [ ] Certificate viewer ("Namecoin Blockchain" trust source)
  [ ] about:namecoin diagnostic page
  [ ] Settings UI in Privacy & Security

PHASE 4 — ANDROID:
  [ ] C++ auto-applies to GeckoView
  [ ] Battery-aware WS management
  [ ] Fenix Settings (Kotlin)


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

5. SUB-AGENT WORKFLOW: The three MEDIUM priority items were implemented in
   parallel by three separate Opus sub-agents. They each modified different
   parts of nsNamecoinResolver.cpp. Git shows them as sequential commits but
   they were developed simultaneously. Check for any subtle merge conflicts
   (I didn't see any, but the connection pooling one touched the most code).


================================================================================
POTENTIAL ISSUES / THINGS TO WATCH
================================================================================

1. The connection pooling completely replaced the ElectrumX transport layer.
   The OnStart send timing fix (from the HIGH PRIORITY batch) was made obsolete
   by this — the pooled connection handles send timing internally. The git
   history shows both changes, but only the pool version matters now.

2. Three sub-agents independently modified nsNamecoinResolver.cpp. While git
   committed them sequentially (cross-validation first, then error pages, then
   pooling), they were developed against the same base. The pooling agent did
   the most invasive changes. A careful read-through of the full .cpp would be
   wise before the Nightly build.

3. The test suite (test_namecoin_resolver.js) tests the JavaScript reference
   implementation, not the C++ directly. The xpcshell integration requires
   the Gecko build to work. The tests verify algorithmic correctness (scripthash,
   decode, parse) which must match between JS and C++.

4. The eTLD patch (0001) WILL break every time Mozilla updates
   effective_tld_names.dat from the upstream PSL. The NAMECOIN_INTEGRATION
   comment markers make this grep-able, but it's a known maintenance burden.

5. The ElectrumX server situation is fragile. testls.space:50003 is really
   the only reliable public WS server. For a production Firefox build, users
   should be encouraged to run their own ElectrumX instance. The default server
   list should eventually include 3+ servers.


================================================================================
END — PICK UP FROM HERE
================================================================================
