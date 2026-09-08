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
# unmodified and each one a real test of M94-M97. Five of them are
# below: zlib, libpng, libjpeg, freetype, expat, sqlite. Not programs.
# A library has to be INSTALLED, because the next one links against it,
# and a header a build cannot find is indistinguishable from a library
# that does not exist - which is a step none of the ports above needed.
#
# ---- what the first three found, and none of it is a bug in them ------
#
# Every line here is a fact about THIS target, which is what M100 is for:
# it counts the gap rather than closing it, and the next port that
# actually needs one of these is the one that should pay for it (M63).
#
#   fseeko is missing. zlib's configure probes for it, does not find
#   it, defines NO_FSEEKO and carries on with a 32-bit file offset in
#   its gz* layer. A libc gap with a name.
#
#   `attempted static link of dynamic object`. zlib builds a second
#   copy of its test programs against libz.so, with no -pie on the
#   link line, because on every other ELF system the default link is
#   DYNAMIC. Here it is static (LINK_SPEC in gcc/config/lean_os.h), so
#   that link fails. Left standing rather than papered over by changing
#   the default: a static default is what a machine whose kernel loads
#   ET_EXEC directly should have, and the day a port needs the other
#   answer it will say so with a link error naming it.
#
#   `checking if libtool supports shared libraries... no`, from both
#   autotools libraries, and it is the same fact from the other end.
#   libtool has a per-OS case statement and lean_os is not in it, so
#   every autotools library on this machine is static-only - silently,
#   with no error anywhere, and with `--enable-shared` accepted and
#   ignored. M97 taught the toolchain's OWN bundled libtool about this
#   target through an anchored edit; a project that ships its own
#   generated `configure` brings its own copy and knows nothing. This
#   is the largest single assumption the stack has made so far and it
#   is recorded rather than fixed, because what makes it worth fixing
#   is a library that must be shared - and none of the first three is.
#
#   `feenableexcept` is missing (libpng probes for it, does without).
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


# ---- M100: the library stack, in dependency order ---------------------
#
# M100's first bullet is nine libraries, each unmodified, each one a real
# test of M94-M97. They are not programs: a library has to be INSTALLED,
# because the next one links against it, and a header a build cannot find
# is indistinguishable from a library that does not exist.
#
# ---- where an installed library goes, and why it is two directories ---
#
# `make sysroot` begins with `rm -rf $(SYSROOT)`, which is right - a
# generated directory that is patched in place is a directory nobody can
# reason about. It also means a library installed straight into the
# sysroot survives exactly until the next thing that types `make
# sysroot`, and tools/gcc-test.sh types it on every run. zlib landed in
# M100's first increment by copying four files in after that rm, and it
# worked because nothing came after it.
#
# The fourth library in the list is what makes that unworkable, so the
# install goes into a staging tree of its own:
#
#   build/thirdparty-sysroot/   DESTDIR for every library's own
#                               `make install`. Owned by this script,
#                               never by `make`.
#   build/sysroot/              generated by `make sysroot`, which now
#                               copies the staging tree over itself at
#                               the end.
#
# So the stack survives a regenerated sysroot, and each library is
# installed by ITS OWN install rule rather than by a list of files
# maintained here - which is the same argument M94 makes about flags:
# a file list written by hand is a file list that is wrong by the time
# the next release moves a header.
STAGE="$ROOT/build/thirdparty-sysroot"
SYSROOT="$ROOT/build/sysroot"
mkdir -p "$STAGE"

stage_into_sysroot() {
  # The staging tree is the record; the sysroot is a copy of it that
  # `make sysroot` is free to destroy.
  cp -R "$STAGE/." "$SYSROOT/"
}

# ---- 1/9 zlib ---------------------------------------------------------
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
  make libz.a "libz.so.$ZLIB_VER" example minigzip > make.log 2>&1 &&
  make install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: zlib did not build:" >&2
  tail -20 "$SRC/zlib-$ZLIB_VER/make.log" >&2
  exit 1
}
cp "zlib-$ZLIB_VER/example"  "$OUT/zlibtest"
cp "zlib-$ZLIB_VER/minigzip" "$OUT/minigzip"
cp "zlib-$ZLIB_VER/libz.so.$ZLIB_VER" "$OUT/libz.so.1"
stage_into_sysroot
echo "build-thirdparty: zlib $ZLIB_VER -> $OUT/zlibtest, $OUT/minigzip, and libz in the sysroot"

# ---- 2/9 libpng -------------------------------------------------------
#
# The first library in this list that LINKS one: `-lz`, found because
# zlib is in the sysroot two paragraphs above, and configure says
# "checking for zlibVersion in -lz... yes" before it will go on. That
# sentence is the whole reason the order in M100's bullet is a
# dependency order.
#
# Autotools, so `--host=x86_64-lean_os` and the one line added to its
# bundled config.sub - the same anchored edit GNU hello needs, and the
# only edit either project gets.
#
# It builds its own test programs, and that is the point: pngtest reads
# a PNG, writes one, reads that back, and compares the two image by
# image and chunk by chunk. Nothing here decides what it checks.
PNG_VER=1.6.44
fetch https://downloads.sourceforge.net/project/libpng/libpng16/$PNG_VER/libpng-$PNG_VER.tar.gz \
      libpng-$PNG_VER.tar.gz
rm -rf "libpng-$PNG_VER"
tar xf "libpng-$PNG_VER.tar.gz"
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        "libpng-$PNG_VER/config.sub" || exit 1
(
  cd "libpng-$PNG_VER"
  ./configure --host=x86_64-lean_os --prefix=/usr > configure.log 2>&1 &&
  make > make.log 2>&1 &&
  make install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: libpng did not build:" >&2
  tail -20 "$SRC/libpng-$PNG_VER/make.log" >&2
  exit 1
}
cp "libpng-$PNG_VER/pngtest" "$OUT/pngtest"
stage_into_sysroot
echo "build-thirdparty: libpng $PNG_VER -> $OUT/pngtest, and libpng16 in the sysroot"

# ---- 3/9 libjpeg ------------------------------------------------------
#
# The IJG's own libjpeg, not libjpeg-turbo: turbo's build is CMake and
# its fast paths are hand-written SIMD, and neither is what M100 is
# asking about. This is autotools and portable C, which is the thing
# under test.
#
# What makes it the best-graded port in this file is that it ships
# REFERENCE OUTPUT. `make test` decodes a JPEG the IJG encoded in 1995
# and requires the result to be byte-identical with the PPM, GIF and BMP
# beside it, then encodes that PPM again and requires the JPEG to be
# byte-identical with theirs. Seven comparisons, none of them written
# here, all of them exact - which for a DCT is a much sharper instrument
# than "the picture looks right".
JPEG_VER=9f
fetch https://www.ijg.org/files/jpegsrc.v$JPEG_VER.tar.gz jpegsrc.v$JPEG_VER.tar.gz
rm -rf jpeg-$JPEG_VER
tar xf "jpegsrc.v$JPEG_VER.tar.gz"
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        "jpeg-$JPEG_VER/config.sub" || exit 1
(
  cd "jpeg-$JPEG_VER"
  ./configure --host=x86_64-lean_os --prefix=/usr > configure.log 2>&1 &&
  make > make.log 2>&1 &&
  make install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: libjpeg did not build:" >&2
  tail -20 "$SRC/jpeg-$JPEG_VER/make.log" >&2
  exit 1
}
cp "jpeg-$JPEG_VER/djpeg"    "$OUT/djpeg"
cp "jpeg-$JPEG_VER/cjpeg"    "$OUT/cjpeg"
cp "jpeg-$JPEG_VER/jpegtran" "$OUT/jpegtran"
stage_into_sysroot
echo "build-thirdparty: libjpeg $JPEG_VER -> $OUT/djpeg, $OUT/cjpeg, $OUT/jpegtran, and libjpeg in the sysroot"

# ---- a pkg-config that answers for the sysroot ------------------------
#
# freetype is the first library in the list that asks pkg-config where
# the last one is (`PKG_CHECK_MODULES(LIBPNG, libpng)`), and pkg-config
# knows nothing about a sysroot unless told. The cross convention every
# autotools project honours is a `$host-pkg-config` on PATH - configure
# looks for `x86_64-lean_os-pkg-config` before it looks for a plain one -
# so that is what this is: pkgconf, pointed at the .pc files the earlier
# libraries installed, with PKG_CONFIG_SYSROOT_DIR so that a `-I/usr/
# include/libpng16` in a .pc file becomes a path into the sysroot rather
# than into the host's /usr. Written here rather than in
# build-toolchain.sh because it belongs to the library stack, and
# rewritten on every run because it embeds a path.
#
# pkgconf itself is a host tool, listed in docs/toolchain.md beside gsed
# and for the same reason: somebody else's build asked for it.
if ! command -v pkgconf >/dev/null 2>&1; then
  echo "build-thirdparty: no pkgconf on this host - freetype's configure needs one" >&2
  echo "                  (brew install pkgconf; see docs/toolchain.md)" >&2
  exit 1
fi
cat > "$PREFIX/bin/x86_64-lean_os-pkg-config" <<EOF
#!/bin/sh
# x86_64-lean_os-pkg-config - pkgconf, answering for this OS's sysroot.
# Generated by tools/build-thirdparty.sh; see its header for why.
SYSROOT="$SYSROOT"
PKG_CONFIG_LIBDIR="\$SYSROOT/usr/lib/pkgconfig:\$SYSROOT/usr/share/pkgconfig" \\
PKG_CONFIG_SYSROOT_DIR="\$SYSROOT" \\
exec pkgconf "\$@"
EOF
chmod +x "$PREFIX/bin/x86_64-lean_os-pkg-config"

# ---- 4/9 freetype -----------------------------------------------------
#
# The first library in the list that links TWO of the ones before it:
# `-lz` for compressed tables and `-lpng16` for colour bitmap fonts, both
# found through the pkg-config above, and its configure will not proceed
# with --with-png=yes until it has. --with-harfbuzz=no because harfbuzz
# is 6/9 and depends on freetype, not the other way round (the circular
# dependency is real and every distribution builds freetype twice; once
# is enough here). No bzip2, no brotli: neither is in the sysroot and
# neither is in M100's list.
#
# Two things about this target that freetype found, recorded rather than
# fixed:
#
#   `checking for working mmap... no`. AC_FUNC_MMAP is a RUN test and
#   autoconf answers it "no" for any cross build, so freetype uses its
#   ANSI stdio stream (src/base/ftsystem.c) rather than the mmap one in
#   builds/unix/. This OS has had file-backed mmap since M91; what it
#   does not have is a way for a configure script to run a program on it.
#   That is a fact about cross-compiling, not about this kernel, and the
#   stdio path is a better test of this libc anyway - a 750 KB font read
#   through fseek/ftell/fread is exactly what M98's ungetc bug lived in.
#
#   `the pthread library is not available` (a WARNING, for FT_DEBUG_LOGGING
#   only). ax_pthread.m4 tries -pthread, -lpthread and friends and finds
#   none, because this libc's pthreads live in libc.a itself and nothing
#   here spells `-lpthread`. Same shape as libm.a's argument in the
#   Makefile; an empty libpthread.a would answer it and nothing has needed
#   the answer yet.
#
# config.sub here is the 2024-05 vintage, which lists one OS per line -
# the first bundled copy this project has met that the six-per-line
# anchor did not fit. tools/toolchain-port/apply.py learned the second
# form; the edit is the same one line.
FT_VER=2.13.3
fetch https://downloads.sourceforge.net/project/freetype/freetype2/$FT_VER/freetype-$FT_VER.tar.xz \
      freetype-$FT_VER.tar.xz
rm -rf "freetype-$FT_VER"
tar xf "freetype-$FT_VER.tar.xz"
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        "freetype-$FT_VER/builds/unix/config.sub" || exit 1
(
  cd "freetype-$FT_VER"
  ./configure --host=x86_64-lean_os --prefix=/usr \
      --with-zlib=yes --with-png=yes --with-harfbuzz=no \
      --with-bzip2=no --with-brotli=no > configure.log 2>&1 &&
  grep -q 'checking for LIBPNG... yes' configure.log &&
  make > make.log 2>&1 &&
  make install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: freetype did not build:" >&2
  tail -20 "$SRC/freetype-$FT_VER/make.log" >&2
  exit 1
}
stage_into_sysroot
echo "build-thirdparty: freetype $FT_VER -> libfreetype in the sysroot"

# ---- and the same freetype, for the host, as the oracle ---------------
#
# freetype's tarball has no test suite that runs without meson, a
# network and a corpus, so it is graded the way tools/sh-test.sh and
# tools/math-test.sh grade their subjects: the same source built for the
# host, and the same fixture run against both, required to agree byte
# for byte. tests/freetype/ftrender.c renders every printable ASCII glyph
# of a real font through the TrueType interpreter, the smooth rasterizer,
# the monochrome one and the autohinter, and hashes every bitmap. What
# that grades is tens of thousands of lines of fixed-point arithmetic in
# somebody else's code, compiled by this project's compiler, against the
# same code compiled by the host's - and nothing here says what a glyph
# should look like.
#
# The host build has no zlib and no png on purpose: it is an oracle for
# the rasterizer, and the fewer host libraries under it the fewer ways
# the two builds can differ for reasons that are not this compiler.
HOST="$ROOT/build/thirdparty-host"
mkdir -p "$HOST"
if [ ! -f "$HOST/ft-install/lib/libfreetype.a" ]; then
  echo "build-thirdparty: building freetype $FT_VER for the host, as the oracle"
  (
    cd "$HOST" && rm -rf "freetype-$FT_VER" && tar xf "$SRC/freetype-$FT_VER.tar.xz" &&
    cd "freetype-$FT_VER" &&
    ./configure --prefix="$HOST/ft-install" --with-zlib=no --with-png=no \
        --with-harfbuzz=no --with-bzip2=no --with-brotli=no > configure.log 2>&1 &&
    make > make.log 2>&1 && make install > install.log 2>&1
  ) || { echo "build-thirdparty: the host freetype did not build" >&2; exit 1; }
fi

# The font. DejaVu Sans, from its own release tarball, for the same
# reason libjpeg's test images are copied rather than vendored: what has
# to be true is that these bytes came from somebody else. It has a real
# TrueType bytecode program and a kern table, which are the two things a
# font renderer can get wrong quietly.
DEJAVU_VER=2.37
fetch https://downloads.sourceforge.net/project/dejavu/dejavu/$DEJAVU_VER/dejavu-fonts-ttf-$DEJAVU_VER.tar.bz2 \
      dejavu-fonts-ttf-$DEJAVU_VER.tar.bz2
rm -rf "dejavu-fonts-ttf-$DEJAVU_VER"
tar xf "dejavu-fonts-ttf-$DEJAVU_VER.tar.bz2"
FONT="$SRC/dejavu-fonts-ttf-$DEJAVU_VER/ttf/DejaVuSans.ttf"

# The fixture, twice. The host copy is compiled with whatever `cc` is,
# against the host build; the machine copy with x86_64-lean_os-gcc and
# nothing but what pkg-config says - no path typed by hand.
cc -O2 -Wall -Wextra -o "$OUT/ftrender-host" "$ROOT/tests/freetype/ftrender.c" \
   -I"$HOST/ft-install/include/freetype2" "$HOST/ft-install/lib/libfreetype.a" || exit 1
x86_64-lean_os-gcc -O2 -Wall -Wextra -o "$OUT/ftrender" "$ROOT/tests/freetype/ftrender.c" \
   $(x86_64-lean_os-pkg-config --cflags --libs freetype2) || {
  echo "build-thirdparty: tests/freetype/ftrender.c did not build against the sysroot's freetype" >&2
  exit 1
}
"$OUT/ftrender-host" "$FONT" > "$OUT/ftrender.expected" || {
  echo "build-thirdparty: the host freetype could not render the font" >&2
  exit 1
}
echo "build-thirdparty: freetype's oracle -> $(wc -l < "$OUT/ftrender.expected" | tr -d ' ') lines of reference output, $(tail -1 "$OUT/ftrender.expected")"

# ---- 5/9 expat --------------------------------------------------------
#
# Autotools, one config.sub line, and then nothing: it built first time.
# What it FOUND is more useful than what it needed. Its configure asks
# for four sources of entropy in turn - arc4random_buf, arc4random,
# getrandom, and the raw SYS_getrandom syscall - and this target has
# none of them, so it settles for XML_DEV_URANDOM: `/dev/urandom`, which
# devfs provides and which its own header says is a xorshift over the
# TSC rather than entropy. Expat wants it for a hash salt, and for a hash
# salt it is enough. A TLS library (9/9) will ask the same question
# about a key, and for a key it is not, which is when this becomes a
# kernel item rather than a note.
#
# Graded by its own suite: tests/runtests is 4,392 checks from expat's
# own repository, cross-built here and run on the machine, and the
# sentence it prints on the host - `100%: Checks: 4392, Failed: 0` - is
# the one the machine has to print. Plus xmlwf, the well-formedness
# checker, on a file this filesystem holds: a program that reads a
# document off the disk and says where the mismatched tag is.
EXPAT_VER=2.6.4
fetch https://github.com/libexpat/libexpat/releases/download/R_$(echo $EXPAT_VER | tr . _)/expat-$EXPAT_VER.tar.xz \
      expat-$EXPAT_VER.tar.xz
rm -rf "expat-$EXPAT_VER"
tar xf "expat-$EXPAT_VER.tar.xz"
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        "expat-$EXPAT_VER/conftools/config.sub" || exit 1
(
  cd "expat-$EXPAT_VER"
  ./configure --host=x86_64-lean_os --prefix=/usr \
      --without-docbook --without-examples > configure.log 2>&1 &&
  make > make.log 2>&1 &&
  make -C tests runtests > tests.log 2>&1 &&
  make install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: expat did not build:" >&2
  tail -20 "$SRC/expat-$EXPAT_VER/make.log" >&2
  exit 1
}
cp "expat-$EXPAT_VER/tests/runtests" "$OUT/expattest"
cp "expat-$EXPAT_VER/xmlwf/xmlwf"    "$OUT/xmlwf"
stage_into_sysroot
echo "build-thirdparty: expat $EXPAT_VER -> $OUT/expattest, $OUT/xmlwf, and libexpat in the sysroot"

# ---- 6/9 sqlite -------------------------------------------------------
#
# The amalgamation package: one 244,000-line C file, its shell, and an
# autotools wrapper. `--disable-dynamic-extensions` is sqlite's own
# documented switch for a system without dlopen, and this target IS one
# for a static program: dlopen lives in /lib/ld-lean.so and reaches a
# `-pie` program through it, and libdl.a is empty on purpose (see the
# Makefile's sysroot rule). The first link said so in four undefined
# symbols, which is how a static default is supposed to announce itself.
#
# ---- what sqlite found, and it is the biggest thing this stack has --
#
# Two libc gaps and one kernel gap, all closed here rather than noted,
# on M63's rule that the port which needs a thing is the one that pays
# for it:
#
#   popen / pclose      shell.c's `.import '|cmd'` and `.output |cmd`.
#                       Written in user_space/libc/src/popen.c, and
#                       system() - refused since M97 for want of a
#                       caller - came along, because `.shell` is it.
#
#   fcntl record locks  sqlite's unix VFS takes an F_SETLK before every
#                       transaction and treats any answer other than
#                       "granted" or "held" as a disk I/O error. This
#                       libc refused all three lock commands with
#                       EOPNOTSUPP since M89, on M65's rule, so every
#                       INSERT on this machine failed. kernel/fs/flock.c
#                       is the real thing: per process, per inode, byte
#                       ranges, the close rule and all. See its header.
#
# Graded differentially, like freetype: tests/sqlite/cases.sql through
# the sqlite3 shell built for the host from this same tarball, and again
# through the one built for the machine, and the two transcripts must be
# byte-identical. Nothing in that file says what an answer is.
SQLITE_VER=3470200
fetch https://www.sqlite.org/2024/sqlite-autoconf-$SQLITE_VER.tar.gz \
      sqlite-autoconf-$SQLITE_VER.tar.gz
rm -rf "sqlite-autoconf-$SQLITE_VER"
tar xf "sqlite-autoconf-$SQLITE_VER.tar.gz"
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        "sqlite-autoconf-$SQLITE_VER/config.sub" || exit 1
(
  cd "sqlite-autoconf-$SQLITE_VER"
  ./configure --host=x86_64-lean_os --prefix=/usr --disable-shared \
      --disable-readline --disable-editline \
      --disable-dynamic-extensions > configure.log 2>&1 &&
  make > make.log 2>&1 &&
  make install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: sqlite did not build:" >&2
  tail -20 "$SRC/sqlite-autoconf-$SQLITE_VER/make.log" >&2
  exit 1
}
cp "sqlite-autoconf-$SQLITE_VER/sqlite3" "$OUT/sqlite3"
stage_into_sysroot
echo "build-thirdparty: sqlite $SQLITE_VER -> $OUT/sqlite3, and libsqlite3 in the sysroot"

# The oracle: the same shell, built for the host with the same options.
# Not the host's own /usr/bin/sqlite3, which is a different version and
# would turn "this compiler miscompiled the query planner" and "3.43
# formats EXPLAIN differently from 3.47" into the same failure.
if [ ! -x "$HOST/sqlite-install/bin/sqlite3" ]; then
  echo "build-thirdparty: building sqlite $SQLITE_VER for the host, as the oracle"
  (
    cd "$HOST" && rm -rf "sqlite-autoconf-$SQLITE_VER" &&
    tar xf "$SRC/sqlite-autoconf-$SQLITE_VER.tar.gz" &&
    cd "sqlite-autoconf-$SQLITE_VER" &&
    ./configure --prefix="$HOST/sqlite-install" --disable-shared \
        --disable-readline --disable-editline > configure.log 2>&1 &&
    make > make.log 2>&1 && make install > install.log 2>&1
  ) || { echo "build-thirdparty: the host sqlite did not build" >&2; exit 1; }
fi
rm -f "$OUT/sqlite-host.db"
"$HOST/sqlite-install/bin/sqlite3" -batch -bail "$OUT/sqlite-host.db" \
    < "$ROOT/tests/sqlite/cases.sql" > "$OUT/sqlite.expected" 2>&1 || {
  echo "build-thirdparty: the host sqlite did not accept tests/sqlite/cases.sql:" >&2
  tail -5 "$OUT/sqlite.expected" >&2
  exit 1
}
rm -f "$OUT/sqlite-host.db"
echo "build-thirdparty: sqlite's oracle -> $(wc -l < "$OUT/sqlite.expected" | tr -d ' ') lines of transcript"

# ---- and the data their own test suites compare against ---------------
#
# Copied out of the two source trees rather than vendored, for the same
# reason none of the sources here are: what has to be true is that these
# bytes came out of somebody else's tarball, and a copy in this
# repository would be a copy this repository could quietly edit.
#
# tools/gcc-test.sh puts them on the image under /usr/share/m100, and
# the kernel's [m100b] self-test runs the two suites against them.
DATA="$OUT/m100-data"
rm -rf "$DATA"
mkdir -p "$DATA"
for f in testorig.jpg testprog.jpg testimg.ppm testimg.jpg testimgp.jpg \
         testimg.gif testimg.bmp; do
  cp "jpeg-$JPEG_VER/$f" "$DATA/$f"
done
cp "libpng-$PNG_VER/pngtest.png" "$DATA/pngtest.png"
# M100's fourth increment: the font, and what the host's freetype made
# of it. The [m100c] self-test renders the first and cmp's against the
# second.
cp "$FONT" "$DATA/DejaVuSans.ttf"
cp "$OUT/ftrender.expected" "$DATA/ftrender.expected"
# And sqlite's: the script, and what the host's sqlite made of it.
cp "$ROOT/tests/sqlite/cases.sql" "$DATA/cases.sql"
cp "$OUT/sqlite.expected" "$DATA/sqlite.expected"
echo "build-thirdparty: the reference output their own suites compare against -> $DATA"
