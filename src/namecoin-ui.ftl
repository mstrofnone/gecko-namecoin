# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

## ==========================================================================
## Namecoin UI Localization Strings
## Covers: address bar indicators, certificate viewer, about:namecoin page,
## settings panel, and DANE validation messages.
## ==========================================================================


## --------------------------------------------------------------------------
## Address Bar — Security Indicator Tooltips (Section 6.1)
## --------------------------------------------------------------------------

# Shown when the .bit site has a valid DANE-TLSA validated certificate
urlbar-namecoin-secure-tooltip =
    Connection verified via Namecoin blockchain (DANE-TLSA)
urlbar-namecoin-secure-label = Namecoin Verified

# Shown when the .bit site is loaded over HTTP or has no TLSA record
urlbar-namecoin-warning-tooltip =
    This .bit domain has no TLS certificate on the blockchain — connection is not encrypted
urlbar-namecoin-warning-label = Namecoin — Not Secure

# Shown when TLSA verification failed (cert doesn't match blockchain record)
urlbar-namecoin-error-tooltip =
    Certificate does not match the TLSA record on the Namecoin blockchain
urlbar-namecoin-error-label = Namecoin — Certificate Mismatch

# Shown in the identity popup for .bit domains
urlbar-namecoin-identity-title = Namecoin Domain
urlbar-namecoin-identity-description =
    This domain is registered on the Namecoin blockchain and resolved via
    ElectrumX. It is not part of the traditional DNS system.

# Connection details in the identity popup
urlbar-namecoin-dane-verified =
    Certificate validated against DANE-TLSA record at block { $blockHeight }
urlbar-namecoin-http-only =
    No TLSA record found — loaded over unencrypted HTTP
urlbar-namecoin-dane-failed =
    TLSA validation failed — the server certificate does not match the
    blockchain record


## --------------------------------------------------------------------------
## Certificate Viewer — Namecoin Trust Source (Section 6.2)
## --------------------------------------------------------------------------

# Trust source label when cert validated via Namecoin DANE
certviewer-namecoin-trust-source = Namecoin Blockchain (DANE-TLSA)

# Section header in certificate details
certviewer-namecoin-section-title = Namecoin Blockchain Verification

# Individual fields
certviewer-namecoin-block-height = Name Registration Block Height
certviewer-namecoin-block-height-value = Block { $height }
certviewer-namecoin-tx-hash = Transaction Hash
certviewer-namecoin-tx-hash-link = { $truncatedHash } (view on namecha.in)
certviewer-namecoin-name-expiry = Name Expiry
certviewer-namecoin-name-expiry-value =
    Expires after block { $expiryBlock } (approx. { $expiryDate })
certviewer-namecoin-name-op = Last Operation
certviewer-namecoin-name-registered = Name registered as d/{ $name }

# TLSA record details
certviewer-namecoin-tlsa-section = DANE-TLSA Record
certviewer-namecoin-tlsa-usage = Usage
certviewer-namecoin-tlsa-usage-2 = 2 — DANE-TA (Trust Anchor)
certviewer-namecoin-tlsa-usage-3 = 3 — DANE-EE (End Entity)
certviewer-namecoin-tlsa-selector = Selector
certviewer-namecoin-tlsa-selector-0 = 0 — Full Certificate
certviewer-namecoin-tlsa-selector-1 = 1 — SubjectPublicKeyInfo
certviewer-namecoin-tlsa-match = Matching Type
certviewer-namecoin-tlsa-match-0 = 0 — Exact Match
certviewer-namecoin-tlsa-match-1 = 1 — SHA-256
certviewer-namecoin-tlsa-match-2 = 2 — SHA-512

# Verification status
certviewer-namecoin-verified = ✓ Certificate matches blockchain TLSA record
certviewer-namecoin-mismatch = ✗ Certificate does NOT match blockchain TLSA record


## --------------------------------------------------------------------------
## about:namecoin Page (Section 6.3)
## --------------------------------------------------------------------------

about-namecoin-title = Namecoin Diagnostics
about-namecoin-subtitle =
    Monitor .bit domain resolution and ElectrumX connectivity

# Status banner
about-namecoin-enabled = Namecoin (.bit) resolution is enabled
about-namecoin-disabled =
    Namecoin resolution is disabled — enable via about:config → network.namecoin.enabled

# Server status card
about-namecoin-servers-title = ElectrumX Servers
about-namecoin-servers-badge =
    { $connected } / { $total } connected
about-namecoin-server-url = Server URL
about-namecoin-server-status = Status
about-namecoin-server-latency = Latency
about-namecoin-server-height = Block Height
about-namecoin-server-lastseen = Last Seen
about-namecoin-server-connected = Connected
about-namecoin-server-disconnected = Disconnected
about-namecoin-server-error = Error

# Stat cards
about-namecoin-stat-height = Blockchain Height
about-namecoin-stat-connected = Connected Servers
about-namecoin-stat-latency = Avg Latency
about-namecoin-stat-cache = Cache Entries

# Resolution log card
about-namecoin-resolutions-title = Recent Resolutions
about-namecoin-resolutions-badge = { $count } lookups
about-namecoin-res-domain = Domain
about-namecoin-res-result = Result
about-namecoin-res-time = Time
about-namecoin-res-cache = Cache
about-namecoin-res-timestamp = Timestamp
about-namecoin-cache-hit = HIT
about-namecoin-cache-miss = MISS

# DANE cache card
about-namecoin-dane-title = TLS / DANE Cache
about-namecoin-dane-badge = { $count } entries
about-namecoin-dane-domain = Domain
about-namecoin-dane-fingerprint = Cert Fingerprint
about-namecoin-dane-matchtype = TLSA Match Type
about-namecoin-dane-usage = Usage
about-namecoin-dane-expiry = Expiry Block

# Name lookup tool
about-namecoin-lookup-title = Name Lookup Tool
about-namecoin-lookup-placeholder = example.bit
about-namecoin-lookup-button = Resolve
about-namecoin-lookup-raw = Raw JSON Value
about-namecoin-lookup-parsed = Parsed Result
about-namecoin-lookup-resolving = Resolving…
about-namecoin-lookup-error-disabled =
    Error: Namecoin resolution is disabled.
    Enable via about:config → network.namecoin.enabled
about-namecoin-lookup-error-not-found =
    Error: NS_ERROR_NAMECOIN_NOT_FOUND
    The name "d/{ $name }" has no registration history on the Namecoin blockchain.

# Empty states
about-namecoin-empty-servers = No servers configured
about-namecoin-empty-resolutions = No recent lookups
about-namecoin-empty-dane = No DANE/TLS entries cached


## --------------------------------------------------------------------------
## Settings UI — Privacy & Security Panel (Section 6.4)
## --------------------------------------------------------------------------

settings-namecoin-section-title = Namecoin (.bit) Domains
settings-namecoin-section-description =
    Resolve .bit domains directly from the Namecoin blockchain using
    ElectrumX servers. This is an experimental feature.

# Main toggle
settings-namecoin-enable-label = Enable Namecoin (.bit) domain resolution
settings-namecoin-enable-description =
    When enabled, .bit domains will be resolved via the Namecoin blockchain
    instead of returning a DNS error.

# Server configuration
settings-namecoin-servers-label = ElectrumX Servers
settings-namecoin-servers-description =
    List of ElectrumX servers used to query the Namecoin blockchain.
    One server URL per line (WebSocket: ws:// or wss://).
settings-namecoin-servers-add = Add Server
settings-namecoin-servers-remove = Remove
settings-namecoin-servers-default = Restore Defaults

# TLS requirement toggle
settings-namecoin-require-tls-label =
    Require HTTPS for .bit domains when TLSA records exist
settings-namecoin-require-tls-description =
    When a .bit domain has DANE-TLSA records on the blockchain, block
    plain HTTP connections and require TLS with blockchain-verified
    certificates.

# Diagnostics link
settings-namecoin-diagnostics-link = Open Namecoin Diagnostics
settings-namecoin-diagnostics-description =
    View ElectrumX server health, recent lookups, and resolve names
    manually.

# Cache settings
settings-namecoin-cache-label = Cache Duration
settings-namecoin-cache-description =
    How long to cache resolved .bit domain records (in seconds).
settings-namecoin-cache-seconds = { $seconds } seconds


## --------------------------------------------------------------------------
## DANE Validation Error/Warning Messages
## --------------------------------------------------------------------------

# Certificate errors specific to Namecoin DANE
namecoin-dane-error-cert-mismatch =
    The server's certificate does not match the DANE-TLSA record stored on the
    Namecoin blockchain for { $hostname }. This could indicate a
    man-in-the-middle attack or an outdated TLSA record.

namecoin-dane-error-expired-name =
    The Namecoin registration for { $hostname } has expired (last updated at
    block { $updateHeight }, current height { $currentHeight }). The TLSA
    record is no longer trustworthy.

namecoin-dane-error-no-tlsa =
    No DANE-TLSA record found for { $hostname } on the Namecoin blockchain.
    The connection cannot be verified via blockchain trust.

namecoin-dane-warning-http-fallback =
    { $hostname } has TLSA records on the blockchain but you are connecting
    over plain HTTP. Consider using HTTPS for blockchain-verified security.

namecoin-dane-warning-weak-match =
    The DANE-TLSA record for { $hostname } uses an exact match (matching
    type 0), which is less secure than hash-based matching. The domain owner
    should consider upgrading to SHA-256 or SHA-512 matching.

namecoin-dane-info-self-signed =
    The certificate for { $hostname } is self-signed but matches the
    DANE-EE record on the Namecoin blockchain. This is expected and secure
    for .bit domains using DANE trust.
