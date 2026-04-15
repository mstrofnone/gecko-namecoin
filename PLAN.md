# Firefox Namecoin Integration Plan — d/ Namespace Resolution with TLS
# =====================================================================
# Generated: 2026-04-16
# Goal: Native Namecoin d/ namespace resolution in Firefox (desktop + Android/mobile)
#       with proper TLS support for .bit websites, using electrum-nmc as the
#       blockchain query backend. No wallet support — query-only.
#
# Reference sources:
#   - Tor Browser Namecoin issue: https://gitlab.torproject.org/tpo/applications/tor-browser/-/issues/30558
#   - noStrudel Namecoin PR: https://github.com/hzrd149/nostrudel/pull/352
#   - Amethyst Nostr client: https://github.com/vitorpamplona/amethyst
#   - electrum-nmc: https://github.com/namecoin/electrum-nmc
#   - Existing extension: tls-namecoin-ext (v0.1.2) in this workspace
#   - Namecoin repos: namecoin.txt in this workspace
#   - Prior session notes: extension10.txt in this workspace


================================================================================
SECTION 1: CURRENT STATE — WHAT EXISTS TODAY
================================================================================

1.1 Existing Browser Extension (tls-namecoin-ext v0.1.2)
---------------------------------------------------------
What it does today:
  - Intercepts .bit domain navigations via webNavigation.onBeforeNavigate
  - Resolves d/ names via ElectrumX WebSocket (ws:// / wss://) directly from
    the browser — no proxy or backend required
  - Falls back to local namecoind JSON-RPC if configured
  - Redirects tab to the resolved IP address (http://<ip>/<path>)
  - Shows an info banner on the redirected page with resolution details
  - Displays TLSA fingerprint data from Namecoin records for manual verification
  - Has options page for configuring RPC and ElectrumX servers
  - Works on Firefox Desktop and Firefox Android (Fenix)
  - Packaged and ready for AMO submission

Resolution flow:
  1. User navigates to example.bit
  2. webNavigation.onBeforeNavigate fires
  3. Extension calls resolveNamecoinWithCache("example.bit")
  4. Resolver strips .bit → looks up "d/example" via ElectrumX WS
  5. ElectrumX resolution:
     a. Compute scripthash: SHA-256 of OP_NAME_UPDATE index script (0x53 opcode)
     b. Query blockchain.scripthash.get_history
     c. Fetch verbose transaction for most recent confirmed tx
     d. Decode NAME_UPDATE script to extract JSON value
     e. Check block height for name expiry (36,000 blocks ≈ 250 days)
  6. Parse JSON: extract ip, ip6, ns, tls fields
  7. Redirect tab to http://<ip>/<original-path>
  8. Content script shows banner with domain, IP, TLSA fingerprints

What it CANNOT do:
  - Automatic TLS validation (extensions have no access to cert chain)
  - HTTPS with Namecoin certificates (browser will show cert error for IP)
  - Native address bar integration (can't suppress search-engine redirect)
  - Modify Firefox's NSS trust store from within an extension

1.2 Namecoin's TLS Ecosystem
-----------------------------
The Namecoin project has mature tooling for TLS that operates OUTSIDE extensions:

  ncp11        — PKCS#11 module (Go, compiled to .so/.dylib/.dll)
                 Provides Namecoin TLS certs to NSS-based apps (Firefox)
                 Requires Encaya running as backend

  encaya       — AIA (Authority Information Access) server (Go daemon)
                 Generates X.509 certificates from Namecoin DANE/TLSA records
                 Needs ncdns for DNS resolution

  ncdns        — Namecoin-to-DNS bridge (Go daemon)
                 Translates blockchain names to DNS responses
                 Needs namecoin-core full node or electrum-nmc

  tlsrestrictnss — Applies name constraints to NSS cert DB
                   Prevents public CAs from issuing .bit certs
                   Run once per Firefox profile

  safetlsa     — Generates X.509 certs from TLSA records with name constraints
                 Ensures certs can't be used for non-.bit domains

  certinject   — Windows CryptoAPI cert injection with EKU constraints
                 (Windows-only, not relevant for Firefox/NSS)

Full TLS chain (desktop):
  electrum-nmc → ncdns → encaya → ncp11 → Firefox NSS → .bit HTTPS works

1.3 How Tor Browser Approaches This (Issue #30558 + Related Issues)
-------------------------------------------------------------------
  Issue #30558 was filed by Arthur Edelstein (arthuredelstein) in May 2019,
  assigned to JeremyRand (Namecoin lead dev), and closed Dec 2019 after the
  initial integration shipped in Tor Browser Nightly (Linux). It had 65 comments
  of discussion. The issue framed it as solving Zooko's Triangle: .bit domains
  are human-meaningful + secure + decentralized, unlike .onion (secure +
  decentralized but not human-meaningful).

  NOTE: The 65 comments on #30558 could not be retrieved — GitLab's API
  requires authentication for issue notes on this project, and the JS-rendered
  comments don't appear in the server-side HTML. The issue description and
  metadata were reviewed via the API, and all related Namecoin issues in the
  Tor Browser project were examined.

  Architecture that shipped in Tor Browser Nightly:
  - StemNS (Python, uses Stem library) monitors Tor controller
  - ncprop279 (Go) implements Tor Proposition 279 "Pluggable Naming"
  - Electrum-NMC runs as a subprocess (routed over Tor)
  - When user navigates to example.bit:
    → StemNS intercepts the stream via __LeaveStreamsUnattached
    → ncprop279 resolves d/example via Electrum-NMC
    → If name has a tor/onion field, redirects to .onion
    → If name has IP, resolves to IP (exit traffic)
  - .bit.onion semantics: always resolve to .onion (encrypted/authenticated)
  - .bit semantics: resolve to onion, IPv6, IPv4, or CNAME

  Patches applied to Firefox/Tor Browser:
  - namecoin-etld.patch: adds "bit.onion" to effective_tld_names.dat
    (netwerk/dns/effective_tld_names.dat) so Firefox treats .bit.onion
    as a proper eTLD, preventing cookie/origin confusion
  - namecoin-torbutton.patch: UI integration in Torbutton, showing .bit
    domain in address bar while routing to .onion underneath

  Related issues that reveal important details:
  
  #33568 — "Namecoin for TLS certificate validation" (JeremyRand, 2020)
    Key quote: "Firefox does not natively support DANE, but we (the Namecoin
    devs) have identified a way to get DANE-like functionality in Firefox with
    no code patches to Firefox (we're using the PKCS11 FindObjects API to
    achieve this). Some small code patches to Firefox would make the code
    cleaner, but this wouldn't be required."
    → This confirms ncp11's PKCS#11 approach works WITHOUT Firefox patches
    → Also notes this approach could work for .onion TLD too (TLSA records
      in onion service descriptors, independent of Namecoin)
    → Closed 2022 by 'morgan' (no further comment data available)
  
  #33752 — "Electrum-NMC tries to connect before Tor has connected"
    → Electrum-NMC launched in parallel with Firefox, made connections before
      Tor was ready, inferred servers unreachable, caused resolution delay
    → Fix: StemNS monitors Tor connection status, launches Electrum-NMC
      only after Tor connects
    → Lesson for us: startup ordering matters; our extension should handle
      ElectrumX connection failures gracefully with retry
  
  #33807 — "Namecoin eTLD patch conflicted with securedrop.tor.onion"
    → The eTLD patch for .bit needed rebasing when effective_tld_names.dat
      changed upstream (securedrop.tor.onion was added nearby)
    → Lesson: patches to Firefox's eTLD list require ongoing maintenance
  
  #33749 — "Stem is Outdated in Tor Browser's Namecoin Support"
    → Dependency version management was an ongoing maintenance burden
  
  #41286 — "Merge namecoin-torbutton.patch to torbutton.git"
    → Patch was maintained out-of-tree in tor-browser-build; Arthur suggested
      merging into torbutton.git for easier maintenance
    → Lesson: out-of-tree patches create maintenance burden; prefer
      extension-based approaches or upstreamed changes

  Key takeaways from the Tor Browser experience:
  1. The ncp11/PKCS#11 approach for TLS works in Firefox WITHOUT patches
  2. Running Electrum-NMC as a subprocess is viable but has startup ordering
     issues and maintenance burden
  3. eTLD patches need ongoing rebasing — extension approach avoids this
  4. The integration was Linux Nightly-only, never reached stable release,
     suggesting the approach was too heavy for general distribution
  5. For our Firefox integration: the pure extension path (Tier 1) avoids
     all the patch maintenance issues; the ncp11 PKCS#11 path (Tier 2) is
     confirmed viable by JeremyRand himself for TLS without patches

1.4 How Web Apps Do It (noStrudel PR #352, Amethyst)
----------------------------------------------------
  - noStrudel: Direct WebSocket to ElectrumX from browser JS
    → Compute scripthash, query history, decode NAME_UPDATE output
    → No proxy needed; works as static web app
    → This is exactly what our extension already does (same approach)
  - Amethyst: Android Nostr client, reads Nostr pubkeys from Namecoin name values
    → Uses same JSON value format for identity resolution
    → Demonstrates the d/ + id/ namespace conventions are stable


================================================================================
SECTION 2: THE TLS PROBLEM — WHY EXTENSIONS ALONE CAN'T DO IT
================================================================================

2.1 The Core Limitation
-----------------------
Browser extensions (WebExtensions API, MV3) CANNOT:
  - Access raw TLS certificate chains
  - Inject certificates into the browser's trust store
  - Register PKCS#11 modules
  - Override TLS validation decisions
  - Intercept connections at the TCP/TLS layer

The extension can resolve .bit → IP and redirect, but when the browser connects
to the IP over HTTPS, it will see a certificate for an IP address (or a
Namecoin-issued cert for example.bit) that it doesn't trust.

2.2 What's Needed for Real TLS
-------------------------------
For HTTPS to work on .bit domains, the browser needs:
  1. DNS resolution: .bit → IP address (extension can handle this)
  2. TLS certificate presented by the server (server-side, already works)
  3. Certificate validation: browser must trust the Namecoin-issued cert
  4. Name matching: cert must match the domain being visited
  5. CA restriction: public CAs must be prevented from issuing .bit certs

Steps 3-5 require changes BELOW the extension layer — either:
  a. Native PKCS#11 module (ncp11) loaded into Firefox's NSS, OR
  b. Modifications to Firefox's network/security stack, OR
  c. A hybrid approach with a native messaging host


================================================================================
SECTION 3: INTEGRATION PLAN — THREE TIERS
================================================================================

The plan has three tiers, from most achievable to most ambitious. Each tier
builds on the previous one.

                    ┌──────────────────────────────────────┐
                    │  TIER 3: Native Firefox Integration   │
                    │  (C++ patches to Gecko network stack) │
                    ├──────────────────────────────────────┤
                    │  TIER 2: Native Messaging + ncp11     │
                    │  (Extension + companion app/daemon)   │
                    ├──────────────────────────────────────┤
                    │  TIER 1: Pure Extension (current)     │
                    │  (WebExtensions API only — no native) │
                    └──────────────────────────────────────┘


================================================================================
TIER 1: PURE EXTENSION — ENHANCED (Desktop + Android)
================================================================================
Timeline: 2-4 weeks
Scope: Maximize what's possible with WebExtensions alone
Platforms: Firefox Desktop 115+, Firefox Android (Fenix) 115+

1.1 Domain Resolution Improvements
-----------------------------------
  ✅ Already working: ElectrumX WebSocket resolution of d/ names
  
  TO DO:
  a. Multiple ElectrumX server rotation with health checking
     - Currently single server (electrumx.testls.space:50003)
     - Add 3+ servers, health-check on startup, round-robin on failure
     - Servers to add: nmc2.bitcoins.sk (if WS enabled), self-hosted option
     
  b. Subdomain resolution from name value "map" field
     - Namecoin names can have: {"map": {"www": {"ip": "1.2.3.4"}}}
     - Current code only reads top-level ip/ip6
     - Add traversal: www.example.bit → d/example → value.map.www.ip
     
  c. NS delegation support
     - If a name has {"ns": ["ns1.example.com"]}, perform standard DNS
       query against those nameservers for the subdomain
     - Allows traditional DNS hosting for .bit subdomains
     
  d. CNAME/redirect support
     - Names can have: {"alias": "other.bit"} or {"translate": "example.com"}
     - Follow alias chains (with loop detection, max 5 hops)

  e. Address bar integration (Firefox-specific)
     - Use browser.urlbar.contextualResults API (Firefox 115+, experimental)
       to show .bit resolution suggestions in the address bar
     - Alternatively: detect search-engine navigation for ".bit" queries
       via webNavigation and intercept before the search happens
     - Firefox Android: search interception is the only viable path

1.2 TLSA Display & Manual Verification (Enhanced)
--------------------------------------------------
  ✅ Already working: TLSA fingerprints shown in info banner
  
  TO DO:
  a. Structured TLSA display in popup
     - Parse TLSA tuple [usage, selector, matchingType, data]
     - Show human-readable descriptions:
       usage=2 → "Domain-issued certificate (DANE-TA)"
       usage=3 → "Domain-issued end-entity cert (DANE-EE)"
       selector=1 → "SubjectPublicKeyInfo"
       matchingType=0 → "Full certificate" / 1 → "SHA-256" / 2 → "SHA-512"
     
  b. Certificate fingerprint comparison (DANE-EE, usage=3)
     - When user is on https://<ip>, fetch the server's cert fingerprint
       via a TLS probe (fetch HEAD with try/catch)
     - Compare against TLSA record data
     - Show ✅ match / ❌ mismatch in popup/banner
     - NOTE: This is informational only — can't prevent the connection
     
  c. Warning system
     - If TLSA mismatch detected, show prominent red warning
     - If name expired, show warning (already partially implemented)
     - If connecting over plain HTTP when TLSA exists, suggest HTTPS

1.3 HTTPS Upgrade Strategy (Extension-Only)
--------------------------------------------
  Problem: Redirecting to http://<ip> loses encryption. Redirecting to
  https://<ip> triggers cert error because cert is for example.bit.
  
  Approach for Tier 1 (best-effort without native support):
  
  a. If TLSA record exists → redirect to https://<ip>
     - Browser WILL show cert error (expected)
     - User must click "Accept Risk and Continue" (Firefox)
     - After accepting, the connection IS encrypted and the cert IS the
       Namecoin-issued one — just not automatically trusted
     - Extension shows banner confirming TLSA fingerprint match
     
  b. Implement "trust on first use" (TOFU) UX
     - After user accepts cert once, store the fingerprint in extension storage
     - On subsequent visits, if fingerprint still matches TLSA, auto-show ✅
     - If fingerprint changes, show prominent warning
     
  c. DNSSEC-HSTS heuristic
     - If name has TLSA records, always redirect to HTTPS (usage=3)
     - Same approach as Namecoin's dnssec-hsts extension

1.4 Firefox Android (Fenix) Specific
-------------------------------------
  ✅ Already compatible: webRequest, webNavigation, tabs, storage APIs
  ✅ Already fixed: data: URL error page replaced with extension page
  
  Remaining work:
  a. Test popup UI on narrow screens — may need responsive layout
  b. Test ElectrumX WebSocket connectivity on mobile networks
  c. Verify service worker lifecycle on Android (Firefox kills idle workers)
     - May need to use chrome.alarms for keepalive
  d. AMO: gecko_android already set with strict_min_version: "115.0"

1.5 Packaging & Distribution
-----------------------------
  ✅ Package pipeline exists: package-firefox.mjs, web-ext build
  ✅ AMO submission errors fixed (service_worker + scripts, add-on ID)
  
  TO DO:
  a. Submit to AMO (addons.mozilla.org)
  b. For Android: extension will appear in Firefox Android's extension list
     once approved on AMO (Firefox Android 120+ supports arbitrary extensions
     from AMO; older versions use curated collection)
  c. Create listing with screenshots, description, privacy policy
  d. Set up auto-update via AMO


================================================================================
================================================================================
Timeline: 2-4 months
Scope: Real TLS validation via native components managed by the extension
Platforms: Firefox Desktop (Linux, macOS, Windows), Firefox Android (with companion)

This tier adds ACTUAL TLS certificate trust for .bit domains by combining the
extension with native components.

2.1 Architecture Overview
-------------------------
  ┌─────────────────────────────────────────────────────────┐
  │                    Firefox Browser                       │
  │                                                         │
  │  ┌─────────────┐    ┌──────────────────────────┐       │
  │  │  Extension   │◄──►│ Native Messaging Host    │       │
  │  │  (resolve    │    │ (Go binary — manages     │       │
  │  │   d/ names,  │    │  encaya, ncp11, and      │       │
  │  │   UI, cache) │    │  electrum-nmc queries)   │       │
  │  └─────────────┘    └──────────┬───────────────┘       │
  │                                │                        │
  │  ┌─────────────────────────────▼───────────────────┐   │
  │  │              NSS Trust Store                      │   │
  │  │  ┌──────────┐  ┌───────────────┐                 │   │
  │  │  │  ncp11    │  │ tlsrestrictnss│                 │   │
  │  │  │ (PKCS#11) │  │ (constraints) │                 │   │
  │  │  └─────┬────┘  └───────────────┘                 │   │
  │  └────────┼────────────────────────────────────────┘   │
  │           │                                             │
  └───────────┼─────────────────────────────────────────────┘
              │
  ┌───────────▼─────────────────────────────────────────────┐
  │         encaya daemon (localhost)                        │
  │    Generates X.509 certs from DANE/TLSA records         │
  │    Served via AIA to ncp11                              │
  │              │                                          │
  │    ┌────────▼──────────┐                                │
  │    │  electrum-nmc      │  ← lightweight SPV queries    │
  │    │  (Python, headless │     OR                        │
  │    │   OR ElectrumX WS) │  ← direct ElectrumX WS       │
  │    └───────────────────┘                                │
  └─────────────────────────────────────────────────────────┘

2.2 Native Messaging Host (New Component: "namecoin-firefox-bridge")
--------------------------------------------------------------------
A single Go binary that acts as the bridge between the extension and the
native Namecoin TLS stack:

  Build: Go → compiled binary per platform (linux-amd64, darwin-arm64, win-amd64)
  
  Responsibilities:
  a. Native Messaging protocol (stdin/stdout JSON, length-prefixed)
  b. Manage encaya lifecycle (start/stop subprocess or in-process)
  c. Register ncp11 PKCS#11 module in Firefox's NSS profile
  d. Run tlsrestrictnss on first setup to apply .bit name constraints
  e. Resolve names via ElectrumX WebSocket (reuse existing approach)
     OR via embedded electrum-nmc library calls
  f. Generate/cache Namecoin TLS certificates via encaya
  g. Provide cert-validation results back to extension for UI

  Native messaging manifest (nm_manifest.json):
  {
    "name": "namecoin_firefox_bridge",
    "description": "Namecoin name resolution and TLS certificate management",
    "path": "/usr/local/bin/namecoin-firefox-bridge",
    "type": "stdio",
    "allowed_extensions": ["tls-namecoin@extension"]
  }

  Manifest locations:
  - Linux: ~/.mozilla/native-messaging-hosts/namecoin_firefox_bridge.json
  - macOS: ~/Library/Application Support/Mozilla/NativeMessagingHosts/...
  - Windows: Registry key under HKCU\SOFTWARE\Mozilla\NativeMessagingHosts

2.3 TLS Trust Chain Setup (One-Time per Profile)
-------------------------------------------------
The native messaging host performs these setup steps on first run:

  Step 1: Generate encaya root CA
    $ encayagen
    → Creates encaya.pem (root CA with .bit name constraints)
    
  Step 2: Register ncp11 as PKCS#11 module in Firefox
    Option A: Drop a .module file in /usr/share/p11-kit/modules/ (Linux)
    Option B: Use modutil: modutil -add ncp11 -libfile /path/to/libncp11.so -dbdir ~/.mozilla/firefox/<profile>/
    Option C: SecurityDevices enterprise policy (managed installs)
    Option D: pkcs11 WebExtensions API — Firefox supports this!
      → chrome.pkcs11.installModule("namecoin_ncp11")
      → Requires pkcs11 permission in manifest
      → This is the CLEANEST approach for extension-managed PKCS#11!
    
  Step 3: Apply name constraints via tlsrestrictnss
    $ tlsrestrictnss -dbdir ~/.mozilla/firefox/<profile>/ -tld bit
    → Modifies NSS DB so no public CA can issue .bit certs
    → Must be run once per profile, and after NSS cert updates
    
  Step 4: Configure encaya to use ElectrumX for name resolution
    → encaya.conf points to localhost ncdns OR direct ElectrumX
    → For lightweight setup: skip ncdns, have the bridge daemon itself
      serve DNS responses for .bit queries that encaya needs

2.4 Extension Changes for Tier 2
---------------------------------
  a. Add "pkcs11" permission to manifest.json
     → Allows: chrome.pkcs11.installModule(), isModuleInstalled(), etc.
     
  b. Add "nativeMessaging" permission to manifest.json
     → Allows: chrome.runtime.connectNative()
     
  c. New module: background/native-host-bridge.js (file exists, needs implementation)
     → Connect to namecoin_firefox_bridge via native messaging
     → Send: resolve requests, setup commands, status queries
     → Receive: resolution results, cert validation status, setup progress
     
  d. Setup wizard in options page
     → "Install Native Components" button
     → Downloads and installs the Go bridge binary
     → Runs PKCS#11 registration
     → Runs tlsrestrictnss
     → Shows setup progress and status
     
  e. Enhanced resolution flow with TLS:
     1. User navigates to example.bit
     2. Extension resolves d/example → IP via ElectrumX (fast, in-extension)
     3. Extension sends "prepare_tls" to native host with domain + TLSA data
     4. Native host → encaya generates cert → ncp11 serves it to NSS
     5. Extension redirects to https://<ip> with Host header override
        → WAIT: Host header can't be set from extension for navigation
        → ALTERNATIVE: Use DNS override via native host to make example.bit
          resolve to the IP locally
     6. Better approach: Native host adds entry to system hosts file
        or runs a local DNS stub that resolves example.bit → IP
     7. Then the extension just navigates to https://example.bit
        and Firefox's NSS (via ncp11) trusts the cert automatically!

2.5 Simplified Approach (Skip ncdns Entirely)
----------------------------------------------
For query-only (no wallet), we can significantly simplify the stack:

  Instead of: electrum-nmc → ncdns → encaya → ncp11
  
  Use:        ElectrumX WS → bridge daemon → encaya (embedded) → ncp11
              ↑ browser                       ↑ local only

  The bridge daemon:
  1. Resolves names via ElectrumX WebSocket (same as extension does now)
  2. Extracts TLSA records from name value
  3. Calls safetlsa library to generate constrained X.509 certs from TLSA
  4. Serves them via a minimal AIA endpoint on localhost
  5. ncp11 fetches certs from this AIA endpoint

  This eliminates ncdns (the DNS bridge) and namecoin-core (full node) entirely.
  The only dependencies are:
  - namecoin-firefox-bridge (Go binary, ~10-20 MB)
    - Embeds: encaya logic, safetlsa, ncp11 Go library
  - A working ElectrumX server with WebSocket (same as Tier 1)

2.6 Firefox Android (Fenix) — Companion App Approach
------------------------------------------------------
Firefox Android does NOT support native messaging or PKCS#11 modules.
Therefore, Tier 2 on Android requires a different approach:

  Option A: Android Companion App
    - Standalone Android app (Kotlin/Java)
    - Runs a local HTTPS proxy on 127.0.0.1:<port>
    - Extension sets Firefox proxy to localhost:<port> for .bit domains
      (Firefox Android supports proxy.settings API)
    - Proxy resolves .bit → IP, terminates Namecoin TLS, re-signs with
      a local CA cert the user installs in Android's cert store
    - Similar to how corporate MITM proxies work, but for .bit only
    
  Option B: Android VPN Service
    - App creates a local VPN (no remote server)
    - Intercepts DNS for .bit → resolves via ElectrumX
    - Returns the resolved IP with proper routing
    - For TLS: injects Namecoin certs into Android system trust store
      (requires user to install CA cert, or root access)
    
  Option C: GeckoView Custom Build
    - Fork GeckoView (Firefox's Android rendering engine)
    - Patch NSS initialization to load ncp11
    - Distribute as a custom browser APK
    - Most complete solution but highest maintenance burden
    
  Recommended for Android: Option A (companion app) is most practical.
  The proxy approach works within Android's security model without root.

2.7 pkcs11 WebExtensions API — Key Details
--------------------------------------------
Firefox's pkcs11 API (https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/pkcs11)
is the cleanest way to register ncp11:

  Manifest permission:
    "permissions": ["pkcs11"]
  
  API calls:
    chrome.pkcs11.installModule("ncp11", flags)  → register PKCS#11 module
    chrome.pkcs11.isModuleInstalled("ncp11")      → check if registered
    chrome.pkcs11.uninstallModule("ncp11")        → remove
    chrome.pkcs11.getModuleSlots("ncp11")         → list slots/tokens

  The module name maps to a JSON manifest file:
    Linux: ~/.mozilla/managed-storage/ncp11.json
           or /usr/lib/mozilla/pkcs11-modules/ncp11.json
    macOS: ~/Library/Application Support/Mozilla/PKCS11Modules/ncp11.json
    
  Module manifest:
    {
      "name": "ncp11",
      "description": "Namecoin TLS certificates",
      "type": "pkcs11",
      "path": "/path/to/libncp11.so",
      "allowed_extensions": ["tls-namecoin@extension"]
    }

  IMPORTANT: pkcs11 API is desktop-only, NOT available on Firefox Android.
  This confirms Android needs the companion app approach (Section 2.6).


================================================================================
TIER 3: NATIVE FIREFOX INTEGRATION (Upstream Patches)
================================================================================
Timeline: 1-3 years
Scope: Patches to Firefox (Gecko) source code for first-class .bit support
Platforms: All Firefox platforms (Desktop, Android, iOS via WebKit limitations)

3.1 DNS Subsystem Patch (netwerk/dns/)
---------------------------------------
  Location: netwerk/dns/nsDNSService.cpp, nsHostResolver.cpp
  
  Approach:
  a. Add new DNS resolution backend for .bit TLD
  b. When a .bit hostname is queried:
     → Route to Namecoin resolver instead of system/DoH resolver
     → Namecoin resolver connects to ElectrumX via WebSocket
     → Returns A/AAAA records from name value JSON
  c. Controlled by about:config preference:
     network.namecoin.enabled = true
     network.namecoin.electrumx_servers = "wss://..."
  
  Implementation:
  - New class: nsNamecoinResolver (C++ or Rust via FFI)
  - Integrates with nsHostResolver's async resolution pipeline
  - Results cached in Firefox's DNS cache (respects name expiry TTL)
  - Handles: d/ namespace lookup, map traversal, NS delegation, alias chains

3.2 TLS/NSS Integration
-------------------------
  Firefox uses NSS (Network Security Services) for TLS. Two sub-approaches:

  Option A: Auto-load ncp11 PKCS#11 module
  - Detect that Namecoin resolution is enabled
  - Automatically register ncp11 as a security module
  - ncp11 dynamically provides Namecoin certs to NSS during TLS handshake
  - Run tlsrestrictnss constraints at profile creation time
  
  Option B: Direct DANE-TLSA validation in NSS
  - More ambitious: add DANE (RFC 6698/7671) support to NSS directly
  - During TLS handshake, if server cert doesn't chain to a trusted CA,
    check if a TLSA record exists for the domain
  - Validate cert against TLSA record (usage=3 DANE-EE, or usage=2 DANE-TA)
  - This would benefit ALL DNS-secured domains, not just Namecoin
  - Could be contributed upstream to NSS project
  
  Option A is more practical for initial integration; Option B is the ideal
  long-term solution that benefits the entire web.

3.3 UI Integration
-------------------
  a. Address bar
     - .bit domains resolve directly, no search-engine interception
     - Show Namecoin shield icon (like the lock icon for HTTPS)
     - Teal/blue for valid Namecoin TLS, yellow for HTTP-only
  
  b. Certificate viewer
     - Show "Namecoin Blockchain" as certificate source
     - Display block height, tx hash, expiry
     - Link to block explorer for verification
  
  c. about:config preferences
     - network.namecoin.enabled (boolean)
     - network.namecoin.electrumx_servers (string, comma-separated)
     - network.namecoin.cache_ttl_seconds (integer)
     - network.namecoin.require_tls (boolean, default true)
  
  d. about:namecoin diagnostic page
     - Server connectivity status
     - Current block height
     - Recent resolution log
     - TLS certificate cache

3.4 Firefox Android (GeckoView)
-------------------------------
  GeckoView is the rendering engine used by Firefox Android (Fenix).
  All Gecko C++ changes from 3.1-3.3 automatically apply to GeckoView.
  
  Additional Android-specific work:
  a. ElectrumX WebSocket connectivity through Android network stack
  b. Background service for maintaining ElectrumX connection
  c. Battery-friendly polling for name expiry updates
  d. Android Settings UI for Namecoin configuration
  e. ncp11 compiled for ARM/ARM64 → loaded into GeckoView's NSS

3.5 Firefox iOS
----------------
  Firefox iOS uses WebKit (Apple requirement), NOT Gecko.
  Native Namecoin integration is NOT possible on Firefox iOS.
  
  Best option: Safari-based approach (see Section 5.2) would be needed,
  which is a separate project entirely.

3.6 Upstream Strategy
----------------------
  Getting patches into Firefox requires Mozilla buy-in:
  
  a. File a meta-bug on Bugzilla (bugzilla.mozilla.org)
     → Component: Core :: Networking: DNS
     → Title: "Support for Namecoin (.bit) decentralized domain resolution"
  
  b. Start as an about:config experiment
     → All features behind pref flags, disabled by default
     → Low risk for Mozilla to merge
  
  c. Engage Mozilla Builders / Innovation Fund
     → Pitch: "Censorship-resistant DNS for Firefox"
     → Aligns with Mozilla's mission statement
  
  d. Build Firefox Nightly custom builds first
     → Test with Namecoin community
     → Prove stability and performance
     → Gather user metrics
  
  e. Phased enablement:
     → Phase 1: about:config only (Nightly)
     → Phase 2: Settings page toggle (Beta)
     → Phase 3: Default-off with discovery UI (Release)
     → Phase 4: Default-on after sufficient adoption


================================================================================
SECTION 4: ELECTRUM-NMC INTEGRATION DETAILS
================================================================================

4.1 Query-Only Usage (No Wallet)
---------------------------------
electrum-nmc is a full SPV wallet, but for our purposes we only need:
  - name_show(identifier) — look up a name's current value
  - blockchain.scripthash.get_history — ElectrumX protocol query
  - blockchain.transaction.get — fetch raw/verbose transaction
  - blockchain.headers.subscribe — get current block height

We do NOT need:
  - Wallet creation, key management, or signing
  - name_new, name_firstupdate, name_update transactions
  - Coin sending or receiving

4.2 Integration Approach: Direct ElectrumX Protocol
----------------------------------------------------
Rather than embedding electrum-nmc (Python), the most portable approach is
to speak the ElectrumX JSON-RPC protocol directly:

  Transport: WebSocket (ws:// or wss://)
  Protocol: JSON-RPC 2.0
  
  Required methods:
  1. server.version — handshake + protocol negotiation
  2. blockchain.scripthash.get_history — get tx history for a name
  3. blockchain.transaction.get — fetch verbose tx with outputs
  4. blockchain.headers.subscribe — current block height

  Scripthash computation (for name index):
  - Build script: OP_NAME_UPDATE (0x53) + pushdata(name) + pushdata("") + OP_2DROP + OP_DROP + OP_RETURN
  - SHA-256 hash the script
  - Reverse bytes → scripthash
  
  Transaction decoding:
  - Find output with OP_NAME_UPDATE (0x53) opcode
  - Read pushdata: name bytes, then value bytes
  - Value is JSON string with ip, ip6, ns, tls, map, etc.

This is exactly what electrumx-ws.js in the existing extension already
implements. The same logic can be used in:
  - Browser extension (JavaScript, WebSocket)
  - Native messaging host (Go, WebSocket)
  - Firefox Gecko patch (C++ or Rust, WebSocket)

4.3 ElectrumX Server Requirements
-----------------------------------
Not all ElectrumX servers support Namecoin name indexing. The server must:
  - Run the namecoin fork of ElectrumX (github.com/namecoin/electrumx)
  - Have WebSocket transport enabled (ws:// port or wss:// port)
  - Index Namecoin name scripts (default for NMC ElectrumX)

Known public servers:
  - electrumx.testls.space:50003 (ws://) — confirmed working
  - nmc2.bitcoins.sk:57002 (TCP/SSL only, no WS currently)

For production use, multiple servers should be configured for redundancy.
The extension should also support user-configured servers.

4.4 Name Value JSON Schema (d/ namespace)
-------------------------------------------
When a d/ name is resolved, the value is a JSON object:

  {
    "ip": "1.2.3.4",                    // IPv4 address
    "ip6": "2001:db8::1",               // IPv6 address
    "ns": ["ns1.example.com"],           // DNS nameservers (delegation)
    "tls": [                             // TLSA records
      [2, 1, 0, "base64-pubkey"],        // [usage, selector, matchType, data]
      [3, 1, 1, "sha256hex"]
    ],
    "map": {                             // Subdomain map
      "www": {"ip": "5.6.7.8"},
      "*": {"ip": "1.2.3.4"},
      "_tcp": {
        "map": {
          "_443": {
            "tls": [[3, 1, 1, "sha256hex"]]
          }
        }
      }
    },
    "alias": "other.bit",               // CNAME-like alias
    "translate": "example.com",          // Translate to standard domain
    "info": {"description": "My site"},  // Informational metadata
    "email": "admin@example.bit",        // Contact email
    "tor": "abcdef.onion",              // Tor hidden service
    "nostr": {                           // Nostr integration (NIP-05 style)
      "names": {
        "_": "hex-pubkey",
        "alice": "hex-pubkey2"
      },
      "relays": {
        "hex-pubkey": ["wss://relay.example"]
      }
    }
  }


================================================================================
SECTION 5: PLATFORM-SPECIFIC NOTES
================================================================================

5.1 Firefox Desktop (Linux, macOS, Windows)
---------------------------------------------
  - Best supported platform for all three tiers
  - NSS-based TLS → ncp11 works natively
  - PKCS#11 WebExtensions API available
  - Native messaging available
  - About:config available for experimental features
  
  Per-OS notes:
  Linux:   ncp11 → libncp11.so, p11-kit module registration
  macOS:   ncp11 → libncp11.dylib, install via Security framework or modutil
  Windows: ncp11 → ncp11.dll, registry-based PKCS#11 registration
           OR certinject for CryptoAPI (Edge/Chrome path, not Firefox)

5.2 Firefox Android (Fenix)
-----------------------------
  - Extension works for Tier 1 (pure WebExtensions)
  - No native messaging, no PKCS#11 API
  - For TLS (Tier 2): companion app required (proxy or VPN approach)
  - For Tier 3: GeckoView patches apply automatically
  - Firefox Android 120+ supports arbitrary AMO extensions
  - Earlier versions: user must add extension via AMO collection ID

5.3 Safari / iOS
------------------
  (Out of scope for Firefox plan, but noted for completeness)
  - Safari uses WebKit, not Gecko
  - Safari Web Extensions have similar limitations to Firefox
  - iOS: even Firefox iOS uses WebKit (Apple mandate)
  - For iOS: the only path is Safari extension + companion app
  - Separate project would be needed

5.4 Other Mobile Browsers
---------------------------
  - Firefox Focus: No extension support
  - Firefox Klar: No extension support
  - Mull (Firefox fork): Full extension support, same as Fenix
  - Tor Browser Android: Uses GeckoView, Tier 3 changes would apply
  - Fennec F-Droid (legacy): Full extension support


================================================================================
SECTION 6: IMPLEMENTATION ROADMAP
================================================================================

Phase 1: Enhanced Extension (Tier 1) — Weeks 1-4
-------------------------------------------------
  Week 1-2:
    [ ] Implement subdomain resolution (map traversal)
    [ ] Add NS delegation fallback
    [ ] Add CNAME/alias chain following
    [ ] Add multiple ElectrumX server rotation
    [ ] Improve TLSA display in popup

  Week 3:
    [ ] Implement HTTPS upgrade with TLSA-guided cert acceptance UX
    [ ] Add TOFU (Trust On First Use) fingerprint storage
    [ ] Address bar .bit detection (search interception)
    [ ] Test thoroughly on Firefox Android

  Week 4:
    [ ] Submit to AMO (both desktop and Android)
    [ ] Write user documentation
    [ ] Set up extension auto-update pipeline

Phase 2: Native Companion (Tier 2) — Months 2-4
-------------------------------------------------
  Month 2:
    [ ] Build namecoin-firefox-bridge Go binary
    [ ] Implement native messaging protocol
    [ ] Embed ElectrumX WebSocket client in Go
    [ ] Embed safetlsa cert generation
    [ ] Build minimal AIA endpoint for cert serving

  Month 3:
    [ ] Compile ncp11 for all desktop platforms
    [ ] Implement pkcs11 WebExtensions API integration
    [ ] Implement tlsrestrictnss setup step
    [ ] Build setup wizard in options page
    [ ] Test end-to-end: .bit → HTTPS with trusted Namecoin cert

  Month 4:
    [ ] Build Android companion app (proxy approach)
    [ ] Package native components with installers
    [ ] Cross-platform testing (Linux, macOS, Windows, Android)
    [ ] Publish companion app to GitHub Releases + F-Droid

Phase 3: Upstream Exploration (Tier 3) — Months 4+
----------------------------------------------------
  [ ] File Firefox Bugzilla meta-bug
  [ ] Build proof-of-concept Gecko DNS patch
  [ ] Engage Mozilla community / builders program
  [ ] Create Firefox Nightly custom builds
  [ ] Gather community testing feedback
  [ ] Iterate on patches based on review

Phase 5: Three-Stage Enablement Gating — 🔄 IN PROGRESS
---------------------------------------------------------
Mozilla's standard graduation path: about:config → Settings toggle → default-off.
All stages keep the feature default-off.

  Stage A (Nightly) — 🔄 In Progress:
  [x] patches/0016-namecoin-phase5-enablement.patch
  [x] src/NamecoinFeature.sys.mjs — JS feature gate module
  [x] StaticPrefList.yaml: network.namecoin.settings_visible pref
  [x] Privacy.js gating: section hidden unless settings_visible or Nightly
  [x] Telemetry scalars: namecoin.settings_visible, namecoin.enabled_changed
  [ ] Integration testing on Nightly build

  Stage B (Beta) — Not started:
  [ ] Normandy experiment: namecoin-stage-b-beta-rollout
  [ ] Gradual rollout: 1% → 10% → 50% over 2–3 Beta cycles
  [ ] Telemetry monitoring dashboard
  [ ] Success criteria: <0.1% crash increase, <50ms p95 DNS latency delta

  Stage C (Release) — Not started:
  [ ] Ship settings_visible = true as default
  [ ] Remove Normandy experiment dependency
  [ ] Release notes entry
  [ ] Default-on decision (separate future phase)


================================================================================
SECTION 7: SECURITY CONSIDERATIONS
================================================================================

7.1 Trust Model
----------------
  - Namecoin names are secured by proof-of-work (merged with Bitcoin)
  - TLSA records in name values provide certificate pinning
  - Name expiry (36,000 blocks) prevents stale records
  - SPV verification via ElectrumX provides reasonable security for queries
    (though not as strong as a full node)
  - For maximum security: users should run their own ElectrumX server or
    namecoin-core full node

7.2 Risks
----------
  - ElectrumX server trust: a malicious server could return false data
    → Mitigation: query multiple servers, compare results
    → Mitigation: SPV proof verification (electrum-nmc level)
  - Name squatting: anyone can register any unregistered d/ name
    → Not a security issue per se, but UX consideration
  - Expired names: extension already checks block height vs expiry
  - CA impersonation: without tlsrestrictnss, a public CA could issue
    a cert for a .bit domain that the browser would trust
    → Tier 2+ solves this with name constraints in NSS

7.3 Privacy
------------
  - ElectrumX server sees which names you look up
    → Mitigation: multiple servers, random order, padding
    → Mitigation: Tor integration (ElectrumX over Tor)
  - No third-party APIs used (removed in current version)
  - All resolution is direct browser-to-ElectrumX via WebSocket
  - Extension does not phone home or collect analytics


================================================================================
SECTION 8: KEY LEARNINGS & DECISIONS
================================================================================

8.1 Why Direct ElectrumX WebSocket is the Right Default
---------------------------------------------------------
  - No backend server required (works as pure browser extension)
  - No full node required (SPV-level security is sufficient for queries)
  - Works on mobile (WebSocket is universal)
  - Same approach proven in noStrudel PR #352 and our extension
  - electrum-nmc as a Python subprocess is too heavy for most users

8.2 Why ncp11 + encaya is the TLS Path for Firefox
----------------------------------------------------
  - Firefox uses NSS, which supports PKCS#11 modules
  - ncp11 is purpose-built for this exact use case
  - Firefox's WebExtensions API has pkcs11 module management
  - encaya handles the complex DANE→X.509 certificate generation
  - This is the approach Namecoin project recommends for Firefox

8.3 Why Android Needs a Different Approach
-------------------------------------------
  - Firefox Android (Fenix/GeckoView) strips native messaging and pkcs11 APIs
  - Android's security model doesn't allow apps to modify other apps' state
  - A companion app can run a local proxy for TLS termination/re-signing
  - Long-term: GeckoView patches (Tier 3) solve this cleanly

8.4 The Scripthash Computation is Critical
--------------------------------------------
  - Must use OP_NAME_UPDATE (0x53), NOT OP_NAME_SHOW (0xd1)
  - The "index script" format is: 0x53 + pushdata(name) + pushdata("") + 0x6d + 0x75 + 0x6a
  - SHA-256 hash, then REVERSE the bytes → this is the ElectrumX scripthash
  - Getting this wrong returns empty history (silent failure)
  - Verified working against electrumx.testls.space for d/testls, d/bitcoin

8.5 Firefox Extension Compatibility (Desktop + Android)
--------------------------------------------------------
  - service_worker + scripts keys needed in manifest for Firefox compat
  - Firefox 128+ uses service_worker; older Firefox uses scripts fallback
  - browser_specific_settings.gecko.id is REQUIRED for MV3 on Firefox
  - gecko_android.strict_min_version ensures Android minimum version
  - data: URL navigation is blocked → use extension pages instead
  - web-ext linter false positive on service_worker is known, ignore it


================================================================================
SECTION 9: FILE REFERENCE
================================================================================

  Workspace files:
    PLAN.md         — this document (integration plan)
    extension10.txt     — extension packaging and AMO submission notes
    namecoin.txt        — full Namecoin GitHub organization catalog
    tls-namecoin-ext/   — the browser extension source code

  Extension key files:
    background/electrumx-ws.js       — ElectrumX WebSocket client (core resolution)
    background/namecoin-resolver.js  — Name resolution with caching
    background/bit-navigator.js      — .bit navigation interception
    background/dns-router.js         — Domain routing layer
    background/tls-inspector.js      — TLS header inspection
    background/service-worker.js     — Main service worker orchestrator
    background/native-host-bridge.js — Native messaging (placeholder for Tier 2)
    manifest.json                    — Chrome manifest
    manifest.firefox.json            — Firefox manifest (with gecko settings)
    scripts/package-firefox.mjs      — Firefox packaging script

  External references:
    https://github.com/namecoin/electrum-nmc     — Electrum-NMC (SPV wallet/query)
    https://github.com/namecoin/ncp11            — PKCS#11 module for Firefox NSS
    https://github.com/namecoin/encaya           — AIA cert server
    https://github.com/namecoin/ncdns            — DNS bridge daemon
    https://github.com/namecoin/tlsrestrictnss   — NSS name constraints
    https://github.com/namecoin/safetlsa         — Safe TLSA cert generation
    https://github.com/namecoin/pkcs11mod        — Go PKCS#11 framework
    https://github.com/namecoin/crosssignnameconstraint — X.509 name constraints
    https://github.com/namecoin/dnssec-hsts      — DANE-based HTTPS upgrade
    https://github.com/namecoin/ncprop279        — Tor Prop279 bridge
    https://github.com/namecoin/StemNS           — Tor pluggable naming


================================================================================
SECTION 10: SOURCE REVIEW STATUS
================================================================================

The following sources were reviewed to inform this document:

  ✅ FULLY REVIEWED:
  - extension10.txt (existing extension session notes)
  - namecoin.txt (full Namecoin GitHub org catalog, 63 repos)
  - noStrudel PR #352 (full PR description + commits, direct WebSocket approach)
  - Amethyst repo (README, architecture, NIP support list)
  - electrum-nmc repo (README, installation, query usage)
  - encaya repo (README, setup, architecture, 3-machine model)
  - ncp11 repo (README, build, installation methods incl. PKCS#11 registration)
  - tlsrestrictnss repo (README, purpose, NSS name constraints)
  - safetlsa repo (README, TLSA → X.509 generation)
  - Namecoin TLS docs (namecoin.org/docs/name-owners/tls/ — full guide)
  - Namecoin Tor resolution docs (namecoin.org/docs/tor-resolution/)
  - Namecoin beta downloads page (ncdns, Electrum-NMC, ConsensusJ)
  - Existing extension source code (all 8 background modules, both manifests)
  - Tor Browser issue #30558 (description + metadata via GitLab API)
  - Tor Browser issue #33568 (TLS cert validation — full description via API)
  - Tor Browser issues #33752, #33749, #33807, #41286 (all via API)

  ⚠️ PARTIALLY REVIEWED (content limitations):
  - Tor Browser issue #30558 comments (65 comments)
    → GitLab API returned 401 Unauthorized for /notes endpoint
    → JS-rendered comments not in server-side HTML
    → Web archive returned empty/redirect pages
    → Only the issue description and metadata were accessible
    → IMPACT: May have missed specific technical discussion about
      integration challenges, rejected approaches, or community feedback
      from those 65 comments. The issue description + related issues
      provided sufficient architectural context.

  ❌ NOT REVIEWED:
  - Grayhat 2020 presentation slides/video (404 on namecoin.org/resources)
    → Jeremy Rand's talk on "Namecoin as Decentralized Alternative to CAs"
    → Would likely contain additional TLS architecture insights


================================================================================
END OF PLAN
================================================================================
