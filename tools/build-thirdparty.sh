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
