#!/usr/bin/env bash
# tools/gcc-test.sh - M94: grade the target port.
#
# Compiles tests/gcc/hello.c with `x86_64-lean_os-gcc hello.c -o gcctest`
# and **nothing else** - no -ffreestanding, no -nostdlib, no -T, no -I,
# no -mcmodel. That is the whole test: the compiler has to know all of
# that already, because a `./configure` script will not be told.
#
# The compile line is written once, below, on one line, so that a person
# checking this claim can check it in five seconds.
#
# Then it installs the result into the disk image as /bin/gcctest, which
# is what the kernel's own [m94] self-test spawns - the compile proves
# the toolchain, and only running it proves the program.
#
# Skips with a message rather than failing when the toolchain is not
# built: it takes half an hour to build and is not part of `make`. See
# tools/build-toolchain.sh.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
CC="$PREFIX/bin/x86_64-lean_os-gcc"
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
OUT=build/gcctest

if [ ! -x "$CC" ]; then
  echo "gcc-test: no x86_64-lean_os-gcc at $PREFIX - skipped."
  echo "          tools/build-toolchain.sh builds it (M94); it is not part of \`make\`."
  exit 0
fi

# The sysroot is what the compiler finds its headers and libc in, and it
# is generated rather than kept - so it is regenerated here, or this test
# would grade whatever was left over from the last time somebody ran
# `make sysroot`.
make -s sysroot >/dev/null || exit 1

echo "gcc-test: $(basename "$CC") tests/gcc/hello.c -o $OUT"
rm -f "$OUT"
"$CC" tests/gcc/hello.c -o "$OUT" || {
  echo "gcc-test: the compile failed - the target port does not produce a program" >&2
  exit 1
}

# The shape, before the machine ever sees it. A binary that linked but is
# the wrong kind of file would fail on the machine with a message about
# the loader rather than about the compiler.
read -r type entry <<EOF
$(x86_64-elf-readelf -h "$OUT" | awk '/^  Type:/{t=$2} /Entry point/{e=$4} END{print t, e}')
EOF
if [ "$type" != "EXEC" ]; then
  echo "gcc-test: produced a $type, not an EXEC - the link script did not apply" >&2
  exit 1
fi
case "$entry" in
  0x80*) ;;
  *) echo "gcc-test: entry point $entry is not in this OS's image region" >&2; exit 1;;
esac
echo "gcc-test: EXEC, entry $entry, $(wc -c < "$OUT" | tr -d ' ') bytes"

if [ -f "$IMAGE" ]; then
  build/leanfs-put "$IMAGE" "$OUT" /bin/gcctest >/dev/null || exit 1
  echo "gcc-test: installed as /bin/gcctest - the [m94] boot self-test runs it"
fi

# ---- and the two programs nobody here wrote ---------------------------
#
# M94's own bar is `./configure --host=x86_64-lean_os && make` on a
# project nobody here wrote. tools/build-thirdparty.sh does exactly that
# for GNU hello and builds bzip2 beside it; if they are there, they go on
# the image too, and the [m94] self-test runs them. Not built here,
# because they need the network - see that script.
# M100 adds zlibtest and minigzip to the list, and they are a different
# kind of thing: `zlibtest` is zlib's OWN test program (its `example`),
# which asserts its way through compress/uncompress, deflate/inflate,
# a dictionary, and the gz* file layer, and returns non-zero when any of
# them is wrong. Nothing here chose what it checks - which is the same
# argument M99 made for CPython's regression suite, applied to a library.
# M100's second increment adds pngtest, djpeg, cjpeg and jpegtran, and
# they are the same kind of thing one step further on: pngtest is
# libpng's own test program and the three jpeg ones are the programs
# libjpeg's own `make test` runs. What that suite compares against is
# reference output the IJG shipped in 1995 - so the assertion is not
# "it decoded something", it is "byte for byte, the same picture their
# encoder produced". That goes on the image too, below.
# M100's fourth increment adds three more. ftrender is this project's
# own fixture (tests/freetype/ftrender.c) linked against the freetype the
# cross compiler built; what it is graded against is the same fixture
# linked against the host's build of the same source, whose output is
# one of the reference files below. expattest is expat's own test suite,
# 4,392 checks, and xmlwf is its well-formedness checker.
# The fifth increment adds sqlite3, graded like ftrender: the transcript
# of tests/sqlite/cases.sql against the host's build of the same source.
# The sixth increment adds hbshape, graded the same way as ftrender. The
# eighth adds mbedtls's own ssl_server2 and ssl_client2, and httpsget,
# tests/tls/httpsget.c against the sysroot's mbedtls - the first https://
# this machine has had.
for prog in gnuhello bzip2 zlibtest minigzip pngtest djpeg cjpeg jpegtran \
            ftrender expattest xmlwf sqlite3 hbshape ssl_server2 ssl_client2 httpsget; do
  src="build/thirdparty/$prog"
  if [ -f "$IMAGE" ] && [ -x "$src" ]; then
    build/leanfs-put "$IMAGE" "$src" "/bin/$prog" >/dev/null || exit 1
    echo "gcc-test: installed /bin/$prog"
  fi
done

# ---- and the reference output their suites compare against ------------
#
# Under /usr/share/m100, because these are data files somebody else
# wrote and not programs: seven from libjpeg (the JPEG it encoded, the
# progressive one, and the PPM, GIF, BMP and two JPEGs its own `make
# test` requires the results to equal), one PNG from libpng, and - since
# M100's fourth increment - DejaVu Sans and what the host's freetype
# rendered from it, which is what [m100c] requires this machine's
# freetype to reproduce byte for byte.
#
# Put one at a time rather than with `leanfs-put -r`, which also writes
# /.image-manifest and would overwrite the one M93's image-tree test
# reads.
DATA=build/thirdparty/m100-data
if [ -f "$IMAGE" ] && [ -d "$DATA" ]; then
  n=0
  for f in "$DATA"/*; do
    build/leanfs-put "$IMAGE" "$f" "/usr/share/m100/$(basename "$f")" >/dev/null || exit 1
    n=$((n + 1))
  done
  echo "gcc-test: installed $n reference files under /usr/share/m100 - the [m100b] and [m100c] self-tests compare against them"
fi

# ---- and mbedtls's own test suites ----------------------------------------
#
# Eighteen of them, each a static binary plus the .datax file of vectors
# it reads, under /usr/share/m100/mbedtls. The [m100g] self-test runs
# every one and requires its own PASSED line. Same one-at-a-time put as
# above, for the same manifest reason.
SUITES=build/thirdparty/mbedtls-suites
if [ -f "$IMAGE" ] && [ -d "$SUITES" ]; then
  n=0
  for f in "$SUITES"/*; do
    build/leanfs-put "$IMAGE" "$f" "/usr/share/m100/mbedtls/$(basename "$f")" >/dev/null || exit 1
    n=$((n + 1))
  done
  echo "gcc-test: installed $n files of mbedtls's own test suites under /usr/share/m100/mbedtls - the [m100g] self-test runs them"
fi
