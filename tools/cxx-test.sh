#!/usr/bin/env bash
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
      build/leanfs-put "$IMAGE" "$out" "/tests/gnucxx$n" >/dev/null || exit 1
    fi
    n=$((n + 1))
  done
  echo "cxx-test: $n of GCC's own libstdc++ tests compiled unmodified and installed as /tests/gnucxx0..$((n - 1))"
else
  echo "cxx-test: no GCC source tree at $SUITE - the third-party half is skipped."
fi
