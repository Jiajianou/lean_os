#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PY_VER=3.12.7
SRC="$ROOT/build/toolchain-src"
PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
OUT="$ROOT/build/python"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

LINK="${LEANOS_PYTHON_LINK:-shared}"
case "$LINK" in
  shared|static) ;;
  *) echo "build-python: LEANOS_PYTHON_LINK must be shared or static" >&2; exit 1;;
esac

if [ ! -x "$PREFIX/bin/x86_64-lean_os-gcc" ]; then
  echo "build-python: no cross compiler - run tools/build-toolchain.sh first" >&2
  exit 1
fi

BUILD_PYTHON="${BUILD_PYTHON:-$(command -v python3.12 || true)}"
if [ -z "$BUILD_PYTHON" ]; then
  echo "build-python: no python3.12 on this machine to build $PY_VER with." >&2
  echo "              CPython's own build needs one; see this script's header." >&2
  exit 1
fi

mkdir -p "$SRC"
cd "$SRC"
if [ ! -f "Python-$PY_VER.tar.xz" ]; then
  echo "build-python: fetching Python-$PY_VER.tar.xz"
  curl -sSL -o "Python-$PY_VER.tar.xz.part" \
    "https://www.python.org/ftp/python/$PY_VER/Python-$PY_VER.tar.xz" \
    && mv "Python-$PY_VER.tar.xz.part" "Python-$PY_VER.tar.xz"
fi
PORT_STAMP=$(cat "$ROOT"/tools/python-port/* | shasum -a 256 | cut -d' ' -f1)
if [ -d "Python-$PY_VER" ] && \
   [ "$(cat "Python-$PY_VER/.lean_os-port-stamp" 2>/dev/null)" != "$PORT_STAMP" ]; then
  echo "build-python: the port changed - unpacking a clean Python-$PY_VER"
  rm -rf "Python-$PY_VER" "$OUT/build"
fi
[ -d "Python-$PY_VER" ] || tar xf "Python-$PY_VER.tar.xz"

python3 "$ROOT/tools/python-port/apply.py" "$SRC/Python-$PY_VER" || exit 1
echo "$PORT_STAMP" > "$SRC/Python-$PY_VER/.lean_os-port-stamp"

ABI_STAMP=$(cat "$ROOT"/user_space/libc/include/*.h "$ROOT"/user_space/libc/include/*/*.h \
                "$ROOT"/system_api/include/*.h 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
for d in "$OUT"/build-shared "$OUT"/build-static; do
  if [ -d "$d" ] && [ "$(cat "$d/.lean_os-abi-stamp" 2>/dev/null)" != "$ABI_STAMP" ]; then
    echo "build-python: the libc headers changed since $d was built - starting clean"
    rm -rf "$d"
  fi
done

make -s -C "$ROOT" sysroot >/dev/null || exit 1

export PATH="$PREFIX/bin:$PATH"

BUILDDIR="$OUT/build-$LINK"
mkdir -p "$BUILDDIR"
echo "$ABI_STAMP" > "$BUILDDIR/.lean_os-abi-stamp"
rm -f "$OUT/build"
ln -sfn "build-$LINK" "$OUT/build"
cd "$BUILDDIR"

cat > config.site <<'SITE'
# /dev/ptmx exists here - M85 built it, and openpty/forkpty go through it.
ac_cv_file__dev_ptmx=yes
# /dev/ptc is AIX's name for the same idea and this machine has no such
# thing.
ac_cv_file__dev_ptc=no
# No support for the pthread stack-size attribute being ignored: threads
# here are real (M79/M96) and pthread_create is the real one.
ac_cv_pthread=yes
# M99: dlopen exists here, and a link probe cannot find that out.
#
# configure decides between dynload_shlib.o and dynload_stub.o by trying
# to link a call to dlopen. Its probe is an ordinary executable, and an
# ordinary executable on this target is STATIC - which is precisely the
# kind of program that has no dlopen, because dlopen is in the loader
# and a static program never maps it (M95). The interpreter being built
# is a PIE, and for a PIE the driver puts /lib/ld-lean.so on the link
# line and dlopen resolves.
#
# So the probe's answer is right about the program it compiled and wrong
# about the program being built, which is the exact case config.site is
# for. Stated as what is true rather than as what makes the build
# proceed: dynamic loading works on this system, and there is a boot
# marker ([m99ld]) that fails if it stops.
ac_cv_func_dlopen=yes
ac_cv_header_dlfcn_h=yes
SITE

if [ ! -f Makefile ]; then
  if [ "$LINK" = shared ]; then
    SHAREDARGS="--enable-shared"
  else
    SHAREDARGS="--disable-shared MODULE_BUILDTYPE=static"
  fi
  CONFIG_SITE="$BUILDDIR/config.site" \
  "$SRC/Python-$PY_VER/configure" \
    --host=x86_64-lean_os \
    --build="$("$SRC/Python-$PY_VER/config.guess")" \
    --prefix=/usr \
    --with-build-python="$BUILD_PYTHON" \
    $SHAREDARGS \
    --disable-ipv6 \
    --without-ensurepip \
    --without-readline \
    --with-system-ffi=no \
    ac_cv_file__dev_ptmx=yes ac_cv_file__dev_ptc=no \
    > configure.log 2>&1 || { tail -40 configure.log >&2; exit 1; }
fi

echo "build-python: building CPython $PY_VER for x86_64-lean_os ($LINK)"
make -j"$JOBS" > build.log 2>&1 || { tail -60 build.log >&2; exit 1; }

echo "build-python: python built - tools/install-python.sh puts it on the image"
