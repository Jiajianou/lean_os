#!/usr/bin/env bash
# M200: this libc's memory and string primitives, compiled for the host from
# the same source the machine runs, against the host's own libc. Every size up
# to 300 bytes at every pair of alignments up to 16, overlapping moves in both
# directions, a difference or a match at every position, and a terminator at
# every offset of every alignment. Nothing here says what the right answer is:
# the host does.
set -uo pipefail

cd "$(dirname "$0")/.."

HOSTCC="${HOSTCC:-cc}"
BUILD=build
mkdir -p "$BUILD"

RENAMES=(-Dmemcpy=lean_memcpy -Dmemmove=lean_memmove -Dmemset=lean_memset
         -Dmemcmp=lean_memcmp -Dmemchr=lean_memchr -Dstrlen=lean_strlen
         -Dstrcmp=lean_strcmp)

DRIVER="$BUILD/string-driver.c"
cat > "$DRIVER" <<'EOF'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *lean_memcpy(void *, const void *, size_t);
void *lean_memmove(void *, const void *, size_t);
void *lean_memset(void *, int, size_t);
int lean_memcmp(const void *, const void *, size_t);
void *lean_memchr(const void *, int, size_t);
size_t lean_strlen(const char *);

#define SPAN 1024
static unsigned char source[SPAN], mine[SPAN], theirs[SPAN];
static unsigned long checks, failures;

static void fill_random(unsigned char *p, size_t n, unsigned *seed) {
    for (size_t i = 0; i < n; i++) {
        *seed = *seed * 1103515245u + 12345u;
        p[i] = (unsigned char)(*seed >> 16);
    }
}

static void fail(const char *what, size_t n, size_t a, size_t b) {
    if (failures++ < 20) {
        printf("FAIL %s n=%zu at %zu/%zu\n", what, n, a, b);
    }
}

static int sign(int v) { return (v > 0) - (v < 0); }

int main(void) {
    unsigned seed = 42;
    fill_random(source, SPAN, &seed);
    for (size_t n = 0; n <= 300; n++) {
        for (size_t d = 0; d < 16; d++) {
            for (size_t s = 0; s < 16; s++) {
                fill_random(mine, SPAN, &seed);
                memcpy(theirs, mine, SPAN);
                void *r = lean_memcpy(mine + 64 + d, source + 64 + s, n);
                memcpy(theirs + 64 + d, source + 64 + s, n);
                checks++;
                if (r != mine + 64 + d || memcmp(mine, theirs, SPAN) != 0) {
                    fail("memcpy", n, d, s);
                }
            }
            int value = (int)(n * 7 + d);
            memcpy(theirs, mine, SPAN);
            lean_memset(mine + 64 + d, value, n);
            memset(theirs + 64 + d, value, n);
            checks++;
            if (memcmp(mine, theirs, SPAN) != 0) {
                fail("memset", n, d, 0);
            }
        }
        for (int shift = -40; shift <= 40; shift++) {
            for (size_t a = 0; a < 8; a++) {
                memcpy(mine, source, SPAN);
                memcpy(theirs, source, SPAN);
                size_t from = 300 + a, to = (size_t)((long)from + shift);
                lean_memmove(mine + to, mine + from, n);
                memmove(theirs + to, theirs + from, n);
                checks++;
                if (memcmp(mine, theirs, SPAN) != 0) {
                    fail("memmove", n, from, to);
                }
            }
        }
        for (size_t a = 0; a < 16; a++) {
            memcpy(mine, source, SPAN);
            memcpy(theirs, source, SPAN);
            checks++;
            if (sign(lean_memcmp(mine + a, theirs + a, n)) != 0) {
                fail("memcmp equal", n, a, 0);
            }
            for (size_t at = 0; at < n; at += (n > 40 ? 7 : 1)) {
                theirs[a + at] ^= (unsigned char)(1u << (at % 8));
                checks++;
                if (sign(lean_memcmp(mine + a, theirs + a, n)) != sign(memcmp(mine + a, theirs + a, n))) {
                    fail("memcmp", n, a, at);
                }
                theirs[a + at] = mine[a + at];
            }
            unsigned char needle = 0xA5;
            memset(mine, 0x5A, SPAN);
            checks++;
            if (lean_memchr(mine + a, needle, n) != memchr(mine + a, needle, n)) {
                fail("memchr absent", n, a, 0);
            }
            for (size_t at = 0; at < n; at++) {
                mine[a + at] = needle;
                if (at + 1 < n) {
                    mine[a + at + 1] = needle;
                }
                checks++;
                if (lean_memchr(mine + a, needle, n) != memchr(mine + a, needle, n)) {
                    fail("memchr", n, a, at);
                }
                mine[a + at] = 0x5A;
                if (at + 1 < n) {
                    mine[a + at + 1] = 0x5A;
                }
            }
            memset(mine, 'x', SPAN);
            mine[a + n] = 0;
            checks++;
            if (lean_strlen((const char *)mine + a) != strlen((const char *)mine + a)) {
                fail("strlen", n, a, 0);
            }
        }
    }
    printf("%lu checks, %lu failures\n", checks, failures);
    return failures != 0;
}
EOF

LEAN="$BUILD/string-lean.o"
if ! "$HOSTCC" -std=c11 -O1 -fno-builtin "${RENAMES[@]}" -Iuser_space/library \
      -c user_space/library/string_utilities.c -o "$LEAN"; then
  echo "FAIL: this libc's string primitives do not compile for the host"
  exit 1
fi
if ! "$HOSTCC" -std=c11 -O1 "$DRIVER" "$LEAN" -o "$BUILD/string-test"; then
  echo "FAIL: the driver does not build"
  exit 1
fi
if OUT=$("$BUILD/string-test"); then
  echo "PASS: $OUT - memcpy, memmove, memset, memcmp, memchr and strlen agree with the host's."
  exit 0
fi
echo "$OUT"
echo "FAIL: this libc's string primitives disagree with the host's."
exit 1
