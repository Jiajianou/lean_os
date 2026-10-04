#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

# M223. Node.js, built by tools/build-chromium.sh out of Electron's
# configuration of the browser's own Chromium:
#
#   LEANOS_CHROMIUM_SERIES=electron tools/build-chromium.sh third_party/electron_node:node
#
# This grades what that build produced and puts it on the image; whether it
# WORKS is the [m223] boot self-test's question, which runs tests/node/smoke.js
# on the machine.

NODE="$ROOT/build/chromium/src/out/ElectronNode/node"
PREFIX="$ROOT/build/toolchain/bin/x86_64-lean_os-"
IMAGE="${LEANOS_IMAGE:-$ROOT/build/os-image.bin}"
STRIPPED="$ROOT/build/node.stripped"

PASS=0
FAIL=0
check() {
  if [ "$1" = "0" ]; then
    echo "node-test: pass - $2"
    PASS=$((PASS + 1))
  else
    echo "node-test: FAIL - $2" >&2
    FAIL=$((FAIL + 1))
  fi
}

if command -v node > /dev/null; then
  node --check tests/node/smoke.js
  check $? "tests/node/smoke.js parses, by the host's own node"
fi

if [ ! -x "$NODE" ]; then
  echo "node-test: no $NODE - build it with the line above; skipping the rest"
  echo "node-test: $PASS passed, $FAIL failed"
  [ "$FAIL" = "0" ]
  exit
fi

# A libc change does not reach a ported program until it is relinked, and
# Chromium's ninja does not track the sysroot's libc.a - so a node older than
# it was linked against a C library that is no longer this one. M223 shipped
# getpeername to the machine and tested a node that did not have it.
LIBC="$ROOT/build/sysroot/usr/lib/libc.a"
[ ! -f "$LIBC" ] || [ "$NODE" -nt "$LIBC" ]
check $? "node is newer than the sysroot's libc.a (rm it and rebuild if not - ninja will not)"

# M224: and the sysroot's libc.a is the tree's. make all builds build/libc.a
# and nothing copies it into the sysroot but `make sysroot`, so the check
# above passed for a whole milestone of libc fixes that node never had -
# kill(2) still left errno alone on the machine after it had been fixed here.
cmp -s "$ROOT/build/libc.a" "$LIBC"
check $? "the sysroot's libc.a is build/libc.a (run make sysroot if not)"

HEADER=$("${PREFIX}readelf" -h "$NODE")
grep -q 'Type: *EXEC' <<< "$HEADER" && grep -q 'Machine: *Advanced Micro Devices X86-64' <<< "$HEADER"
check $? "out/ElectronNode/node is an x86-64 ELF executable"

! "${PREFIX}readelf" -l "$NODE" | grep -q INTERP
check $? "with no interpreter - one static file, the shape every program here has"

ENTRY=$(awk '/Entry point address/ {print $4}' <<< "$HEADER")
[ "$((ENTRY >= 0x8000000000))" = "1" ]
check $? "loaded at 512 GiB where this OS puts programs (entry $ENTRY)"

SYMBOLS=$("${PREFIX}nm" "$NODE")
# The generic POSIX libuv backend and not Linux's: linux.c's io_uring and
# inotify entry points must be absent, and the OS file patch 0003 adds must be
# what answers uv_cpu_info.
! grep -qE ' [Tt] (uv__iou_|uv__inotify_)' <<< "$SYMBOLS"
check $? "libuv is the poll(2) backend - no io_uring or inotify in the binary"
grep -qE ' T uv__platform_loop_init$' <<< "$SYMBOLS" && grep -qE ' T uv_cpu_info$' <<< "$SYMBOLS"
check $? "and the OS file that answers uv_cpu_info is linked"

grep -qE ' [Tt] ZSTD_compressStream2$' <<< "$SYMBOLS"
check $? "zstd's compressor is in it - node's zlib binding calls it (patch 0005); Chromium builds zstd with hidden visibility, so it is a local symbol"

if [ ! -f "$IMAGE" ]; then
  echo "node-test: no $IMAGE - run make, then this again"
else
  "${PREFIX}strip" -o "$STRIPPED" "$NODE"
  check $? "stripped to $(( $(wc -c < "$STRIPPED") / 1024 / 1024 )) MB from $(( $(wc -c < "$NODE") / 1024 / 1024 )) MB"
  make -s leanfs-put > /dev/null 2>&1
  build/leanfs-put "$IMAGE" "$STRIPPED" /bin/node > /dev/null
  check $? "installed as /bin/node"
  build/leanfs-put -r "$IMAGE" tests/node /lib/node-test > /dev/null
  check $? "and tests/node as /lib/node-test - the [m223] boot self-test runs smoke.js"
fi

echo "node-test: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ]
