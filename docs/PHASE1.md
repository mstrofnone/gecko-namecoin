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

---

## Phase 5: Enablement Strategy

Namecoin follows Mozilla's standard three-stage graduation path for shipping
new features. The feature is **default-off at every stage** until an explicit
decision to flip the default.

### Stages

| Stage | Channel | Discovery | Default | Gate |
|-------|---------|-----------|---------|------|
| A | Nightly | `about:config` only | off | `network.namecoin.enabled` |
| B | Beta+ | Settings toggle in Privacy & Security | off | `network.namecoin.settings_visible` (Normandy) |
| C | Release | Settings toggle present | off | Same prefs; default-on is separate |

### Key Prefs

- **`network.namecoin.enabled`** — Master switch for .bit resolution.
- **`network.namecoin.settings_visible`** — Controls whether the Settings UI
  toggle appears. Managed via Normandy experiment rollout.

### Feature Gate Module

`toolkit/modules/NamecoinFeature.sys.mjs` provides the JS API:

```js
import { NamecoinFeature } from "resource://gre/modules/NamecoinFeature.sys.mjs";

NamecoinFeature.isActive();          // Should resolution happen?
NamecoinFeature.isSettingsVisible(); // Should UI show the toggle?
NamecoinFeature.getStage();          // "nightly" | "beta" | "release"
```

### Graduation: Nightly → Beta → Release

1. **Nightly (Stage A)**
   - Land patches, feature is about:config only.
   - Developers and early testers validate.
   - Telemetry: `namecoin.settings_visible` (should be false).

2. **Beta (Stage B)**
   - Create Normandy experiment: `namecoin-stage-b-beta-rollout`
   - Experiment sets `network.namecoin.settings_visible = true` for treatment arm.
   - Start with 1% rollout, increase to 10%, then 50% over 2–3 Beta cycles.
   - Monitor: `namecoin.enabled_changed` scalar for engagement.
   - Success criteria: <0.1% crash increase, <50ms p95 DNS latency delta.

3. **Release (Stage C)**
   - Once Beta metrics are green for 2 consecutive cycles:
   - Ship `network.namecoin.settings_visible = true` as default (remove Normandy).
   - Toggle remains default-off — users opt in via Settings.
   - Future: separate decision to flip `network.namecoin.enabled = true`.

### Normandy Experiment IDs (Placeholders)

| Experiment | Channel | Description |
|------------|---------|-------------|
| `namecoin-stage-b-beta-rollout` | Beta | Settings toggle visibility |
| `namecoin-stage-b-release-rollout` | Release | Settings toggle visibility |
| `namecoin-stage-c-default-on` | Release | Default-on (future) |

### Rollback Procedure

**Immediate (< 1 hour):**
1. Normandy: disable the experiment → `settings_visible` reverts to false.
2. Users who manually enabled keep their setting, but new users won't see the toggle.

**Full rollback:**
1. Set `network.namecoin.settings_visible` to false in StaticPrefList.yaml.
2. Optionally reset `network.namecoin.enabled` to false for all profiles via
   a Normandy "set pref" recipe targeting users where the pref is true.
3. Land a patch that adds `hidden: true` back to the Settings section.

**Emergency (security issue):**
1. All of the above, plus:
2. Remote Settings blocklist entry for the ElectrumX servers.
3. Hot-fix patch to disable the resolver entirely at the C++ level
   (`nsNamecoinResolver::IsEnabled()` returns false unconditionally).
