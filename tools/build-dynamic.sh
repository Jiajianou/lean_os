#!/usr/bin/env bash
# tools/build-dynamic.sh - M95: code that is loaded, not linked.
#
# Builds, with the compiler M94 put on this machine:
#
#   /lib/ld-lean.so   the dynamic linker (user_space/ld/)
#   /lib/libc.so      this project's C library, as a shared object
#   /bin/dyntest      a program linked against it, position-independent
#   /bin/dyntest2     a second one, so that "they share the text" is a
#                     claim two processes can be measured against
#   /lib/libdyn.so    a library built AFTER dyntest, opened with dlopen
#
# ---- why this is a script and not part of `make` ----------------------
#
# Everything here needs `x86_64-lean_os-gcc`, which is not built by
# `make` and takes half an hour (tools/build-toolchain.sh). The static
# tree is untouched: every program in user_space/bin is still a static
# ET_EXEC linked by x86_64-elf-ld exactly as before, and the dynamic ones
# are additional. That separation is deliberate - M95 is about what
# becomes possible, not about changing what already works.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
CC="$PREFIX/bin/x86_64-lean_os-gcc"
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
OUT=build/dynamic

if [ ! -x "$CC" ]; then
  echo "build-dynamic: no x86_64-lean_os-gcc at $PREFIX - skipped."
  echo "               tools/build-toolchain.sh builds it (M94)."
  exit 0
fi

make -s sysroot >/dev/null || exit 1
mkdir -p "$OUT"

INC="-I$ROOT/system_api/include"

# ---- the linker -------------------------------------------------------
#
# -nostdlib because there is no library it could use - it is the program
# that loads them. -fPIC and -shared because it is an ET_DYN and has to
# relocate itself. -mcmodel=large comes from the driver (M94's
# DRIVER_SELF_SPECS) and is wrong here: a shared object mapped in the
# mmap arena is reached by RIP-relative addressing within itself, and the
# large model's absolute 64-bit references would each need a relocation
# this early code cannot apply. So it is overridden - which the specs
# allow on purpose, and this is the one place in the tree that needs to.
echo "build-dynamic: ld-lean.so"
# -fvisibility=hidden for the same reason ld-start.S marks _start hidden:
# every symbol in this object is its own, nothing here is interposable,
# and a hidden symbol is reached without a GOT - which matters because
# the GOT is not filled in until this object has relocated itself.
"$CC" -shared -fPIC -mcmodel=small -nostdlib -nostartfiles \
      -fvisibility=hidden \
      -ffreestanding -fno-stack-protector -O2 -Wall -Wextra \
      $INC \
      -Wl,-e,_start -Wl,--no-undefined \
      -Wl,-soname,ld-lean.so \
      -o "$OUT/ld-lean.so" \
      user_space/ld/ld-start.S user_space/ld/ld-lean.c || exit 1

# ---- libc, as a shared object -----------------------------------------
#
# The same sources the static libc.a is built from. -fPIC and
# -mcmodel=small for the reason above; the static library keeps the large
# model because a static program is linked at 512 GiB and a shared object
# is not.
#
# -ftls-model=initial-exec, and this is the one flag here that is a
# design decision rather than a mechanical necessity. Without it GCC uses
# the general-dynamic model for a shared object, which reaches every
# `__thread` variable through a call to `__tls_get_addr` - a function the
# dynamic linker would have to provide, over a per-module TLS list that
# exists so that a dlopen'ed object can have thread-locals. Initial-exec
# resolves to a fixed offset from the thread pointer instead, which the
# linker computes when it lays out the static TLS block. The cost is
# exactly the thing that is not supported and is written down: an object
# dlopen'ed after startup cannot have thread-local variables.
#
# The include order is the Makefile's and matters: <signal.h> and
# <termios.h> exist in both header trees and the libc one reaches the
# system_api one with #include_next, so libc's directory has to come
# first. Getting it backwards gives "unknown type name 'speed_t'" from a
# header that plainly declares it.
echo "build-dynamic: libc.so"
LIBC_SRCS=$(ls user_space/libc/src/*.c)
LIB_SRCS="user_space/lib/syscall_wrappers.c user_space/lib/str.c \
          user_space/lib/malloc.c user_space/lib/dns.c"
"$CC" -shared -fPIC -mcmodel=small -nostdlib -nostartfiles \
      -ffreestanding -fno-stack-protector -O2 \
      -ftls-model=initial-exec \
      -I$ROOT/user_space/lib -I$ROOT/user_space/libc/include $INC \
      -Wl,-soname,libc.so \
      -o "$OUT/libc.so" $LIBC_SRCS $LIB_SRCS \
      build/user_obj/setjmp.o build/user_obj/symtab.o || exit 1

# ---- the fixtures -----------------------------------------------------
#
# -pie, which the driver turns into ld's default script plus
# `-dynamic-linker /lib/ld-lean.so` - see M94's LINK_SPEC. Nothing about
# tests/dynamic/dyntest.c says it is dynamic; that is the point.
# The PIE startup file. crt1.o's calls are PC-relative to a global,
# which ld refuses in a PIE - see user_space/lib/crt0-pie.asm, which is
# crt0.asm with those two calls routed through the PLT. Named Scrt1.o
# because that is the name every toolchain uses for it.
echo "build-dynamic: Scrt1.o"
nasm -f elf64 -o "$OUT/Scrt1.o" user_space/lib/crt0-pie.asm || exit 1

echo "build-dynamic: dyntest"
"$CC" -pie -fPIE -mcmodel=small -O2 -Wall \
      -L"$OUT" -Wl,-rpath,/lib \
      -o "$OUT/dyntest" tests/dynamic/dyntest.c \
      "$OUT/libc.so" "$OUT/ld-lean.so" -nostdlib -nostartfiles \
      "$OUT/Scrt1.o" \
      "$ROOT/build/sysroot/usr/lib/crti.o" \
      "$ROOT/build/sysroot/usr/lib/crtn.o" || exit 1

# Built AFTER dyntest and never named on its link line - which is what
# makes dlopen of it a real test rather than a spelling of a static
# reference.
echo "build-dynamic: libdyn.so"
"$CC" -shared -fPIC -mcmodel=small -nostdlib -nostartfiles \
      -O2 -Wall -o "$OUT/libdyn.so" tests/dynamic/libdyn.c \
      -Wl,-soname,libdyn.so "$OUT/libc.so" || exit 1

if [ -f "$IMAGE" ]; then
  build/leanfs-put "$IMAGE" "$OUT/ld-lean.so" /lib/ld-lean.so >/dev/null || exit 1
  build/leanfs-put "$IMAGE" "$OUT/libc.so"    /lib/libc.so    >/dev/null || exit 1
  build/leanfs-put "$IMAGE" "$OUT/libdyn.so"  /lib/libdyn.so  >/dev/null || exit 1
  build/leanfs-put "$IMAGE" "$OUT/dyntest"    /bin/dyntest    >/dev/null || exit 1
  echo "build-dynamic: installed /lib/ld-lean.so, /lib/libc.so, /lib/libdyn.so, /bin/dyntest"
fi

echo "build-dynamic: done - $OUT"
