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
