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

SYSROOT="$ROOT/build/sysroot/usr/lib"

# ---- M99: what this script stopped doing ------------------------------
#
# M95 built ld-lean.so and libc.so here, with the cross compiler, and
# linked every fixture with a line that read
#
#     -pie -fPIE -mcmodel=small ... -nostdlib -nostartfiles \
#     "$OUT/Scrt1.o" "$OUT/libc.so" "$OUT/ld-lean.so" crti.o crtn.o
#
# Nine things supplied by hand, in the file whose subject is that this
# machine can load code. Every one of them was a fact about the target
# that the target description should have known, and M94's own rule says
# what that costs: **every flag invented by hand is a flag someone else's
# build system will not pass.** It was exactly the flags on this line
# that CPython's Makefile had no place to put, which is how a box that
# looked like "teach the loader to open more objects" turned out to be
# about gcc/config/lean_os.h.
#
# So: the loader and libc are built by `make sysroot` now, with the same
# x86_64-elf-gcc that has always built libc.so and for the ordering
# reason written above that rule; the startup files were already there
# since M97; and the links below are what somebody else's build system
# would write.
#
#     x86_64-lean_os-gcc -pie prog.c -o prog
#     x86_64-lean_os-gcc -shared lib.c -o lib.so
#
# That is the whole interface, and this script is now a demonstration of
# it rather than a workaround for its absence.
echo "build-dynamic: ld-lean.so and libc.so come from the sysroot"
cp "$SYSROOT/ld-lean.so" "$OUT/ld-lean.so" || exit 1
cp "$SYSROOT/libc.so"    "$OUT/libc.so"    || exit 1

echo "build-dynamic: dyntest"
"$CC" -pie -O2 -Wall -o "$OUT/dyntest" tests/dynamic/dyntest.c || exit 1

# Built AFTER dyntest and never named on its link line - which is what
# makes dlopen of it a real test rather than a spelling of a static
# reference.
echo "build-dynamic: libdyn.so"
"$CC" -shared -O2 -Wall -o "$OUT/libdyn.so" tests/dynamic/libdyn.c \
      -Wl,-soname,libdyn.so || exit 1

# ---- M99: and the fixture for the thing that actually changed ---------
#
# dyntest opens one library. An interpreter opens one per module it
# imports, and MAX_OBJECTS was sixteen - a number M95 chose for a machine
# whose programs linked against libc and nothing else, and which
# dlclose's own note names as the condition for revisiting it. Sixteen
# was also never *tested*: nothing here had ever opened two.
#
# manydyn opens twenty-four, by name, one at a time, and calls a function
# out of each that returns the object's own number. Twenty-four is past
# the old ceiling and inside the new one, which is the only interval
# where this test can tell the two apart. It also opens them by FULL
# PATH, which is the second half of M99's loader work and the bug that
# would otherwise have been found by CPython instead of by this.
echo "build-dynamic: manylib0..23 and manydyn"
MANY=""
for i in $(seq 0 23); do
  sed "s/@N@/$i/g" tests/dynamic/manylib.c.in > "$OUT/manylib$i.c" || exit 1
  "$CC" -shared -O2 -Wall -o "$OUT/manylib$i.so" "$OUT/manylib$i.c" \
        -Wl,-soname,"manylib$i.so" || exit 1
  MANY="$MANY $OUT/manylib$i.so"
done
"$CC" -pie -O2 -Wall -o "$OUT/manydyn" tests/dynamic/manydyn.c || exit 1

# ---- M97: the same boundary, in C++ -----------------------------------
#
# A shared object that throws and an executable that dlopens it and
# catches. Built here rather than in tools/cxx-test.sh because it needs
# libc.so, ld-lean.so and Scrt1.o - the three things this script makes -
# and because the ORDER things reach the image matters: an executable
# whose PT_INTERP names a loader that is not on the disk is refused by
# the kernel with "not on this disk", which is a message about the image
# rather than about the program, and cost one run to read properly.
#
# -nostdlib and the startup files by hand, exactly as the C programs
# above are: the driver's own spec would pull in the static libc and the
# static crt1. What is added on top is libsupc++.a and libgcc.a for the
# C++ ABI runtime and the unwinder, and crtbeginS.o/crtendS.o for the
# shared object - which is the piece that registers ITS OWN .eh_frame
# with the unwinder from its .init_array, and without which an exception
# raised inside it has no frame information to unwind by.
#
# --export-dynamic on the executable, and it is load-bearing: the C++
# runtime is linked statically INTO the executable, so the shared
# object's calls to __cxa_throw and its references to the typeinfo for
# a shared type have to resolve to the executable's copies. Without it
# they are not in the dynamic symbol table, the loader cannot find them,
# and the two sides end up with two type_info objects for one type -
# which is exactly the failure M97's bullet describes.
CXX="$PREFIX/bin/x86_64-lean_os-g++"
if [ -x "$CXX" ] && [ -f "$PREFIX/x86_64-lean_os/lib/libstdc++.so" ]; then
  echo "build-dynamic: libthrow.so"
  "$CXX" -shared -O1 -Wall -Itests/cxx \
        -o "$OUT/libthrow.so" tests/cxx/throwlib.cpp \
        -Wl,-soname,libthrow.so || exit 1

  # ld-lean.so on the line because dlopen/dlsym/dlclose live in the
  # loader itself - there is no libdl here and there is nothing for one
  # to contain. --export-dynamic is not for that, though: the C++ ABI
  # runtime and the typeinfo for the shared types have to be in this
  # program's DYNAMIC symbol table, or the library it opens resolves
  # them to its own copies and the two sides end up with two type_info
  # objects for one type. That is the failure M97's bullet describes,
  # and it looks like a catch clause that simply does not match.
  echo "build-dynamic: throwmain"
  "$CXX" -pie -O1 -Wall -Itests/cxx \
        -Wl,--export-dynamic \
        -o "$OUT/throwmain" tests/cxx/throwmain.cpp || exit 1
else
  echo "build-dynamic: no shared C++ runtime yet - the cross-object throw is skipped."
fi

if [ -f "$IMAGE" ]; then
  build/leanfs-put "$IMAGE" "$OUT/ld-lean.so" /lib/ld-lean.so >/dev/null || exit 1
  build/leanfs-put "$IMAGE" "$OUT/libc.so"    /lib/libc.so    >/dev/null || exit 1
  build/leanfs-put "$IMAGE" "$OUT/libdyn.so"  /lib/libdyn.so  >/dev/null || exit 1
  build/leanfs-put "$IMAGE" "$OUT/dyntest"    /bin/dyntest    >/dev/null || exit 1
  build/leanfs-put "$IMAGE" "$OUT/manydyn"    /bin/manydyn    >/dev/null || exit 1
  # M99: opened by FULL PATH out of a directory that is not searched, so
  # that finding them is the loader doing what it was told rather than
  # what it guesses. /lib/many is that directory.
  for i in $(seq 0 23); do
    build/leanfs-put "$IMAGE" "$OUT/manylib$i.so" "/lib/many/manylib$i.so" \
      >/dev/null || exit 1
  done
  # M99: libgcc_s.so.1 is a DT_NEEDED of every -pie link now - LIBGCC_SPEC
  # asks for the shared unwinder whenever the link is dynamic, which it
  # has since M97 and which used to be reached only through the C++
  # fixtures. It is not conditional on them any more, because dyntest
  # needs it too.
  build/leanfs-put "$IMAGE" \
    "$PREFIX/x86_64-lean_os/lib/libgcc_s.so.1" /lib/libgcc_s.so.1 \
    >/dev/null || exit 1
  if [ -f "$OUT/throwmain" ]; then
    # libstdc++.so.6 by its SONAME, which is the name the loader will
    # look for - the .so.6.0.33 file and the bare .so symlink are a
    # build-time convenience and mean nothing at runtime.
    # libgcc_s.so.1 as well, and it is the load-bearing one: it holds the
    # unwinder, and the whole point of it being shared is that the
    # executable and every library it opens use the SAME registry of
    # .eh_frame tables. Two static copies is two registries, and a throw
    # that crosses between them finds no handler and aborts.
    build/leanfs-put "$IMAGE" \
      "$PREFIX/x86_64-lean_os/lib/libstdc++.so.6.0.33" /lib/libstdc++.so.6 \
      >/dev/null || exit 1
    build/leanfs-put "$IMAGE" "$OUT/libthrow.so" /lib/libthrow.so >/dev/null || exit 1
    build/leanfs-put "$IMAGE" "$OUT/throwmain"   /bin/throwmain   >/dev/null || exit 1
    echo "build-dynamic: installed /lib/libstdc++.so.6, /lib/libthrow.so, /bin/throwmain"
  fi
  echo "build-dynamic: installed /lib/ld-lean.so, /lib/libc.so, /lib/libdyn.so, /lib/libgcc_s.so.1, /bin/dyntest, /bin/manydyn and 24 objects under /lib/many"
fi

echo "build-dynamic: done - $OUT"
