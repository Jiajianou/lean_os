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
else
  CC="${PREFIX}clang"
  CXX="${PREFIX}clang++"
  AR="${PREFIX}ar"
  NM="${PREFIX}nm"
  READELF="${PREFIX}readelf"
  CLANG_BASE="$ROOT/build/toolchain"
  CLANG_VER="19"
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
enable_rust = false
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

use_sysroot = true
sysroot = "$SYSROOT"
lean_os_sysroot = "$SYSROOT"
lean_os_cxx_include = "$ROOT/build/toolchain/x86_64-lean_os/include/c++/v1"
use_custom_libcxx = false
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

echo "build-chromium: gn gen $OUT"
(cd "$SRC" && buildtools/mac/gn gen "out/$OUT_NAME" --root-target="//$TARGET") || exit 1

if [ -n "${LEANOS_CHROMIUM_CONFIGURE_ONLY:-}" ]; then
  echo "build-chromium: configured only, not building"
  exit 0
fi

echo "build-chromium: building $TARGET"
(cd "$SRC" && autoninja -C "out/$OUT_NAME" "$TARGET")
