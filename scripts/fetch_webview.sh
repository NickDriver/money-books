#!/usr/bin/env bash
# Vendor the webview/webview library (system WKWebView wrapper) for `make app`.
# Pin a commit/tag for reproducibility — webview ships no GitHub Releases.
set -euo pipefail

WV_REF="${WV_REF:-master}"   # override with a pinned commit/tag before release
DEST="src/vendor/webview"

cd "$(dirname "$0")/.."

if [ -d "$DEST/.git" ]; then
  echo "webview already vendored at $DEST (ref: $(git -C "$DEST" rev-parse --short HEAD))"
  exit 0
fi

echo "Cloning webview ($WV_REF) into $DEST ..."
rm -rf "$DEST"
git clone --depth 1 --branch "$WV_REF" https://github.com/webview/webview.git "$DEST" 2>/dev/null \
  || git clone --depth 1 https://github.com/webview/webview.git "$DEST"

echo "Done. webview.cc: $(test -f "$DEST/core/src/webview.cc" && echo present || echo MISSING)"
echo "Now run: make app"
