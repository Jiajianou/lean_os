#!/usr/bin/env bash
# tools/scanf-test.sh - M89: grade this project's scanf against a real one.
#
# ---- why this exists, and why it is the shape it is -------------------
#
# The third program in this tree whose whole job is to agree with every
# other implementation of itself, after the shell (M86) and the regular
# expression engine (M89's other half). The argument is the same one
# tools/sh-test.sh makes and is worth restating because it is the reason
# both of those found bugs on their first run: a test written here that
# asserted what this scanf happens to do would pass forever while being
# wrong, and the first thing to notice would be a ported program
# misreading a number.
#
# So nothing in the fixture file says what the right answer is. The
# host's own sscanf decides, and this one has to agree with it - on the
# return value AND on every converted value, because a scanf that
# returns 2 having written the wrong two is the failure that looks like
# success.
#
# ---- how one driver can test a variadic function ---------------------
#
# sscanf's arguments are typed by its format string, which a driver
# cannot build dynamically in portable C. So the driver always passes
# four pointers to a union wide enough for any conversion, and then
# re-reads the format to decide how to PRINT what was written. scanf
# only touches as many arguments as it converts, so passing four is safe
# for a format with one - and four is more conversions than any fixture
# here needs.
#
# What this cannot see is anything about lean_os: sscanf over a string is
# pure computation, so a host build tests all of it. That is why this is
# a shell script and not a boot marker - the same reason the regex test
# gives.
#
# Usage:
#   tools/scanf-test.sh          # every case
#   tools/scanf-test.sh '%x'     # only cases whose line matches this
set -uo pipefail

cd "$(dirname "$0")/.."

HOSTCC="${HOSTCC:-cc}"
BUILD=build
FILTER="${1:-}"
mkdir -p "$BUILD"

DRIVER="$BUILD/scanf-driver.c"
cat > "$DRIVER" <<'EOF'
/* Built twice - see tools/scanf-test.sh. Reads "format<TAB>input" lines
 * and prints the return value and every converted value. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_OURS
/* Declared rather than included: this project's <stdio.h> declares a
 * `stdin` the host does not define, which is the same collision
 * tools/regex-test.sh documents and solves the same way. */
int lean_sscanf(const char *str, const char *fmt, ...);
/* scanf() reads this project's `stdin`, which is a FILE* this library
 * defines and the host does not. Nothing here calls scanf() - every
 * fixture goes through sscanf - so the symbol only has to exist. */
void *lean_stdin;
#define SSCANF lean_sscanf
#else
#define SSCANF sscanf
#endif

typedef union {
    long long ll;
    long l;
    int i;
    short h;
    signed char hh;
    size_t z;
    unsigned long long ull;
    double d;
    float f;
    char s[512];
} slot_t;

/* Walks the format and reports, for conversion `want`, a letter saying
 * how to print what was written: 'd' signed, 'u' unsigned, 'f' float,
 * 'g' double, 's' string, 'c' one character, or 0 for none. Suppressed
 * conversions (%*d) consume no argument and are skipped, exactly as
 * scanf skips them. */
static char slot_kind(const char *fmt, int want, int *out_width) {
    int n = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        int suppress = 0, width = 0, lng = 0;
        if (*p == '*') { suppress = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width*10 + (*p - '0'); p++; }
        while (*p=='h'||*p=='l'||*p=='z'||*p=='j'||*p=='t') { if (*p=='l'||*p=='z'||*p=='j'||*p=='t') lng++; p++; }
        char c = *p;
        if (c == '[') { while (*p && *p != ']') p++; }
        if (suppress) continue;
        if (n == want) {
            *out_width = width;
            switch (c) {
            case 'd': case 'i': case 'n': return 'd';
            case 'u': case 'o': case 'x': case 'X': return 'u';
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
                return lng ? 'g' : 'f';
            case 's': case '[': return 's';
            case 'c': return 'c';
            default: return 0;
            }
        }
        n++;
    }
    return 0;
}

/* How wide the object a conversion writes is, so the union can be read
 * back at the right width. Only the length modifier decides this. */
static int slot_len(const char *fmt, int want) {
    int n = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        int suppress = 0, len = 0;
        if (*p == '*') { suppress = 1; p++; }
        while (*p >= '0' && *p <= '9') p++;
        if (*p == 'h') { p++; len = 1; if (*p == 'h') { p++; len = 2; } }
        else if (*p == 'l') { p++; len = 3; if (*p == 'l') { p++; len = 4; } }
        else if (*p == 'z') { p++; len = 5; }
        if (*p == '[') { while (*p && *p != ']') p++; }
        if (suppress) continue;
        if (n == want) return len;
        n++;
    }
    return 0;
}

int main(void) {
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *fmt = line;
        char *input = strchr(fmt, '\t');
        if (!input) continue;
        *input++ = 0;

        /* "\t" and "\n" in a fixture are the real characters - a case
         * file has to be one line each. */
        char ibuf[2048];
        size_t ii = 0;
        for (char *s = input; *s && ii < sizeof(ibuf)-1; s++) {
            if (s[0] == '\\' && s[1] == 'n') { ibuf[ii++] = '\n'; s++; }
            else if (s[0] == '\\' && s[1] == 't') { ibuf[ii++] = '\t'; s++; }
            else ibuf[ii++] = *s;
        }
        ibuf[ii] = 0;

        slot_t a, b, c, d;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        memset(&c, 0, sizeof c); memset(&d, 0, sizeof d);

        int rc = SSCANF(ibuf, fmt, &a, &b, &c, &d);
        printf("rc=%d", rc);
        slot_t *slots[4] = {&a, &b, &c, &d};
        for (int k = 0; k < 4 && k < rc; k++) {
            int width = 0;
            char kind = slot_kind(fmt, k, &width);
            int len = slot_len(fmt, k);
            switch (kind) {
            case 'd':
                if (len == 1) printf(" %ld", (long)slots[k]->h);
                else if (len == 2) printf(" %ld", (long)slots[k]->hh);
                else if (len == 3) printf(" %ld", slots[k]->l);
                else if (len == 4) printf(" %lld", slots[k]->ll);
                else if (len == 5) printf(" %llu", (unsigned long long)slots[k]->z);
                else printf(" %d", slots[k]->i);
                break;
            case 'u':
                if (len == 1) printf(" %lu", (unsigned long)(unsigned short)slots[k]->h);
                else if (len == 3) printf(" %lu", (unsigned long)slots[k]->l);
                else if (len == 4) printf(" %llu", slots[k]->ull);
                else printf(" %u", (unsigned)slots[k]->i);
                break;
            case 'f': printf(" %.6f", (double)slots[k]->f); break;
            case 'g': printf(" %.6f", slots[k]->d); break;
            case 's': printf(" [%s]", slots[k]->s); break;
            case 'c': {
                int w = width ? width : 1;
                printf(" [");
                for (int q = 0; q < w; q++) putchar(slots[k]->s[q]);
                printf("]");
                break;
            }
            default: printf(" ?"); break;
            }
        }
        printf("\n");
    }
    return 0;
}
EOF

OURS="$BUILD/scanf-ours"
THEIRS="$BUILD/scanf-theirs"

# Two compile steps, for the reason tools/regex-test.sh gives at length:
# the engine must see this project's headers and the driver must not.
# scanf.c's `sscanf` is renamed on the way in so that the host's own is
# still available to the driver's printf side, and so that the two
# binaries differ in exactly one symbol.
if ! $HOSTCC -std=c11 -O1 -g -c -o "$BUILD/scanf-engine.o" user_space/libc/src/scanf.c \
     -I user_space/libc/include -Dsscanf=lean_sscanf -Dvsscanf=lean_vsscanf \
     -Dfscanf=lean_fscanf -Dscanf=lean_scanf -Dstdin=lean_stdin \
     2>"$BUILD/scanf-ours.log"; then
  echo "scanf-test: this project's scanf does not compile for the host:" >&2
  cat "$BUILD/scanf-ours.log" >&2
  exit 1
fi
if ! $HOSTCC -std=c11 -O1 -g -DUSE_OURS -o "$OURS" "$DRIVER" "$BUILD/scanf-engine.o" \
     2>>"$BUILD/scanf-ours.log"; then
  echo "scanf-test: this project's scanf does not link for the host:" >&2
  cat "$BUILD/scanf-ours.log" >&2
  exit 1
fi
if ! $HOSTCC -std=c11 -O1 -o "$THEIRS" "$DRIVER" 2>"$BUILD/scanf-theirs.log"; then
  echo "scanf-test: the reference driver does not compile:" >&2
  cat "$BUILD/scanf-theirs.log" >&2
  exit 1
fi

FIXTURES=tests/scanf/cases.tsv
pass=0
fail=0
failed=""
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
  case:  $line
  ours:  $got
  host:  $want"
  fi
done < "$FIXTURES"

if [ "$fail" -ne 0 ]; then
  echo "FAIL: $fail of $((pass+fail)) scanf case(s) disagree with the host's own:$failed" >&2
  exit 1
fi
echo "PASS: $pass scanf case(s) agree with the host's own."
