#!/usr/bin/env bash
# build-patch-series.sh — Generate Phabricator-ready patch series
#
# Usage:
#   ./tools/build-patch-series.sh /path/to/mozilla-central
#
# This script:
#   1. Copies nsNamecoinResolver.{h,cpp} to netwerk/dns/ in a Firefox tree
#   2. Verifies all 4 patches apply cleanly
#   3. Runs the moz.build sanity check
#   4. Optionally creates a mercurial/git branch for Phabricator submission
#
# Prerequisites:
#   - mozilla-central checkout at $FIREFOX_SRC
#   - patch, diff, and either hg or git

set -euo pipefail

GECKO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FIREFOX_SRC="${1:-}"

if [ -z "$FIREFOX_SRC" ]; then
  echo "Usage: $0 /path/to/mozilla-central"
  echo ""
  echo "Clone Firefox source first:"
  echo "  hg clone https://hg.mozilla.org/mozilla-central"
  echo "  # or"
  echo "  git clone https://github.com/mozilla/gecko-dev"
  exit 1
fi

if [ ! -f "$FIREFOX_SRC/netwerk/dns/nsHostResolver.cpp" ]; then
  echo "Error: $FIREFOX_SRC does not look like a Firefox source tree"
  echo "  (missing netwerk/dns/nsHostResolver.cpp)"
  exit 1
fi

echo "Firefox source: $FIREFOX_SRC"
echo "Gecko-namecoin: $GECKO_DIR"
echo ""

# 1. Copy new source files
echo "Step 1: Copying nsNamecoinResolver to netwerk/dns/"
cp "$GECKO_DIR/src/nsNamecoinResolver.h"   "$FIREFOX_SRC/netwerk/dns/"
cp "$GECKO_DIR/src/nsNamecoinResolver.cpp" "$FIREFOX_SRC/netwerk/dns/"
echo "  ✓ Copied"

# 2. Apply patches (in order)
echo ""
echo "Step 2: Applying patches..."
cd "$FIREFOX_SRC"

for patch in "$GECKO_DIR/patches"/00*.patch; do
  pname="$(basename "$patch")"
  echo "  → $pname"
  if patch --dry-run -p1 < "$patch" > /dev/null 2>&1; then
    patch -p1 < "$patch"
    echo "  ✓ Applied"
  else
    echo "  ⚠ Dry-run failed — patch may need offset adjustment"
    echo "    Check: patch -p1 --fuzz=3 < $patch"
    echo "    The patch file has context lines for guidance."
    echo "    Manually apply changes if needed."
  fi
done

# 3. Check moz.build is consistent
echo ""
echo "Step 3: Checking moz.build..."
if grep -q "nsNamecoinResolver.cpp" "$FIREFOX_SRC/netwerk/dns/moz.build"; then
  echo "  ✓ moz.build contains nsNamecoinResolver"
else
  echo "  ⚠ moz.build patch may not have applied — add nsNamecoinResolver.cpp manually"
  echo "    Location: netwerk/dns/moz.build, UNIFIED_SOURCES list"
fi

# 4. Check eTLD entry
echo ""
echo "Step 4: Checking effective_tld_names.dat..."
if grep -q "^bit$" "$FIREFOX_SRC/netwerk/dns/effective_tld_names.dat"; then
  echo "  ✓ 'bit' is in effective_tld_names.dat"
else
  echo "  ⚠ 'bit' not found in effective_tld_names.dat — add manually"
fi

# 5. Check prefs
echo ""
echo "Step 5: Checking StaticPrefList.yaml..."
if grep -q "network.namecoin.enabled" "$FIREFOX_SRC/modules/libpref/init/StaticPrefList.yaml"; then
  echo "  ✓ network.namecoin.* prefs found"
else
  echo "  ⚠ network.namecoin.* prefs not found — apply 0002 patch manually"
fi

# 6. Summary
echo ""
echo "═══════════════════════════════════════════════════════════"
echo "  Ready to build! Run from $FIREFOX_SRC:"
echo ""
echo "    ./mach build"
echo "    ./mach run"
echo ""
echo "  Then in about:config:"
echo "    network.namecoin.enabled = true"
echo ""
echo "  Navigate to a known .bit domain (e.g. testls.bit)"
echo "  to verify resolution works."
echo "═══════════════════════════════════════════════════════════"
