# Firefox Native Namecoin Integration — Session Handoff Notes
# =============================================================
# Written: 2026-04-16 07:32 AEST
# Purpose: Let a new session pick up Phase 1 (Gecko patches) where we left off.
# Context: PLAN.md (three-tier plan), SPEC.md (full Tier 3 spec)


================================================================================
WHAT WAS BUILT THIS SESSION
================================================================================

Directory: gecko-namecoin/

Created the Phase 1 patch series for native .bit DNS resolution in Firefox.
Everything is committed to git in the workspace.

Files created:
  gecko-namecoin/
  ├── README.md                                    — overview + quick test instructions
  ├── docs/PHASE1.md                               — scope, build instructions, Phabricator notes
  ├── src/nsNamecoinResolver.h                      — C++ header (190 lines)
  ├── src/nsNamecoinResolver.cpp                    — C++ implementation (530 lines)
  ├── patches/0001-etld-bit.patch                   — Add "bit" to effective_tld_names.dat
  ├── patches/0002-namecoin-prefs.patch             — Register network.namecoin.* prefs
  ├── patches/0003-nsNamecoinResolver-new-files.patch — moz.build additions
  ├── patches/0004-nsHostResolver-hook.patch         — Hook .bit into nsHostResolver
  ├── tools/test-resolution.mjs                      — Node.js test suite (all passing)
  └── tools/build-patch-series.sh                    — Apply patches to mozilla-central


================================================================================
KEY DECISIONS MADE
================================================================================

1. LANGUAGE: C++ (not Rust via FFI). Gecko's netwerk/ layer is C++. The
   nsHostResolver integration needs to speak directly to nsHostRecord,
   NetAddr, nsIWebSocketChannel — all C++ interfaces. Rust FFI would add
   complexity for no gain in this layer. Phase 2 TLS/DANE work might use
   Rust if NSS provides Rust bindings by then.

2. WEBSOCKET TRANSPORT: Using Gecko's built-in nsIWebSocketChannel rather
   than raw sockets. This respects Firefox's proxy settings, security
   policies, and content process sandboxing. The channel is created via
   NS_WEBSOCKETCHANNEL_CONTRACTID.

3. SYNCHRONOUS BLOCKING FOR PHASE 1: ResolveInternal() blocks the calling
   thread using a Monitor. This is acceptable for Phase 1 because
   nsHostResolver already dispatches resolution to a background thread pool.
   Phase 2 should convert to proper async (post callback to event target)
   to avoid tying up pool threads during slow ElectrumX responses.

4. JSON PARSING: Phase 1 uses simple string search (Find/Substring) to
   extract ip/ip6/alias/translate from the JSON value. This works for
   top-level flat fields but doesn't handle subdomain map traversal.
   Full JSON parser (mozilla::dom::JSON or a vendored micro-parser like
   simdjson) needed for ParseNameValue completion.

5. HASH IMPLEMENTATION: SHA-256 via NSS HASH_HashBuf (sechash.h). This is
   the same crypto library Firefox already uses everywhere — no new deps.

6. PATCH STRATEGY: 4 separate patches for clean review:
   - eTLD change (trivial, low-risk)
   - Prefs (new keys only, no existing code touched)
   - New files (moz.build + .h/.cpp)
   - Hook (surgical change in nsHostResolver.cpp)
   This order lets reviewers approve low-risk patches first.


================================================================================
CRITICAL IMPLEMENTATION DETAILS (DON'T GET WRONG)
================================================================================

1. SCRIPTHASH OPCODE: OP_NAME_UPDATE = 0x53, NOT OP_NAME_SHOW (0xd1).
   Using the wrong opcode produces empty history from ElectrumX with NO
   error message — a completely silent failure. This was the single hardest
   bug to figure out in the extension development. The C++ ComputeScripthash()
   is correct; don't change the opcode.

   Index script format:
     0x53 + pushdata(name) + pushdata("") + 0x6d + 0x75 + 0x6a

2. SCRIPTHASH BYTE ORDER: SHA-256 the script, then REVERSE the 32 bytes,
   then hex-encode. ElectrumX uses reversed byte order for scripthashes.
   The C++ does this with a simple swap loop (i ↔ 31-i for i in 0..15).

3. PUSHDATA ENCODING: Uses Bitcoin's pushdata rules:
   - len < 0x4c → [len] + data
   - len <= 0xff → [0x4c, len] + data
   - len <= 0xffff → [0x4d, lo, hi] + data
   For "d/example" (9 bytes): [0x09, ...data] (single byte prefix).
   For empty value: [0x00] (push empty, NOT missing).

4. NAME EXPIRY: 36,000 blocks. (current_height - update_height) > 36000
   means the name is expired. MUST NOT resolve expired names — they can
   be re-registered by anyone at any time.

5. NON-JSON VALUES: Some Namecoin names have non-JSON values like "reserved"
   or plain strings. The resolver must handle this gracefully (name exists
   but has no IP — not an error, just unresolvable).

6. .BIT MUST NOT FALL THROUGH TO SYSTEM DNS: If all ElectrumX servers fail,
   return a network error — NOT NXDOMAIN via system DNS. .bit is not a real
   DNS TLD; system DNS would give NXDOMAIN which is misleading.


================================================================================
TEST RESULTS (2026-04-16)
================================================================================

ElectrumX server: ws://electrumx.testls.space:50003 (ElectrumX 1.16.0)
Namecoin block height: 820,568

Scripthash verification (these are stable — depend only on the name string):
  d/testls  → b519574e96740a4b3627674a0708e71a73e654a95117fc828b8e177a0579ab42
  d/bitcoin → bdd490728e7f1cbea1836919db5e932cce651a82f5a13aa18a5267c979c95d3c
  d/example → 92f51fe51fa9c53ea842325c9c63a9b8592b3f64c7477b6e538a64d80cd8c0ac

Live resolution results:
  d/testls:
    - 23 transactions, most recent at height 815,962
    - Value: {"ip": "107.152.38.155", "map": {"*": ..., "sub1": ..., "_tor": ...}}
    - IP: 107.152.38.155
    - Expiry: ~218 days remaining (31,394 blocks)
    - TLSA: has records in map._tor (port-specific)
    - This is the primary test domain — JeremyRand (Namecoin lead) controls it

  d/bitcoin:
    - 28 transactions, most recent at height 809,395
    - Value: "reserved" (non-JSON, just a string)
    - No IP address — unresolvable but valid
    - Expiry: ~172 days remaining

  d/example:
    - Not tested live (may or may not exist)
    - Scripthash computed for future verification

Run tests with: node gecko-namecoin/tools/test-resolution.mjs


================================================================================
WHAT'S LEFT TO DO (Phase 1 completion)
================================================================================

HIGH PRIORITY (needed before first Firefox build):

  [ ] Full ParseNameValue() implementation
      - Subdomain map traversal: www.example.bit → value.map["www"].ip
      - Wildcard handling: value.map["*"]
      - NS record array parsing (value.ns → standard DNS delegation)
      - Port-specific TLSA via map._tcp._443.tls (Phase 2 consumes this)
      Current: only extracts top-level ip/ip6/alias/translate via string search.
      Need: proper JSON parser. Options:
        a. mozilla::dom::JSON (Gecko's built-in, SpiderMonkey-based)
        b. Vendor a micro JSON parser (simdjson is huge; maybe sajson or jsmn)
        c. Write a minimal recursive descent parser (~200 lines for our schema)
      Recommendation: option (c) for Phase 1, switch to (a) for Phase 2.

  [ ] Fix ElectrumXRequest send timing
      Current: sends JSON-RPC request immediately after AsyncOpen().
      Problem: AsyncOpen is async — the channel may not be open yet.
      Fix: move ws.send() into the OnStart callback of nsElectrumXListener.
      The listener already has OnStart() → NS_OK; change it to send the
      request string. Store the request string on the listener at construction.

  [ ] xpcshell test suite
      Location: netwerk/test/unit/test_namecoin_resolver.js
      Tests needed:
        - ComputeScripthash correctness (compare against known-good hashes above)
        - DecodeNameScript for various script formats
        - IsNamecoinHost edge cases ("bit", ".bit", "example.bit", "EXAMPLE.BIT")
        - ParseNameValue with various JSON shapes
        - Mock ElectrumX WebSocket for integration tests (or use httpd.js)
      Can use xpcshell's built-in HTTP server to mock ElectrumX responses.

  [ ] Bugzilla meta-bug
      Component: Core :: Networking: DNS
      Title: "Support for Namecoin (.bit) decentralized domain resolution"
      Blocks: sub-bugs for DNS (Phase 1), TLS (Phase 2), UI (Phase 3), Android (Phase 4)
      CC: necko-reviewers, JeremyRand (Namecoin lead, has Bugzilla account)

MEDIUM PRIORITY (before Phabricator submission):

  [ ] Connection pooling
      Current: opens a new WebSocket for every request, closes after response.
      Better: maintain a persistent connection pool (like the JS extension does
      with each server). Reuse connections across multiple requests in a single
      resolve pipeline (history + tx + headers = 3 requests on same connection).
      Implementation: refcount the WebSocket, keep alive for cache_ttl_seconds,
      reconnect on error.

  [ ] Multi-server cross-validation
      Current: tries servers in order, returns first success.
      Better (when network.namecoin.query_multiple_servers = true): query 2+
      servers, compare tx_hash results. If they disagree, log a warning and
      use the majority result. This guards against malicious servers.

  [ ] Error page for .bit failures
      When a .bit domain fails to resolve, Firefox currently shows a generic
      "Server not found" page. Should show a Namecoin-specific error:
        - "This Namecoin name has expired" (with block info)
        - "ElectrumX servers unreachable" (with retry button)
        - "Name not found on the Namecoin blockchain"
      Location: netwerk/base/nsNetErrorCodes.h (new error codes) +
      docshell/resources/content/netError.xhtml (error page)

LOW PRIORITY (nice to have, can defer to Phase 2):

  [ ] SPV proof verification
      ElectrumX supports blockchain.transaction.get_merkle which returns
      merkle proofs. Verifying these against block headers ensures the
      server isn't fabricating transactions. Not critical for Phase 1
      since the about:config flag means only technical users will enable it.

  [ ] Persist resolver across browser restart
      The nsNamecoinResolver is currently created per nsHostResolver lifetime.
      For faster first-resolve, could persist the server health state and
      cached block height to disk (profile directory).


================================================================================
PHASE 2+ NOTES (future sessions)
================================================================================

Phase 2 (TLS/DANE-TLSA):
  - SPEC.md Section 5 has the full spec
  - Key integration point: security/manager/ssl/nsNSSCallbacks.cpp
  - AuthCertificateCallback is where cert validation decisions happen
  - Add DANE-TLSA as a fallback when standard CA validation fails for .bit
  - TLSA record format: [usage, selector, matchingType, data]
  - Usage=3 (DANE-EE) is the common case: server cert hash matches directly
  - Must also add .bit name constraints to block public CAs (Section 5.4)
  - Reference: ncp11 (PKCS#11 approach) works WITHOUT code patches per
    JeremyRand (Tor Browser issue #33568), but native DANE is cleaner

Phase 3 (UI):
  - Address bar: .bit → green lock for DANE-validated, yellow for HTTP-only
  - Certificate viewer: show "Namecoin Blockchain" as trust source
  - about:namecoin diagnostic page (Section 6.3)
  - Settings UI in Privacy & Security

Phase 4 (Android):
  - All C++ changes auto-apply to GeckoView — just needs testing
  - Battery-aware connection management (close WS when backgrounded)
  - Fenix Settings activity (Kotlin) needs Namecoin toggle

Phase 5 (Enablement):
  - about:config only (Nightly) → Settings toggle (Beta) → Default-off (Release)


================================================================================
WORKSPACE LAYOUT REFERENCE
================================================================================

Key files in this workspace:

  PLAN.md            — original three-tier integration plan
  SPEC.md           — Tier 3 native implementation spec (the big document)
  notes/session-01-phase1-skeleton.md           — THIS FILE (session handoff notes)
  gecko-namecoin/        — Phase 1 patch series (built this session)
  tls-namecoin-ext/      — existing browser extension (JS reference implementation)
  "TLS Namecoin Resolver/" — Safari/macOS extension wrapper (Xcode project)
  namecoin.txt           — full Namecoin GitHub organization catalog
  extension*.txt         — extension development notes (extension.txt through extension10.txt)
  howto-bit-website.txt  — guide for setting up a .bit website
  browser.txt            — browser integration research notes
  electrumx.txt          — ElectrumX protocol documentation

The JS extension in tls-namecoin-ext/ is the ground truth reference for
correctness — the C++ in gecko-namecoin/ mirrors its logic exactly:
  tls-namecoin-ext/background/electrumx-ws.js  → nsNamecoinResolver ElectrumX methods
  tls-namecoin-ext/background/namecoin-resolver.js → nsNamecoinResolver ParseNameValue
  tls-namecoin-ext/background/bit-navigator.js → nsHostResolver hook logic
  tls-namecoin-ext/background/dns-router.js → subdomain routing (map traversal)

Git status: everything committed as of this session.
  Last commit: "gecko-namecoin: Phase 1 patch series — nsNamecoinResolver..."


================================================================================
GOTCHAS AND THINGS I WISH I KNEW EARLIER
================================================================================

1. Gecko's nsIWebSocketChannel is async and callback-based. You can't just
   ws.send() after AsyncOpen() — the channel isn't ready yet. The current
   code has a timing issue here that needs fixing (see "Fix ElectrumXRequest
   send timing" above).

2. The verbose transaction JSON from ElectrumX can be HUGE (hundreds of KB
   for txs with many inputs/outputs). The simple string-search JSON parsing
   in Phase 1 works but is O(n) per field. A proper parser matters for
   performance at scale.

3. ElectrumX server availability is the single point of fragility. There's
   really only one reliable public WS server right now (testls.space:50003).
   nmc2.bitcoins.sk doesn't have WebSocket enabled. The production build
   should encourage users to run their own ElectrumX or at minimum have 3+
   servers with health checking.

4. The eTLD patch (effective_tld_names.dat) needs re-adding every time
   Mozilla updates the file from the upstream Public Suffix List. The
   NAMECOIN_INTEGRATION comment markers make grep-based detection possible
   in rebase scripts. This was the same maintenance burden Tor Browser hit
   (issue #33807).

5. Firefox's DNS resolution thread pool is limited (default 8 threads in
   nsHostResolver). A slow ElectrumX response (10s timeout) could tie up
   a pool thread. Phase 2 should use async resolution to avoid this.

6. The Monitor-based blocking in ElectrumXRequest is fine for the resolver
   thread pool but MUST NEVER run on the main thread. The code has
   MOZ_ASSERT(!NS_IsMainThread()) but this only fires in debug builds.

7. d/bitcoin's value is literally the string "reserved" — not JSON. Many
   Namecoin names have non-JSON or malformed values. The resolver must
   handle this without crashing or logging errors to the console (it's
   valid, just unresolvable).


================================================================================
END — PICK UP FROM HERE
================================================================================
