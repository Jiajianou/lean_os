#!/usr/bin/env bash
# tools/build-python.sh - M99: CPython, built for this machine.
#
# M80 tried this and was abandoned; its own entry says why, and the two
# sentences that matter are that it cross-compiled, froze, and never
# linked. Everything that failure was waiting on has since landed: a
# filesystem that can hold a source tree (M93), fork and exec (M83/M84),
# a real terminal (M85), threads with TLS and a futex (M96), a target
# triple a configure script recognises (M94), and a compiler that runs
# on the machine itself (M98).
#
# ---- the two builds, and why there are two ----------------------------
#
# M99's first bullet asks for `./configure && make` **on the machine**.
# This script is not that: it is the CROSS build, and it exists because
# the on-machine build has to have something to be compared against and
# because every gap in this project's libc is cheaper to find here. The
# order is M63's method - find what a build asks for, add it, repeat -
# run where the loop takes seconds instead of minutes.
#
# What the machine then does with the same source is the milestone's own
# bullet and is graded by tools/bootstrap-test.sh's sibling, not here.
#
# ---- the decisions, recorded rather than discovered twice -------------
#
# --with-build-python: cross-compiling CPython needs a Python of the SAME
#   minor version already running on the build machine, because the build
#   runs `python3 -m sysconfig`, freezes modules and compiles the standard
#   library to .pyc with it. This is not a shortcut around the port; it is
#   how every cross-built CPython in existence is built.
#
# --disable-shared: this machine has a dynamic loader (M95) and the
#   deferral list has the condition under which more of it gets built.
#   A static python3 is the one that can run before any of that is
#   settled, and M99's extension-module bullet is what re-opens it.
#
# MODULE_BUILDTYPE=static: and this is the half of that decision that
#   has to be stated rather than implied. CPython builds the standard
#   library's C modules as .so files by default and links them with
#   $(LDSHARED), which in a cross build is the HOST's linker - the first
#   run of this got "ld: Missing -arch option" from Apple's ld, forty
#   times. But pointing LDSHARED at the cross compiler would not be the
#   fix either: **a static program on this machine cannot dlopen**,
#   because dlopen lives in /lib/ld-lean.so and a static executable
#   never maps it (M95). So a static python3 gets its C modules linked
#   in, which is upstream's own supported configuration for exactly this
#   case and needs no patch. M99's third bullet - extension modules as
#   shared objects, imported at runtime over M95 - is a *dynamic*
#   python3, and it is the second increment rather than a flag on this
#   one.
#
# --without-ensurepip / no network: M99 says so in its own last bullet.
#   pip reaching the network needs TLS and a certificate store, and
#   neither exists here; a wheel installed from a local file is the same
#   mechanism without the fiction.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PY_VER=3.12.7
SRC="$ROOT/build/toolchain-src"
PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
OUT="$ROOT/build/python"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

if [ ! -x "$PREFIX/bin/x86_64-lean_os-gcc" ]; then
  echo "build-python: no cross compiler - run tools/build-toolchain.sh first" >&2
  exit 1
fi

# The build Python. Same minor version as the source being built, which
# is a requirement rather than a preference - see the header.
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
# The port's fingerprint, for the reason tools/build-toolchain.sh states
# at length: an anchored edit re-applies when its own replacement text
# changes, and the copy it added last time stays where it was. A tree
# that does not belong to the port that is about to edit it gets thrown
# away rather than edited twice.
PORT_STAMP=$(cat "$ROOT"/tools/python-port/* | shasum -a 256 | cut -d' ' -f1)
if [ -d "Python-$PY_VER" ] && \
   [ "$(cat "Python-$PY_VER/.lean_os-port-stamp" 2>/dev/null)" != "$PORT_STAMP" ]; then
  echo "build-python: the port changed - unpacking a clean Python-$PY_VER"
  rm -rf "Python-$PY_VER" "$OUT/build"
fi
[ -d "Python-$PY_VER" ] || tar xf "Python-$PY_VER.tar.xz"

python3 "$ROOT/tools/python-port/apply.py" "$SRC/Python-$PY_VER" || exit 1
echo "$PORT_STAMP" > "$SRC/Python-$PY_VER/.lean_os-port-stamp"

# The sysroot is regenerated first for the reason build-native-toolchain.sh
# gives: this build grades the libc as much as it grades anything, and it
# should grade the libc in the tree.
make -s -C "$ROOT" sysroot >/dev/null || exit 1

export PATH="$PREFIX/bin:$PATH"

BUILDDIR="$OUT/build"
mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

# ---- config.site: the answers configure cannot run a program to get ----
#
# Every one of these is a fact about this machine that a cross build
# cannot discover by running a test program, and every one is written
# here rather than guessed by configure's fallback. They are stated as
# what is true, not as what makes the build proceed - a wrong answer here
# is a Python that miscompiles in a way that only shows up at runtime.
cat > config.site <<'SITE'
# /dev/ptmx exists here - M85 built it, and openpty/forkpty go through it.
ac_cv_file__dev_ptmx=yes
# /dev/ptc is AIX's name for the same idea and this machine has no such
# thing.
ac_cv_file__dev_ptc=no
# No support for the pthread stack-size attribute being ignored: threads
# here are real (M79/M96) and pthread_create is the real one.
ac_cv_pthread=yes
SITE

if [ ! -f Makefile ]; then
  CONFIG_SITE="$BUILDDIR/config.site" \
  "$SRC/Python-$PY_VER/configure" \
    --host=x86_64-lean_os \
    --build="$("$SRC/Python-$PY_VER/config.guess")" \
    --prefix=/usr \
    --with-build-python="$BUILD_PYTHON" \
    --disable-shared \
    --disable-ipv6 \
    --without-ensurepip \
    --without-readline \
    --with-system-ffi=no \
    MODULE_BUILDTYPE=static \
    ac_cv_file__dev_ptmx=yes ac_cv_file__dev_ptc=no \
    > configure.log 2>&1 || { tail -40 configure.log >&2; exit 1; }
fi

echo "build-python: building CPython $PY_VER for x86_64-lean_os"
make -j"$JOBS" > build.log 2>&1 || { tail -60 build.log >&2; exit 1; }

echo "build-python: python built - tools/install-python.sh puts it on the image"
