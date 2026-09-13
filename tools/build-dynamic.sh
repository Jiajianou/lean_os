#!/usr/bin/env bash
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

echo "build-dynamic: ld-lean.so and libc.so come from the sysroot"
cp "$SYSROOT/ld-lean.so" "$OUT/ld-lean.so" || exit 1
cp "$SYSROOT/libc.so"    "$OUT/libc.so"    || exit 1

echo "build-dynamic: dyntest"
"$CC" -pie -O2 -Wall -o "$OUT/dyntest" tests/dynamic/dyntest.c || exit 1

echo "build-dynamic: libdyn.so"
"$CC" -shared -O2 -Wall -o "$OUT/libdyn.so" tests/dynamic/libdyn.c \
      -Wl,-soname,libdyn.so || exit 1

echo "build-dynamic: manylib0..23 and manydyn"
MANY=""
for i in $(seq 0 23); do
  sed "s/@N@/$i/g" tests/dynamic/manylib.c.in > "$OUT/manylib$i.c" || exit 1
  "$CC" -shared -O2 -Wall -o "$OUT/manylib$i.so" "$OUT/manylib$i.c" \
        -Wl,-soname,"manylib$i.so" || exit 1
  MANY="$MANY $OUT/manylib$i.so"
done
"$CC" -pie -O2 -Wall -o "$OUT/manydyn" tests/dynamic/manydyn.c || exit 1

CXX="$PREFIX/bin/x86_64-lean_os-g++"
if [ -x "$CXX" ] && [ -f "$PREFIX/x86_64-lean_os/lib/libstdc++.so" ]; then
  echo "build-dynamic: libthrow.so"
  "$CXX" -shared -O1 -Wall -Itests/cxx \
        -o "$OUT/libthrow.so" tests/cxx/throwlib.cpp \
        -Wl,-soname,libthrow.so || exit 1

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
  for i in $(seq 0 23); do
    build/leanfs-put "$IMAGE" "$OUT/manylib$i.so" "/lib/many/manylib$i.so" \
      >/dev/null || exit 1
  done
  build/leanfs-put "$IMAGE" \
    "$PREFIX/x86_64-lean_os/lib/libgcc_s.so.1" /lib/libgcc_s.so.1 \
    >/dev/null || exit 1
  if [ -f "$OUT/throwmain" ]; then
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
