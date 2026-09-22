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

# And that it is a LINUX checkout, which is what target_os = "linux" in
# args.gn means. DEPS gates a dozen sub-repositories on checkout_linux, and
# `fetch chromium` on a Mac leaves every one of them an empty directory with
# a BUILD.gn in it - which //base, //mojo, //url and //net never notice and
# //services/network stops dead on. M153 measured that; the fix is in
# tools/fetch-chromium.sh and this is the check that says it took.
MISSING=""
for d in third_party/fontconfig/src third_party/wayland/src; do
  [ -d "$SRC/$d" ] && [ -n "$(ls -A "$SRC/$d" 2>/dev/null)" ] || MISSING="$MISSING $d"
done
[ -z "$MISSING" ]
check $? "and it has the Linux half of its sub-repositories${MISSING:+ - missing:$MISSING}"

# Does the fork still fit the pinned revision? The way to ask is to reset the
# files the series touches and apply it in order - which is exactly what
# tools/build-chromium.sh does, and for the reason M145 found: two patches can
# touch one file, and then "is this one already applied" has no answer,
# because reversing the earlier one against a file the later one moved past
# fails. This leaves the checkout in the state a build wants it in.
APPLIED=0
DRIFTED=0
PATCH_FILES=$(ls "$ROOT/$PATCHES"/*.patch 2>/dev/null)
for f in $(sed -n 's|^--- a/||p' $PATCH_FILES | sort -u); do
  if (cd "$SRC" && git ls-files --error-unmatch "$f" > /dev/null 2>&1); then
    (cd "$SRC" && git checkout -- "$f")
    continue
  fi
  # A path the top-level checkout does not track belongs to one of the
  # sub-repositories it is assembled from, each with its own .git. M161:
  # leaving those alone made this check report drift that was not there -
  # a patch that had been applied and never reset does not apply again, and
  # the harness called that "does not apply to the pinned revision". The
  # same loop in tools/build-chromium.sh had the same bug.
  d=$(dirname "$SRC/$f")
  top=$(git -C "$d" rev-parse --show-toplevel 2>/dev/null) || continue
  [ "$top" = "$SRC" ] && continue
  rel=${f#${top#$SRC/}/}
  (cd "$top" && git checkout -- "$rel" 2>/dev/null) || true
done
for p in $PATCH_FILES; do
  APPLIED=$((APPLIED + 1))
  if (cd "$SRC" && git apply -p1 < "$p") 2>/dev/null; then
    continue
  fi
  if (cd "$SRC" && git apply --check --reverse -p1 < "$p") 2>/dev/null; then
    continue
  fi
  echo "chromium-test: $(basename "$p") does not apply to $REVISION" >&2
  DRIFTED=$((DRIFTED + 1))
done
[ "$DRIFTED" = "0" ]
check $? "all $APPLIED fork patches apply to the pinned revision, in order"

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
  # Into its OWN output directory. gn gen prunes the graph to the root target
  # it is given, so generating a narrow one over out/LeanOS would delete every
  # other target from it - which is what happened in M145, an hour after the
  # build that had just produced them.
  LEANOS_CHROMIUM_CONFIGURE_ONLY=1 LEANOS_CHROMIUM_CC=chromium \
      LEANOS_CHROMIUM_OUT=ConfigureCheck \
      "$ROOT/tools/build-chromium.sh" base/third_party/double_conversion \
      > "$ROOT/build/chromium-configure.log" 2>&1
  check $? "the lean_os GN configuration generates"

  # M154. The configure check above runs gn; this asks what gn WROTE. The
  # triple and the sysroot have to be in {{cflags}} and not only in the tool
  # command string, because bindgen builds its libclang command line out of
  # {{cflags}} and would otherwise compute this target's type layouts for
  # x86_64-unknown-linux-gnu. tools/chromium-bindgen-test.py grades the
  # answers; this grades the configuration that produces them, in the output
  # directory a person actually builds in.
  BASE_NINJA="$SRC/out/$OUT_NAME/obj/base/base.ninja"
  if [ ! -f "$BASE_NINJA" ]; then
    echo "chromium-test: out/$OUT_NAME has not been generated - skipping" \
         "the cflags check"
  else
    CFLAGS_LINE=$(grep -m1 "^cflags = " "$BASE_NINJA")
    case "$CFLAGS_LINE" in
      *--target=x86_64-lean_os*--sysroot=*|*--sysroot=*--target=x86_64-lean_os*)
        LEANOS_IN_CFLAGS=0 ;;
      *) LEANOS_IN_CFLAGS=1 ;;
    esac
    [ "$LEANOS_IN_CFLAGS" = "0" ]
    check $? "the triple and the sysroot are in {{cflags}}, where bindgen reads"

    # And after the one //build/config/compiler derives from current_os, which
    # is the whole reason the config is last in default_compiler_configs. A
    # clang takes the last --target on the line; so does libclang.
    LEANOS_AT=$(printf '%s' "$CFLAGS_LINE" | grep -bo -- "--target=x86_64-lean_os" | head -1 | cut -d: -f1)
    LINUX_AT=$(printf '%s' "$CFLAGS_LINE" | grep -bo -- "--target=x86_64-unknown-linux-gnu" | head -1 | cut -d: -f1)
    [ -n "$LEANOS_AT" ] && [ -n "$LINUX_AT" ] && [ "$LEANOS_AT" -gt "$LINUX_AT" ]
    check $? "and this target's triple comes after the one current_os implies"
  fi

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

  # M144. //base itself. libbase.a is a thin archive, so what it holds is a
  # list of object paths; the claim is that every one of them exists and is
  # for this machine, which is the only way "it builds" means anything.
  LIBBASE="$SRC/out/$OUT_NAME/obj/base/libbase.a"
  if [ ! -f "$LIBBASE" ]; then
    echo "chromium-test: //base has not been built - skipping" \
         "(tools/build-chromium.sh base)"
  else
    COUNT=$("${PREFIX}ar" t "$LIBBASE" | wc -l | tr -d ' ')
    [ "$COUNT" -gt 300 ]
    check $? "//base built $COUNT objects"

    # A thin archive names its members by path, so listing it proves nothing
    # on its own - the objects have to be there and have to be for this
    # machine. One readelf over all of them answers both at once: a member
    # that is missing produces no header, and one built for something else
    # produces the wrong Machine line.
    MEMBERS=$("${PREFIX}ar" t "$LIBBASE")
    HEADERS=$(echo "$MEMBERS" | xargs "${PREFIX}readelf" -h 2>/dev/null |
              grep -c "Advanced Micro Devices X86-64")
    [ "$HEADERS" = "$COUNT" ]
    check $? "all $COUNT of them exist and are x86-64 ELF ($HEADERS answered)"

    # The seam M144 added, at the //build level this time, asked of the
    # preprocessor for the reason M140's probe gives: it is the preprocessor
    # that acts on it. IS_LINUX gates the build; HAS_LINUX_KERNEL answers
    # whether getdents64, prctl, inotify and the futex system call are there.
    WORK=$(mktemp -d)
    cat > "$WORK/seam.cc" <<'PROBE'
#include "build/build_config.h"
static_assert(BUILDFLAG(IS_LINUX), "GN gates this platform as Linux");
static_assert(BUILDFLAG(IS_POSIX), "and it is POSIX");
static_assert(!BUILDFLAG(HAS_LINUX_KERNEL),
              "but it does not have Linux's kernel interfaces");
PROBE
    "${PREFIX}clang++" -std=c++20 -fsyntax-only -I"$SRC" \
      "$WORK/seam.cc" 2>"$WORK/seam.log"
    check $? "IS_LINUX for the gating, HAS_LINUX_KERNEL=0 for the kernel"
    [ -s "$WORK/seam.log" ] && cat "$WORK/seam.log" >&2
    rm -rf "$WORK"
  fi

  # M145. //base LINKS, into a program this machine can run - which is a
  # different claim from compiling, and the one that found -no-pie being
  # ignored, two definitions of calloc and an interposing close(2).
  BASEPROGRAM="$SRC/out/$OUT_NAME/basetest"
  if [ ! -x "$BASEPROGRAM" ]; then
    echo "chromium-test: //base has not been linked - skipping" \
         "(tools/build-chromium.sh lean_os)"
  else
    SHAPE=$("${PREFIX}readelf" -h "$BASEPROGRAM" |
            awk '/^  Type:/{t=$2} /Entry point/{e=$4} END{print t, e}')
    case "$SHAPE" in
      "EXEC 0x80"*) true;;
      *) false;;
    esac
    check $? "a program linked from //base is an EXEC in this OS's image region ($SHAPE)"

    # No interpreter. A position-independent executable would name one, and
    # the path it would name is not on this machine.
    [ "$("${PREFIX}readelf" -l "$BASEPROGRAM" | grep -c INTERP)" = "0" ]
    check $? "and names no interpreter - nothing has to relocate it"

    # Exactly one definition of each allocator name. Two is what a static
    # link produced while this libc kept calloc and realloc in a different
    # translation unit from malloc: PartitionAlloc's shim brings its own set
    # and the linker found both.
    DOUBLED=""
    for symbol in malloc calloc realloc free; do
      COUNT=$("${PREFIX}nm" "$BASEPROGRAM" | grep -cE " T $symbol\$")
      [ "$COUNT" = "1" ] || DOUBLED="$DOUBLED $symbol=$COUNT"
    done
    [ -z "$DOUBLED" ]
    check $? "with exactly one definition of malloc, calloc, realloc and free${DOUBLED:+ -$DOUBLED}"

    # Onto the image, so the [m145] boot self-test can run it. Compiling and
    # linking are what the checks above grade; whether //base WORKS on this
    # machine is a question only the machine answers.
    IMAGE="$ROOT/build/os-image.bin"
    if [ ! -f "$IMAGE" ]; then
      echo "chromium-test: no $IMAGE - run make, then this again"
    else
      make -s -C "$ROOT" leanfs-put > /dev/null 2>&1
      "$ROOT/build/leanfs-put" "$IMAGE" "$BASEPROGRAM" /bin/chromiumbase > /dev/null
      check $? "installed as /bin/chromiumbase - the [m145] boot self-test runs it"
    fi
  fi

  # M148. //mojo, which is the layer every multi-process piece of Chromium is
  # made of. The same rule as everything above: these grade what a build
  # produced, and the machine answers whether it works.
  MOJOPROGRAM="$SRC/out/$OUT_NAME/mojotest"
  if [ ! -x "$MOJOPROGRAM" ]; then
    echo "chromium-test: //mojo has not been linked - skipping" \
         "(tools/build-chromium.sh lean_os)"
  else
    SHAPE=$("${PREFIX}readelf" -h "$MOJOPROGRAM" |
            awk '/^  Type:/{t=$2} /Entry point/{e=$4} END{print t, e}')
    case "$SHAPE" in
      "EXEC 0x80"*) true;;
      *) false;;
    esac
    check $? "a program linked from //mojo is an EXEC in this OS's image region ($SHAPE)"

    # ChannelPosix, not ChannelLinux. The shared-memory upgrade is
    # memfd_create through syscall(2), eventfd and futex(2) - the Linux
    # kernel's own interfaces - and a build that compiled it for this machine
    # would link and then fail at run time on a kernel version check. The
    # symbol table is where that decision is visible from outside.
    HAVE_LINUX_CHANNEL=$("${PREFIX}nm" -C "$MOJOPROGRAM" |
                         grep -c "mojo::core::ChannelLinux")
    [ "$HAVE_LINUX_CHANNEL" = "0" ]
    check $? "and carries ChannelPosix rather than ChannelLinux ($HAVE_LINUX_CHANNEL Linux-channel symbols)"

    HAVE_POSIX_CHANNEL=$("${PREFIX}nm" -C "$MOJOPROGRAM" |
                         grep -c "mojo::core::ChannelPosix")
    [ "$HAVE_POSIX_CHANNEL" -gt 0 ]
    check $? "with ChannelPosix in it ($HAVE_POSIX_CHANNEL symbols)"

    # And Chromium's own epoll message pump, which is what an IO thread here
    # runs on. M119 built the epoll it calls.
    HAVE_EPOLL_PUMP=$("${PREFIX}nm" -C "$MOJOPROGRAM" |
                      grep -c "base::MessagePumpEpoll")
    [ "$HAVE_EPOLL_PUMP" -gt 0 ]
    check $? "on base::MessagePumpEpoll ($HAVE_EPOLL_PUMP symbols)"

    # M149. A mojom interface, which is how Chromium describes every one of
    # its own: the .mojom file goes through Chromium's own generator on the
    # host and the C++ it produces is compiled for this machine. Both halves
    # have to be true, and only the second is about this port.
    MOJOM_OBJECT="$SRC/out/$OUT_NAME/obj/lean_os/mojotest/interface/lean_os_echo.mojom.o"
    if [ ! -f "$MOJOM_OBJECT" ]; then
      echo "chromium-test: the mojom interface has not been generated - skipping"
    else
      "${PREFIX}readelf" -h "$MOJOM_OBJECT" |
        grep -q "Advanced Micro Devices X86-64"
      check $? "C++ generated from lean_os_echo.mojom is x86-64 ELF"

      GENERATED="$SRC/out/$OUT_NAME/gen/lean_os/mojotest/lean_os_echo.mojom.h"
      grep -q "class .*Echo" "$GENERATED" 2>/dev/null
      check $? "and the generator wrote the interface this program includes"

      # In the linked program, not merely compiled beside it. A generated
      # binding that nothing references links to nothing at all.
      BOUND=$("${PREFIX}nm" -C "$MOJOPROGRAM" | grep -c "lean_os::mojom::Echo")
      [ "$BOUND" -gt 0 ]
      check $? "and the program carries the generated bindings ($BOUND symbols)"
    fi

    IMAGE="$ROOT/build/os-image.bin"
    if [ ! -f "$IMAGE" ]; then
      echo "chromium-test: no $IMAGE - run make, then this again"
    else
      make -s -C "$ROOT" leanfs-put > /dev/null 2>&1
      "$ROOT/build/leanfs-put" "$IMAGE" "$MOJOPROGRAM" /bin/chromiummojo > /dev/null
      check $? "installed as /bin/chromiummojo - the [m148] and [m149] boot self-tests run it"
    fi
  fi

  # M150. //url and //net.
  NETPROGRAM="$SRC/out/$OUT_NAME/nettest"
  if [ ! -x "$NETPROGRAM" ]; then
    echo "chromium-test: //net has not been linked - skipping" \
         "(tools/build-chromium.sh lean_os)"
  else
    NETCOUNT=$("${PREFIX}ar" t "$SRC/out/$OUT_NAME/obj/net/libnet.a" | wc -l | tr -d ' ')
    [ "$NETCOUNT" -gt 500 ]
    check $? "//net built $NETCOUNT objects into libnet.a"

    URLCOUNT=$("${PREFIX}ar" t "$SRC/out/$OUT_NAME/obj/url/liburl.a" | wc -l | tr -d ' ')
    [ "$URLCOUNT" -gt 20 ]
    check $? "and //url built $URLCOUNT"

    SHAPE=$("${PREFIX}readelf" -h "$NETPROGRAM" |
            awk '/^  Type:/{t=$2} /Entry point/{e=$4} END{print t, e}')
    case "$SHAPE" in
      "EXEC 0x80"*) true;;
      *) false;;
    esac
    check $? "a program linked from //net is an EXEC in this OS's image region ($SHAPE)"

    # getifaddrs rather than netlink, and res_ninit rather than nothing.
    # Both are symbol-table questions because the wrong choice links: the
    # netlink path would compile against headers this machine does not have,
    # and it is only absent because //net's own condition was moved onto
    # HAS_LINUX_KERNEL.
    NETOBJ="$SRC/out/$OUT_NAME/obj/net/net"
    [ -f "$NETOBJ/network_interfaces_getifaddrs.o" ]
    check $? "with net's getifaddrs interface list compiled"

    [ ! -f "$NETOBJ/address_tracker_linux.o" ] &&
      [ ! -f "$NETOBJ/network_interfaces_linux.o" ]
    check $? "and neither the netlink address tracker nor its interface list"

    RESOLVED=$("${PREFIX}nm" "$NETPROGRAM" | grep -cE " [UT] getifaddrs\$")
    [ "$RESOLVED" -gt 0 ]
    check $? "and getifaddrs itself, which is this libc's ($RESOLVED)"

    RESOLV=$("${PREFIX}nm" -C "$NETPROGRAM" | grep -cE " T res_ninit\$")
    [ "$RESOLV" = "1" ]
    check $? "and exactly one res_ninit, this libc's ($RESOLV)"

    # M152. BoringSSL, which is where a browser's https lives. Its assembly
    # is generated per-architecture, so an object from the _asm target being
    # x86-64 ELF is the claim that matters - a C-only fallback would compile
    # for anything.
    BSSL=$(find "$SRC/out/$OUT_NAME/obj/third_party/boringssl" -name "*.o" \
           2>/dev/null | wc -l | tr -d ' ')
    [ "$BSSL" -gt 300 ]
    check $? "BoringSSL built $BSSL objects for this target"

    BSSL_ASM=$(find "$SRC/out/$OUT_NAME/obj/third_party/boringssl/boringssl_asm" \
               -name "*.o" 2>/dev/null | head -1)
    if [ -z "$BSSL_ASM" ]; then
      echo "chromium-test: no BoringSSL assembly objects - skipping"
    else
      "${PREFIX}readelf" -h "$BSSL_ASM" |
        grep -q "Advanced Micro Devices X86-64"
      check $? "including its x86-64 assembly ($(basename "$BSSL_ASM"))"
    fi

    # Both ends. SSL_accept is the server half this program runs itself and
    # SSL_do_handshake is what Chromium's SSLClientSocket drives - SSL_connect
    # is not here because nothing calls it and the link garbage-collects what
    # nothing calls, which is itself worth knowing.
    TLS=$("${PREFIX}nm" "$NETPROGRAM" |
          grep -cE " [Tt] (SSL_accept|SSL_do_handshake)\$")
    [ "$TLS" = "2" ]
    check $? "and the program carries both ends of BoringSSL's handshake ($TLS)"

    IMAGE="$ROOT/build/os-image.bin"
    if [ ! -f "$IMAGE" ]; then
      echo "chromium-test: no $IMAGE - run make, then this again"
    else
      make -s -C "$ROOT" leanfs-put > /dev/null 2>&1
      "$ROOT/build/leanfs-put" "$IMAGE" "$NETPROGRAM" /bin/chromiumnet > /dev/null
      check $? "installed as /bin/chromiumnet - the [m150] to [m152] boot self-tests run it"
    fi
  fi

  # M155. V8. The first piece of Chromium here that writes machine code at run
  # time rather than at build time, and the one Blink cannot be reached
  # without.
  V8PROGRAM="$SRC/out/$OUT_NAME/v8test"
  if [ ! -x "$V8PROGRAM" ]; then
    echo "chromium-test: V8 has not been built - skipping" \
         "(tools/build-chromium.sh lean_os)"
  else
    "${PREFIX}readelf" -h "$V8PROGRAM" | grep -q "EXEC (Executable file)"
    check $? "a program linked from V8 is an EXEC in this OS's image region ($("${PREFIX}readelf" -h "$V8PROGRAM" | awk '/Type:/{t=$2} /Entry point/{print t, $NF}'))"

    # The snapshot is IN the binary rather than in a file beside it. V8 names
    # its embedded blob with this symbol and loads from a file without it, so
    # this is the difference between a program this machine runs and a program
    # that looks for snapshot_blob.bin and exits.
    EMBEDDED=$("${PREFIX}nm" "$V8PROGRAM" |
               grep -cE " [TtDdRr] v8_Default_embedded_blob_(code_|data_)?" )
    [ "$EMBEDDED" -ge 2 ]
    check $? "with V8's startup snapshot linked into it ($EMBEDDED symbols)"

    # TurboFan and the interpreter, asked of the binary rather than of the
    # build: a V8 without its optimising compiler links and runs and is a
    # different engine.
    JIT=$("${PREFIX}nm" "$V8PROGRAM" |
          grep -cE " [Tt] .*(TurboFan|Ignition|RegExpMacroAssembler)" )
    [ "$JIT" -gt 20 ]
    check $? "and TurboFan, Ignition and the regular expression assembler in it ($JIT symbols)"

    IMAGE="$ROOT/build/os-image.bin"
    if [ ! -f "$IMAGE" ]; then
      echo "chromium-test: no $IMAGE - run make, then this again"
    else
      make -s -C "$ROOT" leanfs-put > /dev/null 2>&1
      "$ROOT/build/leanfs-put" "$IMAGE" "$V8PROGRAM" /bin/chromiumv8 > /dev/null
      check $? "installed as /bin/chromiumv8 - the [m156] boot self-test runs it"
    fi
  fi

  # M157. Skia. What a browser paints with, and the other half of what Blink
  # needs - V8 runs the scripts and this draws the result.
  SKIAPROGRAM="$SRC/out/$OUT_NAME/skiatest"
  if [ ! -x "$SKIAPROGRAM" ]; then
    echo "chromium-test: Skia has not been built - skipping" \
         "(tools/build-chromium.sh lean_os)"
  else
    "${PREFIX}readelf" -h "$SKIAPROGRAM" | grep -q "EXEC (Executable file)"
    check $? "a program linked from Skia is an EXEC in this OS's image region ($("${PREFIX}readelf" -h "$SKIAPROGRAM" | awk '/Type:/{t=$2} /Entry point/{print t, $NF}'))"

    # The software rasteriser, asked of the binary rather than of the build.
    # skia_use_dawn is false here because this machine has no GPU at all, and
    # a Skia that had been built without its CPU backend would link and draw
    # nothing.
    RASTER=$("${PREFIX}nm" "$SKIAPROGRAM" |
             grep -cE " [Tt] .*(SkRasterPipeline|SkScan|SkBlitter)" )
    [ "$RASTER" -gt 50 ]
    check $? "with the software rasteriser in it ($RASTER symbols)"

    # And no GPU backend, which is the claim skia_use_dawn = false makes. A
    # Dawn that got linked in anyway would be a WebGPU implementation with no
    # device to talk to, and this is what says it is not there.
    DAWN=$("${PREFIX}nm" "$SKIAPROGRAM" | grep -cE " [Tt] .*(dawn::|wgpu)" )
    [ "$DAWN" = "0" ]
    check $? "and no Dawn in it, because this machine has no GPU to give it"

    # The PNG codecs are Rust here - SK_CODEC_ENCODES_PNG_WITH_RUST - so the
    # cxx bridge between Skia's C++ and the png crate has to be in the binary
    # for check 8 of the program to mean anything.
    RUSTPNG=$("${PREFIX}nm" "$SKIAPROGRAM" |
              grep -cE "rust_png|SkPngRust" )
    [ "$RUSTPNG" -gt 10 ]
    check $? "and Chromium's Rust PNG codec, over the cxx bridge ($RUSTPNG symbols)"

    IMAGE="$ROOT/build/os-image.bin"
    if [ ! -f "$IMAGE" ]; then
      echo "chromium-test: no $IMAGE - run make, then this again"
    else
      make -s -C "$ROOT" leanfs-put > /dev/null 2>&1
      "$ROOT/build/leanfs-put" "$IMAGE" "$SKIAPROGRAM" /bin/chromiumskia > /dev/null
      check $? "installed as /bin/chromiumskia - the [m157] boot self-test runs it"
    fi
  fi

  # M158. //cc/paint, and the decision the whole milestone is: there is no GPU
  # on this machine, so what is built is the software raster path Chromium
  # maintains for every platform - and the GPU stack is ABSENT from the build
  # rather than present and unused.
  CCPROGRAM="$SRC/out/$OUT_NAME/ccpainttest"
  if [ ! -x "$CCPROGRAM" ]; then
    echo "chromium-test: //cc/paint has not been built - skipping" \
         "(tools/build-chromium.sh lean_os)"
  else
    "${PREFIX}readelf" -h "$CCPROGRAM" | grep -q "EXEC (Executable file)"
    check $? "a program linked from //cc/paint is an EXEC in this OS's image region ($("${PREFIX}readelf" -h "$CCPROGRAM" | awk '/Type:/{t=$2} /Entry point/{print t, $NF}'))"

    # The layer itself: the recorded paint, the buffer it lives in, the rtree
    # that culls it and the solid-colour analysis. Asked of the binary rather
    # than of the build, for M157's reason - a //cc/paint that linked and
    # replayed nothing would satisfy every check that only asks whether the
    # build succeeded.
    # -C, because these are C++ names: nm without it reports
    # _ZN2cc15DisplayItemList..., in which the string "cc::" never appears.
    # M157's Skia checks got away without it by naming identifiers that
    # survive mangling.
    CCRASTER=$("${PREFIX}nm" -C "$CCPROGRAM" |
               grep -cE " [Tt] .*cc::(DisplayItemList|PaintOpBuffer|SolidColorAnalyzer)")
    [ "$CCRASTER" -gt 10 ]
    check $? "with cc's recorded paint and its rtree in it ($CCRASTER symbols)"

    # And none of the GPU stack, which is what enable_vulkan, enable_swiftshader
    # and use_dawn = false claim. Each of these would be a rendering back end
    # with no device to talk to: SwiftShader would additionally be a SECOND
    # run-time code generator to port after V8, since Reactor's backends are
    # vendored copies of LLVM.
    check_absent() {
      FOUND=$("${PREFIX}nm" -C "$CCPROGRAM" | grep -cE "$2")
      [ "$FOUND" = "0" ]
      check $? "and no $1 in it, because this machine has no GPU to give it"
    }
    check_absent Dawn "dawn::|wgpu::"
    check_absent SwiftShader "SwiftShader|sw::Reactor"
    check_absent ANGLE "rx::|EGL_|eglInitialize"
    check_absent Vulkan "vkCreate|VkPhysicalDevice|vkGetInstance"

    # The same claim read from the configured build's own args.gn rather than
    # copied here, which is M154's rule for this kind of check: a value this
    # file asserted for itself would grade nothing.
    ARGSFILE="$SRC/out/$OUT_NAME/args.gn"
    OFF=0
    for flag in enable_vulkan enable_swiftshader enable_swiftshader_vulkan \
                skia_use_dawn; do
      grep -qE "^$flag = false\$" "$ARGSFILE" || OFF=1
    done
    [ "$OFF" = "0" ]
    check $? "and the build's own args.gn turns off four of the five GPU back ends"

    # use_dawn is the FIFTH and it is true since M165, which is a reversal of
    # M158's position on this one flag and is recorded as one rather than
    # quietly dropped from the list. Blink has no configuration without WebGPU
    # - modules/webgpu is unconditional in its module list - and the first
    # link of a program against //content failed on nothing else. What keeps
    # the GPU stack out is now Dawn's own six flags instead, which this file
    # checks where it checks the binary they produce.
    #
    # The claim this check used to make is still true and is made in a
    # stronger place: no binary in this port contains Dawn except
    # /bin/chromiumcontent, and that one contains only its null backend.
    grep -qE "^use_dawn = true\$" "$ARGSFILE"
    check $? "and use_dawn is TRUE, which M165 reversed and Blink requires"

    # The two headers M158 added to this libc, checked in the SYSROOT rather
    # than in the tree. `make sysroot` begins with rm -rf and so is not run
    # here; `make sysroot-headers` is what copies a new header across, and a
    # header added to user_space/libc/include and never copied is invisible
    # to every cross compile while looking perfectly present in git. That is
    # the failure this grades. <sys/poll.h> is what WebRTC's socket server
    # asks for and M_SQRT2 is what pffft asks for; both are in //cc's graph
    # rather than //cc/paint's, so this is the only place they are graded on
    # the Chromium side.
    SYSPOLL="$ROOT/build/sysroot/usr/local/include/sys/poll.h"
    [ -f "$SYSPOLL" ] && grep -q "poll.h" "$SYSPOLL"
    check $? "this libc's <sys/poll.h> is in the sysroot a cross compile reads"
    grep -q "M_SQRT2" "$ROOT/build/sysroot/usr/local/include/math.h"
    check $? "and the XSI constants are in its <math.h>"

    IMAGE="$ROOT/build/os-image.bin"
    if [ ! -f "$IMAGE" ]; then
      echo "chromium-test: no $IMAGE - run make, then this again"
    else
      make -s -C "$ROOT" leanfs-put > /dev/null 2>&1
      "$ROOT/build/leanfs-put" "$IMAGE" "$CCPROGRAM" /bin/chromiumcc > /dev/null
      check $? "installed as /bin/chromiumcc - the [m158] boot self-test runs it"
    fi
  fi

  # M159. //gpu/config, which is the gate to everything above //net: //cc
  # reaches it through //components/viz/common, so does //media, so does
  # //services/network's mojom, and so does Blink's own string library
  # through //third_party/blink/public/common:headers. Nothing in this port
  # gets past //net without it.
  GPUPROGRAM="$SRC/out/$OUT_NAME/gpuinfotest"
  if [ ! -x "$GPUPROGRAM" ]; then
    echo "chromium-test: //gpu/config has not been built - skipping" \
         "(tools/build-chromium.sh lean_os)"
  else
    "${PREFIX}readelf" -h "$GPUPROGRAM" | grep -q "EXEC (Executable file)"
    check $? "a program linked from //gpu/config is an EXEC in this OS's image region ($("${PREFIX}readelf" -h "$GPUPROGRAM" | awk '/Type:/{t=$2} /Entry point/{print t, $NF}'))"

    # The decision procedure itself, which is what this program runs: the
    # blocklist built from software_rendering_list.json, the driver bug list
    # built from gpu_driver_bug_list.json, and the collector. A //gpu/config
    # that linked with an empty rule list would satisfy every check that only
    # asks whether the call returned - M157's sentence, a fourth time.
    GPUCONF=$("${PREFIX}nm" -C "$GPUPROGRAM" |
              grep -cE " [Tt] .*gpu::(GpuBlocklist|GpuDriverBugList|GpuControlList|ComputeGpuFeatureInfo|CollectBasicGraphicsInfo)")
    [ "$GPUCONF" -gt 5 ]
    check $? "with Chromium's blocklist, driver bug list and collector in it ($GPUCONF symbols)"

    # Where the GPU information comes from, which is the question M158 left
    # open. ANGLE's own system-information reader, unmodified: with no
    # libpci, no X11 and no Vulkan compiled in, angle::GetSystemInfo finds no
    # devices and returns false.
    ANGLEINFO=$("${PREFIX}nm" -C "$GPUPROGRAM" | grep -cE "angle::GetSystemInfo|angle::GetDualGPUInfo")
    [ "$ANGLEINFO" -gt 0 ]
    check $? "and ANGLE's system-information reader as the answer to what GPU this machine has ($ANGLEINFO symbols)"

    # And still none of the rendering stack. These are the same four checks
    # //cc/paint gets, asked of a program that links //gpu/config - which is
    # where the GPU stack was arriving from in M158's measurement. ANGLE is
    # the exception and has to be: gpu_info_util IS ANGLE. What must be
    # absent is ANGLE's GL implementation, which is libGLESv2 and libANGLE,
    # not its system-information reader.
    # These patterns are NAMESPACES rather than words, and that is the whole
    # point of them. `nm -C | grep SwiftShader` finds three symbols in this
    # binary and none of them is SwiftShader: two are
    # gl::kANGLEImplementationSwiftShaderName - a string in //ui/gl's table of
    # implementation names - and the third is features::IsSwiftShaderAllowed,
    # which is the predicate that answers no. A check written against the word
    # would have failed on a build that is right.
    check_gpu_absent() {
      FOUND=$("${PREFIX}nm" -C "$GPUPROGRAM" | grep -cE "$2")
      [ "$FOUND" = "0" ]
      check $? "and no $1 in a program that links //gpu/config"
    }
    check_gpu_absent SwiftShader "sw::Reactor|rr::Nucleus|marl::|Ice::Cfg"
    check_gpu_absent Vulkan "vkCreateInstance|VkPhysicalDevice|vkGetInstanceProcAddr"
    check_gpu_absent Dawn "dawn::native|wgpu::"

    # And ANGLE's GL IMPLEMENTATION, which is the distinction this milestone
    # turns on. angle_gpu_info_util is ANGLE and it is here, deliberately -
    # it is where the answer to "what GPU does this machine have" comes from.
    # libANGLE, the GLSL translator and the EGL display are also BUILT,
    # because //ui/gl/init names them when use_static_angle is true. They are
    # in no binary: nothing here calls gl::init::InitializeGLOneOff, so the
    # linker takes nothing out of those archives. That is a stronger claim
    # than the flags make and it is the one worth grading, because it is
    # about the program rather than about the build.
    check_gpu_absent "ANGLE's GL implementation" \
                     "egl::Display|sh::TCompiler|gl::Context::|rx::DisplayImpl"
    NOINIT=$("${PREFIX}nm" -C "$GPUPROGRAM" | grep -cE "gl::init::")
    [ "$NOINIT" = "0" ]
    check $? "and nothing that would initialise a GL context (gl::init:: is absent)"

    # The three ANGLE flags M159 found and the one optional system library,
    # read out of the configured build's own args.gn rather than asserted
    # here - M154's rule. M158 turned off Chromium's four and SwiftShader
    # stayed in the graph, because ANGLE has its OWN switches for the same
    # question and angle_build_vulkan_system_info's default is on in every
    # Chromium build.
    ARGSFILE="$SRC/out/$OUT_NAME/args.gn"
    AOFF=0
    for flag in angle_build_vulkan_system_info angle_enable_vulkan \
                angle_shared_libvulkan use_xkbcommon; do
      grep -qE "^$flag = false\$" "$ARGSFILE" || AOFF=1
    done
    [ "$AOFF" = "0" ]
    check $? "and args.gn turns off ANGLE's own three Vulkan switches and xkbcommon"

    # pthread_setname_np, in the sysroot rather than in the tree, for the
    # reason M158 wrote down about <sys/poll.h>: a header added to
    # user_space/libc/include and never copied across is invisible to every
    # cross compile while looking perfectly present in git. ANGLE's
    # system_utils_linux.cpp is the caller.
    grep -q "pthread_setname_np" "$ROOT/build/sysroot/usr/local/include/pthread.h"
    check $? "this libc's pthread_setname_np is in the sysroot a cross compile reads"

    IMAGE="$ROOT/build/os-image.bin"
    if [ ! -f "$IMAGE" ]; then
      echo "chromium-test: no $IMAGE - run make, then this again"
    else
      make -s -C "$ROOT" leanfs-put > /dev/null 2>&1
      "$ROOT/build/leanfs-put" "$IMAGE" "$GPUPROGRAM" /bin/chromiumgpu > /dev/null
      check $? "installed as /bin/chromiumgpu - the [m159] boot self-test runs it"
    fi
  fi

  # M160. //cc itself - the compositor, not just its recorded paint. The
  # measurement that made this rung reachable is M159's: a -k 0 build of //cc
  # failed in 546 places for M158 and in 68 for M160, of which 47 were one
  # missing declaration in this libc.
  CC2PROGRAM="$SRC/out/$OUT_NAME/cctest2"
  if [ ! -x "$CC2PROGRAM" ]; then
    echo "chromium-test: //cc has not been built - skipping" \
         "(tools/build-chromium.sh lean_os)"
  else
    "${PREFIX}readelf" -h "$CC2PROGRAM" | grep -q "EXEC (Executable file)"
    check $? "a program linked from //cc is an EXEC in this OS's image region ($("${PREFIX}readelf" -h "$CC2PROGRAM" | awk '/Type:/{t=$2} /Entry point/{print t, $NF}'))"

    # The layer half, which is what separates this from M158's //cc/paint:
    # a RecordingSource holds the invalidation, a RasterSource is the
    # snapshot, and TilingData is the arithmetic under both.
    CCLAYER=$("${PREFIX}nm" -C "$CC2PROGRAM" |
              grep -cE " [TtWw] .*cc::(RecordingSource|RasterSource|TilingData)")
    [ "$CCLAYER" -gt 20 ]
    check $? "with cc's recording source, raster source and tiling data in it ($CCLAYER symbols)"

    # And none of the four stacks M158, M159 and M160 took out of the graph.
    # The patterns are namespaces rather than words for M159's reason.
    check_cc_absent() {
      FOUND=$("${PREFIX}nm" -C "$CC2PROGRAM" | grep -cE "$2")
      [ "$FOUND" = "0" ]
      check $? "and no $1 in a program that links //cc"
    }
    check_cc_absent SwiftShader "sw::Reactor|rr::Nucleus|marl::"
    check_cc_absent Vulkan "vkCreateInstance|VkPhysicalDevice"
    check_cc_absent Dawn "dawn::native|wgpu::"
    check_cc_absent "ANGLE's GL implementation" "egl::Display|sh::TCompiler|gl::init::"

    # WebRTC and what it brings. This one is not about a GPU: it is about a
    # machine with no camera and no audio input, and it is the claim
    # is_p2p_enabled = false makes - asked of the binary.
    check_cc_absent "WebRTC, XNNPACK or tflite" "webrtc::|xnn_|tflite::"

    # M160 checked is_p2p_enabled = false here. M161 removed that flag - see
    # tools/build-chromium.sh for why - so what is graded now is the claim
    # that survived it: WebNN's GPU back end is off, which is what keeps
    # Dawn's native library and SwiftShader out of every binary above.
    ARGSFILE="$SRC/out/$OUT_NAME/args.gn"
    grep -qE "^webnn_use_litert = false\$" "$ARGSFILE"
    check $? "and the build's own args.gn says why: webnn_use_litert = false"

    # The four calls //cc's graph asked this libc for, in the SYSROOT rather
    # than in the tree - M158's lesson about a header that is in git and
    # reaches no cross compile. lgammaf is the one that cost 47 of the 68
    # failures, in Eigen, through tflite.
    MISSING=0
    for name in lgammaf tgammaf rand_r mlock; do
      grep -rq "$name" "$ROOT/build/sysroot/usr/local/include/" || MISSING=1
    done
    [ "$MISSING" = "0" ]
    check $? "this libc's lgammaf, tgammaf, rand_r and mlock are in the sysroot a cross compile reads"
    grep -q "LOCK_EX" "$ROOT/build/sysroot/usr/local/include/sys/file.h"
    check $? "and flock's operations are in its <sys/file.h>"

    IMAGE="$ROOT/build/os-image.bin"
    if [ ! -f "$IMAGE" ]; then
      echo "chromium-test: no $IMAGE - run make, then this again"
    else
      make -s -C "$ROOT" leanfs-put > /dev/null 2>&1
      "$ROOT/build/leanfs-put" "$IMAGE" "$CC2PROGRAM" /bin/chromiumcc2 > /dev/null
      check $? "installed as /bin/chromiumcc2 - the [m160] boot self-test runs it"
    fi
  fi

  # M161. Blink - the engine itself. //cc arriving in M160 is what made this
  # reachable: blink::GraphicsContext records into a cc::PaintRecord, so the
  # compositor has to exist before the thing that feeds it can.
  BLINKPROGRAM="$SRC/out/$OUT_NAME/blinktest"
  if [ ! -x "$BLINKPROGRAM" ]; then
    echo "chromium-test: Blink has not been built - skipping" \
         "(tools/build-chromium.sh lean_os)"
  else
    "${PREFIX}readelf" -h "$BLINKPROGRAM" | grep -q "EXEC (Executable file)"
    check $? "a program linked from Blink is an EXEC in this OS's image region ($("${PREFIX}readelf" -h "$BLINKPROGRAM" | awk '/Type:/{t=$2} /Entry point/{print t, $NF}'))"

    # WTF, which is the library every other part of the engine is written in.
    # The counts are of what the LINKER took, not of what was built: this
    # program uses strings, the atom table and a graphics context, so that is
    # what is in it.
    WTFSYMS=$("${PREFIX}nm" -C "$BLINKPROGRAM" |
              grep -cE " [TtWw] .*blink::(StringImpl|AtomicString|StringBuilder|StringView)")
    [ "$WTFSYMS" -gt 100 ]
    check $? "with WTF's string machinery in it - StringImpl, AtomicString, StringBuilder ($WTFSYMS symbols)"

    # And none of the four stacks. Blink is where WebGPU and WebGL live, so
    # this is the strongest place to ask: the engine that would use Dawn is
    # linked and Dawn's native library is not in it.
    check_blink_absent() {
      FOUND=$("${PREFIX}nm" -C "$BLINKPROGRAM" | grep -cE "$2")
      [ "$FOUND" = "0" ]
      check $? "and no $1 in a program that links Blink"
    }
    check_blink_absent SwiftShader "sw::Reactor|rr::Nucleus|marl::"
    check_blink_absent "Dawn's native library" "dawn::native|wgpu::"
    check_blink_absent "ANGLE's GL implementation" "egl::Display|sh::TCompiler|gl::init::"

    ARGSFILE="$SRC/out/$OUT_NAME/args.gn"
    NOFF=0
    for flag in webnn_use_litert webnn_use_tflite use_pangocairo; do
      grep -qE "^$flag = false\$" "$ARGSFILE" || NOFF=1
    done
    [ "$NOFF" = "0" ]
    check $? "and args.gn turns off WebNN's two back ends and pangocairo"

    # is_p2p_enabled is back ON since M161, and that is a claim worth
    # grading rather than leaving in a comment: Chromium has no supported
    # configuration of Blink without WebRTC.
    grep -qE "^is_p2p_enabled = true\$" "$ARGSFILE" ||
      ! grep -qE "^is_p2p_enabled" "$ARGSFILE"
    check $? "and does NOT turn off is_p2p_enabled, because Blink's modules are not optional"

    # The libc this milestone grew, in the SYSROOT rather than the tree.
    LFSMISSING=0
    for name in fopen64 fseeko64 ftello64 off64_t; do
      grep -rq "$name" "$ROOT/build/sysroot/usr/local/include/" || LFSMISSING=1
    done
    [ "$LFSMISSING" = "0" ]
    check $? "this libc's LFS64 names are in the sysroot a cross compile reads"
    grep -q "SO_TIMESTAMP" "$ROOT/build/sysroot/usr/local/include/sys/socket.h"
    check $? "and SO_TIMESTAMP and SCM_TIMESTAMP are in its <sys/socket.h>"

    IMAGE="$ROOT/build/os-image.bin"
    if [ ! -f "$IMAGE" ]; then
      echo "chromium-test: no $IMAGE - run make, then this again"
    else
      make -s -C "$ROOT" leanfs-put > /dev/null 2>&1
      "$ROOT/build/leanfs-put" "$IMAGE" "$BLINKPROGRAM" /bin/chromiumblink > /dev/null
      check $? "installed as /bin/chromiumblink - the [m161] boot self-test runs it"
    fi
  fi

  # M162. The DISPLAY compositor - the one the browser runs, whose input is
  # whole CompositorFrames submitted by other processes and whose output is
  # the framebuffer. M161 set this target aside because it IS the GPU service
  # and so wants the GL implementation compiled; what that did not say is
  # that Blink already reaches all of it, which is why this rung is 22
  # targets wide.
  VIZPROGRAM="$SRC/out/$OUT_NAME/viztest"
  if [ ! -x "$VIZPROGRAM" ]; then
    echo "chromium-test: the display compositor has not been built - skipping" \
         "(tools/build-chromium.sh lean_os)"
  else
    "${PREFIX}readelf" -h "$VIZPROGRAM" | grep -q "EXEC (Executable file)"
    check $? "a program linked from the display compositor is an EXEC in this OS's image region ($("${PREFIX}readelf" -h "$VIZPROGRAM" | awk '/Type:/{t=$2} /Entry point/{print t, $NF}'))"

    # What the LINKER took. The aggregator is the piece that makes this the
    # display compositor rather than another rasteriser: it is what turns N
    # surfaces from N processes into one frame.
    VIZSYMS=$("${PREFIX}nm" -C "$VIZPROGRAM" |
              grep -cE " [TtWw] .*viz::(SurfaceAggregator|SoftwareRenderer|Display|DisplayResourceProviderSoftware)::")
    [ "$VIZSYMS" -gt 50 ]
    check $? "with the aggregator and the software renderer in it ($VIZSYMS symbols)"

    # And this is the strongest place in the whole port to ask the question
    # M159 first asked, because this target IS //gpu/ipc/service: the GPU
    # service is COMPILED and the GL implementation is in no binary. A check
    # written against args.gn could not say this at all.
    check_viz_absent() {
      FOUND=$("${PREFIX}nm" -C "$VIZPROGRAM" | grep -cE "$2")
      [ "$FOUND" = "0" ]
      check $? "and no $1 in a program that links the GPU service"
    }
    check_viz_absent "ANGLE's GL implementation" "egl::Display|sh::TCompiler|gl::init::"
    check_viz_absent SwiftShader "sw::Reactor|rr::Nucleus|marl::"
    check_viz_absent "Dawn's native library" "dawn::native|wgpu::"

    # //sandbox, which reached the display compositor through ONE edge -
    # //components/vrp_flags, whose README describes it as a controlled read
    # and write primitive handed to a renderer for reward-program
    # researchers. It is off, so nothing of seccomp is here to be linked.
    check_viz_absent "seccomp policy" "sandbox::policy::|sandbox::SandboxBPF"

    ARGSFILE="$SRC/out/$OUT_NAME/args.gn"
    grep -qE "^enable_vrp_flags = false\$" "$ARGSFILE"
    check $? "and args.gn turns off the vulnerability reward program's read/write primitive"

    IMAGE="$ROOT/build/os-image.bin"
    if [ ! -f "$IMAGE" ]; then
      echo "chromium-test: no $IMAGE - run make, then this again"
    else
      make -s -C "$ROOT" leanfs-put > /dev/null 2>&1
      "$ROOT/build/leanfs-put" "$IMAGE" "$VIZPROGRAM" /bin/chromiumviz > /dev/null
      check $? "installed as /bin/chromiumviz - the [m162] boot self-test runs it"
    fi
  fi

  # M163. //content builds for this machine - the browser and renderer
  # processes, and the first rung in this port that is not a library. A -k 0
  # build of it failed in 86 places out of 36,010 steps and every one
  # resolved to a seam Chromium's own build already had.
  #
  # This grades the OBJECTS rather than a program: linking //content into an
  # executable is M164, because what remains there is a link surface -
  # Blink's WebGPU, a cert verifier vtable entry, GpuPreSandboxHook and
  # dlopen - rather than anything about whether //content compiles.
  CONTENTOBJ="$SRC/out/$OUT_NAME/obj/content/browser/browser"
  if [ ! -d "$CONTENTOBJ" ]; then
    echo "chromium-test: //content has not been built - skipping" \
         "(tools/build-chromium.sh content)"
  else
    NOBJ=$(find "$CONTENTOBJ" -name '*.o' | wc -l | tr -d ' ')
    [ "$NOBJ" -gt 500 ]
    check $? "//content's browser process compiles for this machine ($NOBJ objects)"

    SAMPLE=$(find "$CONTENTOBJ" -name 'browser_main_loop.o' | head -1)
    [ -n "$SAMPLE" ] && "${PREFIX}readelf" -h "$SAMPLE" | grep -q "Advanced Micro Devices X86-64"
    check $? "and they are x86-64 ELF for this target, not the host's"

    # The zygote and the seccomp policies produce no OBJECTS, which is a
    # stronger statement than a flag. It has to be the .o rather than the
    # directory: a directory keeps its .ninja and its .o.d after gn drops the
    # target, so asking about the directory asks about the output tree's
    # history rather than about this configuration.
    for absent in obj/content/zygote/zygote/zygote_linux.o \
                  obj/sandbox/linux/seccomp_bpf/sandbox_bpf.o \
                  obj/content/browser/browser/zygote_host_impl_linux.o; do
      [ ! -e "$SRC/out/$OUT_NAME/$absent" ]
      check $? "and $absent was not built"
    done

    ARGSFILE="$SRC/out/$OUT_NAME/args.gn"
    NOFF=0
    for flag in use_dbus use_on_device_model_service; do
      grep -qE "^$flag = false\$" "$ARGSFILE" || NOFF=1
    done
    [ "$NOFF" = "0" ]
    check $? "and args.gn turns off the Linux message bus and the on-device model service"

    # pause(2), which content/common's WaitForDebugger asked this libc for.
    # In the SYSROOT rather than the tree, because that is what a cross
    # compile reads.
    grep -q "int pause(void);" "$ROOT/build/sysroot/usr/local/include/unistd.h"
    check $? "this libc's pause(2) is declared in the sysroot a cross compile reads"
    # grep -c rather than grep -q: this script runs under `set -o pipefail`,
    # and grep -q exits on the first match, so nm upstream dies of SIGPIPE and
    # the pipeline reports 141 - a check that fails because it succeeded.
    PAUSEDEF=$("${PREFIX}nm" "$ROOT/build/sysroot/usr/lib/libc.a" 2>/dev/null |
               grep -c "T pause")
    [ "$PAUSEDEF" -ge 1 ]
    check $? "and defined in the libc.a beside it"

    # M164. Two more the LINK asked for rather than the compile.
    #
    # strtoimax and strtoumax had been DECLARED in this libc's <inttypes.h>
    # since it was written and never defined, which a compile cannot notice.
    IMAXDEF=$("${PREFIX}nm" "$ROOT/build/sysroot/usr/lib/libc.a" 2>/dev/null |
              grep -cE "T strtoi?u?max")
    [ "$IMAXDEF" -ge 2 ]
    check $? "this libc defines the two <inttypes.h> declared and did not ($IMAXDEF of 2)"

    # And libdl.a, which was an empty archive. dlopen and its three
    # companions live in the dynamic loader, so a STATICALLY linked program
    # had nothing to resolve them against - and -ldl is already on the link
    # line that asks.
    DLDEF=$("${PREFIX}nm" "$ROOT/build/sysroot/usr/lib/libdl.a" 2>/dev/null |
            grep -cE "T dl(open|sym|close|error)")
    [ "$DLDEF" = "4" ]
    check $? "and libdl.a answers the four a static program cannot get from the loader ($DLDEF of 4)"

    # The four must NOT be in libc.a as well: a dynamically linked program
    # resolves them from ld-lean.so at run time, and a definition in libc.a
    # would be bound at link time and silently win.
    DLINLIBC=$("${PREFIX}nm" "$ROOT/build/sysroot/usr/lib/libc.a" 2>/dev/null |
               grep -cE "T dl(open|sym|close|error)")
    [ "$DLINLIBC" = "0" ]
    check $? "and they are NOT in libc.a, where they would outrank the loader's"
  fi

  # M165. //content LINKS, which is a different claim from M163's "compiles"
  # and is what found everything in this milestone.
  CONTENTPROGRAM="$SRC/out/$OUT_NAME/contenttest"
  if [ ! -f "$CONTENTPROGRAM" ]; then
    echo "chromium-test: //content has not been linked - skipping" \
         "(tools/build-chromium.sh lean_os)"
  else
    "${PREFIX}readelf" -h "$CONTENTPROGRAM" | grep -q "EXEC (Executable file)"
    check $? "//content links into a program for this machine"

    NOINTERP=$("${PREFIX}readelf" -l "$CONTENTPROGRAM" 2>/dev/null |
               grep -cE "^  INTERP")
    [ "$NOINTERP" = "0" ]
    check $? "and it is static - no interpreter, like every program on this image"

    # 767,000 symbols, so the table is written once and every check below
    # greps that rather than running nm fourteen times over a 318 MB binary.
    SYMWORK=$(mktemp -d)
    TMPSYMS="$SYMWORK/contenttest.sym"
    "${PREFIX}nm" -C "$CONTENTPROGRAM" > "$TMPSYMS" 2>/dev/null
    content_has() {
      FOUND=$(grep -cE " [TtWwDdBbVv] .*($2)" "$TMPSYMS")
      [ "$FOUND" -ge "$3" ]
      check $? "and $1 is in it ($FOUND symbols)"
    }
    content_absent() {
      FOUND=$(grep -cE " [TtWwDdBbVv] .*($2)" "$TMPSYMS")
      [ "$FOUND" = "0" ]
      check $? "and no $1 in it"
    }

    # The browser really is here rather than a library that links.
    content_has "//content's own process dispatch" "content::ContentMainRunnerImpl" 5
    content_has "the render process host" "content::RenderProcessHostImpl" 100
    content_has "the render frame host" "content::RenderFrameHostImpl" 500
    content_has "Blink's document" "blink::Document::" 100
    content_has "V8" "v8::Isolate" 100

    # WEBGPU. use_dawn is true since M165 - Blink has no configuration without
    # it - so Dawn IS in this binary, and the question is WHICH Dawn. The null
    # backend is the honest one on a machine with no adapter: requestAdapter()
    # answers null, which is what the specification says to do.
    content_has "Dawn's null backend" "dawn::native::null::" 50
    for backend in vulkan opengl d3d metal; do
      content_absent "Dawn's $backend backend" "dawn::native::${backend}"
    done

    # And the three that must still be absent, by NAMESPACE rather than by
    # word - M159's rule. Dawn's five flags are what keep them out.
    content_absent SwiftShader "sw::Reactor|rr::Nucleus|marl::"
    content_absent "the Vulkan loader" "vk::|VulkanLoader|vulkan_loader"
    content_absent "seccomp policy" "sandbox::policy::SandboxLinux|sandbox::SandboxBPF"

    # ANGLE's GL IS in this binary, and that is a change from M159 rather than
    # a regression. M159 measured /bin/chromiumgpu and found no gl::init::,
    # because nothing in it called gl::init::InitializeGLOneOff. //content
    # CONTAINS THE GPU PROCESS, and a GPU process initialises GL on its first
    # line - so this is the first program in the port where the answer is
    # different, and the check asserts the new answer rather than the old one.
    #
    # It is not a claim that this machine has GL. The initialisation has
    # nothing to succeed with - no SwiftShader, no Vulkan loader, no display -
    # and what Chromium does then is ComputeGpuFeatureInfoWithNoGpu(), which
    # M159 graded and which leaves the 2D canvas on the CPU.
    content_has "ANGLE's GL implementation, which the GPU process initialises" \
                "gl::init::" 10

    # M166. The sandbox this platform has, and the one function that enters
    # it. Asked of the BINARY rather than of the patch, because //sandbox/policy
    # reaches this program through a dependency content/renderer declares only
    # for this platform - on Linux it arrives through use_seccomp_bpf, which is
    # false here, so before M166 sandbox::policy::Sandbox was in no binary at
    # all and a caller of it would have linked against nothing.
    content_has "the capability sandbox a renderer enters" \
                "sandbox::policy::Sandbox::EnterCapabilitySandbox" 1
    content_has "and the function that answers whether it is in it" \
                "sandbox::policy::Sandbox::IsProcessSandboxed" 1

    # And still none of Linux's own, which is what M166 did NOT do: entering a
    # capability sandbox is not seccomp arriving by another name.
    content_absent "seccomp-bpf policy" "sandbox::bpf_dsl::|SandboxBPF"
    content_absent "a namespace sandbox" "sandbox::NamespaceSandbox"

    rm -rf "$SYMWORK"

    ARGSFILE="$SRC/out/$OUT_NAME/args.gn"
    NOFF=0
    for flag in dawn_enable_vulkan dawn_enable_vulkan_loader \
                dawn_enable_vulkan_validation_layers dawn_use_swiftshader \
                dawn_enable_opengles dawn_enable_desktop_gl; do
      grep -qE "^$flag = false\$" "$ARGSFILE" || NOFF=1
    done
    [ "$NOFF" = "0" ]
    check $? "and args.gn turns off all six of Dawn's own back-end flags"
    grep -qE "^use_dawn = true\$" "$ARGSFILE"
    check $? "with use_dawn TRUE, because Blink has no configuration without it"

    IMAGE="$ROOT/build/os-image.bin"
    if [ ! -f "$IMAGE" ]; then
      echo "chromium-test: no $IMAGE - run make, then this again"
    else
      # Stripped, unlike the smaller programs this port installs. 318 MB
      # becomes 205 MB, and the kernel reads the whole file into its own heap
      # before it copies the image into the process - so the symbol table
      # would be paid for twice, to answer questions only the host asks.
      STRIPPED="$ROOT/build/chromiumcontent.stripped"
      cp "$CONTENTPROGRAM" "$STRIPPED"
      "${PREFIX}strip" "$STRIPPED"
      make -s -C "$ROOT" leanfs-put > /dev/null 2>&1
      "$ROOT/build/leanfs-put" "$IMAGE" "$STRIPPED" /bin/chromiumcontent > /dev/null
      check $? "installed as /bin/chromiumcontent - the [m165] boot self-test runs it"
    fi

  # M167. Chromium's OWN browser, rather than a program written here that
  # links its libraries. content_shell is the embedder the Chromium tree
  # ships, and what it proves is different in kind: it starts the processes a
  # browser is made of, through the paths Chromium uses to start them.
  SHELLPROGRAM="$SRC/out/$OUT_NAME/content_shell"
  if [ ! -f "$SHELLPROGRAM" ]; then
    echo "chromium-test: content_shell has not been linked - skipping" \
         "(tools/build-chromium.sh content/shell:content_shell)"
  else
    file "$SHELLPROGRAM" | grep -q "ELF 64-bit LSB executable, x86-64"
    check $? "Chromium's own browser links for this machine as an x86-64 EXEC"

    "${PREFIX}readelf" -l "$SHELLPROGRAM" | grep -q "INTERP"
    [ $? -ne 0 ]
    check $? "and has no interpreter, like every other program in this image"

    # A file rather than a shell variable: this binary's symbol table does not
    # fit in one, and what a command substitution does when it does not fit is
    # give back something shorter without saying so - which reads as "the
    # symbol is absent" and is how these two checks failed on a binary that
    # has 56 and 579 of them.
    SHELLWORK=$(mktemp -d)
    SHELLSYMS="$SHELLWORK/shell.syms"
    "${PREFIX}nm" -C "$SHELLPROGRAM" > "$SHELLSYMS" 2>/dev/null

    # The browser's own window, and the process types it starts. Asked of the
    # binary rather than of the build, which is M159's rule.
    grep -qE " [TtWw] .*content::Shell::CreateNewWindow" "$SHELLSYMS"
    check $? "and content::Shell is in it - the browser, not a library that links"
    grep -qE " [TtWw] .*content::RenderProcessHostImpl" "$SHELLSYMS"
    check $? "and RenderProcessHostImpl, which is what launches a renderer"

    # Crashpad is ptrace(2), PR_SET_DUMPABLE and /proc/<pid>/task - Linux's
    # kernel - and there is no crash reporting service here for a report to
    # reach. It arrived as a DATA dependency, which nothing links and ninja
    # builds anyway: M162's note about gn path --with-data, a second time.
    grep -qE " [TtWwDdBb] .*crashpad::" "$SHELLSYMS"
    [ $? -ne 0 ]
    check $? "and no crashpad in it, whose Linux half is ptrace and PR_SET_DUMPABLE"

    # libpfm4 is perf_event_open(2) and <linux/perf_event.h>, reached from
    # google_benchmark through base's test support.
    grep -qE " [TtWw] .*pfm_" "$SHELLSYMS"
    [ $? -ne 0 ]
    check $? "and no hardware performance counters, which are Linux's own kernel"

    # M171. The browser is a window on this desktop: an ozone platform of this
    # project's own, under third_party/chromium/lean_os/ozone, that draws
    # into the compositor's shared segment and reads the window's event pipe
    # through the same client library every desktop program links. Graded on
    # the BINARY (M159's rule): the constructor ui/ozone's generated list
    # names, and the client library's connect call reached from it.
    grep -qE " [TtWw] .*ui::CreateOzonePlatformLeanos" "$SHELLSYMS"
    check $? "and this desktop's own ozone platform is in it, as ui::CreateOzonePlatformLeanos"
    grep -qE " [TtWw] window_manager_connect$" "$SHELLSYMS"
    check $? "which connects to the compositor through this project's window_manager_connect"
    grep -qE " [TtWw] .*ui::LeanOsWindow::DispatchKey" "$SHELLSYMS"
    check $? "and turns the compositor's key events into ui::KeyEvents"

    # Headless stays the default: the boot battery runs the browser before
    # the desktop is up and grades its picture over the serial line, and
    # the desktop's launcher passes --ozone-platform=leanos itself.
    ARGSFILE="$SRC/out/$OUT_NAME/args.gn"
    grep -qE '^ozone_platform = "headless"$' "$ARGSFILE"
    check $? "while headless stays the default platform, for the battery's serial-line picture"
    grep -qE 'ozone_external_platforms = \[ "leanos" \]' "$ROOT/third_party/chromium/lean_os/ozone_extra.gni"
    check $? "and ozone_extra.gni is how ui/ozone learned the platform's name - no patch"

    # The launcher and the engine carry the same grant, because an exec keeps
    # the intersection (M166) - see user_space/binaries/browser.c.
    grep -qE '\{"browser", *CAP_APP_DEFAULT \| CAP_NETWORK\}' \
         "$ROOT/system_api/include/capabilities.h"
    check $? "/bin/browser, the launcher the desktop opens, is granted what the engine is"

    IMAGE="$ROOT/build/os-image.bin"
    if [ ! -f "$IMAGE" ]; then
      echo "chromium-test: no $IMAGE - run make, then this again"
    else
      # One installer, shared with `make browser`: the engine, its resource
      # bundle, the launcher, the home page and the fonts.
      "$ROOT/tools/install-browser.sh" > /dev/null
      check $? "installed by tools/install-browser.sh - the [m113], [m167] and [m169] boot self-tests run it"
    fi
    rm -rf "$SHELLWORK"
  fi

    # M165. __cxa_thread_atexit, which is the last of M164's list and the only
    # one with a subsystem behind it rather than a definition: a thread_local
    # with a non-trivial destructor needs a per-thread list run at thread exit,
    # and this library had no such hook at all.
    # M166. The browser is granted something the renderer must give up, and
    # it is granted BY NAME - a program with no manifest entry gets
    # CAP_APP_DEFAULT, and a renderer dropping to nothing from that would be
    # a smaller claim than the one [m166] prints.
    grep -qE '\{"chromiumcontent", *CAP_APP_DEFAULT \| CAP_NETWORK\}' \
         "$ROOT/system_api/include/capabilities.h"
    check $? "the browser process is granted CAP_NETWORK by name, so a renderer has something to lose"

    # The two calls the patch makes are C, and the header now says so. Without
    # this the renderer compiles and the LINK fails on a mangled name - which
    # is how M166 found it.
    grep -q 'extern "C"' "$ROOT/user_space/library/syscall_wrappers.h"
    check $? "syscall_wrappers.h declares its calls as C, which a C++ caller needs"

    TADEF=$("${PREFIX}nm" "$ROOT/build/sysroot/usr/lib/libc.a" 2>/dev/null |
            grep -c "T __cxa_thread_atexit")
    [ "$TADEF" = "1" ]
    check $? "this libc defines __cxa_thread_atexit, which a thread_local with a destructor needs"

    # The hook has to be REACHED, not merely present. exit() calls it for the
    # main thread and pthread.c calls it on both of a thread's ways out, so a
    # definition nothing referenced would pass the check above and destroy
    # nothing. What is asked is the caller rather than the callee.
    TAUSE=$("${PREFIX}nm" "$ROOT/build/sysroot/usr/lib/libc.a" 2>/dev/null |
            grep -c "U __lean_run_thread_destructors")
    [ "$TAUSE" -ge 2 ]
    check $? "and exit(2) and the thread exit paths both reach it ($TAUSE callers)"
  fi

  # M157. The libc port reaches Chromium's OWN vendored copy of the libc
  # crate, not just rust-src's - and they are the same 0.2.189, so a copy
  # that drifted would be a build that disagrees with itself about what this
  # target's struct stat is. This grades that they are the same bytes.
  CRATE="$SRC/third_party/rust/chromium_crates_io/vendor/libc-v0_2/src/unix/lean_os"
  if [ -d "$CRATE" ]; then
    SAME=0
    for name in mod.rs constants.rs; do
      cmp -s "$ROOT/tools/rust-port/libc/$name" "$CRATE/$name" 2>/dev/null ||
        cmp -s "$ROOT/tools/rust-port/libc/lean_os/$name" "$CRATE/$name" ||
        SAME=1
    done
    [ "$SAME" = "0" ]
    check $? "Chromium's vendored libc crate carries the same lean_os module rust-src does"
  fi
fi

echo "chromium-test: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ]
