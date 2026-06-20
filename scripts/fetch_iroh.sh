#!/usr/bin/env bash
# Vendor n0-computer/iroh-c-ffi (the C FFI for iroh QUIC) for `make share-lib`.
# Pinned to a release tag for reproducibility. This is the ONLY Rust dependency, and
# only the share/app-with-sharing build needs it — pure-C targets (test, mcp) don't.
set -euo pipefail

IROH_REF="${IROH_REF:-v0.101.0}"   # override before release if bumping iroh
DEST="src/vendor/iroh-c-ffi"

cd "$(dirname "$0")/.."

if [ -d "$DEST/.git" ]; then
  echo "iroh-c-ffi already vendored at $DEST (ref: $(git -C "$DEST" rev-parse --short HEAD))"
  exit 0
fi

echo "Cloning iroh-c-ffi ($IROH_REF) into $DEST ..."
rm -rf "$DEST"
git clone --depth 1 --branch "$IROH_REF" https://github.com/n0-computer/iroh-c-ffi.git "$DEST" 2>/dev/null \
  || git clone --depth 1 https://github.com/n0-computer/iroh-c-ffi.git "$DEST"

echo "Done. irohnet.h: $(test -f "$DEST/irohnet.h" && echo present || echo MISSING)"
echo "Now run: make share-lib   (first build compiles the full iroh tree — slow)"
