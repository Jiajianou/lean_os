#!/usr/bin/env bash
# tools/cxx-test.sh - M97: grade the C++ runtime.
#
# Compiles tests/cxx/exceptions.cpp with `x86_64-lean_os-g++ file.cpp -o
# prog` and **nothing else** - no -fno-exceptions, no -nostdlib, no -I,
# no -T. Same standard M94 set for the C compiler and for the same
# reason: a C++ project's build system will not be told any of that.
#
# The compile is half the test. A program that links against libsupc++
# proves the runtime exists; only running it proves the unwinder works,
# and only COUNTING destructors proves it unwound rather than jumped.
# The kernel's [m97] self-test spawns the result and reads its exit
# code, and tests/cxx/exceptions.cpp documents what each code means.
#
# Skips with a message rather than failing when the toolchain is not
# built, exactly as tools/gcc-test.sh does.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
CXX="$PREFIX/bin/x86_64-lean_os-g++"
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
OUT=build/cxxtest

if [ ! -x "$CXX" ]; then
  echo "cxx-test: no x86_64-lean_os-g++ at $PREFIX - skipped."
  echo "          tools/build-toolchain.sh builds it (M94/M97); it is not part of \`make\`."
  exit 0
fi

make -s sysroot >/dev/null || exit 1

echo "cxx-test: $(basename "$CXX") tests/cxx/exceptions.cpp -o $OUT"
rm -f "$OUT"
"$CXX" tests/cxx/exceptions.cpp -o "$OUT" || {
  echo "cxx-test: the compile failed - this toolchain does not produce a C++ program" >&2
  exit 1
}

# The unwind tables, checked before the machine ever runs it. A C++
# binary that linked without .eh_frame is a binary whose every throw
# reaches std::terminate, and it fails on the machine as an exit code
# rather than as the missing section it is.
# Read once, match many. `nm | grep -q` looks obvious and is a trap under
# `set -o pipefail`: grep -q exits the moment it matches, nm gets SIGPIPE,
# and the PIPELINE reports the failure grep did not have. It fires
# intermittently - on whichever symbol happens to be found before the
# producer finished writing - which is the worst version of a bug in a
# test. A here-string is not a pipeline.
sections=$(x86_64-elf-readelf -S "$OUT")
symbols=$(x86_64-elf-nm "$OUT")

missing=0
for sec in .eh_frame .gcc_except_table .init_array; do
  if ! grep -q " $sec " <<< "$sections"; then
    echo "cxx-test: $OUT has no $sec section" >&2
    missing=1
  fi
done
[ "$missing" = 0 ] || exit 1

# And that the frame registry is actually linked in, rather than the
# section sitting there with nobody reading it. __register_frame_info is
# crtbegin's hook into libgcc's unwinder; without it the table is data.
for sym in __register_frame_info _Unwind_RaiseException __cxa_throw __cxa_atexit; do
  if ! grep -q " T $sym$" <<< "$symbols"; then
    echo "cxx-test: $OUT does not define $sym - the C++ runtime is not linked" >&2
    exit 1
  fi
done

echo "cxx-test: .eh_frame, .gcc_except_table and the unwinder all present, $(wc -c < "$OUT" | tr -d ' ') bytes"

if [ -f "$IMAGE" ]; then
  build/leanfs-put "$IMAGE" "$OUT" /bin/cxxtest >/dev/null || exit 1
  echo "cxx-test: installed as /bin/cxxtest - the [m97] boot self-test runs it"
fi

# ---- and the standard library, which is a separate question ----------
#
# The fixture above needs only libsupc++: the unwinder, the personality
# routine and __cxa_atexit. This one needs the whole of libstdc++ and,
# through it, most of this project's libc - containers, iostreams,
# locale, RTTI and std::thread over M79's tasks.
#
# Built and graded separately on purpose. They fail for entirely
# different reasons, and a single fixture would report "C++ is broken"
# when what is actually broken is one missing declaration in <cstdio>.
OUT2=build/cxxlib
echo "cxx-test: $(basename "$CXX") tests/cxx/library.cpp -o $OUT2"
rm -f "$OUT2"
"$CXX" tests/cxx/library.cpp -o "$OUT2" || {
  echo "cxx-test: the standard library does not link for this target" >&2
  exit 1
}
echo "cxx-test: linked against libstdc++, $(wc -c < "$OUT2" | tr -d ' ') bytes"

if [ -f "$IMAGE" ]; then
  build/leanfs-put "$IMAGE" "$OUT2" /bin/cxxlib >/dev/null || exit 1
  echo "cxx-test: installed as /bin/cxxlib - the [m97] boot self-test runs it too"
fi

# ---- and C++ nobody here wrote, run unmodified ------------------------
#
# M97's "how we'll know" asks for "a C++ program nobody here wrote, with
# templates, containers and iostreams, run unmodified". These are eight
# of GCC's own libstdc++ regression tests, one from each of the areas
# this port touches, taken from the source tree the toolchain was built
# from and compiled with no edits of any kind - not a flag, not a
# #define, not a line.
#
# They are a better test than a program written here would be, and for a
# specific reason: they were written by people testing a standard
# library, not by somebody who knows what this OS can do. 23_containers/
# map/pthread6.cc is the clearest case - it puts a std::map behind
# threads because that is what its author wanted to check about
# libstdc++, and on this machine it happens to exercise M79's tasks,
# M96's futex and the C++ threading model at the same time.
#
# The list is fixed rather than discovered, so that a change which stops
# one of them compiling is a failure here rather than a quietly shorter
# run. Each is asserted to compile AND to exit 0 on the machine; the
# kernel side is the [m97] self-test.
GCCSRC="${LEANOS_GCC_SRC:-build/toolchain-src/gcc-14.2.0}"
SUITE="$GCCSRC/libstdc++-v3/testsuite"
THEIRS="
23_containers/vector/59829.cc
23_containers/map/pthread6.cc
23_containers/set/move_comparison.cc
21_strings/basic_string/56166.cc
25_algorithms/move/108846.cc
27_io/basic_stringstream/assign/1.cc
20_util/tuple/moveable2.cc
26_numerics/complex/13450.cc
"

if [ -d "$SUITE" ]; then
  n=0
  for t in $THEIRS; do
    src="$SUITE/$t"
    if [ ! -f "$src" ]; then
      echo "cxx-test: $t is not in $SUITE - the GCC source tree moved" >&2
      exit 1
    fi
    out="build/gnucxx$n"
    if ! "$CXX" -std=gnu++17 -I"$SUITE/util" -O1 -o "$out" "$src" 2>/dev/null; then
      echo "cxx-test: $t does not compile for this target" >&2
      exit 1
    fi
    if [ -f "$IMAGE" ]; then
      # /tests, not /bin. These are somebody else's regression tests,
      # not applications - putting them in /bin puts them in the
      # launcher, in the file manager's default view and in every
      # `ls /bin`, which is a lie about what is installed on the
      # machine. It also overflowed the launcher's list, which is how
      # this was noticed (see M97 and compositor.c's
      # LAUNCHER_MAX_ENTRIES).
      build/leanfs-put "$IMAGE" "$out" "/tests/gnucxx$n" >/dev/null || exit 1
    fi
    n=$((n + 1))
  done
  echo "cxx-test: $n of GCC's own libstdc++ tests compiled unmodified and installed as /tests/gnucxx0..$((n - 1))"
else
  echo "cxx-test: no GCC source tree at $SUITE - the third-party half is skipped."
fi
