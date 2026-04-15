# Phase 1: Proof-of-Concept Gecko DNS Patch

**Goal:** DNS resolution only (.bit → HTTP, no TLS yet)  
**Target:** Firefox Nightly (mozilla-central)  
**Status:** In progress  
**Flag:** `network.namecoin.enabled = false` (off by default)

## What This Phase Delivers

- `nsNamecoinResolver` C++ class: ElectrumX WebSocket client embedded in Gecko
- Hook into `nsHostResolver` to route `.bit` queries to `nsNamecoinResolver`
- `effective_tld_names.dat` patch: adds `bit` as a proper eTLD
- `about:config` preferences for server configuration
- Basic connection pool with health checking + failover
- DNS result caching with TTL based on name expiry
- Phabricator-ready patch series

## Out of Scope for Phase 1

- TLS / DANE-TLSA validation (Phase 2)
- UI indicators (Phase 3)
- Android GeckoView work (Phase 4)
- Default-on enablement (Phase 5)

## Files

| File | Description |
|------|-------------|
| `patches/0001-etld-bit.patch` | Add `bit` to effective_tld_names.dat |
| `patches/0002-namecoin-prefs.patch` | Add about:config preferences |
| `patches/0003-nsNamecoinResolver.patch` | New resolver class (C++) |
| `patches/0004-nsHostResolver-hook.patch` | Route .bit queries to resolver |
| `src/nsNamecoinResolver.h` | C++ header (to copy into netwerk/dns/) |
| `src/nsNamecoinResolver.cpp` | C++ implementation |
| `tools/test-resolution.mjs` | Node.js test: verify scripthash + resolution |
| `tools/build-patch-series.sh` | Helper to generate .patch files from src/ |

## Building Against mozilla-central

```bash
# Clone Firefox source (large — ~10GB)
hg clone https://hg.mozilla.org/mozilla-central
# OR use Git mirror:
git clone https://github.com/mozilla/gecko-dev

# Copy our new files
cp gecko-namecoin/src/nsNamecoinResolver.{h,cpp} netwerk/dns/

# Apply patches
cd mozilla-central
patch -p1 < ../gecko-namecoin/patches/0001-etld-bit.patch
patch -p1 < ../gecko-namecoin/patches/0002-namecoin-prefs.patch
patch -p1 < ../gecko-namecoin/patches/0004-nsHostResolver-hook.patch

# Build (requires ~1h first build)
./mach build
./mach run
```

## Phabricator Submission

Patches should be submitted to https://phabricator.services.mozilla.com/
with the following metadata:

- **Summary:** Namecoin (.bit) domain resolution — Phase 1: DNS
- **Bug:** (link to Bugzilla meta-bug once filed)
- **Reviewers:** necko-reviewers (networking team)
- **Tags:** necko, dns, namecoin
- **Test Plan:** Run `tools/test-resolution.mjs`, navigate to known .bit domains
