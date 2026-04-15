# Firefox Native Namecoin Integration — Implementation Spec
# ===========================================================
# Generated: 2026-04-16
# Goal: First-class .bit domain resolution and TLS in Firefox via Gecko patches.
#       User experience: type example.bit → page loads over HTTPS, no extensions,
#       no companion apps, no manual cert acceptance. Just works.
# Scope: Patches to Firefox (Gecko/GeckoView) source code.
# Platforms: Firefox Desktop (Linux, macOS, Windows), Firefox Android (GeckoView).
#            Firefox iOS is out of scope (uses WebKit, not Gecko).


================================================================================
SECTION 1: WHAT EXISTS TODAY (Context for Implementers)
================================================================================

1.1 Existing Browser Extension (tls-namecoin-ext v0.1.2)
---------------------------------------------------------
A working WebExtension in this workspace (tls-namecoin-ext/) that proves the
resolution approach works. It resolves .bit domains via ElectrumX WebSocket
directly from browser JavaScript. This is a reference implementation — the
native integration should replicate its resolution logic in C++ or Rust.

What it does:
  - Intercepts .bit navigations via webNavigation.onBeforeNavigate
  - Resolves d/ names via ElectrumX WebSocket (ws:// / wss://)
  - Computes scripthash for name lookup (OP_NAME_UPDATE index script)
  - Queries blockchain.scripthash.get_history + blockchain.transaction.get
  - Decodes NAME_UPDATE script to extract JSON value
  - Checks block height for name expiry (36,000 blocks ≈ 250 days)
  - Parses JSON value: ip, ip6, ns, tls, map, alias, translate fields
  - Redirects tab to the resolved IP address

What it CANNOT do (limitations that motivate native integration):
  - Automatic TLS validation (extensions cannot access cert chains)
  - HTTPS without cert errors (browser won't trust Namecoin-issued certs)
  - Native address bar integration (can't suppress search-engine redirect)
  - Modify Firefox's NSS trust store from within an extension

Key extension source files (reference implementations):
  background/electrumx-ws.js       — ElectrumX WebSocket client
  background/namecoin-resolver.js  — Name resolution with caching
  background/bit-navigator.js      — .bit navigation interception
  background/dns-router.js         — Domain routing layer

1.2 Namecoin's Existing TLS Tooling
-------------------------------------
The Namecoin project has mature Go-based tooling for TLS. These are external
to the browser and relevant as reference implementations:

  ncp11        — PKCS#11 module (Go → .so/.dylib/.dll)
                 Provides Namecoin TLS certs to NSS-based apps
                 Source: https://github.com/namecoin/ncp11

  encaya       — AIA server (Go daemon)
                 Generates X.509 certificates from DANE/TLSA records
                 Source: https://github.com/namecoin/encaya

  safetlsa     — Generates X.509 certs from TLSA records with name constraints
                 Source: https://github.com/namecoin/safetlsa

  tlsrestrictnss — Applies name constraints to NSS cert DB
                   Prevents public CAs from issuing .bit certs
                   Source: https://github.com/namecoin/tlsrestrictnss

  ncdns        — Namecoin-to-DNS bridge (Go daemon)
                 Source: https://github.com/namecoin/ncdns

  crosssignnameconstraint — X.509 name constraint tooling
                 Source: https://github.com/namecoin/crosssignnameconstraint

  dnssec-hsts  — DANE-based HTTPS upgrade extension
                 Source: https://github.com/namecoin/dnssec-hsts

For native Firefox integration, we reimplement the essential logic (name
resolution + DANE-TLSA validation) directly in Gecko, rather than depending
on these external daemons. They serve as reference for correctness.

1.3 Lessons from Tor Browser Integration (Issue #30558)
--------------------------------------------------------
Tor Browser shipped a Namecoin integration in Nightly (Linux only) circa 2019.
Key lessons from that effort:

Architecture that shipped:
  - StemNS (Python) monitored Tor controller
  - ncprop279 (Go) implemented Tor Proposition 279 "Pluggable Naming"
  - Electrum-NMC ran as a subprocess (routed over Tor)
  - Resolved d/ names → .onion addresses or IPs

Patches applied to Firefox/Tor Browser:
  - namecoin-etld.patch: added "bit.onion" to effective_tld_names.dat
    (netwerk/dns/effective_tld_names.dat) for proper eTLD handling
  - namecoin-torbutton.patch: UI integration showing .bit domain in address bar

Key findings from related Tor Browser issues:
  - Issue #33568: JeremyRand (Namecoin lead) confirmed ncp11's PKCS#11 approach
    works in Firefox WITHOUT code patches. Quote: "Firefox does not natively
    support DANE, but we have identified a way to get DANE-like functionality
    in Firefox with no code patches (using the PKCS11 FindObjects API). Some
    small code patches would make the code cleaner but are not required."
  - Issue #33752: Startup ordering matters — ElectrumX connections must handle
    failures gracefully with retry
  - Issue #33807: eTLD patches to effective_tld_names.dat require ongoing
    rebasing when the list changes upstream
  - Issue #41286: Out-of-tree patches create maintenance burden

Why their approach was too heavy for general distribution:
  - Linux Nightly-only, never reached stable release
  - Multiple external daemons (Python + Go + Python subprocess)
  - Startup ordering bugs between Tor and Electrum-NMC
  - eTLD patch required constant rebasing

Our approach avoids these problems by integrating directly into Gecko.


================================================================================
SECTION 2: ELECTRUMX PROTOCOL — THE RESOLUTION BACKEND
================================================================================

The core of .bit resolution is speaking the ElectrumX JSON-RPC protocol over
WebSocket to query the Namecoin blockchain. This section specifies exactly
what the native resolver must implement.

2.1 Transport & Protocol
--------------------------
  Transport: WebSocket (ws:// or wss://)
  Protocol: JSON-RPC 2.0 over WebSocket frames
  Message format: {"jsonrpc": "2.0", "id": <int>, "method": "<method>", "params": [...]}

2.2 Required ElectrumX Methods
--------------------------------
  1. server.version(client_name, protocol_version)
     → Handshake, negotiate protocol version
     → Call once on connection establishment

  2. blockchain.scripthash.get_history(scripthash)
     → Returns array of {tx_hash, height} for all transactions touching
       the scripthash (i.e., all NAME_UPDATE transactions for a name)
     → Use the most recent confirmed transaction (highest height)

  3. blockchain.transaction.get(tx_hash, verbose=true)
     → Returns full transaction including decoded outputs
     → Parse outputs to find the NAME_UPDATE operation

  4. blockchain.headers.subscribe()
     → Returns current block height
     → Needed to check name expiry (current_height - name_height > 36000?)

2.3 Scripthash Computation (Critical — Must Be Exact)
-------------------------------------------------------
To look up a name like "d/example", compute the ElectrumX scripthash:

  Step 1: Build the "index script" for the name
    The script represents what OP_NAME_UPDATE outputs look like:
      0x53                      — OP_NAME_UPDATE opcode
      + pushdata("d/example")   — name bytes with length prefix
      + pushdata("")            — empty value (index only uses name)
      + 0x6d                    — OP_2DROP
      + 0x75                    — OP_DROP
      + 0x6a                    — OP_RETURN

    Pushdata encoding:
      If len <= 75: single byte length prefix
      If len <= 255: 0x4c + single byte length
      If len <= 65535: 0x4d + two byte little-endian length

    For "d/example" (9 bytes):
      Script = [0x53, 0x09, 0x64, 0x2f, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c,
                0x65, 0x00, 0x6d, 0x75, 0x6a]
      (0x53 = OP_NAME_UPDATE, 0x09 = pushdata length 9, then "d/example" bytes,
       0x00 = pushdata length 0 for empty value, 0x6d 0x75 0x6a = OP_2DROP OP_DROP OP_RETURN)

  Step 2: SHA-256 hash the script bytes
  Step 3: Reverse the hash bytes → this is the ElectrumX scripthash

  CRITICAL: Must use OP_NAME_UPDATE (0x53), NOT OP_NAME_SHOW (0xd1).
  Getting this wrong returns empty history (silent failure — no error).
  Verified working against electrumx.testls.space for d/testls, d/bitcoin.

2.4 Transaction Decoding
--------------------------
  From the verbose transaction returned by blockchain.transaction.get:
  1. Iterate through vout (transaction outputs)
  2. Find the output whose scriptPubKey contains OP_NAME_UPDATE (0x53)
  3. Read the script:
     - Skip OP_NAME_UPDATE (0x53)
     - Read pushdata → name bytes (e.g., "d/example")
     - Read pushdata → value bytes (JSON string)
     - Remaining opcodes are the standard scriptPubKey (ignored for our purposes)
  4. Decode value bytes as UTF-8 → parse as JSON
  5. Check expiry: if (current_block_height - tx_block_height) > 36000 → name expired

2.5 ElectrumX Server Requirements
-----------------------------------
  - Must run the Namecoin fork of ElectrumX (github.com/namecoin/electrumx)
  - Must have WebSocket transport enabled
  - Must index Namecoin name scripts (default for NMC ElectrumX)

  Known public servers:
    - electrumx.testls.space:50003 (ws://) — confirmed working
    - nmc2.bitcoins.sk:57002 (TCP/SSL only, no WS currently)

  The implementation must support multiple servers for redundancy, with
  health checking and failover.


================================================================================
SECTION 3: NAME VALUE JSON SCHEMA (d/ namespace)
================================================================================

When a d/ name is resolved, the value is a JSON object. The resolver must
handle all of these fields:

  {
    "ip": "1.2.3.4",                    // A record — IPv4 address
    "ip6": "2001:db8::1",               // AAAA record — IPv6 address
    "ns": ["ns1.example.com"],           // NS delegation — use standard DNS
    "tls": [                             // TLSA records for TLS validation
      [2, 1, 0, "base64-pubkey"],        // [usage, selector, matchType, data]
      [3, 1, 1, "sha256hex"]
    ],
    "map": {                             // Subdomain map
      "www": {"ip": "5.6.7.8"},
      "*": {"ip": "1.2.3.4"},           // Wildcard
      "_tcp": {
        "map": {
          "_443": {
            "tls": [[3, 1, 1, "sha256hex"]]  // Port-specific TLSA
          }
        }
      }
    },
    "alias": "other.bit",               // CNAME-like — follow chain (max 5 hops)
    "translate": "example.com",          // Map to standard domain
    "info": {"description": "My site"},  // Informational metadata
    "email": "admin@example.bit",        // Contact
    "tor": "abcdef.onion"               // Tor hidden service
  }

3.1 Resolution Logic
----------------------
  Given a query for "www.example.bit":
  1. Strip .bit → "example"
  2. Look up "d/example" via ElectrumX
  3. Parse JSON value
  4. Check for subdomain "www" in value.map:
     - If value.map.www exists → use its ip/ip6/tls fields
     - If value.map.* exists → use wildcard
     - If neither → use top-level ip/ip6
  5. If value.alias exists → follow alias (recursive, loop detection, max 5)
  6. If value.translate exists → resolve via standard DNS instead
  7. If value.ns exists → delegate to those nameservers for the subdomain

3.2 TLSA Record Format
------------------------
  Each TLSA entry is a tuple: [usage, selector, matchingType, data]

  Usage:
    2 = DANE-TA (Trust Anchor — CA cert in chain must match)
    3 = DANE-EE (End Entity — server cert must match directly)

  Selector:
    0 = Full certificate
    1 = SubjectPublicKeyInfo only

  Matching Type:
    0 = Exact match (full cert/SPKI bytes)
    1 = SHA-256 hash
    2 = SHA-512 hash

  Data: hex-encoded or base64-encoded certificate/hash data

  Port-specific TLSA records are in the subdomain map under _tcp._<port>.


================================================================================
SECTION 4: GECKO DNS SUBSYSTEM PATCH
================================================================================

This is the first major component: teaching Firefox's DNS resolver to handle
.bit domains by routing them to the Namecoin/ElectrumX resolver.

4.1 Target Files
-----------------
  netwerk/dns/nsDNSService.cpp     — Main DNS service
  netwerk/dns/nsHostResolver.cpp   — Async host resolution
  netwerk/dns/effective_tld_names.dat — eTLD list (add "bit" entry)

4.2 New Component: nsNamecoinResolver
--------------------------------------
  Language: C++ or Rust via FFI (Rust preferred for new Gecko code)
  Location: netwerk/dns/nsNamecoinResolver.{cpp,h} (or Rust module)

  Responsibilities:
  a. Detect .bit TLD in hostname queries
  b. Connect to configured ElectrumX servers via WebSocket
  c. Compute scripthash for the d/ name (Section 2.3)
  d. Query ElectrumX for name history + transaction data (Section 2.2)
  e. Decode NAME_UPDATE value from transaction (Section 2.4)
  f. Handle subdomain resolution via map traversal (Section 3.1)
  g. Handle alias chains, NS delegation, translate directives
  h. Return A/AAAA records to the standard resolution pipeline
  i. Cache results in Firefox's DNS cache with appropriate TTL
     (TTL should reflect name expiry: remaining blocks × ~10 min/block,
      but capped to something reasonable like 1 hour for responsiveness)

  Integration with nsHostResolver:
  - nsHostResolver::ResolveHost checks the TLD
  - If TLD is "bit" AND network.namecoin.enabled is true:
    → Route to nsNamecoinResolver instead of system/DoH resolver
    → nsNamecoinResolver returns nsHostRecord with A/AAAA data
  - Otherwise: proceed with normal DNS resolution

4.3 eTLD List Update
----------------------
  Add to netwerk/dns/effective_tld_names.dat:
    bit
  
  This ensures Firefox treats .bit as a proper eTLD, preventing cookie/origin
  confusion across different .bit domains. (Same approach Tor Browser used
  with their namecoin-etld.patch.)

  NOTE: This requires ongoing maintenance when the eTLD list is updated
  upstream (PSL updates). The entry should be added with a clear comment
  referencing the Namecoin integration feature flag.

4.4 Configuration (about:config)
----------------------------------
  network.namecoin.enabled                — boolean, default false
  network.namecoin.electrumx_servers      — string, comma-separated wss:// URLs
  network.namecoin.cache_ttl_seconds      — integer, default 3600
  network.namecoin.require_tls            — boolean, default true
                                            (refuse plaintext .bit if TLSA exists)
  network.namecoin.max_alias_hops         — integer, default 5
  network.namecoin.connection_timeout_ms  — integer, default 10000
  network.namecoin.query_multiple_servers — boolean, default true
                                            (query 2+ servers, compare results)

4.5 Connection Management
---------------------------
  - Maintain a persistent WebSocket connection pool to configured servers
  - Health-check on startup; reconnect with exponential backoff on failure
  - If all servers are unreachable, fail the DNS query (do not fall through
    to standard DNS — .bit is not a real TLD and standard DNS will NXDOMAIN)
  - Lesson from Tor Browser (#33752): handle connection failures gracefully;
    don't assume servers are available immediately at startup


================================================================================
SECTION 5: TLS/NSS INTEGRATION — DANE-TLSA VALIDATION
================================================================================

This is the second major component: making Firefox trust Namecoin-issued TLS
certificates by validating them against DANE/TLSA records from the blockchain.

5.1 Approach: Direct DANE-TLSA Validation in NSS
--------------------------------------------------
Rather than loading external PKCS#11 modules (ncp11), the native integration
should add DANE-TLSA validation directly into Firefox's TLS handshake path.
This is cleaner, has no external dependencies, and benefits all DANE-secured
domains (not just Namecoin).

During TLS handshake for a .bit domain:
  1. Firefox connects to the resolved IP
  2. Server presents its certificate
  3. Normal CA validation runs and may fail (expected — .bit certs are self-signed
     or issued by non-standard CAs)
  4. If the domain is .bit AND the name value has TLSA records:
     → Validate the server certificate against the TLSA records
     → If TLSA validation passes → accept the certificate
     → If TLSA validation fails → reject (hard fail, not soft)
  5. Show appropriate security indicator in the UI

5.2 DANE-TLSA Validation Logic
---------------------------------
  Given a TLSA record [usage, selector, matchingType, data] and the server cert:

  Step 1: Extract the relevant cert data based on selector
    selector=0 → full DER-encoded certificate
    selector=1 → DER-encoded SubjectPublicKeyInfo from the certificate

  Step 2: Compute match based on matchingType
    matchingType=0 → compare raw bytes (data == extracted)
    matchingType=1 → compare SHA-256(extracted) against data
    matchingType=2 → compare SHA-512(extracted) against data

  Step 3: Apply usage semantics
    usage=2 (DANE-TA) → the TLSA data must match a CA certificate in the
                         chain (not necessarily the end-entity cert)
    usage=3 (DANE-EE) → the TLSA data must match the end-entity (server) cert
                         directly; chain validation is bypassed entirely

  For usage=3 (DANE-EE), if the hash matches, the cert is trusted regardless
  of issuer. This is the most common usage for Namecoin .bit sites.

5.3 NSS Integration Points
----------------------------
  Firefox uses NSS (Network Security Services) for all TLS operations.

  Target area: security/manager/ssl/
    - nsNSSCallbacks.cpp — TLS handshake callbacks
    - AuthCertificateCallback — where cert validation decisions are made

  The DANE-TLSA check should be added as a fallback in the cert verification
  path: if standard CA validation fails for a .bit domain, invoke the DANE
  validator before returning an error.

  Alternatively, implement as a custom NSS trust domain or override that
  participates in the CERT_VerifyCert / CERT_PKIXVerifyCert pipeline.

5.4 Name Constraints (Preventing .bit Cert Abuse)
---------------------------------------------------
  Public CAs must NOT be able to issue trusted certificates for .bit domains.
  Without this, a compromised or malicious CA could issue a .bit cert that
  Firefox would trust via the normal CA path, bypassing blockchain validation.

  Implementation options:
  a. Add .bit to Firefox's built-in name constraint exclusion list
     → Similar to how .onion was handled for Tor
     → Reject any CA-issued cert where SAN includes *.bit or *.bit.*
  b. Apply name constraints at the NSS level during profile creation
     → Similar to what tlsrestrictnss does externally
  c. Check during cert validation: if domain is .bit and cert chains to a
     public CA (not DANE-validated), reject it

  Option (a) is cleanest for native integration.

5.5 Certificate Caching
-------------------------
  DANE-TLSA validation involves blockchain queries which may be slow.
  Cache validated cert→TLSA associations:
  - Key: (domain, cert fingerprint, TLSA record hash)
  - TTL: same as DNS cache TTL for the name
  - Invalidate when name value changes or expires


================================================================================
SECTION 6: UI INTEGRATION
================================================================================

6.1 Address Bar
----------------
  - .bit domains resolve directly — no search-engine interception
  - User types "example.bit" → resolves via Namecoin, loads page
  - Show security indicator in address bar:
    → Green/teal lock icon: valid Namecoin TLS (DANE-TLSA validated)
    → Yellow/warning icon: HTTP-only .bit (no TLS, or TLSA missing)
    → Red/error icon: TLSA validation failed (cert doesn't match blockchain)

6.2 Certificate Viewer
------------------------
  When viewing certificate details for a .bit site:
  - Show "Namecoin Blockchain" or "DANE-TLSA (Namecoin)" as trust source
  - Display: block height of name registration, transaction hash, expiry block
  - Link to a block explorer (e.g., namecha.in) for independent verification
  - Show the TLSA record details: usage, selector, matching type

6.3 about:namecoin Diagnostic Page
------------------------------------
  A special about: page for troubleshooting:
  - ElectrumX server connectivity status (connected/disconnected per server)
  - Current blockchain height (confirms server is synced)
  - Recent resolution log (last N lookups with timing)
  - TLS certificate cache contents
  - Name lookup tool (type a .bit name, see resolution result)

6.4 Settings UI
-----------------
  In Firefox Settings → Privacy & Security (or Network Settings):
  - Toggle: "Enable Namecoin (.bit) domain resolution"
  - ElectrumX server configuration (list of servers, add/remove)
  - "Require HTTPS for .bit domains when TLSA records exist" toggle
  - Link to about:namecoin for diagnostics


================================================================================
SECTION 7: GECKOVIEW / FIREFOX ANDROID
================================================================================

Firefox Android (Fenix) uses GeckoView as its rendering engine. All Gecko C++
changes from Sections 4-6 automatically apply to GeckoView with no additional
porting. This is the key advantage of native integration.

7.1 What Works Automatically
------------------------------
  - DNS resolution (nsNamecoinResolver) — same C++ code
  - TLS/DANE validation — same NSS integration
  - eTLD handling — same effective_tld_names.dat
  - Certificate caching — same cache implementation

7.2 Android-Specific Work Required
-------------------------------------
  a. ElectrumX WebSocket connectivity through Android's network stack
     → Gecko's WebSocket implementation works on Android, but test connectivity
       on mobile networks (some carriers block non-standard WebSocket ports)
     → Ensure wss:// (TLS WebSocket) works through Android's network security
  
  b. Battery considerations
     → The persistent WebSocket connection pool must be power-aware
     → Close connections when the app is backgrounded
     → Reconnect when foregrounded or when a .bit navigation occurs
     → Do NOT maintain always-on connections — resolve on demand
  
  c. Settings UI
     → Fenix has its own Settings activity (Kotlin), separate from desktop
     → Add Namecoin toggle to Fenix Settings → Privacy & Security
     → Server configuration UI in Fenix settings
  
  d. GeckoView API surface
     → Expose namecoin.enabled preference via GeckoView runtime settings
     → Allow embedding apps (not just Fenix) to enable/disable Namecoin
  
  e. about:namecoin
     → Ensure the diagnostic page renders correctly on mobile viewports


================================================================================
SECTION 8: SECURITY CONSIDERATIONS
================================================================================

8.1 Trust Model
----------------
  - Namecoin names are secured by proof-of-work (merged-mined with Bitcoin)
  - TLSA records in name values provide certificate pinning via DANE
  - Name expiry (36,000 blocks ≈ 250 days) prevents indefinitely stale records
  - SPV verification via ElectrumX provides reasonable security
    (not as strong as a full node, but practical for browser integration)

8.2 ElectrumX Server Trust
-----------------------------
  A malicious ElectrumX server could return false name data.

  Mitigations:
  - Query multiple servers and compare results (cross-validation)
  - SPV proof verification: request merkle proofs for transactions and verify
    against block headers (ElectrumX supports blockchain.transaction.get_merkle)
  - Allow users to configure their own trusted server (self-hosted)
  - Display warnings if servers disagree on resolution results

8.3 Privacy
-------------
  - ElectrumX servers see which .bit names the user looks up
  - Mitigations:
    → Query multiple servers (each sees only some queries)
    → Support Tor integration (ElectrumX over Tor)
    → Cache aggressively to reduce query frequency
    → No analytics, telemetry, or phone-home behavior
  - All resolution is direct browser-to-ElectrumX via WebSocket

8.4 Name Constraints (Anti-Spoofing)
--------------------------------------
  - Public CAs must be blocked from issuing .bit certificates (Section 5.4)
  - Without this, a CA could issue a .bit cert that bypasses DANE validation
  - Implementation: reject any cert for a .bit domain that chains to a
    public CA root in the NSS trust store

8.5 Expired Name Handling
---------------------------
  - If a name has expired (current_height - registration_height > 36000):
    → Do NOT resolve it
    → Show a clear error: "This Namecoin name has expired"
    → An expired name could be re-registered by anyone — resolving it is unsafe


================================================================================
SECTION 9: UPSTREAM STRATEGY
================================================================================

Getting patches merged into Firefox requires Mozilla buy-in and careful
phasing to minimize risk and review burden.

9.1 Bugzilla
--------------
  File a meta-bug on bugzilla.mozilla.org:
    Component: Core :: Networking: DNS
    Title: "Support for Namecoin (.bit) decentralized domain resolution"
    Depends on: sub-bugs for DNS, TLS, UI, eTLD, Android

9.2 Phased Approach
---------------------
  Phase 1: Proof of concept
    - Build against Firefox Nightly source (mozilla-central)
    - DNS resolution only (no TLS yet) — .bit → HTTP
    - Behind about:config flag (network.namecoin.enabled = false by default)
    - Submit as patches on Phabricator for review/discussion

  Phase 2: TLS integration
    - Add DANE-TLSA validation (Section 5)
    - Add name constraints (Section 5.4)
    - Still behind about:config flag

  Phase 3: UI integration
    - Address bar indicators (Section 6.1)
    - Certificate viewer changes (Section 6.2)
    - about:namecoin page (Section 6.3)
    - Settings UI (Section 6.4)

  Phase 4: Android
    - Verify GeckoView integration
    - Add Fenix settings UI
    - Test on real devices and mobile networks

  Phase 5: Enablement
    - about:config only (Nightly) → evaluate stability
    - Settings toggle (Beta) → broader testing
    - Default-off with discovery UI (Release) → available to all users
    - Default-on after sufficient adoption/confidence

9.3 Mozilla Engagement
------------------------
  - Mozilla Builders / Innovation Fund — pitch as "censorship-resistant DNS"
    (aligns with Mozilla's mission: "an internet that is a global public
    resource, open and accessible to all")
  - Engage with Mozilla's networking and security teams early
  - Build custom Firefox Nightly binaries for Namecoin community testing
  - Gather stability/performance metrics to support enablement decisions

9.4 DANE as a Broader Proposal
---------------------------------
  The TLS/DANE-TLSA work (Section 5) could be pitched independently of
  Namecoin — DANE support benefits the entire web (RFC 6698/7671). Mozilla
  may be more receptive to "add DANE support to Firefox" than "add Namecoin
  support to Firefox," and Namecoin resolution becomes a natural consumer
  of the DANE infrastructure once it exists.


================================================================================
SECTION 10: IMPLEMENTATION CHECKLIST
================================================================================

DNS Resolution:
  [ ] Add "bit" to effective_tld_names.dat
  [ ] Implement nsNamecoinResolver (ElectrumX WebSocket client in C++/Rust)
  [ ] Implement scripthash computation (Section 2.3 — exact byte layout)
  [ ] Implement ElectrumX JSON-RPC: server.version, scripthash.get_history,
      transaction.get, headers.subscribe
  [ ] Implement NAME_UPDATE transaction decoding (Section 2.4)
  [ ] Implement name value JSON parsing (Section 3)
  [ ] Implement subdomain resolution via map traversal (Section 3.1)
  [ ] Implement alias chain following (max 5 hops, loop detection)
  [ ] Implement NS delegation (fall through to standard DNS for subdomains)
  [ ] Implement translate directive (resolve via standard DNS)
  [ ] Implement name expiry checking (36,000 block threshold)
  [ ] Integrate with nsHostResolver to route .bit queries
  [ ] Implement WebSocket connection pool with health checking + failover
  [ ] Implement DNS result caching with appropriate TTL
  [ ] Add about:config preferences (Section 4.4)

TLS/DANE Validation:
  [ ] Implement DANE-TLSA validator (Section 5.2 — usage, selector, matchType)
  [ ] Integrate with NSS cert verification path (AuthCertificateCallback)
  [ ] Add .bit name constraints to block public CA certs (Section 5.4)
  [ ] Implement DANE cert caching (Section 5.5)
  [ ] Handle HTTPS upgrade when TLSA records exist

UI:
  [ ] Address bar security indicators for .bit domains (Section 6.1)
  [ ] Certificate viewer: show Namecoin/DANE trust source (Section 6.2)
  [ ] Implement about:namecoin diagnostic page (Section 6.3)
  [ ] Add settings UI for desktop (Section 6.4)
  [ ] Add settings UI for Firefox Android / Fenix (Section 7.2c)

Android:
  [ ] Verify GeckoView integration compiles and works
  [ ] Test WebSocket connectivity on mobile networks
  [ ] Implement battery-aware connection management (Section 7.2b)
  [ ] Expose GeckoView runtime settings API (Section 7.2d)
  [ ] Test about:namecoin on mobile viewports


================================================================================
SECTION 11: FILE REFERENCE
================================================================================

Workspace files:
  PLAN.md            — original three-tier integration plan (background)
  SPEC.md           — this document (Tier 3 native implementation spec)
  extension10.txt        — extension packaging and AMO submission notes
  namecoin.txt           — full Namecoin GitHub organization catalog (63 repos)
  tls-namecoin-ext/      — existing browser extension source (reference impl)

Extension reference implementation (key files):
  tls-namecoin-ext/background/electrumx-ws.js       — ElectrumX WebSocket client
  tls-namecoin-ext/background/namecoin-resolver.js   — Name resolution + caching
  tls-namecoin-ext/background/bit-navigator.js       — .bit navigation interception
  tls-namecoin-ext/background/dns-router.js          — Domain routing layer
  tls-namecoin-ext/manifest.firefox.json             — Firefox extension manifest

Namecoin project repos (reference for correctness):
  https://github.com/namecoin/ncp11                  — PKCS#11 module (Go)
  https://github.com/namecoin/encaya                 — AIA cert server (Go)
  https://github.com/namecoin/safetlsa               — TLSA→X.509 generation
  https://github.com/namecoin/tlsrestrictnss         — NSS name constraints
  https://github.com/namecoin/ncdns                  — DNS bridge
  https://github.com/namecoin/electrum-nmc           — SPV wallet/query client
  https://github.com/namecoin/electrumx              — ElectrumX server (NMC fork)
  https://github.com/namecoin/crosssignnameconstraint — X.509 name constraints
  https://github.com/namecoin/dnssec-hsts            — DANE-based HTTPS upgrade
  https://github.com/namecoin/ncprop279              — Tor Prop279 bridge
  https://github.com/namecoin/pkcs11mod              — Go PKCS#11 framework

Tor Browser integration issues (reference):
  https://gitlab.torproject.org/tpo/applications/tor-browser/-/issues/30558
  https://gitlab.torproject.org/tpo/applications/tor-browser/-/issues/33568
  https://gitlab.torproject.org/tpo/applications/tor-browser/-/issues/33752
  https://gitlab.torproject.org/tpo/applications/tor-browser/-/issues/33807
  https://gitlab.torproject.org/tpo/applications/tor-browser/-/issues/41286


================================================================================
END OF SPEC
================================================================================
