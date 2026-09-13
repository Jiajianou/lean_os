#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."

OUT=build/iconv-test
INC=$OUT/inc
rm -rf "$OUT"
mkdir -p "$INC"
cp user_space/libc/include/iconv.h "$INC/iconv.h"

HOSTCC="${HOSTCC:-cc}"

if ! python3 tools/gen-iconv-tables.py "$OUT/iconv_tables.c.generated" >/dev/null; then
  echo "iconv-test: the table generator failed" >&2
  exit 1
fi
if ! cmp -s "$OUT/iconv_tables.c.generated" user_space/libc/src/iconv_tables.c; then
  echo "iconv-test: user_space/libc/src/iconv_tables.c is not what" >&2
  echo "            tools/gen-iconv-tables.py produces. It says GENERATED" >&2
  echo "            at the top; edit the generator, not the table." >&2
  diff user_space/libc/src/iconv_tables.c "$OUT/iconv_tables.c.generated" | head -20 >&2
  exit 1
fi

DEFS="-Diconv_open=lean_iconv_open -Diconv_close=lean_iconv_close"
DEFS="$DEFS -Diconv=lean_iconv -Diconv_t=lean_iconv_t"

$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -I "$INC" -I user_space/libc/src $DEFS \
  -o "$OUT/iconv.o" user_space/libc/src/iconv.c || exit 1
$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -I "$INC" -I user_space/libc/src $DEFS \
  -o "$OUT/iconv_tables.o" user_space/libc/src/iconv_tables.c || exit 1
$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -o "$OUT/main.o" tests/iconv/main.c || exit 1

if ! $HOSTCC -o "$OUT/iconv-test" "$OUT/iconv.o" "$OUT/iconv_tables.o" \
     "$OUT/main.o" 2>/dev/null; then
  $HOSTCC -o "$OUT/iconv-test" "$OUT/iconv.o" "$OUT/iconv_tables.o" \
     "$OUT/main.o" -liconv || exit 1
fi

"$OUT/iconv-test"
rc=$?
if [ "$rc" -ne 0 ]; then
  echo
  echo "FAIL - this iconv disagrees with the host's. A charset table is"
  echo "       generated (tools/gen-iconv-tables.py); a disagreement is"
  echo "       either a bug in the engine or a charset this libc should"
  echo "       not be claiming to have."
  exit 1
fi
