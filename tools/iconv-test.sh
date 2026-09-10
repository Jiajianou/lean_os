#!/usr/bin/env bash
# tools/iconv-test.sh - M100: this project's iconv, against the host's.
#
# The seventh differential test, and the same instrument as
# tools/sh-test.sh, regex-test.sh, scanf-test.sh, printf-test.sh,
# math-test.sh and pkg-test.sh: compile the thing this OS runs for the
# machine you are sitting at, put it in one process beside somebody
# else's implementation of the same interface, and let theirs decide.
#
# ---- why iconv gets one of these, specifically ------------------------
#
# user_space/libc/src/iconv_tables.c is 3,328 numbers. A wrong one does
# not crash and does not look wrong: it renders a plausible letter in a
# script nobody on this project reads, on a page nobody here would open.
# There is no amount of reading the file that finds that. There is a
# program on this machine that already knows every one of those numbers,
# and it disagrees for free.
#
# ---- the include trick, which is the only awkward part ----------------
#
# iconv.c has to see THIS project's <iconv.h> and the HOST's <stdlib.h>,
# <string.h> and <errno.h> - it uses malloc, memset, strstr and the E*
# constants, and those are the host's at link time. So instead of
# -I user_space/libc/include (which would give it this project's stdio
# and stdlib too, the way math-test.sh's own header comment describes),
# a directory is built containing exactly one header: a copy of this
# project's iconv.h. Everything else resolves to the host's.
set -uo pipefail

cd "$(dirname "$0")/.."

OUT=build/iconv-test
INC=$OUT/inc
rm -rf "$OUT"
mkdir -p "$INC"
cp user_space/libc/include/iconv.h "$INC/iconv.h"

HOSTCC="${HOSTCC:-cc}"

# ---- the generated table is regenerated and compared ------------------
#
# tools/gen-iconv-tables.py writes iconv_tables.c from Python's codec
# tables. Running it here and requiring the result to be byte-identical
# to what is checked in means a hand edit to a generated file is a test
# failure rather than a surprise three months later. It is also the only
# way the "GENERATED - do not edit" banner is worth anything.
cp user_space/libc/src/iconv_tables.c "$OUT/iconv_tables.c.orig"
if ! python3 tools/gen-iconv-tables.py >/dev/null; then
  echo "iconv-test: the table generator failed" >&2
  exit 1
fi
if ! cmp -s "$OUT/iconv_tables.c.orig" user_space/libc/src/iconv_tables.c; then
  echo "iconv-test: user_space/libc/src/iconv_tables.c is not what" >&2
  echo "            tools/gen-iconv-tables.py produces. It says GENERATED" >&2
  echo "            at the top; edit the generator, not the table." >&2
  diff "$OUT/iconv_tables.c.orig" user_space/libc/src/iconv_tables.c | head -20 >&2
  exit 1
fi

# The renames, so both implementations live in one process. iconv_t is
# renamed too - it is a typedef in this project's header and would
# otherwise collide with the host's.
DEFS="-Diconv_open=lean_iconv_open -Diconv_close=lean_iconv_close"
DEFS="$DEFS -Diconv=lean_iconv -Diconv_t=lean_iconv_t"

# shellcheck disable=SC2086
$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -I "$INC" -I user_space/libc/src $DEFS \
  -o "$OUT/iconv.o" user_space/libc/src/iconv.c || exit 1
# shellcheck disable=SC2086
$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -I "$INC" -I user_space/libc/src $DEFS \
  -o "$OUT/iconv_tables.o" user_space/libc/src/iconv_tables.c || exit 1
$HOSTCC -O2 -std=c11 -Wall -Wextra -Werror -c \
  -o "$OUT/main.o" tests/iconv/main.c || exit 1

# macOS carries iconv in libc; most other systems need -liconv. Try the
# bare link first and add the library only if the bare one fails, so
# neither host needs a flag typed for the other.
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
