#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SRC="$ROOT/build/chromium/src"
VENDORED=third_party/chromium/VENDORED
PATCHES=tools/chromium-port
BASELINE="$SRC/out/Baseline/headless_shell"
OUT_NAME="${LEANOS_CHROMIUM_OUT:-LeanOS}"
PREFIX="$ROOT/build/toolchain/bin/x86_64-lean_os-"

PASS=0
FAIL=0

check() {
  if [ "$1" = "0" ]; then
    echo "chromium-test: pass - $2"
    PASS=$((PASS + 1))
  else
    echo "chromium-test: FAIL - $2" >&2
    FAIL=$((FAIL + 1))
  fi
}

if [ ! -d "$SRC" ]; then
  echo "chromium-test: no checkout at $SRC - run tools/fetch-chromium.sh"
  exit 0
fi

REVISION=$(awk '/^REVISION /{print $2}' "$VENDORED")
HAVE=$(cd "$SRC" && git rev-parse HEAD 2>/dev/null)
[ "$REVISION" = "$HAVE" ]
check $? "the checkout is at the revision $VENDORED pins"

APPLIED=0
DRIFTED=0
for p in "$ROOT/$PATCHES"/*.patch; do
  [ -e "$p" ] || continue
  APPLIED=$((APPLIED + 1))
  if (cd "$SRC" && git apply --check -p1 < "$p") 2>/dev/null; then
    continue
  fi
  # Already on the tree is not drift. tools/build-chromium.sh leaves the
  # series applied, so the question this check asks is "does the fork still
  # fit the pinned revision", not "is the tree pristine".
  if (cd "$SRC" && git apply --check --reverse -p1 < "$p") 2>/dev/null; then
    continue
  fi
  echo "chromium-test: $(basename "$p") neither applies to nor is applied" \
       "to $REVISION" >&2
  DRIFTED=$((DRIFTED + 1))
done
[ "$DRIFTED" = "0" ]
check $? "all $APPLIED fork patches fit the pinned revision"

if [ ! -x "$BASELINE" ]; then
  echo "chromium-test: no baseline build - skipping the render check"
else
  OUT=$("$BASELINE" --dump-dom --disable-gpu \
        "file://$ROOT/tests/chromium/dom.html" 2>/dev/null)
  echo "$OUT" | diff -q - tests/chromium/dom.expected >/dev/null 2>&1
  check $? "the baseline parses the fixture and V8 mutates its DOM"
fi

if [ ! -x "$ROOT/build/toolchain/bin/x86_64-lean_os-clang" ]; then
  echo "chromium-test: no cross toolchain - skipping the configure check"
else
  LEANOS_CHROMIUM_CONFIGURE_ONLY=1 LEANOS_CHROMIUM_CC=chromium \
      "$ROOT/tools/build-chromium.sh" base/third_party/double_conversion \
      > "$ROOT/build/chromium-configure.log" 2>&1
  check $? "the lean_os GN configuration generates"

  RUST_SYSROOT="$ROOT/build/rust-sysroot-lean_os"
  TARGET_LIBDIR="$RUST_SYSROOT/lib/rustlib/x86_64-unknown-lean_os/lib"
  if [ ! -x "$RUST_SYSROOT/bin/rustc" ]; then
    echo "chromium-test: no Rust sysroot - run tools/build-rust-sysroot.sh"
  else
    REPORTED=$("$RUST_SYSROOT/bin/rustc" --print target-libdir \
        --target x86_64-unknown-lean_os 2>/dev/null)
    [ "$REPORTED" = "$TARGET_LIBDIR" ]
    check $? "rustc resolves x86_64-unknown-lean_os by name and finds its libraries"

    ls "$TARGET_LIBDIR"/libstd-*.rlib > /dev/null 2>&1
    check $? "the standard library is in that sysroot"
  fi

  CHROMIUM_LIBDIR="$SRC/out/$OUT_NAME/local_rustc_sysroot/lib/rustlib"
  CHROMIUM_STD="$CHROMIUM_LIBDIR/x86_64-unknown-lean_os/lib/libstd_std.rlib"
  if [ ! -f "$CHROMIUM_STD" ]; then
    echo "chromium-test: Chromium has not built std - skipping the rlib check"
  else
    # This rlib came out of CHROMIUM'S build of the forked standard library
    # rather than out of cargo, and the objects in it have to be for this
    # machine - which is the whole claim the Rust half of the port makes.
    WORK=$(mktemp -d)
    MEMBER=$("${PREFIX}ar" t "$CHROMIUM_STD" | grep -F ".o" | head -1)
    ( cd "$WORK" && "${PREFIX}ar" x "$CHROMIUM_STD" "$MEMBER" &&
      "${PREFIX}readelf" -h "$MEMBER" |
        grep -q "Advanced Micro Devices X86-64" )
    check $? "Chromium's own build of std for x86_64-unknown-lean_os is x86-64 ELF"
    rm -rf "$WORK"
  fi

  # M140. PartitionAlloc is what every Chromium Rust target depends on, through
  # //build/rust/allocator, and M139 stopped here. These grade what a build
  # produced rather than re-running one: the battery is already 400 seconds and
  # a Chromium compile is minutes, so `tools/build-chromium.sh build/rust/allocator`
  # is the step a person runs and this is the step that says whether it worked.
  PA_OBJECT="$SRC/out/$OUT_NAME/obj/base/allocator/partition_allocator/src/partition_alloc/allocator_base/process_handle_posix.o"
  if [ ! -f "$PA_OBJECT" ]; then
    echo "chromium-test: PartitionAlloc has not been built - skipping" \
         "(tools/build-chromium.sh build/rust/allocator)"
  else
    "${PREFIX}readelf" -h "$PA_OBJECT" |
      grep -q "Advanced Micro Devices X86-64"
    check $? "PartitionAlloc compiles for this target and is x86-64 ELF"

    # The seam M140 added, asked of the preprocessor rather than of args.gn,
    # because it is the preprocessor that acts on it: this platform is Linux
    # as far as PartitionAlloc's feature gating goes and does NOT have Linux's
    # system calls, and those are two different claims. IS_LINUX comes from
    # build_config.h reading __lean_os__ and HAS_LINUX_KERNEL from the
    # generated buildflags, so a probe that sees both agrees with what every
    # PartitionAlloc translation unit sees.
    WORK=$(mktemp -d)
    cat > "$WORK/seam.cc" <<'PROBE'
#include "partition_alloc/build_config.h"
#include "partition_alloc/buildflags.h"
static_assert(PA_BUILDFLAG(IS_LINUX), "GN gates this platform as Linux");
static_assert(PA_BUILDFLAG(IS_POSIX), "and it is POSIX");
static_assert(!PA_BUILDFLAG(HAS_LINUX_KERNEL),
              "but it does not have Linux's system calls");
static_assert(!PA_BUILDFLAG(PA_LIBC_GLIBC), "and its C library is not glibc");
PROBE
    "${PREFIX}clang++" -std=c++20 -fsyntax-only \
      -I"$SRC/base/allocator/partition_allocator/src" \
      -I"$SRC/out/$OUT_NAME/gen/base/allocator/partition_allocator/src" \
      "$WORK/seam.cc" 2>"$WORK/seam.log"
    check $? "IS_LINUX for the gating, HAS_LINUX_KERNEL=0 for the system calls"
    [ -s "$WORK/seam.log" ] && cat "$WORK/seam.log" >&2
    rm -rf "$WORK"
  fi

  # A real rust_static_library, with cxx bindings, is the claim M139 could not
  # make. Its C++ half is compiled by x86_64-lean_os-clang and its Rust half by
  # rustc for x86_64-unknown-lean_os, so an rlib here means both agree.
  B32=$(ls "$SRC/out/$OUT_NAME"/obj/components/base32/libbase32_rust_*.rlib \
        2>/dev/null | head -1)
  if [ -z "$B32" ]; then
    echo "chromium-test: no Chromium Rust library built - skipping" \
         "(tools/build-chromium.sh components/base32:base32_rust)"
  else
    WORK=$(mktemp -d)
    MEMBER=$("${PREFIX}ar" t "$B32" | grep -F ".o" | head -1)
    ( cd "$WORK" && "${PREFIX}ar" x "$B32" "$MEMBER" &&
      "${PREFIX}readelf" -h "$MEMBER" |
        grep -q "Advanced Micro Devices X86-64" )
    check $? "a Chromium rust_static_library builds for this target"
    rm -rf "$WORK"
  fi

  # M141. //base's dependencies - perfetto, abseil and ICU - which is where
  # M140 stopped. Same rule as the PartitionAlloc checks above: these grade
  # what a build produced rather than running one, because a //base compile is
  # minutes and this battery is already long.
  PERFETTO_OBJECT="$SRC/out/$OUT_NAME/obj/third_party/perfetto/src/base/base/time.o"
  if [ ! -f "$PERFETTO_OBJECT" ]; then
    echo "chromium-test: perfetto has not been built - skipping" \
         "(tools/build-chromium.sh base)"
  else
    "${PREFIX}readelf" -h "$PERFETTO_OBJECT" |
      grep -q "Advanced Micro Devices X86-64"
    check $? "perfetto's base compiles for this target and is x86-64 ELF"

    # The same question M140 asked of PartitionAlloc, asked of perfetto, which
    # keeps its OWN copy of the platform detection: is this the Linux FAMILY
    # (yes - the build is configured that way and PosixSharedMemory is gated on
    # it) and does it have Linux's KERNEL API (no - inotify, prctl and abstract
    # socket names are not here). Perfetto already separates those two claims,
    # so this probe asks the preprocessor which side of the seam it is on.
    WORK=$(mktemp -d)
    cat > "$WORK/seam.cc" <<'PROBE'
#include "perfetto/base/build_config.h"
static_assert(PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX),
              "perfetto gates this platform's code paths as Linux");
static_assert(!PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX),
              "but it does not have Linux's kernel API");
static_assert(!PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID), "not Android");
static_assert(!PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE), "not Apple");
static_assert(!PERFETTO_BUILDFLAG(PERFETTO_OS_WIN), "not Windows");
PROBE
    "${PREFIX}clang++" -std=c++20 -fsyntax-only \
      -I"$SRC/third_party/perfetto/include" \
      -I"$SRC/out/$OUT_NAME/gen/third_party/perfetto/build_config" \
      "$WORK/seam.cc" 2>"$WORK/seam.log"
    check $? "perfetto: the Linux family yes, Linux's kernel API no"
    [ -s "$WORK/seam.log" ] && cat "$WORK/seam.log" >&2
    rm -rf "$WORK"
  fi

  # abseil's crash handler is the one file in //base's dependencies that needs
  # sigaltstack, and ICU's is the one that needed expf. Both are third-party
  # C++ nobody here wrote, compiled by this project's clang against this
  # project's libc.
  ABSL_OBJECT="$SRC/out/$OUT_NAME/obj/third_party/abseil-cpp/absl/debugging/failure_signal_handler/failure_signal_handler.o"
  if [ ! -f "$ABSL_OBJECT" ]; then
    echo "chromium-test: abseil has not been built - skipping"
  else
    "${PREFIX}readelf" -h "$ABSL_OBJECT" |
      grep -q "Advanced Micro Devices X86-64"
    check $? "abseil's crash handler compiles against this sigaltstack"
  fi

  ICU_OBJECT="$SRC/out/$OUT_NAME/obj/third_party/icu/icuuc_private/unisetspan.o"
  if [ ! -f "$ICU_OBJECT" ]; then
    echo "chromium-test: ICU has not been built - skipping"
  else
    "${PREFIX}readelf" -h "$ICU_OBJECT" |
      grep -q "Advanced Micro Devices X86-64"
    check $? "ICU compiles for this target and is x86-64 ELF"
  fi

  # <fstream> is what enabling libc++'s filesystem bought, and abseil's time
  # zone reader is what asked for it. A libc++ without it compiles every other
  # file in this tree and fails exactly this one.
  CCTZ_OBJECT="$SRC/out/$OUT_NAME/obj/third_party/abseil-cpp/absl/time/internal/cctz/time_zone/time_zone_info.o"
  if [ ! -f "$CCTZ_OBJECT" ]; then
    echo "chromium-test: abseil's time zone reader has not been built - skipping"
  else
    "${PREFIX}readelf" -h "$CCTZ_OBJECT" |
      grep -q "Advanced Micro Devices X86-64"
    check $? "std::ifstream reaches this filesystem - libc++ has <fstream>"
  fi
fi

echo "chromium-test: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ]
