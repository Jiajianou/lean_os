#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."

HOSTCC="${HOSTCC:-cc}"
BUILD=build
mkdir -p "$BUILD"

DRIVER="$BUILD/realpath-driver.c"
cat > "$DRIVER" <<'EOF'
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REALPATH realpath

int main(void) {
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!line[0] || line[0] == '#') continue;
        errno = 0;
        char *r = REALPATH(line, NULL);
        if (r) {
            printf("%s\n", r);
            free(r);
        } else {
            const char *e = errno == ENOENT ? "ENOENT"
                          : errno == ENOTDIR ? "ENOTDIR"
                          : errno == ELOOP ? "ELOOP"
                          : errno == EINVAL ? "EINVAL"
                          : errno == ENAMETOOLONG ? "ENAMETOOLONG" : "E?";
            printf("NULL %s\n", e);
        }
    }
    return 0;
}
EOF

if ! $HOSTCC -std=c11 -O1 -g -c -o "$BUILD/realpath-engine.o" \
     user_space/libc/src/realpath.c 2>"$BUILD/realpath-engine.log"; then
  echo "realpath-test: could not compile realpath.c for the host:" >&2
  tail -20 "$BUILD/realpath-engine.log" >&2
  exit 1
fi

$HOSTCC -std=c11 -O1 -g -DUSE_OURS -o "$BUILD/realpath-ours" "$DRIVER" \
        "$BUILD/realpath-engine.o" || exit 1
$HOSTCC -std=c11 -O1 -o "$BUILD/realpath-theirs" "$DRIVER" || exit 1

T="$PWD/$BUILD/realpath-tree"
rm -rf "$T"
mkdir -p "$T/a/b/c" "$T/other"
echo x > "$T/a/b/c/file"
echo y > "$T/other/target"
ln -s "$T/other/target" "$T/a/abslink"
ln -s ../other "$T/a/uplink"
ln -s b "$T/a/rel"
ln -s rel "$T/a/rel2"
ln -s loop1 "$T/a/loop2"
ln -s loop2 "$T/a/loop1"
ln -s missing "$T/a/dangle"

CASES="$BUILD/realpath-cases.txt"
cat > "$CASES" <<EOF
$T
$T/
$T/a/b/c/file
$T/a/./b/../b/c/file
$T/a/rel/c/file
$T/a/rel2/c/file
$T/a/abslink
$T/a/uplink/target
$T/a/uplink/../a/b/c/file
$T/a/loop1
$T/a/dangle
$T/a/b/c/file/notdir
$T/nope
$T/a/../..$T/a
relative-probe
.
..
EOF

A="$BUILD/realpath-ours.out"
B="$BUILD/realpath-theirs.out"
( cd "$T/a" && "$PWD/../../realpath-ours"  < "$PWD/../../realpath-cases.txt" ) > "$A" 2>&1
( cd "$T/a" && "$PWD/../../realpath-theirs" < "$PWD/../../realpath-cases.txt" ) > "$B" 2>&1

TOTAL=$(grep -c . "$CASES" || true)
if ! cmp -s "$A" "$B"; then
  echo "realpath-test: DISAGREEMENT with the host's realpath:" >&2
  paste "$CASES" "$A" "$B" | awk -F'\t' '$2 != $3 {
    printf "  %-40s ours=%s theirs=%s\n", $1, $2, $3 }' >&2
  exit 1
fi
echo "realpath-test: $TOTAL cases, byte-identical with the host's realpath"
