#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SRC="$ROOT/build/chromium/src"
VENDORED=third_party/chromium/VENDORED
PATCHES=tools/chromium-port
BASELINE="$SRC/out/Baseline/headless_shell"
OUT_NAME="${LEANOS_CHROMIUM_OUT:-LeanOS}"
PREFIX="$ROOT/build/toolchain/bin/x86_64-lean_os-"

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
for p in "$ROOT/$PATCHES"/*.patch; do
  [ -e "$p" ] || continue
  APPLIED=$((APPLIED + 1))
  if (cd "$SRC" && git apply --check -p1 < "$p") 2>/dev/null; then
    continue
  fi
  # Already on the tree is not drift. tools/build-chromium.sh leaves the
  # series applied, so the question this check asks is "does the fork still
  # fit the pinned revision", not "is the tree pristine".
  if (cd "$SRC" && git apply --check --reverse -p1 < "$p") 2>/dev/null; then
    continue
  fi
  echo "chromium-test: $(basename "$p") neither applies to nor is applied" \
       "to $REVISION" >&2
  DRIFTED=$((DRIFTED + 1))
done
[ "$DRIFTED" = "0" ]
check $? "all $APPLIED fork patches fit the pinned revision"

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
  check $? "the lean_os GN configuration generates"

  RUST_SYSROOT="$ROOT/build/rust-sysroot-lean_os"
  TARGET_LIBDIR="$RUST_SYSROOT/lib/rustlib/x86_64-unknown-lean_os/lib"
  if [ ! -x "$RUST_SYSROOT/bin/rustc" ]; then
    echo "chromium-test: no Rust sysroot - run tools/build-rust-sysroot.sh"
  else
    REPORTED=$("$RUST_SYSROOT/bin/rustc" --print target-libdir \
        --target x86_64-unknown-lean_os 2>/dev/null)
    [ "$REPORTED" = "$TARGET_LIBDIR" ]
    check $? "rustc resolves x86_64-unknown-lean_os by name and finds its libraries"

    ls "$TARGET_LIBDIR"/libstd-*.rlib > /dev/null 2>&1
    check $? "the standard library is in that sysroot"
  fi

  CHROMIUM_LIBDIR="$SRC/out/$OUT_NAME/local_rustc_sysroot/lib/rustlib"
  CHROMIUM_STD="$CHROMIUM_LIBDIR/x86_64-unknown-lean_os/lib/libstd_std.rlib"
  if [ ! -f "$CHROMIUM_STD" ]; then
    echo "chromium-test: Chromium has not built std - skipping the rlib check"
  else
    # This rlib came out of CHROMIUM'S build of the forked standard library
    # rather than out of cargo, and the objects in it have to be for this
    # machine - which is the whole claim the Rust half of the port makes.
    WORK=$(mktemp -d)
    MEMBER=$("${PREFIX}ar" t "$CHROMIUM_STD" | grep -F ".o" | head -1)
    ( cd "$WORK" && "${PREFIX}ar" x "$CHROMIUM_STD" "$MEMBER" &&
      "${PREFIX}readelf" -h "$MEMBER" |
        grep -q "Advanced Micro Devices X86-64" )
    check $? "Chromium's own build of std for x86_64-unknown-lean_os is x86-64 ELF"
    rm -rf "$WORK"
  fi
fi

echo "chromium-test: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ]
