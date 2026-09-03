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
echo "install-native-toolchain: 8 tools in /bin ($TOTAL stripped) + /tests/binutils-hello.s"
