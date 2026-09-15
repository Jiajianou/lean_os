#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SRC="$ROOT/build/chromium/src"
DEPOT="$ROOT/build/chromium/depot_tools"
FORK="$ROOT/third_party/chromium/lean_os"
LINK="$SRC/lean_os"
OUT_NAME="${LEANOS_CHROMIUM_OUT:-LeanOS}"
OUT="$SRC/out/$OUT_NAME"
PREFIX="$ROOT/build/toolchain/bin/x86_64-lean_os-"
CHROMIUM_CLANG="$SRC/third_party/llvm-build/Release+Asserts/bin"
if [ "${LEANOS_CHROMIUM_CC:-lean_os}" = "chromium" ]; then
  CC="$CHROMIUM_CLANG/clang"
  CXX="$CHROMIUM_CLANG/clang++"
  AR="$CHROMIUM_CLANG/llvm-ar"
  NM="$CHROMIUM_CLANG/llvm-nm"
  READELF="$CHROMIUM_CLANG/llvm-readelf"
  CLANG_BASE="$SRC/third_party/llvm-build/Release+Asserts"
  CLANG_VER="24"
  LEANOS_TARGET_FLAG=""
else
  CC="${PREFIX}clang"
  CXX="${PREFIX}clang++"
  AR="${PREFIX}ar"
  NM="${PREFIX}nm"
  READELF="${PREFIX}readelf"
  LEANOS_TARGET_FLAG="--target=x86_64-lean_os"
  CLANG_VER="24"
  # clang_base_path is one path for the whole build, and the HOST half of a
  # Rust build uses it too - build scripts and proc macros are native
  # binaries, and they want macOS's compiler-rt. So it points at Chromium's
  # own clang even when this OS's clang is the one compiling the target: cc
  # and cxx below are what decide that, and they stay ours.
  CLANG_BASE="$SRC/third_party/llvm-build/Release+Asserts"
fi
SYSROOT="$ROOT/build/sysroot"
TARGET="${1:-base}"

if [ ! -d "$SRC" ]; then
  echo "build-chromium: no checkout - run tools/fetch-chromium.sh" >&2
  exit 1
fi
if [ ! -x "$CC" ] && [ ! -x "${PREFIX}clang" ]; then
  echo "build-chromium: ${PREFIX}clang is missing - run tools/build-clang.sh" >&2
  exit 1
fi

rm -f "$LINK"
ln -s "$FORK" "$LINK"

# The fork's patch series. Every one of these is meant to be a seam somebody
# else could use rather than a mention of this OS, which is why Chromium's
# tree still contains the word lean_os nowhere.
#
# The series is applied to a CLEAN tree every time rather than patch by patch
# onto whatever is there. M145 is why: two patches can touch one file - 0013
# edits base/BUILD.gn after 0009 does - and then "is this patch already
# applied" has no answer, because reversing the earlier one against a file the
# later one has moved past fails. Resetting first makes the question
# unnecessary, and it also means an edit made by hand in the checkout is
# discarded rather than silently becoming part of the build.
PATCH_FILES=$(ls "$ROOT"/tools/chromium-port/*.patch 2>/dev/null)
if [ -n "$PATCH_FILES" ]; then
  TOUCHED=$(sed -n 's|^--- a/||p' $PATCH_FILES | sort -u)
  for f in $TOUCHED; do
    # A path git does not track here belongs to one of the sub-repositories
    # the checkout is assembled from - third_party/perfetto is one - and
    # those are left as they are.
    (cd "$SRC" && git ls-files --error-unmatch "$f" > /dev/null 2>&1) || continue
    (cd "$SRC" && git checkout -- "$f") || exit 1
  done
fi
for patch in $PATCH_FILES; do
  name=$(basename "$patch")
  if (cd "$SRC" && git apply -p1 < "$patch") 2>/dev/null; then
    echo "build-chromium: $name applied"
  elif (cd "$SRC" && git apply --check --reverse -p1 < "$patch") 2>/dev/null; then
    echo "build-chromium: $name already applied - it is outside this repository"
  else
    echo "build-chromium: $name does not apply to this checkout" >&2
    exit 1
  fi
done

# Rust. Chromium builds the standard library from the rust-src beside its own
# rustc, and that is the configuration it supports: an external sysroot turns
# use_chromium_rust_toolchain off, which turns enable_cpp_api_from_rust off,
# and //components/cbor then depends on a bindings target nobody defined. So
# the fork goes on the checkout's own rust-src instead, and the only thing
# this OS has to inject is a rustc that knows where x86_64-unknown-lean_os
# is described - which gcc_toolchain already has a hook for.
RUST_TARGET=x86_64-unknown-lean_os
RUST_SRC="$SRC/third_party/rust-toolchain/lib/rustlib/src/rust/library"
ENABLE_RUST=false
if [ -d "$RUST_SRC/std" ]; then
  echo "build-chromium: applying the lean_os Rust port to the checkout's rust-src"
  python3 "$ROOT/tools/rust-port/apply.py" "$RUST_SRC" > /dev/null || exit 1
  ENABLE_RUST=true
else
  echo "build-chromium: no rust-src in the checkout - building without Rust"
fi

BUILTINS_DIR="$CLANG_BASE/lib/clang/$CLANG_VER/lib/x86_64-unknown-linux-gnu"
LIBGCC="$ROOT/build/toolchain/lib/gcc/x86_64-lean_os/14.2.0/libgcc.a"
if [ ! -f "$BUILTINS_DIR/libclang_rt.builtins.a" ]; then
  echo "build-chromium: exposing libgcc as the builtins library clang expects"
  mkdir -p "$BUILTINS_DIR"
  cp "$LIBGCC" "$BUILTINS_DIR/libclang_rt.builtins.a" || exit 1
fi

mkdir -p "$OUT"
cat > "$OUT/args.gn" <<ARGS
target_os = "linux"
target_cpu = "x64"
custom_toolchain = "//lean_os/toolchain:x64"
host_toolchain = "//build/toolchain/mac:clang_arm64"
lean_os_cc = "$CC"
lean_os_cxx = "$CXX"
lean_os_ar = "$AR"
lean_os_nm = "$NM"
lean_os_readelf = "$READELF"
enable_rust = $ENABLE_RUST
rust_abi_target_override = "$RUST_TARGET"
has_linux_kernel = false
libc_provides_libatomic = false
has_symbol_interposition = false
partition_alloc_has_linux_kernel = false
enable_pkeys = false
clang_base_path = "$CLANG_BASE"
clang_version = "$CLANG_VER"
ozone_extra_path = "//lean_os/ozone_extra.gni"

is_debug = false
is_component_build = false
symbol_level = 0
blink_symbol_level = 0
use_remoteexec = false
dcheck_always_on = false
clang_use_chrome_plugins = false
treat_warnings_as_errors = false

use_sysroot = false
lean_os_sysroot = "$SYSROOT"
lean_os_target = "$LEANOS_TARGET_FLAG"
use_custom_libcxx = true
libcxx_provides_default_rune_table = true
use_glib = false
use_dbus = false
use_udev = false
use_gio = false
use_ozone = true
use_aura = true
angle_enable_metal = false
use_gtk = false
use_qt5 = false
use_qt6 = false
use_cups = false
use_alsa = false
use_pulseaudio = false
use_libpci = false
use_kerberos = false
use_bluez = false
use_atk = false
use_vaapi = false
use_v4l2_codec = false
enable_printing = false
enable_remoting = false
enable_pdf = false
enable_plugins = false
enable_extensions = false
rtc_use_pipewire = false
ARGS

PATH="$DEPOT:$DEPOT/.cipd_bin:$PATH"
export PATH

# rustc finds x86_64-unknown-lean_os's description here and nowhere else, and
# it is read by gn, by ninja and by every rustc the build starts - so it is an
# environment variable rather than a flag. Anyone driving ninja by hand in
# this output directory needs it set the same way.
RUST_TARGET_PATH="$ROOT/tools/rust-port${RUST_TARGET_PATH:+:$RUST_TARGET_PATH}"
export RUST_TARGET_PATH

echo "build-chromium: gn gen $OUT"
(cd "$SRC" && buildtools/mac/gn gen "out/$OUT_NAME" --root-target="//$TARGET") || exit 1

if [ -n "${LEANOS_CHROMIUM_CONFIGURE_ONLY:-}" ]; then
  echo "build-chromium: configured only, not building"
  exit 0
fi

echo "build-chromium: building $TARGET"
(cd "$SRC" && autoninja -C "out/$OUT_NAME" "$TARGET")
