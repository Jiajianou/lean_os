#!/usr/bin/env bash
# tools/math-test.sh - M99: this project's libm, against the host's.
#
# The same instrument as tools/sh-test.sh, regex-test.sh, scanf-test.sh
# and printf-test.sh, pointed at the fifth place in this tree where
# "correct" means "agrees with everybody else". Compile
# user_space/libc/src/math.c for THIS machine, put it in one process
# beside the host's own libm, and let the host decide what sin(0.7) is.
#
# ---- why the renaming, and why it is generated ------------------------
#
# Both libms have to be in the same process or the comparison is two runs
# of a program with a file between them. So this project's are compiled
# with -Dsin=lean_sin and so on, which renames the definitions AND the
# calls math.c makes to itself - erf calling exp gets lean_exp, which is
# what should be graded.
#
# The list is generated from math.h rather than written here, so a
# function added to this libc is renamed without anybody remembering to.
# The counterpart is the check below: every function DECLARED in math.h
# must have a row in tests/math/cases.tsv, or this fails. A libm function
# nobody grades is exactly what this harness exists to prevent, and the
# way that happens is not malice - it is a function added on a Tuesday.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

HDR=user_space/libc/include/math.h
SRC=user_space/libc/src/math.c
CASES=tests/math/cases.tsv
OUT=build/math-test
mkdir -p "$OUT"

HOSTCC="${HOSTCC:-cc}"

# Every `double name(` at the start of a line in the header - which is
# the shape every declaration in it has, and a declaration that stops
# having that shape stops being renamed, so the build fails loudly with a
# duplicate symbol rather than quietly grading the host against itself.
# M100: and `float name(` - the six float variants harfbuzz asked for.
# M100 again: and `long`/`long long` - the lround family libsvgtiny asked
# for, which are the first functions in this header that do not return a
# floating-point type. "long long" is stripped before "long" because the
# other order leaves a stray "long " on the front of the name and the
# lookup then silently finds nothing.
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

# Two compiles, and they are separate for a reason that cost a link
# error: this libc has its own <stdio.h> too, so a single command line
# with -I user_space/libc/include on it compiles the DRIVER against this
# project's headers, and the driver is supposed to be an ordinary host
# program. math.c gets this project's headers; main.c gets the host's,
# and never the two.
#
# The optimiser is left on because a libm compiled at -O0 is not the libm
# this project ships.
# shellcheck disable=SC2086
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
