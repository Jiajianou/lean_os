#!/usr/bin/env bash
set -uo pipefail

# M226. libuv's own test suite, on this machine.
#
# Node runs on libuv, and Electron runs Node inside Chromium by waiting on
# libuv's backend descriptor from a second thread - so the loop has to be one
# descriptor, which is what electron-port 0006's epoll backend is. libuv is
# plain C, so it does not need Chromium's build to be graded: this compiles
# the same libuv Node does (Node's copy, Electron's patches to it, this fork's
# patches to it) with x86_64-lean_os-gcc, links libuv's own test runner, and
# boots the machine to run it. Like CPython's suite (M99) and Node's (M224),
# it is the instrument that neither wrote its own assertions nor chose what
# to assert.
#
#   tools/libuv-test.sh                  build, install, run every test
#   tools/libuv-test.sh embed            run one test (libuv's runner names)
#   tools/libuv-test.sh --build-only     build build/libuv-test/uvtest
#
# Not in a tier: it needs the Chromium checkout for libuv's source, and the
# full suite takes most of an hour under TCG.

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SRC="$ROOT/build/chromium/src"
NODE="$SRC/third_party/electron_node"
WORK="$ROOT/build/libuv-test"
FITTED="$ROOT/build/electron-fitted"
PREFIX="$ROOT/build/toolchain/bin/x86_64-lean_os-"
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
LOG="${LEANOS_LIBUV_LOG:-build/libuv-serial.log}"

if [ ! -d "$NODE/.git" ]; then
  echo "libuv-test: no Node checkout - tools/fetch-chromium.sh, then tools/fetch-electron.sh" >&2
  exit 0
fi
if [ ! -x "${PREFIX}gcc" ]; then
  echo "libuv-test: no ${PREFIX}gcc - tools/build-toolchain.sh" >&2
  exit 1
fi

# uvtest links the SYSROOT's libc.a and reads its headers, which `make all`
# does not refresh (M224 lost a milestone of libc fixes to that). These two
# halves of `make sysroot` refresh them without deleting anything else.
make -s sysroot-headers > /dev/null || exit 1
make -s sysroot-libc > /dev/null || exit 1

# The source is Node's own deps/uv at the pinned commit, with the libuv half
# of each series applied in the order build-chromium.sh applies them - taken
# from a clean export rather than from the checkout, whose working tree is in
# whichever program's state the last Chromium build left it.
python3 "$ROOT/tools/electron-fit.py" --emit "$FITTED" > "$WORK.fit.log" 2>&1 || {
  cat "$WORK.fit.log" >&2
  echo "libuv-test: the series do not fit this checkout" >&2
  exit 1
}
rm -rf "$WORK/src"
mkdir -p "$WORK/src"
# build/ is inside this project's own repository, and git apply run there
# applies relative to THAT repository and skips - successfully - every path
# outside the directory it was run from. The ceiling keeps it from finding
# the repository, and a "Skipped patch" line is a failure all the same.
export GIT_CEILING_DIRECTORIES="$WORK"
apply_libuv() {
  local strip="$1" patch="$2" out
  out=$(cd "$WORK/src" && git apply --verbose -p"$strip" --include='deps/uv/*' < "$patch" 2>&1) || {
    echo "$out" >&2
    return 1
  }
  if echo "$out" | grep -q '^Skipped patch'; then
    echo "$out" >&2
    return 1
  fi
}
git -C "$NODE" archive HEAD deps/uv | tar -x -C "$WORK/src" || exit 1
applied=0
while read -r name; do
  [ -n "$name" ] || continue
  patch="$FITTED/node/$name"
  grep -q '^diff --git a/deps/uv/' "$patch" || continue
  apply_libuv 1 "$patch" || {
    echo "libuv-test: Electron's $name does not apply to libuv" >&2
    exit 1
  }
  applied=$((applied + 1))
done < "$FITTED/node/.patches"
for patch in "$ROOT"/tools/electron-port/*.patch; do
  grep -q '^diff --git a/third_party/electron_node/deps/uv/' "$patch" || continue
  apply_libuv 3 "$patch" || {
    echo "libuv-test: $(basename "$patch") does not apply to libuv" >&2
    exit 1
  }
  applied=$((applied + 1))
done
echo "libuv-test: libuv from Node's checkout, $applied patches applied"

UV="$WORK/src/deps/uv"
cd "$UV" || exit 1

# The sources are unofficial.gni's for this platform - asked of the patched
# file rather than repeated here, so the build graded is the build Node gets.
LIBRARY_SOURCES=$(python3 - <<'EOF'
import ast, re
gyp = ast.literal_eval(re.sub(r'#.*', '', open('uv.gyp').read()))['variables']
gni = open('unofficial.gni').read()
arm = gni[gni.index('} else if (is_linux) {'):]
arm = arm[:arm.index(']')]
ours = re.findall(r'"(src/[^"]+\.c)"', arm)
common = [f for f in gyp['uv_sources_common'] + gyp['uv_sources_posix'] if f.endswith('.c')]
print(' '.join(common + ['src/unix/proctitle.c'] + ours))
EOF
) || exit 1
TEST_SOURCES=$(python3 - <<'EOF'
import re
s = open('CMakeLists.txt').read()
i = s.index('list(APPEND uv_test_sources', s.index('Small hack'))
print(' '.join(f for f in s[i:s.index(')', i)].split()[2:] if f.endswith('.c')))
EOF
) || exit 1

# libuv's own flags, from unofficial.gni and CMakeLists.txt. HAVE_EPOLL is
# what test-embed.c defines for itself on Linux: the embedding thread waits
# on uv_backend_fd() with epoll_wait, which is how Electron waits on it.
CFLAGS="-O1 -std=gnu11 -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -Iinclude -Isrc"
mkdir -p "$WORK/obj" "$WORK/test-obj"
rm -f "$WORK"/obj/*.o "$WORK"/test-obj/*.o
failed=0
for f in $LIBRARY_SOURCES; do
  "${PREFIX}gcc" $CFLAGS -c "$f" -o "$WORK/obj/$(basename "$f" .c).o" 2> "$WORK/obj/$(basename "$f" .c).err" || {
    echo "libuv-test: $f did not compile:" >&2
    head -5 "$WORK/obj/$(basename "$f" .c).err" >&2
    failed=1
  }
done
for f in $TEST_SOURCES test/runner-unix.c; do
  extra="-DHAVE_EPOLL=1"
  # test-tty.c picks the header openpty is declared in by operating system
  # name and knows none for this one; <pty.h> is where it is here, as on
  # Linux. Test code only - nothing Node links.
  [ "$f" = test/test-tty.c ] && extra="$extra -include pty.h"
  "${PREFIX}gcc" $CFLAGS -Itest $extra -c "$f" -o "$WORK/test-obj/$(basename "$f" .c).o" \
    2> "$WORK/test-obj/$(basename "$f" .c).err" || {
    echo "libuv-test: $f did not compile:" >&2
    head -5 "$WORK/test-obj/$(basename "$f" .c).err" >&2
    failed=1
  }
done
[ "$failed" = 0 ] || exit 1
rm -f "$WORK/libuv.a"
# An archive, as CMake builds it: several tests #include a library source to
# reach its static functions, and only an archive lets those copies win.
"${PREFIX}ar" rcs "$WORK/libuv.a" "$WORK"/obj/*.o || exit 1
"${PREFIX}gcc" -o "$WORK/uvtest" "$WORK"/test-obj/*.o "$WORK/libuv.a" -lm -ldl \
  2> "$WORK/link.err" || {
  head -20 "$WORK/link.err" >&2
  exit 1
}
echo "libuv-test: $(echo $LIBRARY_SOURCES | wc -w | tr -d ' ') library sources," \
     "$(echo $TEST_SOURCES | wc -w | tr -d ' ') test sources -> build/libuv-test/uvtest"
cd "$ROOT"

if [ "${1:-}" = "--build-only" ]; then
  exit 0
fi

make -s leanfs-put > /dev/null 2>&1
build/leanfs-put "$IMAGE" "$WORK/uvtest" /bin/uvtest > /dev/null || exit 1

OVMF_CODE=build/ovmf/OVMF_CODE.fd
OVMF_VARS=build/ovmf/OVMF_VARS.fd
[ -f "$OVMF_CODE" ] || ./tools/build-ovmf.sh || exit 1
OVMF_VARS_RUNTIME=build/ovmf/OVMF_VARS_libuv.fd
cp "$OVMF_VARS" "$OVMF_VARS_RUNTIME"
: > "$LOG"

# The runner runs each test in a child process of its own, which is
# libuv's design: a test that crashes is one failure, not the end of the run.
# This machine has one principal and reports it as uid 0 (M65), and libuv's
# runner refuses root unless UV_RUN_AS_ROOT says it was meant. The switch
# takes it as a leading NAME=VALUE rather than through /bin/env: uvtest is
# granted the network by name, and a program exec'd by env keeps env's set.
COMMAND="UV_RUN_AS_ROOT=1 uvtest${1:+ $1}"
SECONDS_TO_RUN="${LEANOS_LIBUV_SECONDS:-5400}"
echo "libuv-test: running $COMMAND on the machine - ceiling ${SECONDS_TO_RUN}s"
qemu-system-x86_64 \
  -m "${QEMU_MEM:-4096}" -smp "${QEMU_CPUS:-1}" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  -drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE" \
  -device virtio-blk-pci,drive=disk0 -display none \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -fw_cfg name=opt/leanos/selftest,string=1 \
  -fw_cfg "name=opt/leanos/run,string=$COMMAND" \
  -serial file:"$LOG" -monitor none ${LEANOS_LIBUV_QEMU_ARGS:-} &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

deadline=$(( $(date +%s) + SECONDS_TO_RUN ))
while kill -0 "$QEMU_PID" 2>/dev/null; do
  grep -q "\[run\] done\|KERNEL PANIC" "$LOG" 2>/dev/null && break
  if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "libuv-test: the machine did not finish within ${SECONDS_TO_RUN}s" >&2
    break
  fi
  sleep 2
done
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null

# libuv's runner speaks TAP: "ok N - name", "not ok N - name", and a skip
# is "ok N - name # SKIP reason". The serial line ends each with \r\n, and a
# $ that a \r stands in front of matches nothing.
tr -d '\r' < "$LOG" > "$LOG.tap" && mv "$LOG.tap" "$LOG"
passed=$(grep -cE '^ok [0-9]+ - [^#]*$' "$LOG")
skipped=$(grep -cE '^ok [0-9]+ - .*# SKIP' "$LOG")
failed=$(grep -cE '^not ok [0-9]+ - ' "$LOG")
echo "libuv-test: $passed passed, $failed failed, $skipped skipped"
grep -E '^not ok [0-9]+ - ' "$LOG" | sed 's/^/  /'

# What this milestone is for, required by name: the loop embedded in another
# - Electron's whole arrangement - and the ordinary loop on the new backend.
status=0
for required in embed embed_with_external_timer loop_alive loop_stop timer \
                tcp_ping_pong pipe_ping_pong poll_duplex async; do
  if [ -z "${1:-}" ] || [ "${1:-}" = "$required" ]; then
    grep -qE "^ok [0-9]+ - $required\$" "$LOG" || {
      echo "libuv-test: required test $required did not pass" >&2
      status=1
    }
  fi
done
grep -q "\[run\] exit" "$LOG" || status=1
exit $status
