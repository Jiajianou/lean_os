#!/usr/bin/env bash
# tools/install-native-toolchain.sh - M98: put the toolchain ON the disk.
#
# tools/build-toolchain.sh builds the compiler that runs on the Mac and
# targets lean_os. tools/build-native-toolchain.sh builds the one that
# runs ON lean_os. This script is the second one's install half: it
# writes what that build produced into the disk image, where the [m98]
# boot self-test - and eventually a person at the machine's own shell -
# runs it.
#
# Stripped on the way in with the cross strip, because the unstripped
# eight arrive at ~50 MB and the debug info in them is only readable
# from the host anyway. The unstripped originals stay in build/.
#
# Skips with a message when the native build is absent, exactly as
# gcc-test.sh does for the cross one: the build takes minutes and is not
# part of `make`, and an image without a toolchain is a valid image.
#
# ---- names, and the one collision --------------------------------------
#
# Everything lands in /bin because that is execvp's default PATH (see
# unistd.c's M89 note). Seven of the eight names are free; `readelf` is
# also a toybox command, and binutils' takes the short name on M89's own
# reasoning read from the other side: the short name should be the
# program a build system expects, and a configure script probing
# `readelf` means this one. Toybox's stays reachable as `toybox
# readelf`, which is what a multi-call binary is for.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

NATIVE=build/native/binutils
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
PUT=build/leanfs-put
CROSS_STRIP=build/toolchain/bin/x86_64-lean_os-strip

if [ ! -x "$NATIVE/gas/as-new" ]; then
  echo "install-native-toolchain: no native binutils at $NATIVE - skipped."
  echo "          tools/build-native-toolchain.sh builds it (M98); it is not part of \`make\`."
  exit 0
fi
[ -f "$IMAGE" ] || { echo "install-native-toolchain: no image at $IMAGE - run make first" >&2; exit 1; }
[ -x "$PUT" ] || make -s leanfs-put || exit 1

STAGE=build/native/install
mkdir -p "$STAGE"

# built-name : installed-name, the -new suffixes dropped the same way
# `make install` drops them.
install_one() {
  local from=$1 name=$2
  cp "$from" "$STAGE/$name" || exit 1
  "$CROSS_STRIP" "$STAGE/$name" || exit 1
  "$PUT" "$IMAGE" "$STAGE/$name" "/bin/$name" >/dev/null || exit 1
}

install_one "$NATIVE/gas/as-new"         as
install_one "$NATIVE/ld/ld-new"          ld
install_one "$NATIVE/binutils/ar"        ar
install_one "$NATIVE/binutils/nm-new"    nm
install_one "$NATIVE/binutils/objdump"   objdump
install_one "$NATIVE/binutils/strip-new" strip
install_one "$NATIVE/binutils/ranlib"    ranlib
install_one "$NATIVE/binutils/readelf"   readelf

# The fixture the [m98] boot self-test assembles, links and runs - see
# tests/binutils/hello.s for why it is beneath the runtime on purpose.
"$PUT" "$IMAGE" tests/binutils/hello.s /tests/binutils-hello.s >/dev/null || exit 1

TOTAL=$(du -sh "$STAGE" | cut -f1)
echo "install-native-toolchain: 8 binutils tools in /bin ($TOTAL stripped)"

# ---- the compiler, if its build has happened ---------------------------
#
# Same skip discipline as the binutils half: an image without the
# compiler is a valid image, and the [m98] marker's compile half says
# "skipped" out loud rather than failing.
GCCSTAGE=build/native/gcc-install
CROSS=build/toolchain
if [ ! -x "$GCCSTAGE/usr/libexec/gcc/x86_64-lean_os/14.2.0/cc1" ]; then
  echo "install-native-toolchain: no native gcc staged - binutils only."
  exit 0
fi

GCCDIR=/usr/lib/gcc/x86_64-lean_os/14.2.0
EXECDIR=/usr/libexec/gcc/x86_64-lean_os/14.2.0

strip_put() { # host-file image-path: strip a copy, then put
  local from=$1 to=$2 base
  base=$(basename "$from")
  cp "$from" "$STAGE/$base" || exit 1
  "$CROSS_STRIP" "$STAGE/$base" 2>/dev/null || true
  "$PUT" "$IMAGE" "$STAGE/$base" "$to" >/dev/null || exit 1
}

# The driver and its two proper compilers. collect2 is the linker shim
# the driver actually execs; without it every link dies looking for it.
strip_put "$GCCSTAGE/usr/bin/gcc"      /usr/bin/gcc
strip_put "$GCCSTAGE/usr/bin/g++"      /usr/bin/g++
strip_put "$GCCSTAGE/usr/bin/cpp"      /usr/bin/cpp
strip_put "$GCCSTAGE/$EXECDIR/cc1"      $EXECDIR/cc1
strip_put "$GCCSTAGE/$EXECDIR/cc1plus"  $EXECDIR/cc1plus
strip_put "$GCCSTAGE/$EXECDIR/collect2" $EXECDIR/collect2

# The compiler's own headers (stddef.h and kin), and the chained
# limits.h whose story is told in build-toolchain.sh.
"$PUT" -r "$IMAGE" "$GCCSTAGE$GCCDIR/include" $GCCDIR/include >/dev/null || exit 1

# The target runtime objects, from the cross build - same source, same
# port, same version, and the native build deliberately does not build
# its own copy of what would be byte-equivalent.
for f in crtbegin.o crtend.o crtbeginS.o crtendS.o libgcc.a libgcc_eh.a; do
  "$PUT" "$IMAGE" "$CROSS/lib/gcc/x86_64-lean_os/14.2.0/$f" $GCCDIR/$f >/dev/null || exit 1
done

# The sysroot ON the image: what `gcc hello.c` needs to find at the
# paths the target description names - headers at /usr/include and
# /usr/local/include, startup files, libc and the linker script at
# /usr/lib. Regenerated by the Makefile before this script runs.
"$PUT" -r "$IMAGE" build/sysroot/usr/include       /usr/include       >/dev/null || exit 1
"$PUT" -r "$IMAGE" build/sysroot/usr/local/include /usr/local/include >/dev/null || exit 1
for f in libc.a libm.a crt1.o crti.o crtn.o Scrt1.o lean_os.ld; do
  "$PUT" "$IMAGE" "build/sysroot/usr/lib/$f" /usr/lib/$f >/dev/null || exit 1
done

# C++: the standard library the cross toolchain built (M97), so g++ on
# the machine links the same libstdc++ programs on the Mac side link.
"$PUT" -r "$IMAGE" "$CROSS/x86_64-lean_os/include/c++" /usr/include/c++ >/dev/null || exit 1
for f in libstdc++.a libsupc++.a; do
  "$PUT" "$IMAGE" "$CROSS/x86_64-lean_os/lib/$f" /usr/lib/$f >/dev/null || exit 1
done

# And make, which is what turns one compile into a build.
if [ -x build/native/make/make ]; then
  strip_put build/native/make/make /usr/bin/make
fi

# The compile fixture for [m98]'s second half, and the make project for
# its third.
"$PUT" "$IMAGE" tests/binutils/m98c.c /tests/m98c.c >/dev/null || exit 1
"$PUT" -r "$IMAGE" tests/binutils/m98mk /tests/m98mk >/dev/null || exit 1

# ---- M98's fourth box: the fixtures the measurement boot builds --------
#
# tests/bootstrap/run.sh is the script the kernel spawns when a boot is
# given opt/leanos/bootstrap=1 (tools/bootstrap-test.sh is what passes
# it). What it needs on the disk: its own two files, a clean copy of
# somebody else's source tree, and the objects the CROSS compiler
# produces from that tree - which is what lets the machine compare its
# own compiler's output byte for byte with the one that built it.
#
# bzip2 rather than a fixture written here, for M89's reason applied to
# a build system instead of to a userland: a Makefile nobody here wrote,
# with its own test suite that grades the result against reference files
# nobody here produced. Skipped, with a message, when its source is
# absent - tools/build-thirdparty.sh is what fetches it, and an image
# without it is a valid image.
"$PUT" -r "$IMAGE" tests/bootstrap /tests/bootstrap >/dev/null || exit 1
# M99: and the build-cost fixture, which needs the same compiler and the
# same /tests/bzip2 this one installs - so it goes on beside it rather
# than in install-python.sh, which runs whether or not there is a native
# toolchain here to measure.
"$PUT" -r "$IMAGE" tests/pybuild /tests/pybuild >/dev/null || exit 1

BZSRC=build/thirdparty-src/bzip2-1.0.8
if [ -d "$BZSRC" ]; then
  BZSTAGE=build/native/bzip2-src
  BZREF=build/native/bzip2-ref
  rm -rf "$BZSTAGE" "$BZREF"
  mkdir -p "$BZSTAGE"
  # Only what a build needs, and nothing it produced: the source tree in
  # build/ has been built in, and putting yesterday's objects on the
  # image would let the machine's `make` decide there was nothing to do.
  for f in "$BZSRC"/*.c "$BZSRC"/*.h "$BZSRC"/Makefile \
           "$BZSRC"/sample*.ref "$BZSRC"/sample*.bz2 "$BZSRC"/words*; do
    [ -f "$f" ] && cp "$f" "$BZSTAGE/"
  done
  cp -R "$BZSTAGE" "$BZREF"
  # The reference build, with the cross compiler and the exact flags
  # run.sh passes on the machine. -g is deliberately not among them: it
  # records the compilation directory, which differs by construction.
  BZCFLAGS="-Wall -Winline -O2 -D_FILE_OFFSET_BITS=64"
  ( cd "$BZREF" && make -s CC="$ROOT/$CROSS/bin/x86_64-lean_os-gcc" \
      AR="$ROOT/$CROSS/bin/x86_64-lean_os-ar" \
      RANLIB="$ROOT/$CROSS/bin/x86_64-lean_os-ranlib" \
      CFLAGS="$BZCFLAGS" libbz2.a bzip2 bzip2recover \
      > cross-build.log 2>&1 ) || {
    echo "install-native-toolchain: the cross reference build of bzip2 failed:" >&2
    tail -20 "$BZREF/cross-build.log" >&2
    exit 1
  }
  "$PUT" -r "$IMAGE" "$BZSTAGE" /tests/bzip2 >/dev/null || exit 1
  mkdir -p build/native/bzip2-ref-objs
  for o in "$BZREF"/*.o; do
    cp "$o" build/native/bzip2-ref-objs/
  done
  "$PUT" -r "$IMAGE" build/native/bzip2-ref-objs /tests/bzip2-ref >/dev/null || exit 1
  echo "install-native-toolchain: bzip2's source in /tests/bzip2, and the cross compiler's own objects beside it"
else
  echo "install-native-toolchain: no bzip2 source at $BZSRC - the bootstrap boot will skip the build half."
  echo "          tools/build-thirdparty.sh fetches it."
fi

echo "install-native-toolchain: gcc, g++, cpp, make, headers and libraries under /usr"
