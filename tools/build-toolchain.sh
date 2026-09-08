#!/usr/bin/env bash
# tools/build-toolchain.sh - M94: a compiler that knows this OS by name.
#
# Builds `x86_64-lean_os-gcc` and its binutils against the sysroot `make
# sysroot` produces, so that
#
#     x86_64-lean_os-gcc hello.c -o hello
#
# produces a program this machine runs, with **no flag invented by
# hand** - which is the entire point of the milestone, because every flag
# invented by hand is a flag someone else's build system will not pass.
#
# ---- why this is not `brew install` -------------------------------------
#
# docs/toolchain.md's table is dev-time tools this project uses. This is
# a compiler *for* this project's target, and no distribution has one
# because the target did not exist until M94 named it. The port itself is
# tools/toolchain-port/, which is nine anchored edits and one header -
# see that directory's apply.py for why it is a script of edits rather
# than a patch series, and for the line M94 draws between configuring a
# target and forking a compiler.
#
# ---- what it does NOT build ---------------------------------------------
#
# A cross compiler only: it runs on the machine you are sitting at and
# emits code for lean_os. GCC running *on* lean_os is M98 and needs this
# one first. No libstdc++ (M97), no shared libraries (M95) - the specs
# say -static and lean_os.h says why.
#
# Usage:
#   tools/build-toolchain.sh          # fetch, port, configure, build, install
#   tools/build-toolchain.sh --check  # just say whether it is already there
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

BINUTILS_VER=2.43
GCC_VER=14.2.0
TARGET=x86_64-lean_os

SRC="$ROOT/build/toolchain-src"
PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
SYSROOT="$ROOT/build/sysroot"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

if [ "${1:-}" = "--check" ]; then
  if [ -x "$PREFIX/bin/$TARGET-gcc" ]; then
    echo "build-toolchain: $PREFIX/bin/$TARGET-gcc is present"
    "$PREFIX/bin/$TARGET-gcc" --version | head -1
    exit 0
  fi
  echo "build-toolchain: no $TARGET-gcc at $PREFIX - run this script" >&2
  exit 1
fi

# The sysroot has to exist BEFORE gcc is configured: libgcc is compiled
# against it, and a libgcc built against no headers is a libgcc that
# cannot use one later.
echo "build-toolchain: generating the sysroot"
make -s -C "$ROOT" sysroot || exit 1

mkdir -p "$SRC"
cd "$SRC"

fetch() {
  local url=$1 file=$2
  [ -f "$file" ] && return 0
  echo "build-toolchain: fetching $file"
  curl -sSL -o "$file.part" "$url" && mv "$file.part" "$file"
}

fetch "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz" \
      "binutils-$BINUTILS_VER.tar.xz"
fetch "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz" \
      "gcc-$GCC_VER.tar.xz"

# ---- M99: the port's own fingerprint, and why an unpacked tree is not
# ---- kept across a change to it ---------------------------------------
#
# tools/toolchain-port/apply.py is a set of ANCHORED EDITS, and an
# anchored edit is idempotent only while the edit itself does not change:
# it re-applies when it cannot find its own replacement text, and it
# finds its anchor in the pristine text either side of the block it added
# last time. So changing an edit adds a SECOND copy of it, and leaves the
# first.
#
# That was not theoretical. M99 added `extra_options` to the `*-*-lean_os*)`
# block in gcc/config.gcc, ran the build, and got a compiler that still
# refused -pthread: config.gcc is a shell `case`, the old block was above
# the new one, and **the first arm wins**. Nothing failed, nothing warned,
# and half an hour of build later the only symptom was the option still
# not existing.
#
# The fix is to make an unpacked tree belong to the port that edited it.
# The stamp is a hash of the ANCHORED EDITS, so any change to one throws
# the tree away and unpacks a clean one - which costs a re-extract and a
# rebuild, and is the price of the edits being readable rather than a
# patch series.
#
# ---- M99: and why lean_os.h is not in the stamp ------------------------
#
# It was, and that was wrong in a way that cost an hour before anybody
# looked at it. The hazard above is specific to an *anchored edit*: it
# re-applies when it cannot find its own replacement text, so changing
# one leaves the old copy in place. gcc/config/lean_os.h is not edited -
# apply.py copies the whole file over whatever was there, which is
# idempotent by construction and has no old copy to leave behind.
#
# Hashing it anyway made every change to the target description - the
# specs, the startup files, what -pthread means, the file this port
# exists to install - throw away binutils and gcc and rebuild both from
# source. That is the tightest loop in this port paying the price of the
# loosest one, and it is why M99's driver work was cheaper to reason
# about than to try.
PORT_STAMP=$(cat "$ROOT"/tools/toolchain-port/apply.py | shasum -a 256 | cut -d' ' -f1)
for d in "binutils-$BINUTILS_VER" "gcc-$GCC_VER"; do
  if [ -d "$d" ] && [ "$(cat "$d/.lean_os-port-stamp" 2>/dev/null)" != "$PORT_STAMP" ]; then
    echo "build-toolchain: the target port changed - unpacking a clean $d"
    rm -rf "$d" "$SRC/build-${d%%-*}"
  fi
done

[ -d "binutils-$BINUTILS_VER" ] || tar xf "binutils-$BINUTILS_VER.tar.xz"
[ -d "gcc-$GCC_VER" ] || tar xf "gcc-$GCC_VER.tar.xz"

echo "build-toolchain: applying the target port"
python3 "$ROOT/tools/toolchain-port/apply.py" \
        "$SRC/binutils-$BINUTILS_VER" "$SRC/gcc-$GCC_VER" || exit 1
echo "$PORT_STAMP" > "$SRC/binutils-$BINUTILS_VER/.lean_os-port-stamp"
echo "$PORT_STAMP" > "$SRC/gcc-$GCC_VER/.lean_os-port-stamp"

# GCC needs gmp/mpfr/mpc. Homebrew's are what this machine has; pointed
# at explicitly rather than left to configure's search, because a
# configure that finds a different one than the person expected is a
# build that fails an hour later.
GMP="$(brew --prefix gmp 2>/dev/null || echo /usr/local)"
MPFR="$(brew --prefix mpfr 2>/dev/null || echo /usr/local)"
MPC="$(brew --prefix libmpc 2>/dev/null || echo /usr/local)"

mkdir -p "$SRC/build-binutils" "$SRC/build-gcc"

echo "build-toolchain: configuring binutils ($JOBS jobs)"
cd "$SRC/build-binutils"
if [ ! -f Makefile ]; then
  "../binutils-$BINUTILS_VER/configure" \
    --target="$TARGET" --prefix="$PREFIX" --with-sysroot="$SYSROOT" \
    --disable-nls --disable-werror --enable-lto \
    --with-system-zlib \
    > configure.log 2>&1 || { tail -30 configure.log >&2; exit 1; }
  # --with-system-zlib, because the bundled one does not compile against
  # a current macOS SDK: zlib's zutil.c uses K&R definitions that clash
  # with the SDK's own <_stdio.h> renaming, and the first error names a
  # header nobody here wrote. The host has a zlib; using it is both the
  # fix and the smaller thing.
fi
# MAKEINFO=true, because `makeinfo` is not on this machine and the info
# manuals are not what is being built. binutils' own build treats a
# missing makeinfo as a hard error (exit 127 from a doc rule), which
# stops a compiler build over documentation nobody asked for. `true` is
# the substitution its own maintainers document for exactly this.
echo "build-toolchain: building binutils"
make -j"$JOBS" MAKEINFO=true > build.log 2>&1 || { tail -40 build.log >&2; exit 1; }
make install MAKEINFO=true > install.log 2>&1 || { tail -20 install.log >&2; exit 1; }

export PATH="$PREFIX/bin:$PATH"

echo "build-toolchain: configuring gcc"
cd "$SRC/build-gcc"
if [ ! -f Makefile ]; then
  "../gcc-$GCC_VER/configure" \
    --target="$TARGET" --prefix="$PREFIX" --with-sysroot="$SYSROOT" \
    --enable-languages=c,c++ --disable-nls --disable-werror \
    --enable-shared --disable-libssp --disable-libquadmath \
    --disable-libgomp --disable-libatomic --disable-multilib \
    --enable-initfini-array \
    --disable-libstdcxx-verbose --enable-threads=posix \
    --with-gmp="$GMP" --with-mpfr="$MPFR" --with-mpc="$MPC" \
    --with-system-zlib \
    > configure.log 2>&1 || { tail -40 configure.log >&2; exit 1; }
fi
echo "build-toolchain: building gcc (this is the long one)"
# LIMITS_H_TEST=true, found by M98 two milestones after it mattered: GCC
# ships its own <limits.h> and decides *at build time* whether it should
# chain on to the system's, by testing for $SYSROOT/usr/include/limits.h.
# This sysroot keeps its headers in usr/local/include, so the test said
# no, the installed header did not chain, and every program compiled by
# this toolchain since M94 saw GCC's limits.h and never this libc's -
# no PATH_MAX, invisibly, until binutils' getpwd.c asked for it. Forcing
# the test true generates the chaining header (limitx.h + glimits.h +
# limity.h), which is what a build against a /usr/include sysroot would
# have produced on its own.
make -j"$JOBS" MAKEINFO=true LIMITS_H_TEST=true all-gcc > build-gcc.log 2>&1 || { tail -40 build-gcc.log >&2; exit 1; }
make -j"$JOBS" MAKEINFO=true all-target-libgcc > build-libgcc.log 2>&1 || { tail -40 build-libgcc.log >&2; exit 1; }
make MAKEINFO=true LIMITS_H_TEST=true install-gcc > install-gcc.log 2>&1 || { tail -20 install-gcc.log >&2; exit 1; }
make MAKEINFO=true install-target-libgcc > install-libgcc.log 2>&1 || { tail -20 install-libgcc.log >&2; exit 1; }

# ---- M97: the C++ runtime -----------------------------------------------
#
# `--disable-hosted-libstdcxx` builds libstdc++-v3 in FREESTANDING mode,
# which produces libsupc++.a and nothing else. That is not a reduced
# ambition, it is the correct decomposition: libsupc++ is the C++ ABI
# runtime - operator new/delete, __cxa_throw, the personality routine,
# type_info and dynamic_cast - and it is the half that has to exist
# before any C++ program links at all. The containers and iostreams sit
# on top of a libc and are a separate question with a separate set of
# things that can be missing.
#
# It is built as a third stage rather than folded into the second because
# it needs a working target libgcc to configure against, and libgcc is
# what stage two produces. `all-target-libstdc++-v3` after
# `install-target-libgcc` is that ordering stated in the makefile's own
# terms.
#
# --disable-libstdcxx-verbose: the verbose terminate handler formats a
# diagnostic through the full stdio machinery on the way to aborting.
# What it buys is a message; what it costs is a dependency on that
# machinery from the one code path that runs when the program has already
# lost control of itself.
echo "build-toolchain: building the C++ runtime (libsupc++)"
# M100: libstdc++ is compiled against this libc's headers and automake's
# dependency files do not name them, so a header change leaves its
# objects stale and `make` content. That is how libstdc++.a stayed a day
# older than M99's "-fPIC means small-model" rule for five milestones,
# and how M100's sixteen-byte pthread_mutex_t was first "rebuilt" into an
# archive of Sep 3 objects. A hash of the headers, and a clean when it
# moves - the same stamp tools/build-python.sh keeps, for the same reason.
ABI_STAMP=$(cat "$ROOT"/user_space/libc/include/*.h "$ROOT"/user_space/libc/include/*/*.h \
                "$ROOT"/system_api/include/*.h 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
#
# Thrown away rather than `make clean`ed, because libstdc++'s CONFIGURE
# answers depend on these headers too - "is <math.h> C99" is a test
# program that names twelve macros - and a clean keeps config.cache. The
# directory gone, the top-level make reconfigures it.
if [ -d "$TARGET/libstdc++-v3" ] && \
   [ "$(cat "$TARGET/libstdc++-v3/.lean_os-abi-stamp" 2>/dev/null)" != "$ABI_STAMP" ]; then
  echo "build-toolchain: the libc headers changed since libstdc++ was built - reconfiguring it"
  rm -rf "$TARGET/libstdc++-v3"
fi
make -j"$JOBS" MAKEINFO=true all-target-libstdc++-v3 > build-cxx.log 2>&1 \
  || { tail -40 build-cxx.log >&2; exit 1; }
echo "$ABI_STAMP" > "$TARGET/libstdc++-v3/.lean_os-abi-stamp"
make MAKEINFO=true install-target-libstdc++-v3 > install-cxx.log 2>&1 \
  || { tail -20 install-cxx.log >&2; exit 1; }

echo "build-toolchain: done"
"$PREFIX/bin/$TARGET-gcc" --version | head -1
echo "build-toolchain: add $PREFIX/bin to PATH"
