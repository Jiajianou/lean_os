#!/usr/bin/env bash
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

make -s sysroot >/dev/null || exit 1

echo "gcc-test: $(basename "$CC") tests/gcc/hello.c -o $OUT"
rm -f "$OUT"
"$CC" tests/gcc/hello.c -o "$OUT" || {
  echo "gcc-test: the compile failed - the target port does not produce a program" >&2
  exit 1
}

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

for prog in gnuhello bzip2 zlibtest minigzip pngtest djpeg cjpeg jpegtran \
            ftrender expattest xmlwf sqlite3 hbshape ssl_server2 ssl_client2 httpsget; do
  src="build/thirdparty/$prog"
  if [ -f "$IMAGE" ] && [ -x "$src" ]; then
    build/leanfs-put "$IMAGE" "$src" "/bin/$prog" >/dev/null || exit 1
    echo "gcc-test: installed /bin/$prog"
  fi
done

DATA=build/thirdparty/m100-data
if [ -f "$IMAGE" ] && [ -d "$DATA" ]; then
  n=0
  for f in "$DATA"/*; do
    build/leanfs-put "$IMAGE" "$f" "/usr/share/m100/$(basename "$f")" >/dev/null || exit 1
    n=$((n + 1))
  done
  echo "gcc-test: installed $n reference files under /usr/share/m100 - the [m100b] and [m100c] self-tests compare against them"
fi

SUITES=build/thirdparty/mbedtls-suites
if [ -f "$IMAGE" ] && [ -d "$SUITES" ]; then
  n=0
  for f in "$SUITES"/*; do
    build/leanfs-put "$IMAGE" "$f" "/usr/share/m100/mbedtls/$(basename "$f")" >/dev/null || exit 1
    n=$((n + 1))
  done
  echo "gcc-test: installed $n files of mbedtls's own test suites under /usr/share/m100/mbedtls - the [m100g] self-test runs them"
fi
