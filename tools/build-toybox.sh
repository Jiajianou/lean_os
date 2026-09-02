#!/usr/bin/env bash
# tools/build-toybox.sh - M89: build toybox for lean_os.
#
# ---- why this script exists rather than a Makefile rule ---------------
#
# Toybox has its own build system, and the whole point of porting it is
# that this project does not get to replace that. So this drives toybox's
# `make` the way any packager would, and everything lean_os-specific is
# in the arguments.
#
# ---- the patch series, and why third_party/ is not edited ------------
#
# Toybox supports a closed set of operating systems. lib/portability.c
# has three sites that read
#
#   #if defined(__linux__) ... #elif defined(__APPLE__) ...
#   #elif defined(__FreeBSD__)... #else #error #endif
#
# so a system that is none of those does not fail to link - it fails to
# compile, on purpose, with the author telling you to come and add your
# case. That is the honest design for a portability layer and it means
# porting toybox is *editing toybox*, which CLAUDE.md's first
# non-negotiable forbids doing in third_party/.
#
# The way out is the one M94 already writes down for the GCC target port:
# "a target port is upstream-shaped configuration; a patch to the
# compiler's own passes is the thing M63's rule exists to forbid." Adding
# an OS to a portability layer is the upstream-shaped kind. So the tree
# in third_party/ stays byte-identical to the published tarball, this
# script copies it, and tools/toybox-port/*.patch is applied to the copy.
# The patches are ours, they are small, they are readable, and a diff
# against the tarball is still meaningful - which is the property that
# would be lost by editing in place.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SRC=third_party/toybox
WORK=build/toybox
PATCHES=tools/toybox-port

if [ ! -d "$SRC" ]; then
  echo "build-toybox: $SRC is missing - see milestones.md M89" >&2
  exit 1
fi
if [ -z "$(command -v gsed 2>/dev/null)" ]; then
  # Toybox's own scripts need GNU sed and look for it under this name;
  # macOS's is BSD sed and fails at the first code-generation step. See
  # docs/toolchain.md.
  echo "build-toybox: GNU sed (gsed) is required - brew install gnu-sed" >&2
  exit 1
fi

echo "build-toybox: unpacking a clean copy of $SRC"
rm -rf "$WORK"
cp -R "$SRC" "$WORK"

for p in "$PATCHES"/*.patch; do
  [ -e "$p" ] || continue
  echo "build-toybox: applying $(basename "$p")"
  # -p0 with the paths as generated, from the tree root, and --forward so
  # a re-run on an already-patched copy is an error rather than a mess.
  if ! patch -p0 --forward --silent < "$p"; then
    echo "build-toybox: $p did not apply - the vendored toybox may have moved" >&2
    exit 1
  fi
done

# The flags lean_os builds everything with, plus the define the port
# above keys on. -ffreestanding because this libc is not the host's, and
# the three include paths are the same three every program in
# user_space/bin compiles against.
LEANOS_CFLAGS="-ffreestanding -fno-stack-protector -D__lean_os__ \
  -I$ROOT/user_space/libc/include -I$ROOT/user_space/lib -I$ROOT/system_api/include"

cd "$WORK"
echo "build-toybox: configuring"
make defconfig >/dev/null 2>&1 || { echo "build-toybox: defconfig failed" >&2; exit 1; }

echo "build-toybox: building"
make CROSS_COMPILE=x86_64-elf- CC=gcc HOSTCC=cc CFLAGS="$LEANOS_CFLAGS" "$@"
