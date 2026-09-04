#!/usr/bin/env bash
# tools/build-thirdparty.sh - M94: somebody else's project, built for
# this OS by this project's own compiler.
#
# ---- what this proves that tests/gcc/hello.c cannot --------------------
#
# That fixture is a program written here to exercise the toolchain. This
# builds two that were not:
#
#   bzip2 1.0.8   a plain Makefile, seven source files and a library,
#                 built with `make CC=x86_64-lean_os-gcc` and its own
#                 flags. No configure, so what it proves is the compiler
#                 and the libc and nothing about autotools.
#   GNU hello 2.12.1  `./configure --host=x86_64-lean_os && make`, which
#                 is M94's own stated bar - and it is a much harder test
#                 than its name suggests, because it drags in about fifty
#                 gnulib modules whose whole job is to probe a system and
#                 replace what it lacks.
#
# Neither source is vendored: both are downloaded here and thrown away,
# and neither is modified. The ONE edit either needs is one line added to
# its bundled `config.sub` - which is older than the toolchain's and
# refuses `--host=x86_64-lean_os` before doing anything else. Replacing a
# project's config.sub is what every distribution does; this adds a name
# to the copy that is there, through the same anchored edit
# tools/toolchain-port/apply.py uses for the toolchain itself.
#
# ---- what building GNU hello found, which is the point ----------------
#
# Five gaps in this project's C library, every one named by a build
# rather than by a checklist - M63's rule at the scale M94 is for:
#
#   wprintf and the wide stdio family   src/hello.c's own first line
#   getprogname / program_invocation_name  gnulib's per-OS #error
#   __fpending                          gnulib's per-OS #error
#   pthread_kill/pthread_mutexattr_*    a configure PROBE, not a call:
#                                       failing it made gnulib substitute
#                                       its own mbrtowc, which then hit a
#                                       per-OS #error of its own
#   wcwidth declared in <wchar.h>       it existed, in <wctype.h> only -
#                                       and a header that has a function
#                                       and does not declare it where the
#                                       standard says is, to a build,
#                                       indistinguishable from not having
#                                       it
#
# ---- M100: and the library stack a browser links against --------------
#
# M100's first bullet is nine libraries in dependency order, each one
# unmodified and each one a real test of M94-M97. zlib is the first
# because everything else in the list depends on it, and because it is
# the smallest thing that is a *library* rather than a program: it has
# to be installed into the sysroot so the next one can link against it,
# which is a step none of the ports above ever needed.
#
# What zlib found on its way in, both of which are facts about this
# target rather than bugs in zlib:
#
#   fseeko is missing. zlib's configure probes for it, does not find
#   it, defines NO_FSEEKO and carries on with a 32-bit file offset in
#   its gz* layer. Recorded rather than fixed here: it is a libc gap
#   with a name, and the next library that wants it is the one that
#   should pay for it - which is M63's rule and the reason every port
#   in this file found what it found.
#
#   `attempted static link of dynamic object`. zlib builds a second
#   copy of its test programs against libz.so, with no -pie on the
#   link line, because on every other ELF system the default link is
#   DYNAMIC. Here it is static (LINK_SPEC in gcc/config/lean_os.h), so
#   that link fails. This is exactly the kind of assumption M100 exists
#   to find and count, and it is left standing rather than papered over
#   by changing the default: a static default is what a machine whose
#   kernel loads ET_EXEC directly should have, and the day a port needs
#   the other answer it will say so with a link error naming it.
#
# Usage: tools/build-thirdparty.sh
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
if [ ! -x "$PREFIX/bin/x86_64-lean_os-gcc" ]; then
  echo "build-thirdparty: no x86_64-lean_os-gcc - run tools/build-toolchain.sh" >&2
  exit 1
fi
export PATH="$PREFIX/bin:$PATH"

make -s -C "$ROOT" sysroot >/dev/null || exit 1

SRC="$ROOT/build/thirdparty-src"
OUT="$ROOT/build/thirdparty"
mkdir -p "$SRC" "$OUT"
cd "$SRC"

fetch() {
  [ -f "$2" ] && return 0
  echo "build-thirdparty: fetching $2"
  curl -sSL -o "$2.part" "$1" && mv "$2.part" "$2"
}

# ---- bzip2: a Makefile, and nothing else ------------------------------
fetch https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz bzip2-1.0.8.tar.gz
rm -rf bzip2-1.0.8
tar xf bzip2-1.0.8.tar.gz
( cd bzip2-1.0.8 && make -s CC=x86_64-lean_os-gcc AR=x86_64-lean_os-ar \
      RANLIB=x86_64-lean_os-ranlib \
      CFLAGS="-Wall -O2 -D_FILE_OFFSET_BITS=64" bzip2 >bzip2.log 2>&1 ) || {
  echo "build-thirdparty: bzip2 did not build:" >&2
  tail -20 "$SRC/bzip2-1.0.8/bzip2.log" >&2
  exit 1
}
cp bzip2-1.0.8/bzip2 "$OUT/bzip2"
echo "build-thirdparty: bzip2 -> $OUT/bzip2"

# ---- GNU hello: ./configure --host=x86_64-lean_os && make -------------
fetch https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz hello-2.12.1.tar.gz
rm -rf hello-2.12.1
tar xf hello-2.12.1.tar.gz
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        hello-2.12.1/build-aux/config.sub || exit 1
(
  cd hello-2.12.1
  # --disable-nls, because gettext is a second port and a message
  # catalogue is not what this is testing.
  ./configure --host=x86_64-lean_os --disable-nls >configure.out 2>&1 &&
  make >make.out 2>&1
) || {
  echo "build-thirdparty: GNU hello did not build. The configure log is the" >&2
  echo "                  evidence M94 asked for: $SRC/hello-2.12.1/config.log" >&2
  tail -20 "$SRC/hello-2.12.1/make.out" 2>/dev/null >&2
  exit 1
}
cp hello-2.12.1/hello "$OUT/gnuhello"
echo "build-thirdparty: GNU hello -> $OUT/gnuhello"
echo "build-thirdparty: the configure log is $SRC/hello-2.12.1/config.log"

# ---- M100, first library: zlib ----------------------------------------
#
# A hand-written configure (not autotools) and a plain Makefile. Cross
# built by naming CHOST, which is zlib's own documented way and needs no
# edit to anything - the first port in this file that needed no edit at
# all, config.sub included, because zlib does not have one.
#
# `make libz.a libz.so.1.3.1 example minigzip` rather than `make`: the
# default target also builds `examplesh` and `minigzipsh`, the same two
# programs linked against libz.so without -pie, which cannot link here.
# See the header.
ZLIB_VER=1.3.1
fetch https://zlib.net/fossils/zlib-$ZLIB_VER.tar.gz zlib-$ZLIB_VER.tar.gz
rm -rf "zlib-$ZLIB_VER"
tar xf "zlib-$ZLIB_VER.tar.gz"
(
  cd "zlib-$ZLIB_VER"
  CHOST=x86_64-lean_os CC=x86_64-lean_os-gcc AR=x86_64-lean_os-ar \
    RANLIB=x86_64-lean_os-ranlib ./configure --prefix=/usr \
    > configure.log 2>&1 &&
  make libz.a "libz.so.$ZLIB_VER" example minigzip > make.log 2>&1
) || {
  echo "build-thirdparty: zlib did not build:" >&2
  tail -20 "$SRC/zlib-$ZLIB_VER/make.log" >&2
  exit 1
}
cp "zlib-$ZLIB_VER/example"  "$OUT/zlibtest"
cp "zlib-$ZLIB_VER/minigzip" "$OUT/minigzip"
cp "zlib-$ZLIB_VER/libz.so.$ZLIB_VER" "$OUT/libz.so.1"

# Into the sysroot, which is the part that makes this a LIBRARY port
# rather than a third program: libpng, freetype and the rest are all
# `-lz` away from here, and a header a build cannot find is
# indistinguishable from a library that does not exist.
SYSROOT="$ROOT/build/sysroot"
cp "zlib-$ZLIB_VER/libz.a"           "$SYSROOT/usr/lib/libz.a"
cp "zlib-$ZLIB_VER/libz.so.$ZLIB_VER" "$SYSROOT/usr/lib/libz.so"
cp "zlib-$ZLIB_VER/zlib.h"           "$SYSROOT/usr/local/include/zlib.h"
cp "zlib-$ZLIB_VER/zconf.h"          "$SYSROOT/usr/local/include/zconf.h"
echo "build-thirdparty: zlib $ZLIB_VER -> $OUT/zlibtest, $OUT/minigzip, and libz in the sysroot"
