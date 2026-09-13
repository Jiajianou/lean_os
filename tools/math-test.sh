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

NAMES=$(grep -oE '^(double|float|long long|long) [a-z0-9_]+\(' "$HDR" | sed 's/^long long //; s/^long //; s/^double //; s/^float //; s/($//; s/(//' | sort -u)
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

$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -I user_space/libc/include $DEFS \
  -o "$OUT/math.o" "$SRC" || exit 1
$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -o "$OUT/main.o" tests/math/main.c || exit 1
$HOSTCC -o "$OUT/math-test" "$OUT/math.o" "$OUT/main.o" -lm || exit 1

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
