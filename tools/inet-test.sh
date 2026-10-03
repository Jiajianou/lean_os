#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."

# M223: inet_ntop and inet_pton, both families, against the host's own. The
# text form of an IPv6 address is decided by an algorithm every C library
# inherited from BIND - which zero run becomes "::", when the last 32 bits are
# printed dotted - and the only useful oracle for that is somebody else's
# implementation of the same algorithm.

HOSTCC="${HOSTCC:-cc}"
BUILD=build
mkdir -p "$BUILD"

DRIVER="$BUILD/inet-driver.c"
cat > "$DRIVER" <<'EOF'
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static int hexbyte(const char *s) {
    unsigned v;
    return sscanf(s, "%2x", &v) == 1 ? (int)v : -1;
}

int main(void) {
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 'N') {
            unsigned char bytes[16];
            int family = line[1] == '4' ? AF_INET : AF_INET6;
            int count = family == AF_INET ? 4 : 16;
            for (int i = 0; i < count; i++) {
                bytes[i] = (unsigned char)hexbyte(line + 3 + 2 * i);
            }
            char text[INET6_ADDRSTRLEN];
            const char *r = inet_ntop(family, bytes, text, sizeof(text));
            printf("%s\n", r ? r : "NULL");
            char small[8];
            r = inet_ntop(family, bytes, small, sizeof(small));
            printf("%s\n", r ? "fits" : "NULL");
        } else if (line[0] == 'R') {
            unsigned char bytes[16];
            unsigned char back[16];
            for (int i = 0; i < 16; i++) {
                bytes[i] = (unsigned char)hexbyte(line + 3 + 2 * i);
            }
            char text[INET6_ADDRSTRLEN];
            const char *r = inet_ntop(AF_INET6, bytes, text, sizeof(text));
            int ok = r && inet_pton(AF_INET6, text, back) == 1 && memcmp(bytes, back, 16) == 0;
            printf("%s\n", ok ? "round trip" : "BROKEN");
        } else if (line[0] == 'P') {
            int family = line[1] == '4' ? AF_INET : AF_INET6;
            unsigned char bytes[16];
            memset(bytes, 0xAA, sizeof(bytes));
            int r = inet_pton(family, line + 3, bytes);
            printf("%d", r);
            if (r == 1) {
                for (int i = 0; i < (family == AF_INET ? 4 : 16); i++) {
                    printf("%02x", bytes[i]);
                }
            }
            printf("\n");
        }
    }
    return 0;
}
EOF

if ! $HOSTCC -std=c11 -O1 -g -I"$BUILD/inet-shim" -c -o "$BUILD/inet-engine.o" \
     user_space/libc/src/inet.c 2> "$BUILD/inet-engine.log"; then
  echo "inet-test: could not compile inet.c for the host:" >&2
  tail -20 "$BUILD/inet-engine.log" >&2
  exit 1
fi
$HOSTCC -std=c11 -O1 -g -o "$BUILD/inet-ours" "$DRIVER" "$BUILD/inet-engine.o" || exit 1
$HOSTCC -std=c11 -O1 -o "$BUILD/inet-theirs" "$DRIVER" || exit 1

CASES="$BUILD/inet-cases.txt"
python3 - "$CASES" <<'PY'
import random, sys
random.seed(223)
lines = []
fixed6 = ["::", "::1", "1::", "::ffff:1.2.3.4", "::1.2.3.4", "1:2:3:4:5:6:7:8",
          "1::8", "1:0:0:4::8", "FE80::ABCD", "1:2:3:4:5:6:1.2.3.4",
          "1:2:3:4:5:6:7::", "::2:3:4:5:6:7:8", ":1::", "1:::2", "1::2::3",
          "12345::", "1:2:3:4:5:6:7:8:9", "1:", ":", "", "::ffff:256.1.1.1",
          "::ffff:1.2.3", "1.2.3.4", "::0:0:0:0:0:0:0",
          "0:0:0:0:0:0:0:0", "2001:db8::ff00:42:8329", "2001:0db8:0000:0000:0000:ff00:0042:8329",
          "::ffff:0:1.2.3.4", "1:2:3:4:5:6:7:8:", "::1.2.3.4.5", "g::1", " ::1"]
fixed4 = ["0.0.0.0", "255.255.255.255", "1.2.3.4", "1.2.3", "1.2.3.4.5",
          "256.0.0.0", "1..2.3", "1.2.3.4 ", "", "a.b.c.d", 
          "127.1", "1.2.3.-4", "4294967295"]
for s in fixed6:
    lines.append("P6 " + s)
for s in fixed4:
    lines.append("P4 " + s)
for _ in range(4000):
    lines.append("N4 " + "".join("%02x" % random.randrange(256) for _ in range(4)))
shapes = [0, 0xffff, 1, 0x100, 0xabcd]
for _ in range(20000):
    words = []
    for _ in range(8):
        r = random.random()
        words.append(0 if r < 0.55 else random.choice(shapes) if r < 0.7 else random.randrange(65536))
    if random.random() < 0.1:
        words = [0, 0, 0, 0, 0, random.choice([0, 0xffff]), random.randrange(65536), random.randrange(65536)]
    hexed = "".join("%04x" % w for w in words)
    lines.append("N6 " + hexed)
    lines.append("R6 " + hexed)
    text = ":".join("%x" % w for w in words)
    lines.append("P6 " + text)
open(sys.argv[1], "w").write("\n".join(lines) + "\n")
PY

"$BUILD/inet-ours" < "$CASES" > "$BUILD/inet-ours.out"
"$BUILD/inet-theirs" < "$CASES" > "$BUILD/inet-theirs.out"

TOTAL=$(wc -l < "$CASES" | tr -d ' ')
# Round trip: what our inet_ntop printed must parse back, through our own
# inet_pton, to the bytes it came from - a check that needs no oracle.
if grep -q BROKEN "$BUILD/inet-ours.out"; then
  echo "inet-test: FAIL - an address inet_ntop printed did not parse back to its own bytes" >&2
  exit 1
fi
if [ "$(grep -c '^round trip$' "$BUILD/inet-ours.out")" -lt 20000 ]; then
  echo "inet-test: FAIL - fewer round trips ran than were generated" >&2
  exit 1
fi

# Where the host is not the oracle. macOS's inet_pton is lax in two ways
# glibc - which is what every program ported here was written against - is
# not: it takes a leading zero in an IPv4 octet, the spelling some parsers
# read as octal, and a zone suffix that POSIX's inet_pton does not define.
# These are graded against glibc's answer, which is to refuse them, and run
# through this libc alone.
STRICT=$(printf 'P4 01.2.3.4\nP4 1.2.3.04\nP4 0.0.0.00\nP6 ::ffff:01.2.3.4\nP6 fe80::1%%eth0\n' |
         "$BUILD/inet-ours")
if [ "$STRICT" != "$(printf '0\n0\n0\n0\n0')" ]; then
  echo "inet-test: FAIL - a leading zero or a zone suffix was accepted:" >&2
  echo "$STRICT" >&2
  exit 1
fi

if ! cmp -s "$BUILD/inet-ours.out" "$BUILD/inet-theirs.out"; then
  echo "inet-test: FAIL - this libc and the host's disagree:" >&2
  diff "$BUILD/inet-ours.out" "$BUILD/inet-theirs.out" | head -20 >&2
  exit 1
fi
echo "inet-test: $TOTAL cases agree with the host's inet_ntop and inet_pton, and every printed IPv6 address parses back to its bytes"
