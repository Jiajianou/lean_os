#!/usr/bin/env bash
# tools/chromium-host.sh - what Chromium calls the Mac a build runs ON.
#
#   tools/chromium-host.sh [MACHINE]     MACHINE defaults to this Mac's (below)
#   tools/chromium-host.sh --runs FILE   exit 0 if this Mac can run FILE;
#                                        otherwise say why on stdout, exit 1
#
# The first form prints two words: the cpu in Chromium's spelling, which
# names the host toolchain (//build/toolchain/mac:clang_<cpu>), and the
# directory Dawn's checkout keeps a Go toolchain under
# (third_party/dawn/tools/golang/<dir>).
#
#   arm64   ->  arm64 mac-arm64
#   x86_64  ->  x64   mac-amd64
#
# The TARGET is x86_64-lean_os on either host. The host half - gn, every code
# generator, Rust's build scripts and proc macros - runs on the Mac, and
# tools/build-chromium.sh named clang_arm64 for it unconditionally, because
# that was the Mac it was written on. On an Intel Mac that is a host
# toolchain whose binaries cannot run on the host. It is a separate script
# so the mapping can be asked about without a checkout: build-chromium.sh
# needs 100 GB of Chromium before it reaches the line that uses it.
#
# The second form is for a HOST program a build left behind - the Baseline
# headless_shell tools/chromium-test.sh renders its fixture with. A checkout
# built on one Mac and carried to the other holds a binary of the other
# architecture, and running it is "Bad CPU type in executable", which would
# read as Chromium failing to render. An arm64 Mac with Rosetta can run an
# x86_64 one; nothing runs an arm64 one on an Intel Mac.
#
# "This Mac" is the hardware, not `uname -m`: a shell running under Rosetta
# on Apple Silicon is told x86_64, and the Mac under it still runs arm64
# natively - so it is asked of hw.optional.arm64, which says 1 there even
# to a translated process and does not exist on an Intel Mac.
set -u

this_mac() {
  if [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = "1" ]; then
    echo arm64
  else
    uname -m
  fi
}

if [ "${1:-}" = "--runs" ]; then
  if [ $# -ne 2 ]; then
    echo "usage: tools/chromium-host.sh --runs FILE" >&2
    exit 2
  fi
  file="$2"
  host=$(this_mac)
  archs=$(lipo -archs "$file" 2>/dev/null)
  if [ -z "$archs" ]; then
    echo "$file is not a Mach-O program lipo can read"
    exit 1
  fi
  for a in $archs; do
    if [ "$a" = "$host" ] || { [ "$host" = "arm64" ] && [ "$a" = "arm64e" ]; }; then
      exit 0
    fi
  done
  for a in $archs; do
    if [ "$host" = "arm64" ] && [ "$a" = "x86_64" ] &&
       /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null; then
      exit 0
    fi
  done
  echo "$file is built for $archs and this Mac is $host"
  exit 1
fi

machine="${1:-$(this_mac)}"
case "$machine" in
  arm64|aarch64) echo "arm64 mac-arm64" ;;
  x86_64|amd64)  echo "x64 mac-amd64" ;;
  *)
    echo "chromium-host: no Chromium host toolchain for a $machine Mac" >&2
    exit 1
    ;;
esac
