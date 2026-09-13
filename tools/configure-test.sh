#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PY_VER=3.12.7
SRC="$ROOT/build/toolchain-src/Python-$PY_VER"
OURS="$ROOT/build/sh-host"
REFERENCE_SH="${REFERENCE_SH:-/bin/sh}"
OUT="$ROOT/build/configure-test"

if [ ! -d "$SRC" ]; then
  echo "configure-test: no CPython source at $SRC - skipped."
  echo "                tools/build-python.sh downloads it (M99)."
  exit 0
fi
if [ ! -x "$REFERENCE_SH" ]; then
  echo "configure-test: no reference shell at $REFERENCE_SH - set REFERENCE_SH" >&2
  exit 1
fi

SHINC="$ROOT/build/sh-host-include"
mkdir -p "$SHINC"
cp system_api/include/paths.h "$SHINC/paths.h"
if ! ${HOSTCC:-cc} -std=c11 -O1 -Wall -Wextra -Werror \
     -I"$SHINC" -o "$OURS" user_space/shell/sh.c 2>"$ROOT/build/sh-host.log"; then
  echo "configure-test: the shell does not compile for the host:" >&2
  cat "$ROOT/build/sh-host.log" >&2
  exit 1
fi

ARGS=(--without-ensurepip --disable-ipv6)

run_one() {
  local shell=$1 dir=$2 label=$3
  rm -rf "$dir"
  mkdir -p "$dir"
  ( cd "$dir" && CONFIG_SHELL="$shell" "$shell" "$SRC/configure" "${ARGS[@]}" \
      > configure.out 2> configure.err )
  local rc=$?
  if [ $rc -ne 0 ]; then
    echo "configure-test: configure FAILED under $label (exit $rc)" >&2
    tail -12 "$dir/configure.err" >&2
    return 1
  fi
  return 0
}

echo "configure-test: running CPython $PY_VER's configure under this project's shell"
run_one "$OURS" "$OUT/ours" "this project's sh" || exit 1
echo "configure-test: ...and under $REFERENCE_SH, for the answers"
run_one "$REFERENCE_SH" "$OUT/reference" "$REFERENCE_SH" || exit 1

CHECKS=$(grep -c '^checking' "$OUT/ours/configure.out")
REFCHECKS=$(grep -c '^checking' "$OUT/reference/configure.out")
if [ "$CHECKS" != "$REFCHECKS" ]; then
  echo "configure-test: FAIL - $CHECKS checks under this shell, $REFCHECKS under $REFERENCE_SH" >&2
  exit 1
fi

FAIL=0
for f in pyconfig.h Modules/config.c Makefile; do
  sed "s,$OUT/ours,BUILDDIR,g" "$OUT/ours/$f"      > "$OUT/a.txt" 2>/dev/null
  sed "s,$OUT/reference,BUILDDIR,g" "$OUT/reference/$f" > "$OUT/b.txt" 2>/dev/null
  if [ ! -f "$OUT/ours/$f" ]; then
    echo "configure-test: FAIL - configure produced no $f under this shell" >&2
    FAIL=1
    continue
  fi
  if diff -q "$OUT/a.txt" "$OUT/b.txt" >/dev/null 2>&1; then
    echo "  same   $f ($(wc -c < "$OUT/ours/$f" | tr -d ' ') bytes)"
  else
    echo "configure-test: FAIL - $f differs:" >&2
    diff "$OUT/a.txt" "$OUT/b.txt" | head -20 >&2
    FAIL=1
  fi
done

if [ $FAIL -ne 0 ]; then
  exit 1
fi

echo "configure-test: $CHECKS checks, and every file byte-identical with $REFERENCE_SH's run."
exit 0
