#!/usr/bin/env bash
# tools/realpath-test.sh - M98: grade this project's realpath against a
# real one.
#
# The fifth differential test. realpath's whole job is agreement - two
# implementations that canonicalize the same tree differently break
# every "is X the same file as Y" question a build system asks - and
# the function is pure logic over lstat/readlink/getcwd, so compiling
# realpath.c for the host (those three resolve to the host's own) and
# walking a fixture tree of real directories and real symlinks grades
# all of it. Nothing here says what the right answer is; the host's
# realpath does.
#
# Why it exists: this libc had no realpath, and libiberty's lrealpath
# handles that case by falling off the end of a non-void function. The
# gcc driver compared the resulting garbage to more of it, decided
# every input file was its own output file, and refused to compile
# anything given -o. See realpath.c.
set -uo pipefail

cd "$(dirname "$0")/.."

HOSTCC="${HOSTCC:-cc}"
BUILD=build
mkdir -p "$BUILD"

DRIVER="$BUILD/realpath-driver.c"
cat > "$DRIVER" <<'EOF'
/* Built twice. Reads one path per line, prints what realpath said -
 * the canonical path, or NULL and errno's name. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No rename trick here, unlike the printf and stdio harnesses: the
 * host's own <stdlib.h> gives realpath an asm-level alias on Darwin,
 * which follows any renamed prototype onto our definition and defeats
 * it. Instead the "ours" binary simply LINKS realpath.c in front of
 * the host libc - a symbol defined in the executable wins - and the
 * "theirs" binary does not. */
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
# Note: compiled against the HOST's headers and libc on purpose - the
# lstat/readlink/getcwd inside are the host's, which is exactly what
# makes the comparison meaningful.

$HOSTCC -std=c11 -O1 -g -DUSE_OURS -o "$BUILD/realpath-ours" "$DRIVER" \
        "$BUILD/realpath-engine.o" || exit 1
$HOSTCC -std=c11 -O1 -o "$BUILD/realpath-theirs" "$DRIVER" || exit 1

# ---- the fixture tree --------------------------------------------------
#
# Built fresh every run in build/, because a tree that only exists in a
# script cannot rot apart from it. Every shape realpath distinguishes:
# plain nesting, links relative and absolute, a link chain, a loop,
# dot and dot-dot (including through a link, the case that breaks
# string-only implementations), and things that do not exist.
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

# The relative cases run from inside the tree, so both binaries agree
# on what "." is.
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
