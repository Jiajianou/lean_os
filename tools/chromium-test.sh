#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SRC="$ROOT/build/chromium/src"
VENDORED=third_party/chromium/VENDORED
PATCHES=tools/chromium-port
BASELINE="$SRC/out/Baseline/headless_shell"

PASS=0
FAIL=0

check() {
  if [ "$1" = "0" ]; then
    echo "chromium-test: pass - $2"
    PASS=$((PASS + 1))
  else
    echo "chromium-test: FAIL - $2" >&2
    FAIL=$((FAIL + 1))
  fi
}

if [ ! -d "$SRC" ]; then
  echo "chromium-test: no checkout at $SRC - run tools/fetch-chromium.sh"
  exit 0
fi

REVISION=$(awk '/^REVISION /{print $2}' "$VENDORED")
HAVE=$(cd "$SRC" && git rev-parse HEAD 2>/dev/null)
[ "$REVISION" = "$HAVE" ]
check $? "the checkout is at the revision $VENDORED pins"

APPLIED=0
DRIFTED=0
for p in "$PATCHES"/*.patch; do
  [ -e "$p" ] || continue
  APPLIED=$((APPLIED + 1))
  if ! (cd "$SRC" && git apply --check -p1 < "$p") 2>/dev/null; then
    echo "chromium-test: $(basename "$p") does not apply to $REVISION" >&2
    DRIFTED=$((DRIFTED + 1))
  fi
done
[ "$DRIFTED" = "0" ]
check $? "all $APPLIED fork patches apply to the pinned revision"

if [ ! -x "$BASELINE" ]; then
  echo "chromium-test: no baseline build - skipping the render check"
else
  OUT=$("$BASELINE" --dump-dom --disable-gpu \
        "file://$ROOT/tests/chromium/dom.html" 2>/dev/null)
  echo "$OUT" | diff -q - tests/chromium/dom.expected >/dev/null 2>&1
  check $? "the baseline parses the fixture and V8 mutates its DOM"
fi

if [ ! -x "$ROOT/build/toolchain/bin/x86_64-lean_os-clang" ]; then
  echo "chromium-test: no cross toolchain - skipping the configure check"
else
  LEANOS_CHROMIUM_CONFIGURE_ONLY=1 LEANOS_CHROMIUM_CC=chromium \
      "$ROOT/tools/build-chromium.sh" base/third_party/double_conversion \
      > "$ROOT/build/chromium-configure.log" 2>&1
  check $? "the lean_os GN configuration generates with no patch to Chromium"
fi

echo "chromium-test: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ]
