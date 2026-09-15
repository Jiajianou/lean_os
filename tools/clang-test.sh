#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
CC="$PREFIX/bin/x86_64-lean_os-clang"
CXX="$PREFIX/bin/x86_64-lean_os-clang++"
GCC="$PREFIX/bin/x86_64-lean_os-gcc"
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
OUT=build/clangtest
MIXED=build/mixedtest
FAILED=0

if [ ! -x "$CC" ]; then
  echo "clang-test: no x86_64-lean_os-clang at $PREFIX - skipped."
  echo "            tools/build-clang.sh builds it (M121); it is not part of \`make\`."
  exit 0
fi

make -s sysroot >/dev/null || exit 1

fail() { echo "clang-test: $*" >&2; FAILED=1; }

echo "clang-test: what the preprocessor defines, with no --target"
DEFS=$("$CC" -dM -E -x c /dev/null 2>/dev/null)
for macro in __lean_os__ __lean_os __unix__ __unix __ELF__ __x86_64__; do
  case "$DEFS" in
    *"#define $macro"*) ;;
    *) fail "$macro is not defined - the Triple or TargetInfo edit did not take";;
  esac
done
TRIPLE=$("$CC" -print-target-triple 2>/dev/null)
[ "$TRIPLE" = "x86_64-unknown-lean_os" ] || \
  fail "the default target triple is '$TRIPLE', not x86_64-unknown-lean_os"
echo "clang-test: $TRIPLE, and six macros a configure script asks for"

echo "clang-test: what the driver supplies so that nobody has to"
SPEC=$("$CC" -### tests/clang/hello.c -o "$OUT" 2>&1)
check_spec() {
  case "$SPEC" in
    *"$1"*) ;;
    *) fail "the driver did not supply $1 ($2)";;
  esac
}
check_spec '"-mcmodel=large"'          'a static program here is linked at 512 GiB'
check_spec '"-disable-red-zone"'       'the kernel builds a signal frame on this stack'
check_spec '"-static"'                 'there is no dynamic loader on a plain link'
check_spec '"--no-relax"'              'ld cannot relax GOTPCREL at 512 GiB'
check_spec 'lean_os.ld'                'the linker script every lean_os program uses'
check_spec 'crt1.o'                    "this project's crt0, under the name drivers look for"
check_spec 'crtbegin.o'                'the startup files, in ELF order'
check_spec 'crtn.o'                    'and their closing half'
check_spec '"-lc"'                     'the C library'
check_spec '"-lgcc_eh"'                "M97's split unwinder, which a static link needs"
check_spec 'x86_64-lean_os-ld'         "the linker from M94's binutils, not the host's"
check_spec '/build/sysroot'            'the sysroot, baked in rather than passed'
check_spec 'noexecstack'               'a stack this kernel maps without VMM_FLAG_EXEC'
case "$SPEC" in
  *'"-fno-plt"'*) fail "-fno-plt on a static link, where the large model never emits a PLT32";;
esac
case "$SPEC" in
  *'"-mcmodel=small"'*) fail "the small code model on a static link";;
esac
echo "clang-test: thirteen flags supplied by the driver, two correctly absent"

PICSPEC=$("$CC" -### -fPIC -shared tests/clang/hello.c -o /dev/null 2>&1)
case "$PICSPEC" in
  *'"-mcmodel=large"'*) fail "-fPIC still got the large code model";;
esac
case "$PICSPEC" in
  *'"-fno-plt"'*) ;;
  *) fail "-fPIC did not get -fno-plt";;
esac
case "$PICSPEC" in
  *'"-ftls-model=initial-exec"'*) ;;
  *) fail "-fPIC did not get -ftls-model=initial-exec (there is no __tls_get_addr here)";;
esac
case "$PICSPEC" in
  *crt1.o*) fail "a -shared link got a crt1.o, whose call to main does not exist";;
esac
echo "clang-test: and -fPIC -shared flips the code model, the PLT and the TLS model"

# M145. -pie and -no-pie are a PAIR and the last one wins. M121's driver read
# only -pie, so -no-pie was silently ignored and a build system that adds -pie
# to every link - Chromium's does - could not take it back. The bug produced a
# position-independent executable naming an interpreter this machine has no
# path for, and nothing here asked the question until //base was linked.
PIESPEC=$("$CC" -### -pie tests/clang/hello.c -o /dev/null 2>&1)
case "$PIESPEC" in
  *'"-pie"'*) ;;
  *) fail "-pie did not reach the linker";;
esac
NOPIESPEC=$("$CC" -### -pie -no-pie tests/clang/hello.c -o /dev/null 2>&1)
case "$NOPIESPEC" in
  *'"-pie"'*) fail "-no-pie after -pie still linked position-independent";;
esac
case "$NOPIESPEC" in
  *'"-static"'*) ;;
  *) fail "-no-pie did not fall back to a static link";;
esac
case "$NOPIESPEC" in
  *lean_os.ld*) ;;
  *) fail "-no-pie did not get the linker script a static program needs";;
esac
echo "clang-test: and -no-pie after -pie wins, which is what a pair of flags means"

[ "$FAILED" -eq 0 ] || exit 1

echo "clang-test: $(basename "$CC") tests/clang/hello.c -o $OUT"
rm -f "$OUT"
"$CC" tests/clang/hello.c -o "$OUT" || {
  echo "clang-test: the compile failed - the target port does not produce a program" >&2
  exit 1
}

shape_of() {
  x86_64-elf-readelf -h "$1" | awk '/^  Type:/{t=$2} /Entry point/{e=$4} END{print t, e}'
}
read -r type entry <<EOF
$(shape_of "$OUT")
EOF
if [ "$type" != "EXEC" ]; then
  echo "clang-test: produced a $type, not an EXEC - the link script did not apply" >&2
  exit 1
fi
case "$entry" in
  0x80*) ;;
  *) echo "clang-test: entry point $entry is not in this OS's image region" >&2; exit 1;;
esac
echo "clang-test: EXEC, entry $entry, $(wc -c < "$OUT" | tr -d ' ') bytes"

if [ -x "$GCC" ]; then
  rm -f build/gcc-shape-check
  "$GCC" tests/clang/hello.c -o build/gcc-shape-check 2>/dev/null && {
    read -r gtype gentry <<EOF
$(shape_of build/gcc-shape-check)
EOF
    [ "$gtype" = "$type" ] || fail "gcc produced a $gtype where clang produced a $type"
    [ "$gentry" = "$entry" ] || \
      fail "gcc entered at $gentry and clang at $entry - the two disagree about the load address"
    echo "clang-test: gcc agrees: $gtype, entry $gentry"
  }
  rm -f build/gcc-shape-check
fi

if [ -x "$GCC" ]; then
  echo "clang-test: clang's object + gcc's object -> $MIXED"
  rm -f "$MIXED" build/abi_main.o build/abi_peer.o
  "$CC" -c tests/clang/abi_main.c -o build/abi_main.o || \
    fail "clang would not compile abi_main.c"
  "$GCC" -c tests/clang/abi_peer.c -o build/abi_peer.o || \
    fail "gcc would not compile abi_peer.c"
  "$CC" build/abi_main.o build/abi_peer.o -o "$MIXED" || \
    fail "the two objects would not link together"
  if [ -f "$MIXED" ]; then
    echo "clang-test: linked, $(wc -c < "$MIXED" | tr -d ' ') bytes"
  fi
  rm -f build/abi_main.o build/abi_peer.o
else
  echo "clang-test: no x86_64-lean_os-gcc - the cross-compiler ABI fixture is skipped"
fi

if [ -x "$CXX" ] && [ -f "$PREFIX/x86_64-lean_os/lib/libc++.a" ]; then
  echo "clang-test: $(basename "$CXX") tests/clang/cxx.cpp -o build/clangcxxtest"
  rm -f build/clangcxxtest
  "$CXX" tests/clang/cxx.cpp -o build/clangcxxtest || \
    fail "clang++ would not build the C++ fixture against libc++"
else
  echo "clang-test: no libc++ in the sysroot - the C++ half is skipped."
  echo "            tools/build-libcxx.sh builds it (M121)."
fi

[ "$FAILED" -eq 0 ] || exit 1

if [ -f "$IMAGE" ]; then
  build/leanfs-put "$IMAGE" "$OUT" /bin/clangtest >/dev/null || exit 1
  echo "clang-test: installed as /bin/clangtest - the [m121] boot self-test runs it"
  if [ -x "$MIXED" ]; then
    build/leanfs-put "$IMAGE" "$MIXED" /bin/mixedtest >/dev/null || exit 1
    echo "clang-test: installed as /bin/mixedtest"
  fi
  if [ -x build/clangcxxtest ]; then
    build/leanfs-put "$IMAGE" build/clangcxxtest /bin/clangcxxtest >/dev/null || exit 1
    echo "clang-test: installed as /bin/clangcxxtest"
  fi
fi
