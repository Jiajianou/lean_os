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
  # A path git does not track here belongs to one of the sub-repositories the
  # checkout is assembled from, and those are left alone.
  (cd "$SRC" && git ls-files --error-unmatch "$f" > /dev/null 2>&1) || continue
  (cd "$SRC" && git checkout -- "$f")
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
fi

echo "chromium-test: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ]
