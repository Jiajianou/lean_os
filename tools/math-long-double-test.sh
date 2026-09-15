#!/usr/bin/env bash
# M142. The host half of grading this libm's long double family.
#
# There is no differential test here in the shape tools/math-test.sh has, and
# the reason is the host: this Mac is arm64, where `long double` IS `double`.
# A host libm cannot answer a question about a 64-bit mantissa it does not
# have. So the oracle is MPFR, reached through the cross compiler - GCC folds
# __builtin_<name>l() on constant arguments at compile time, with MPFR, at
# the TARGET's precision - and the answers travel into the image as .rodata.
#
# What this script grades, therefore, is the instrument rather than the
# library: that the table still regenerates byte for byte from the case file,
# and that every expected value in it really was folded. The library itself
# is graded on the machine, by /bin/mathltest, because a wrong answer at the
# sixty-fourth bit builds and links perfectly well.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

HEADER=user_space/libc/include/math.h
CASES=tests/math/long_double_cases.tsv
TABLE=user_space/binaries/math_long_double_table.c
OUT=build/math-long-double
CROSS="${CROSS:-x86_64-elf}"

mkdir -p "$OUT"

PASS=0
FAIL=0
check() {
  if [ "$1" = "0" ]; then
    echo "math-long-double-test: pass - $2"
    PASS=$((PASS + 1))
  else
    echo "math-long-double-test: FAIL - $2" >&2
    FAIL=$((FAIL + 1))
  fi
}

NAMES=$(grep -oE '^(long double|long long|long|int) [a-z0-9_]+l\(' "$HEADER" \
        | sed -E 's/^(long double|long long|long|int) //; s/\($//; s/\(//' \
        | sort -u)
if [ -z "$NAMES" ]; then
  echo "math-long-double-test: no long double declarations found in $HEADER" >&2
  echo "                       - the grep needs updating" >&2
  exit 2
fi

missing=0
for n in $NAMES; do
  if ! awk -F'\t' -v n="$n" '$1 == n {found=1} END {exit !found}' "$CASES"; then
    echo "math-long-double-test: $n is declared in $HEADER and has no row in" >&2
    echo "                       $CASES. Add one - a '-' row with a note is a" >&2
    echo "                       valid answer; a missing row is a function" >&2
    echo "                       nobody grades." >&2
    missing=1
  fi
done
[ "$missing" -eq 0 ]
check $? "every long double declaration in $HEADER has a row in $CASES"

stale=0
for n in $(awk -F'\t' '!/^#/ && NF > 1 {print $1}' "$CASES"); do
  echo "$NAMES" | grep -qx "$n" || {
    echo "math-long-double-test: $n has a row in $CASES and is not declared" >&2
    echo "                       in $HEADER." >&2
    stale=1
  }
done
[ "$stale" -eq 0 ]
check $? "every row in $CASES names a function this libc declares"

python3 tools/gen-math-long-double-table.py "$OUT/regenerated.c" || exit 1
cmp -s "$OUT/regenerated.c" "$TABLE"
check $? "$TABLE regenerates byte for byte from $CASES"

if ! command -v "$CROSS-gcc" > /dev/null 2>&1; then
  echo "math-long-double-test: no $CROSS-gcc - cannot check the fold"
else
  "$CROSS-gcc" -std=c11 -O1 -ffreestanding -fno-stack-protector -fno-pic \
      -mcmodel=large -mno-red-zone -Wall -Wextra -Werror \
      -I user_space/libc/include -I user_space/binaries \
      -c "$TABLE" -o "$OUT/table.o" 2> "$OUT/table.log"
  check $? "the reference table compiles, which is MPFR evaluating it"

  if [ -f "$OUT/table.o" ]; then
    # This is the whole instrument in one line. A __builtin_ call GCC could
    # not fold becomes a CALL to this libm, and this libm is what is being
    # graded - so an object with an undefined symbol in it is an exam that
    # marked itself.
    UNDEFINED=$("$CROSS-nm" -u "$OUT/table.o" | wc -l | tr -d ' ')
    [ "$UNDEFINED" = "0" ]
    check $? "the table references no symbol at all - every value was folded"

    VALUES=$(grep -c '__builtin_' "$TABLE")
    echo "math-long-double-test: $VALUES expected values, all of them MPFR's,"
    echo "                       at this target's own 64-bit mantissa."
  fi
fi

# The grader goes onto the image the way /bin/clangtest and /bin/ruststd do,
# and for the same reason: the kernel incbins every embedded program, and
# 12,840 answers is more data than a kernel should carry. $(IMAGE)'s recipe
# recreates the disk whenever the kernel is newer, so this runs after a build
# rather than as part of one.
IMAGE=build/os-image.bin
if [ "${LEANOS_NO_INSTALL:-}" = "1" ]; then
  echo "math-long-double-test: not installing (LEANOS_NO_INSTALL=1)"
elif [ ! -f "$IMAGE" ]; then
  echo "math-long-double-test: no $IMAGE yet - run make, then this again"
else
  make build/mathltest.elf > "$OUT/build.log" 2>&1 || {
    tail -20 "$OUT/build.log" >&2; exit 1; }
  make leanfs-put > /dev/null 2>&1 || exit 1
  build/leanfs-put "$IMAGE" build/mathltest.elf /bin/mathltest > /dev/null
  check $? "installed as /bin/mathltest - the [m142] boot self-test runs it"
fi

GRADED=$(awk -F'\t' '!/^#/ && NF > 1 && $2 != "-"' "$CASES" | wc -l | tr -d ' ')
DEFERRED=$(awk -F'\t' '!/^#/ && NF > 1 && $2 == "-"' "$CASES" | wc -l | tr -d ' ')
echo "math-long-double-test: $GRADED functions swept and $DEFERRED graded by"
echo "                       an identity on the machine; /bin/mathltest is"
echo "                       what runs the comparison, in the boot battery."

if [ "$FAIL" -ne 0 ]; then
  echo
  echo "FAIL - $FAIL of $((PASS + FAIL)) checks"
  exit 1
fi
echo "math-long-double-test: $PASS checks passed"
