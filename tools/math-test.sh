#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

HDR=user_space/libc/include/math.h
SRC=user_space/libc/src/math.c
CASES=tests/math/cases.tsv
OUT=build/math-test
mkdir -p "$OUT"

HOSTCC="${HOSTCC:-cc}"

# `int` joined this list in M155, with ilogb and ilogbf: a return type the
# grep did not name was a declaration this harness could not see, and so a
# function it could not notice was ungraded.
#
# The long double family is graded by tools/math-long-double-test.sh and
# /bin/mathltest instead, for a reason that is about hosts rather than about
# those functions: on an arm64 Mac `long double` IS `double`, so the host
# libm this harness compares against has no answer to give, and on an x86_64
# Mac it has one at the right precision but this harness cannot rely on
# being run there. MPFR at the target's own precision is an oracle on both.
# Their declarations are filtered out here rather than left to match
# `^long `.
NAMES=$(grep -v 'long double' "$HDR" \
        | grep -oE '^(double|float|long long|long|int) [a-z0-9_]+\(' \
        | sed 's/^long long //; s/^long //; s/^double //; s/^float //; s/^int //; s/($//; s/(//' | sort -u)
if [ -z "$NAMES" ]; then
  echo "math-test: no declarations found in $HDR - the grep needs updating" >&2
  exit 2
fi

missing=0
for n in $NAMES; do
  if ! awk -F'\t' -v n="$n" '$1 == n {found=1} END {exit !found}' "$CASES"; then
    echo "math-test: $n is declared in $HDR and has no row in $CASES." >&2
    echo "           Add one - a '-' row with a note is a valid answer; a" >&2
    echo "           missing row is a function nobody grades." >&2
    missing=1
  fi
done
[ "$missing" -eq 0 ] || exit 1

DEFS=""
for n in $NAMES; do
  DEFS="$DEFS -D$n=lean_$n"
done
# nexttoward and nexttowardf are double and float functions whose SECOND
# argument is a long double, so the grep above filters them out with the
# long double family - and without a rename, math.o's definitions would
# replace the host's in this link and the edge sweep in main.c would grade
# this library against itself.
DEFS="$DEFS -Dnexttoward=lean_nexttoward -Dnexttowardf=lean_nexttowardf"
# The flags this library raises by hand are in this project's <fenv.h>
# layout; main.c's lean_test_feraiseexcept raises them at the host's bits,
# which is the only way one flag word means one thing on both an arm64 and
# an x86_64 host. See the comment above it.
DEFS="$DEFS -Dferaiseexcept=lean_test_feraiseexcept"

# The long double family is renamed too, although nothing here grades it
# directly - because math.c CALLS it. fdim is fdiml cast down, and so are
# rint, nearbyint, lrint, llrint, remainder, remquo, logb, ilogb, scalbn,
# scalbln and their float twins; lgamma and tgamma compute in logl, expl,
# sinl and powl. Unrenamed, those calls resolved to the HOST's long double
# libm - or were not calls at all, because the host compiler treats rintl
# and fabsl as builtins and emitted its own instructions - and this harness
# graded the host's fdiml under the name of this library's fdim. On an x86_64
# host that passed because the host's fdiml happens to be right at
# fdim(inf, inf) where its fdim is not; on an arm64 one fdiml IS fdim.
LD_NAMES=$(grep 'long double' "$HDR" \
           | grep -oE '^(long double|long long|long|int) [a-z0-9_]+\(' \
           | sed -E 's/^(long double|long long|long|int) //; s/\($//' | sort -u)
if [ -z "$LD_NAMES" ]; then
  echo "math-test: no long double declarations found in $HDR - the grep needs updating" >&2
  exit 2
fi
for n in $LD_NAMES; do
  DEFS="$DEFS -D$n=lean_$n"
  if ! grep -qE "^(ONE|TWO)\($n\)$|^FORWARD\([a-z ]+, $n," tests/math/host_long_double.c; then
    echo "math-test: $n is declared in $HDR and tests/math/host_long_double.c" >&2
    echo "           does not stand in for it - an arm64 host could not link" >&2
    echo "           math.c the day math.c calls it." >&2
    exit 1
  fi
done

# Which long double half answers those calls. Where the target's long double
# is the x87's 80 bits (an x86_64 Mac), it is THIS library's: math_long_double.c
# is x87 code and builds here as it does for the target. Where it is not (an
# arm64 Mac, where long double is double), that file cannot exist, and
# tests/math/host_long_double.c stands in: each lean_<name>l forwards to the
# host's <name>l and is COUNTED, so main.c knows which answers on this host
# are the host's rather than this library's and does not grade them as
# this library's - /bin/mathltest grades those on the target, on either host.
#
# Both files are compiled every time and linked together, and each is empty
# where the other answers - the choice is made by the TARGET of the compile
# (tests/math/long_double_library.c says how), not by a probe of the host
# here. That is for tools/cross-arch-test.sh, which repeats each compile for
# the other Mac with the same arguments: a choice made here would be one file
# for both architectures, and the arm64 Mac's cross stage would never build
# math_long_double.c the way an Intel Mac does. MATH_TEST_LONG_DOUBLE=host
# forces the stand-in on an x86_64 host too, which is how the arm64
# arrangement is exercised on a Mac that cannot run arm64.
LONG_DOUBLE_FLAGS=""
if [ "${MATH_TEST_LONG_DOUBLE:-}" = "host" ]; then
  LONG_DOUBLE_FLAGS="-DLEAN_MATH_TEST_LONG_DOUBLE_HOST"
fi

$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -I user_space/libc/include $DEFS \
  -o "$OUT/math.o" "$SRC" || exit 1
$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -I user_space/libc/include $DEFS $LONG_DOUBLE_FLAGS \
  -o "$OUT/long_double.o" tests/math/long_double_library.c || exit 1
$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c $LONG_DOUBLE_FLAGS \
  -o "$OUT/host_long_double.o" tests/math/host_long_double.c || exit 1

# The library under test reaches no host function <math.h> declares. A name
# math.c or math_long_double.c calls without a rename above links to the
# host's libm and is graded as this library's - which is how fdim was the
# host's fdiml until this check. (The stand-in is not checked: reaching the
# host is its job, and every call it makes is counted. long_double.o is
# empty where the stand-in answers, so it is checked everywhere.)
MATH_NAMES=$(printf '%s\n' $NAMES $LD_NAMES nexttoward nexttowardf)
leaks() {
  nm -u "$@" | sed -E 's/^[[:space:]]*_?//' | sort -u | grep -xF "$MATH_NAMES"
}
leaked=$(leaks "$OUT/math.o" "$OUT/long_double.o")
if [ -n "$leaked" ]; then
  echo "math-test: the library under test calls the host's" $leaked >&2
  echo "           - a name <math.h> declares that the renames above missed," >&2
  echo "           so this harness would grade the host's under this library's name." >&2
  exit 1
fi
# Under tools/cross-arch-test.sh the wrapper on PATH has just built the same
# two objects for the other Mac, under $CROSS_ARCH_ROOT/out - and that Mac's
# compiler may turn a different builtin into a libm call, or (from an arm64
# Mac) that object is the x87 library this Mac's run never checks. The stage
# grades its ledger and not this script's status, so a leak there is written
# to the ledger as the failed check it is.
if [ -n "${CROSS_ARCH:-}" ] && [ -n "${CROSS_ARCH_ROOT:-}" ]; then
  other=()
  for o in math.o long_double.o; do
    [ -f "$CROSS_ARCH_ROOT/out/$OUT/$o" ] && other+=("$CROSS_ARCH_ROOT/out/$OUT/$o")
  done
  if [ "${#other[@]}" -ne 2 ]; then
    printf 'FAIL\t%s\t%s\n    %s\n' "${CROSS_ARCH_LABEL:-math}" \
      "nm -u (the $CROSS_ARCH objects)" \
      "the $CROSS_ARCH build of math.o or long_double.o is not under $CROSS_ARCH_ROOT/out/$OUT" \
      >> "$CROSS_ARCH_ROOT/ledger"
  else
    leaked=$(leaks "${other[@]}")
    if [ -n "$leaked" ]; then
      printf 'FAIL\t%s\t%s\n    %s\n' "${CROSS_ARCH_LABEL:-math}" \
        "nm -u ${other[*]}" \
        "the $CROSS_ARCH library under test calls the host's $(echo $leaked)" \
        >> "$CROSS_ARCH_ROOT/ledger"
      echo "math-test: the $CROSS_ARCH library under test calls the host's" $leaked >&2
      exit 1
    fi
  fi
fi
$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -o "$OUT/main.o" tests/math/main.c || exit 1
# The one translation unit that sees THIS libc's <math.h>, so that the XSI
# constants can be compared against what the host's libm computes rather than
# against the host's copy of the same decimal literals. main.c above is
# deliberately compiled without the -I.
$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -I user_space/libc/include \
  -o "$OUT/constants.o" tests/math/constants.c || exit 1
$HOSTCC -o "$OUT/math-test" "$OUT/math.o" "$OUT/long_double.o" \
  "$OUT/host_long_double.o" "$OUT/main.o" "$OUT/constants.o" -lm || exit 1

# This harness runs on arm64 and x86_64 Macs, and one of them cannot run the
# other's binary - but it can build it. HOSTCC="cc -arch arm64"
# MATH_TEST_BUILD_ONLY=1 on an Intel Mac (or -arch x86_64 on Apple Silicon)
# is the check that the other host's build still compiles and links.
if [ "${MATH_TEST_BUILD_ONLY:-}" = "1" ]; then
  echo "math-test: built $OUT/math-test with $HOSTCC; not run (MATH_TEST_BUILD_ONLY=1)"
  exit 0
fi

"$OUT/math-test" "$CASES"
rc=$?

if [ "$rc" -ne 0 ]; then
  echo
  echo "FAIL - this libm disagrees with the host's by more than this"
  echo "       project claims it does. Either the function is wrong or the"
  echo "       claim in $CASES is; both are worth knowing and neither is"
  echo "       fixed by widening the tolerance without a note saying why."
  exit 1
fi
echo "math-test: $(grep -cvE '^#|^$' "$CASES") rows, every function this"
echo "           libc declares graded or explicitly not, against a libm"
echo "           nobody here wrote."
