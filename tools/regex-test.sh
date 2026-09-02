#!/usr/bin/env bash
# tools/regex-test.sh - M89: grade this project's regex engine against a
# real one.
#
# ---- why this exists, and why it is the shape it is -------------------
#
# The same argument tools/sh-test.sh (M86) makes about the shell, applied
# to the one other place in this tree where "correct" is defined by
# agreement rather than by taste. A regular expression engine's whole job
# is to decide the same spans every other engine decides; a test written
# here that asserts what this engine happens to do would pass forever
# while being wrong, and nobody would find out until sed replaced the
# wrong half of a line.
#
# So nothing in the fixture list says what the right answer is. The
# host's own <regex.h> - which on this machine is a BSD implementation
# nobody here wrote - decides, and this engine has to agree with it
# character for character, including the offsets of every captured
# subexpression.
#
# What this CANNOT see is anything about lean_os: the engine is pure
# computation over a string, so a host build tests all of it. That is
# unusual in this tree and is the reason this is a shell script rather
# than a boot marker.
#
# Usage:
#   tools/regex-test.sh            # every case
#   tools/regex-test.sh 'a*'       # only cases whose pattern matches this
set -uo pipefail

cd "$(dirname "$0")/.."

HOSTCC="${HOSTCC:-cc}"
BUILD=build
FILTER="${1:-}"
mkdir -p "$BUILD"

# Two programs, same source, different engine. -DUSE_HOST_REGEX picks the
# host's <regex.h>; without it, this project's own is compiled straight
# in. The driver is identical either way, which is what makes a
# difference in output a difference in the engine.
DRIVER="$BUILD/regex-driver.c"
cat > "$DRIVER" <<'EOF'
/* Built twice - see tools/regex-test.sh. Reads "flags<TAB>pattern<TAB>
 * subject" lines and prints the match offsets, or "nomatch", or the
 * compile error. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef USE_HOST_REGEX
#include <regex.h>
#else
#include "../user_space/libc/include/regex.h"
#endif

int main(void) {
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *flags = line;
        char *pat = strchr(flags, '\t');
        if (!pat) continue;
        *pat++ = 0;
        char *subj = strchr(pat, '\t');
        if (!subj) continue;
        *subj++ = 0;

        int cflags = 0, eflags = 0;
        for (char *f = flags; *f; f++) {
            if (*f == 'E') cflags |= REG_EXTENDED;
            if (*f == 'I') cflags |= REG_ICASE;
            if (*f == 'N') cflags |= REG_NEWLINE;
            if (*f == 'S') cflags |= REG_NOSUB;
            if (*f == 'b') eflags |= REG_NOTBOL;
            if (*f == 'e') eflags |= REG_NOTEOL;
        }
        /* "\n" in a subject means a real newline - the fixtures have to
         * be one line each. */
        char sbuf[4096];
        size_t si = 0;
        for (char *s = subj; *s && si < sizeof(sbuf)-1; s++) {
            if (s[0] == '\\' && s[1] == 'n') { sbuf[si++] = '\n'; s++; }
            else sbuf[si++] = *s;
        }
        sbuf[si] = 0;

        regex_t re;
        int rc = regcomp(&re, pat, cflags);
        if (rc) {
            /* The message text differs between implementations by
             * design; what must agree is THAT it failed. */
            printf("error\n");
            continue;
        }
        regmatch_t pm[10];
        for (int i = 0; i < 10; i++) pm[i].rm_so = pm[i].rm_eo = -1;
        rc = regexec(&re, sbuf, 10, pm, eflags);
        if (rc) {
            printf("nomatch\n");
        } else if (cflags & REG_NOSUB) {
            printf("match\n");
        } else {
            printf("%ld-%ld", (long)pm[0].rm_so, (long)pm[0].rm_eo);
            for (size_t i = 1; i <= re.re_nsub && i < 10; i++)
                printf(" %ld-%ld", (long)pm[i].rm_so, (long)pm[i].rm_eo);
            printf("\n");
        }
        regfree(&re);
    }
    return 0;
}
EOF

OURS="$BUILD/regex-ours"
THEIRS="$BUILD/regex-theirs"

# Two compile steps rather than one, and the reason is worth a line: the
# engine has to see this project's <regex.h>, <ctype.h> and <string.h>,
# and the driver must NOT - it uses the host's stdio, and our <stdio.h>
# declares a `stdin` that nothing on the host defines. So the engine is
# compiled with -Iuser_space/libc/include and the driver without it,
# reaching our header by relative path instead.
if ! $HOSTCC -std=c11 -O1 -g -c -o "$BUILD/regex-engine.o" user_space/libc/src/regex.c \
     -I user_space/libc/include 2>"$BUILD/regex-ours.log"; then
  echo "regex-test: this project's engine does not compile for the host:" >&2
  cat "$BUILD/regex-ours.log" >&2
  exit 1
fi
if ! $HOSTCC -std=c11 -O1 -g -o "$OURS" "$DRIVER" "$BUILD/regex-engine.o" \
     2>>"$BUILD/regex-ours.log"; then
  echo "regex-test: this project's engine does not compile for the host:" >&2
  cat "$BUILD/regex-ours.log" >&2
  exit 1
fi
if ! $HOSTCC -std=c11 -O1 -DUSE_HOST_REGEX -o "$THEIRS" "$DRIVER" 2>"$BUILD/regex-theirs.log"; then
  echo "regex-test: the reference driver does not compile:" >&2
  cat "$BUILD/regex-theirs.log" >&2
  exit 1
fi

FIXTURES=tests/regex/cases.tsv
pass=0
fail=0
failed=""
# Lines beginning '#' are comments, and two of them are cases this file
# deliberately does not grade - see the note in cases.tsv about the
# questions POSIX leaves undefined.
while IFS= read -r line; do
  case "$line" in ''|'#'*) continue;; esac
  if [ -n "$FILTER" ]; then
    case "$line" in *"$FILTER"*) ;; *) continue;; esac
  fi
  got=$(printf '%s\n' "$line" | "$OURS" 2>/dev/null)
  want=$(printf '%s\n' "$line" | "$THEIRS" 2>/dev/null)
  if [ "$got" = "$want" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    failed="$failed
  case:      $line
  reference: $want
  ours:      $got"
  fi
done < "$FIXTURES"

if [ "$fail" -gt 0 ]; then
  echo "FAIL: $fail of $((pass+fail)) case(s) disagree with the host's regex:$failed" >&2
  exit 1
fi
echo "PASS: $pass regex case(s) agree with the host's own engine."
