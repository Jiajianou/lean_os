#!/usr/bin/env bash
# tools/cross-arch-test.sh - the host-side test code builds for the OTHER Mac.
#
# Everything in the fast tier that is compiled for the host is compiled for
# one architecture: whichever Mac ran it. The host tests had only ever been
# built on arm64 until an Intel Mac ran them, and nothing would have said if
# a source, a header or a link line worked for one and not the other,
# because the Mac doing the grading was always the one where it worked.
#
# So this compiles AND LINKS the same code for the other architecture -
# arm64 on an Intel Mac, x86_64 on Apple Silicon - with Apple's own clang and
# SDK, which carry both. It runs nothing it builds (an Intel Mac cannot run
# arm64), so it is a claim about building and only about building: x86
# inline assembly that only an x86 host compiles into a host test links
# cleanly and faults when it runs, and that is the ordinary stages' to find,
# on the Mac in front of you.
#
#   1. The host unit tests: make's own test-fast binaries, with the Makefile's
#      own source lists, built by make itself with HOSTCC="cc -arch <other>"
#      into build/cross-arch/tests. The binary names are asked of make
#      (`make -qp`), not written down here.
#   2. The Makefile's own host tools - os-pkg, which tools/pkg-test.sh
#      grades, and the font and icon generators - the same way: make's rule,
#      an output path under build/cross-arch.
#   3. The differential tests' host compiles: each script is run as it always
#      is, with tools/cross-arch-cc.sh first on PATH as `cc`. That wrapper
#      makes every compile the script asks for exactly as asked, and then
#      once more with -arch <other> into build/cross-arch/out, writing what
#      happened to a ledger. A script's own verdict is not this stage's (the
#      ordinary stage grades that); the ledger is. Each script must have made
#      at least one compile, or the stage would be passing on nothing. The
#      scripts run side by side - each writes only its own files in build/ -
#      so this costs about the slowest of them rather than their sum.
#
# Not covered, and why: tools/tls-layout-test.sh compiles only with this
# project's cross compilers - there is no host compile in it to repeat;
# tools/math-long-double-test.sh is a target build, not a host one; and
# make's leanfs-put, for the dependency-file reason given at step 2.
#
# Only on macOS: elsewhere there is no "other architecture" this SDK carries.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

if [ "$(uname -s)" != "Darwin" ]; then
  echo "cross-arch-test: not a Mac - nothing to build for the other architecture; skipping"
  exit 0
fi
REAL_CC=$(command -v cc)
if [ -z "$REAL_CC" ]; then
  echo "cross-arch-test: no cc on PATH" >&2
  exit 1
fi

WORK="$ROOT/build/cross-arch"
rm -rf "$WORK"
mkdir -p "$WORK/bin" "$WORK/logs" "$WORK/out" "$WORK/ledger.d"

# "This Mac" is what cc builds when it is not told, because that is what
# every ordinary stage built and ran - not `uname -m`, which a shell running
# under Rosetta on Apple Silicon answers x86_64 while its cc may not.
printf 'int main(void) { return 0; }\n' > "$WORK/probe.c"
if ! "$REAL_CC" -o "$WORK/native" "$WORK/probe.c" 2> "$WORK/logs/probe.log"; then
  echo "cross-arch-test: FAIL - cc cannot link an empty program for this Mac:" >&2
  cat "$WORK/logs/probe.log" >&2
  exit 1
fi
NATIVE=$(lipo -archs "$WORK/native" 2>/dev/null)
case "$NATIVE" in
  x86_64) OTHER=arm64 ;;
  arm64)  OTHER=x86_64 ;;
  *) echo "cross-arch-test: cc builds '$NATIVE' here, neither of the two Macs this knows" >&2; exit 1 ;;
esac

# The SDK has to be able to do it at all before a failure below means
# anything about this tree.
if ! "$REAL_CC" -arch "$OTHER" -o "$WORK/probe" "$WORK/probe.c" 2>> "$WORK/logs/probe.log"; then
  echo "cross-arch-test: FAIL - this SDK cannot link even an empty program for $OTHER:" >&2
  cat "$WORK/logs/probe.log" >&2
  exit 1
fi
if [ "$(lipo -archs "$WORK/probe" 2>/dev/null)" != "$OTHER" ]; then
  echo "cross-arch-test: FAIL - -arch $OTHER did not produce an $OTHER program" >&2
  exit 1
fi
echo "cross-arch-test: this is $NATIVE; building the host test code for $OTHER"

FAIL=0

# ---- 1. the host unit tests, through make ---------------------------------
UNIT_DIR=build/cross-arch/tests
UNIT_BINS=$(make -qp TEST_BUILD="$UNIT_DIR" test-fast 2>/dev/null |
            sed -n 's/^test-fast: *//p' | head -1)
if [ -z "$UNIT_BINS" ]; then
  echo "cross-arch-test: FAIL - make named no prerequisites for test-fast" >&2
  FAIL=1
elif make --no-print-directory TEST_BUILD="$UNIT_DIR" \
       HOSTCC="$REAL_CC -arch $OTHER" $UNIT_BINS > "$WORK/logs/unit.log" 2>&1; then
  for b in $UNIT_BINS; do
    if [ "$(lipo -archs "$b" 2>/dev/null)" != "$OTHER" ]; then
      echo "cross-arch-test: FAIL - $b is not an $OTHER program" >&2
      FAIL=1
    fi
  done
  [ "$FAIL" -eq 0 ] &&
    echo "cross-arch-test: pass - the host unit tests ($(echo $UNIT_BINS | wc -w | tr -d ' ') binaries) build and link for $OTHER"
else
  echo "cross-arch-test: FAIL - the host unit tests do not build for $OTHER:" >&2
  # The diagnostics, not the command lines - every one of those says -Werror.
  grep -E 'error:|warning:|^ld: |Undefined symbols|\*\*\*' "$WORK/logs/unit.log" |
    head -20 | sed 's/^/  /' >&2
  FAIL=1
fi

# ---- 2. the Makefile's host tools ----------------------------------------
# os-pkg is what tools/pkg-test.sh grades (it asks make for it, and make
# compiles nothing when it is up to date - so it is built here, the same way
# as the unit tests: make's own rule, its own source list, another output
# path). gen-font and gen-icons are the generators the image is built with.
# leanfs-put is not: its rule writes a dependency file at a fixed path in
# build/, and a second build of it would rewrite the native one's.
for tool in OS_PKG:os-pkg GEN_FONT:gen-font GEN_ICONS:gen-icons; do
  var=${tool%%:*}
  out="build/cross-arch/${tool#*:}"
  if make --no-print-directory "$var=$out" HOSTCC="$REAL_CC -arch $OTHER" "$out" \
       > "$WORK/logs/${tool#*:}.log" 2>&1 &&
     [ "$(lipo -archs "$out" 2>/dev/null)" = "$OTHER" ]; then
    echo "cross-arch-test: pass - make's ${tool#*:} builds and links for $OTHER"
  else
    echo "cross-arch-test: FAIL - make's ${tool#*:} does not build for $OTHER:" >&2
    tail -20 "$WORK/logs/${tool#*:}.log" | sed 's/^/  /' >&2
    FAIL=1
  fi
done

# ---- 3. the differential tests' host compiles, through the wrapper --------
ln -sf "$ROOT/tools/cross-arch-cc.sh" "$WORK/bin/cc"
SCRIPTS="sh regex scanf math printf stdio iconv uchar realpath inet string intel-wireless"
pids=()
for s in $SCRIPTS; do
  (
    unset HOSTCC
    PATH="$WORK/bin:$PATH" CROSS_ARCH="$OTHER" CROSS_ARCH_ROOT="$WORK" \
      CROSS_ARCH_LABEL="$s" CROSS_ARCH_CC="$REAL_CC" \
      "./tools/$s-test.sh" > "$WORK/logs/$s.log" 2>&1
  ) &
  pids+=($!)
done
for p in "${pids[@]}"; do
  wait "$p"
done

# One file per compile (see cross-arch-cc.sh), gathered into one to read,
# oldest first so a script's compiles read in the order it made them.
LEDGER="$WORK/ledger"
( cd "$WORK/ledger.d" && ls -tr | while read -r f; do cat "$f"; done ) > "$LEDGER"
for s in $SCRIPTS; do
  ok=$(awk -F'\t' -v s="$s" '$1 == "ok" && $2 == s' "$LEDGER" | wc -l | tr -d ' ')
  bad=$(awk -F'\t' -v s="$s" '$1 == "FAIL" && $2 == s' "$LEDGER" | wc -l | tr -d ' ')
  if [ "$bad" -gt 0 ]; then
    echo "cross-arch-test: FAIL - tools/$s-test.sh: $bad of $((ok + bad)) host compile(s) fail for $OTHER:" >&2
    awk -v s="$s" '
      /^(ok|FAIL|NATIVE)\t/ { show = ($1 == "FAIL" && $2 == s) }
      show' FS='\t' "$LEDGER" | head -24 | sed 's/^/  /' >&2
    FAIL=1
  elif [ "$ok" -eq 0 ] && grep -q "^NATIVE	$s	" "$LEDGER"; then
    echo "cross-arch-test: FAIL - tools/$s-test.sh's host compile fails on this $NATIVE Mac itself, so there was nothing to build for $OTHER:" >&2
    tail -5 "$WORK/logs/$s.log" | sed 's/^/  /' >&2
    FAIL=1
  elif [ "$ok" -eq 0 ]; then
    echo "cross-arch-test: FAIL - tools/$s-test.sh made no host compile at all, so nothing was graded" >&2
    tail -5 "$WORK/logs/$s.log" | sed 's/^/  /' >&2
    FAIL=1
  else
    echo "cross-arch-test: pass - tools/$s-test.sh: $ok host compile(s) build for $OTHER"
  fi
done

if [ "$FAIL" -ne 0 ]; then
  echo "cross-arch-test: FAIL - logs in build/cross-arch/logs, the compiles in build/cross-arch/ledger"
  exit 1
fi
echo "cross-arch-test: everything compiled for the host here also builds for $OTHER"
