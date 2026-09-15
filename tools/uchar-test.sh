#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."

OUT=build/uchar-test
INC=$OUT/inc
rm -rf "$OUT"
mkdir -p "$INC"

# Only the two headers, so everything else this file includes is the host's
# and the object links against the host's C library the way iconv-test.sh's
# does. The symbols are renamed for the same reason: a platform that DOES
# have <uchar.h> would otherwise have two of each.
cp user_space/libc/include/wchar.h "$INC/wchar.h"
cp user_space/libc/include/uchar.h "$INC/uchar.h"

HOSTCC="${HOSTCC:-cc}"

DEFS="-Dmbstate_t=lean_mbstate_t -Dmbrtowc=lean_mbrtowc -Dwcrtomb=lean_wcrtomb"
DEFS="$DEFS -Dmbrtoc16=lean_mbrtoc16 -Dc16rtomb=lean_c16rtomb"
DEFS="$DEFS -Dmbrtoc32=lean_mbrtoc32 -Dc32rtomb=lean_c32rtomb"
DEFS="$DEFS -Dchar16_t=lean_char16_t -Dchar32_t=lean_char32_t"

$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -I "$INC" $DEFS -o "$OUT/uchar.o" user_space/libc/src/uchar.c || exit 1
# wchar.c is not what this test is about and it is compiled here only
# because mbrtoc32 IS mbrtowc: its own warnings are the host toolchain's
# opinion of code written for a freestanding target.
$HOSTCC -O2 -std=c11 -w -c \
  -I "$INC" $DEFS -o "$OUT/wchar.o" user_space/libc/src/wchar.c || exit 1
$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -I "$INC" $DEFS -o "$OUT/main.o" tests/uchar/main.c || exit 1
$HOSTCC -o "$OUT/uchar-test" "$OUT/uchar.o" "$OUT/wchar.o" "$OUT/main.o" || exit 1

# The oracle. Generated on every run rather than checked in, because a
# fixture nobody can regenerate is a fixture somebody eventually edits.
if ! python3 tools/gen-uchar-cases.py "$OUT/cases.txt"; then
  echo "uchar-test: the case generator failed" >&2
  exit 1
fi

"$OUT/uchar-test" "$OUT/cases.txt"
rc=$?
if [ "$rc" -ne 0 ]; then
  echo
  echo "FAIL - this <uchar.h> disagrees with Python's own encoders. The"
  echo "       expectations come from chr(n).encode('utf-8') and"
  echo "       .encode('utf-16-le'); nothing in this tree decides them."
  exit 1
fi
