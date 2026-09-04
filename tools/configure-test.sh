#!/usr/bin/env bash
# tools/configure-test.sh - M99: this shell, against somebody else's
# configure.
#
# Runs CPython's `./configure` twice against the same source tree with
# the same arguments - once with this project's own /bin/sh compiled for
# the host, once with the host's own /bin/sh - and requires the two runs
# to produce the same files.
#
# ---- why this is a different instrument from tools/sh-test.sh ---------
#
# sh-test grades fixtures somebody here wrote, against an oracle nobody
# here wrote. That is the right shape and it has a ceiling: a fixture can
# only test a construct someone thought of, and the whole difficulty with
# a shell is the constructs nobody thinks of. Every one of the eleven
# bugs M99 found in this shell was in that category - a here-document on
# a line with `||`, a backtick inside double quotes inside a
# here-document, `${*-Setup}`, `set -e` on line 2 of a script. None of
# them was reachable from anything this project had written, and all of
# them are ordinary in a generated script.
#
# A 33,000-line configure is not a fixture. It is a program that uses the
# shell the way shell scripts are actually written, and it comes with its
# own definition of correct: **the same answers.** 753 checks, a 56 KB
# pyconfig.h, a 113 KB Makefile and a config.c - byte for byte.
#
# ---- what it does NOT prove -------------------------------------------
#
# That configure runs on lean_os. It runs the shell on the machine you
# are sitting at, so what it grades is the shell's own logic - the same
# separation tools/sh-test.sh's header draws, and for the same reason:
# what happens on the machine is the kernel's fork, its pipes, its
# `#!` handling, and a boot marker is what grades those.
#
# Usage: tools/configure-test.sh
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

# The same build of the shell tools/sh-test.sh grades, produced the same
# way - one file on the include path, for the reason that script's own
# comment gives.
SHINC="$ROOT/build/sh-host-include"
mkdir -p "$SHINC"
cp system_api/include/paths.h "$SHINC/paths.h"
if ! ${HOSTCC:-cc} -std=c11 -O1 -Wall -Wextra -Werror \
     -I"$SHINC" -o "$OURS" user_space/shell/sh.c 2>"$ROOT/build/sh-host.log"; then
  echo "configure-test: the shell does not compile for the host:" >&2
  cat "$ROOT/build/sh-host.log" >&2
  exit 1
fi

# ---- the arguments -----------------------------------------------------
#
# --without-ensurepip and --disable-ipv6 for no reason to do with this
# test: they are what tools/build-python.sh passes, so the path being
# exercised is the one this project actually uses. A configure run with
# no arguments would be a different 753 checks.
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

# ---- the comparison ----------------------------------------------------
#
# Three generated files, and the build directory's own path is
# normalised out of all of them - it is in the Makefile by design and is
# the one thing that is SUPPOSED to differ between two directories.
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
