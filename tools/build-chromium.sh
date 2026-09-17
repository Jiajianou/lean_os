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
    if (cd "$SRC" && git ls-files --error-unmatch "$f" > /dev/null 2>&1); then
      (cd "$SRC" && git checkout -- "$f") || exit 1
      continue
    fi
    # A path the top-level checkout does not track belongs to one of the
    # sub-repositories it is assembled from - third_party/angle, dawn, webrtc,
    # perfetto and a hundred others, each with its own .git. M161: leaving
    # those alone was wrong, and wrong in a way that wastes an afternoon
    # rather than failing loudly. The series is applied to a CLEAN tree every
    # time precisely so that "is this patch already applied" needs no answer -
    # and a sub-repository file that never got reset makes the question come
    # back, as "does not apply to this checkout" on a patch that is perfectly
    # good. It cost this milestone three builds before it was worth fixing.
    #
    # git -C finds the sub-repository from the file's own directory, so this
    # needs no list of which ones there are.
    d=$(dirname "$SRC/$f")
    top=$(git -C "$d" rev-parse --show-toplevel 2>/dev/null) || continue
    [ "$top" = "$SRC" ] && continue
    rel=${f#${top#$SRC/}/}
    (cd "$top" && git checkout -- "$rel" 2>/dev/null) || true
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

# Chromium vendors the libc crate a SECOND time, at the same 0.2.189, for its
# own Rust targets rather than for std - and that is the copy //skia reaches,
# through fontconfig's fontations font backend. Without this the crate builds
# for an unknown unix: no struct stat, no O_*, no time_t, and the family-wide
# `pub use unistd::*` in src/new/mod.rs resolves to nothing.
#
# It is the same port applied a second time rather than a patch carrying a
# copy of it, because 948 lines duplicated is 948 lines that drift. M157.
CHROMIUM_LIBC_CRATE="$SRC/third_party/rust/chromium_crates_io/vendor/libc-v0_2"
if [ -d "$CHROMIUM_LIBC_CRATE/src/unix" ]; then
  echo "build-chromium: applying the same libc port to Chromium's vendored copy"
  python3 "$ROOT/tools/rust-port/apply.py" --libc "$CHROMIUM_LIBC_CRATE" \
    "$SRC/third_party/rust/libc/v0_2/BUILD.gn" > /dev/null || exit 1
fi

BUILTINS_DIR="$CLANG_BASE/lib/clang/$CLANG_VER/lib/x86_64-unknown-linux-gnu"
LIBGCC="$ROOT/build/toolchain/lib/gcc/x86_64-lean_os/14.2.0/libgcc.a"
if [ ! -f "$BUILTINS_DIR/libclang_rt.builtins.a" ]; then
  echo "build-chromium: exposing libgcc as the builtins library clang expects"
  mkdir -p "$BUILTINS_DIR"
  cp "$LIBGCC" "$BUILTINS_DIR/libclang_rt.builtins.a" || exit 1
fi

# Asked of the compiler rather than spelled out, because the version directory
# under lib/clang moves whenever the toolchain is rebuilt. This is the
# directory every libclang-based tool in the build parses with - bindgen
# (M154) and V8's metagen (M155) - and the one named in
# default_toolchain_cflags below, and they all have to be the SAME directory.
LEANOS_RESOURCE_DIR=$("${PREFIX}clang" -print-resource-dir)
if [ ! -d "$LEANOS_RESOURCE_DIR/include" ]; then
  echo "build-chromium: ${PREFIX}clang has no resource directory" >&2
  exit 1
fi
if [ "${LEANOS_CHROMIUM_CC:-lean_os}" = "chromium" ]; then
  LEANOS_RESOURCE_DIR="$CLANG_BASE/lib/clang/$CLANG_VER"
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
libclang_resource_dir = "$LEANOS_RESOURCE_DIR"
ozone_extra_path = "//lean_os/ozone_extra.gni"

# The snapshot - V8's pre-built heap, the thing that makes starting an isolate
# take a millisecond rather than a second - is linked into the binary rather
# than loaded from a file beside it. Chromium's own builds put it in
# snapshot_blob.bin because a browser ships a directory; a program on this
# machine is one file the kernel maps, and /bin/chromiumv8 having to find a
# second one would be a fact about this port rather than about V8.
v8_use_external_startup_data = false

# Ozone's platform backends, which are the one place a Linux GN
# configuration reaches for a host library this machine has no idea about:
# the automatic selection turns on X11 and Wayland, and the Wayland one
# wants third_party/wayland, which this checkout does not even contain.
# Headless is the one that needs nothing, and this OS's compositor is not
# ozone's to talk to anyway.
ozone_auto_platforms = false
ozone_platform = "headless"
ozone_platform_headless = true
ozone_platform_x11 = false
ozone_platform_wayland = false

is_debug = false
is_component_build = false
symbol_level = 0
blink_symbol_level = 0
use_remoteexec = false
dcheck_always_on = false
clang_use_chrome_plugins = false
treat_warnings_as_errors = false

# Off because the apple toolchain's objcxx tool writes its object to a path it
# spells out rather than to {{output}} - deliberately, with a comment saying
# why - so a precompiled header, whose output GN renames to .gch, is written
# to precompile.o and the build stops on a file that is not there. It is
# dormant upstream: enable_precompiled_headers is already false whenever clang
# modules are on, and they are on for any build using Chromium's own sysroot.
# This one does not, because the target is not Linux.
enable_precompiled_headers = false

# The features NOT turned off here are the reason this list is shorter than
# M144 left it. GN resolves every target defined in a file it loads, whether
# or not --root-target's tree reaches it, so //mojo pulls //net pulls //ipc
# pulls //content/test pulls //chrome - and a feature switched off makes
# every assert() guarding that feature's directory fire in a directory this
# build never compiles. A default is a configuration Chromium tests; an
# unusual one is a configuration only this port is in. So the flags left are
# the ones that decide what the LINKED programs contain and the ones naming a
# host library that is not here, and nothing else.
#
# use_ozone and use_aura are gone for a sharper reason: a declare_args()
# default is evaluated per toolchain and a value in args.gn is not. Setting
# both true set them for the mac HOST toolchain as well, where is_mac and
# use_ozone together make //ui/base/clipboard depend on //ui/base, which
# depends on it - a dependency cycle that exists in no configuration
# Chromium ships.
# use_sysroot is Chromium's "download a Debian image and build against it",
# which this is not. target_sysroot is the other hook and it is the right one:
# sysroot.gni applies it only when current_os == target_os && current_cpu ==
# target_cpu, which is true for this toolchain and false for the mac HOST one -
# so the host still gets mac_sdk_path. Setting the \`sysroot\` global directly
# would reach both, because it is one variable for the whole build.
#
# M154 left this empty and recorded it as harmless; M155 found what it costs.
# Any gn command that loads the WHOLE tree - and V8's metagen runs \`gn desc\`
# during the build - evaluates //build/modules, which expands
# "\${sysroot}/usr/include" and stops on the literal /usr/include of this Mac.
use_sysroot = false
target_sysroot = "$SYSROOT"
lean_os_target = "$LEANOS_TARGET_FLAG"

# What this target IS, said in a place every tool can read.
#
# The toolchain's extra_cflags are baked into the command string its tools
# run, so bindgen - which assembles its own libclang command line out of
# {{defines}}, {{include_dirs}} and {{cflags}} - never sees them, and computes
# this target's type layouts from whatever triple //build/config/compiler put
# in {{cflags}} instead. That is x86_64-unknown-linux-gnu, which has a
# different struct stat, a different sigset_t and a glibc that is not here.
# M154's config carries the four facts that decide the answer:
#
#   --target      the triple, which has to win over the one already in cflags
#   --sysroot     where this OS's headers are, which the clang parsing them
#                 has no default for the way x86_64-lean_os-clang does
#   -D__lean_os__ the identity build_config.h branches on, which a clang that
#                 has never heard of this triple cannot derive from it
#   -idirafter    the resource directory of the clang doing the parsing,
#                 spelled JOINED rather than as two arguments - see below
#
# The last one is M153's prediction and it is right for the reason M153 gave -
# libc++'s own stddef.h does #include_next <stddef.h> and, for a target this
# libclang has never heard of, finds nothing after it - but it is right only
# for ONE value. It has to be the resource directory of x86_64-lean_os-clang,
# which is also what patch 0025 makes bindgen parse with, because TWO
# DIFFERENT clang resource directories in one include path are fatal: clang's
# stdint.h ends with
#
#   #if __STDC_HOSTED__ && __has_include_next(<stdint.h>)
#
# so each of them includes the other, the include guard the first one set
# swallows the second, and nothing declares uint8_t. It fails in a header
# three levels away that is not wrong about anything. Naming one directory
# twice is harmless; naming a second one is not, and both halves of this
# milestone spent an hour each proving that from opposite ends.
#
# The joined spelling is M155's. Every flag Chromium puts in cflags is either
# an option or a path relative to root_build_dir, so a token beginning with a
# slash means one thing to a tool reading them: clang-cl, which spells its
# options that way. V8's metagen decides exactly that -
#
#   cl_mode = any(f.startswith("/") for f in cflags)
#
# - and "-idirafter", "/Users/..." as two arguments made it re-parse V8 in
# MSVC mode and fail with "no input files". Joined, the token starts with a
# dash and the heuristic is right again. This is a real constraint rather
# than one tool's quirk: anything downstream that splits cflags sees an
# absolute path with no option attached to it.
#
# Everything here is already true for the real compile - x86_64-lean_os-clang
# defines __lean_os__ itself, was configured with DEFAULT_SYSROOT, and has its
# own resource directory ahead of this one - so this changes what bindgen
# reads and nothing about what the compiler does.
default_toolchain_cflags = [
  "--target=x86_64-lean_os",
  "--sysroot=$SYSROOT",
  "-D__lean_os__=1",
  "-D__lean_os=1",
  "-idirafter$LEANOS_RESOURCE_DIR/include",
]
use_custom_libcxx = true
libcxx_provides_default_rune_table = true
use_glib = false
use_nss_client_certs = false
use_nss_server_certs = false
use_udev = false
use_gio = false
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
use_vaapi = false
use_v4l2_codec = false
enable_remoting = false
rtc_use_pipewire = false

# Skia's Graphite Dawn backend. Its declare_args() default is a list of
# platforms with a comment saying it is enabled "where the team has verified
# that at least basic rendering to the screen is working" - and it reaches
# this OS only because is_linux is true here, which says which family the
# build gates this platform as and nothing about whether there is a GPU. This
# machine has no GL, no Vulkan and no display driver a GPU process could talk
# to, so Dawn is the one answer that would be a pretence. Skia rasterises on
# the CPU here.
#
# This is an args.gn value rather than a patch on purpose: the default is a
# verified-platforms list, and an out-of-tree platform not being on it is the
# mechanism working rather than something to fix upstream.
skia_use_dawn = false

# The GPU stack, and why none of it is built.
#
# M157 turned off skia_use_dawn and wrote down why: a declare_args() default
# that is a list of platforms reaches this OS because is_linux is true, which
# says which family the build gates this platform as and nothing about
# whether there is a GPU. These four are the same sentence three directories
# further out, and the defaults say so in their own words:
#
#   ui/gl/features.gni     use_dawn = ... || (is_linux && !is_castos)
#   ui/gl/features.gni     enable_swiftshader = (is_win || is_linux || ...)
#   gpu/vulkan/features.gni  enable_vulkan = is_linux || is_chromeos || ...
#
# There is no GL here, no Vulkan, and no display a GPU process could talk to.
# The answer is not a software GL implementation underneath a GPU path - it
# is the path Chromium maintains for every platform for the case where GPU
# compositing is off, and it is intact and reachable:
#
#   cc    ZeroCopyRasterBufferProvider(is_software=true)
#         "Software compositor always uses BGRA 8888 format for tiles"
#   viz   SoftwareRenderer + DisplayResourceProviderSoftware
#         SoftwareOutputSurface -> SoftwareOutputDeviceOzone
#   ui    ui::SurfaceOzoneCanvas, which is an SkCanvas the PLATFORM provides
#
# That last line is the seam this OS fills, and it is the same shape NetSurf's
# display port was: one class handing back a canvas. BGRA_8888 premultiplied
# is byte-for-byte this compositor's own word layout, which is the third time
# that has paid - libnsfb in M113, Skia in M157, cc's tiles here.
#
# What it costs is WebGL and WebGPU, and that is a truthful unsupported rather
# than a stub that returns a context and paints nothing - M65's rule. The
# condition for revisiting SwiftShader is named rather than left as a mood:
# a page this machine must render that requires WebGL, or real GPU hardware
# with a driver. Neither is true, and SwiftShader would cost a SECOND runtime
# code generator to port after V8 - it vendors llvm-10.0, llvm-16.0 and
# subzero as Reactor's backends - to feed a compositor that is software
# anyway.
enable_vulkan = false
enable_swiftshader = false
enable_swiftshader_vulkan = false
use_dawn = false

# ANGLE's own two, which are not the same switches as Chromium's above and
# which is why M158's four did not remove SwiftShader from the graph.
#
# angle_build_vulkan_system_info is "may ANGLE ask Vulkan which GPU is here",
# and its default is angle_has_build - so it is on in every Chromium build,
# on a machine with no Vulkan to ask. It is the ONLY reason SwiftShader is in
# this build's dependency graph at all: angle_gpu_info_util deps on
# angle_vulkan_icd, which DATA-deps on swiftshader_libvulkan, which is the
# subzero JIT - a second run-time code generator to port after V8, to answer
# a question about hardware this machine does not have.
#
# angle_enable_vulkan is ANGLE's Vulkan BACK END, whose default on this
# platform is the same is_linux that M157 and M158 wrote down twice. It is
# the second route to the same ICD, through libANGLE.
#
# Together they take SwiftShader, the Vulkan loader, the ICD, SPIRV-Tools,
# Marl and Reactor out of the graph - 55 + 15 objects M158 measured and all
# of subzero - and leave ANGLE's system-information reader, which is the
# thing this build actually wants from ANGLE.
angle_build_vulkan_system_info = false
angle_enable_vulkan = false

# And ANGLE's third, which is the one that survives the other two. The Vulkan
# LOADER - not the ICD behind it - reaches this build through //ui/gl:gl as a
# data dependency, guarded by \`use_ozone && angle_shared_libvulkan &&
# !is_chromeos\` with a comment about run-time search paths. Nothing links it;
# ninja builds it because building //ui/gl builds what //ui/gl says should
# sit beside it. It is 15 of the 23 failures a -k 0 build of this target had,
# and all 15 are its loader_platform.h not knowing this platform - a header
# whose entire job is to name the operating system it is running on.
#
# There is no Vulkan on this machine for a loader to find a driver for.
angle_shared_libvulkan = false

# xkbcommon, which is the keyboard-layout library X11 and Wayland programs
# use. Its declare_args() comment says what it is in two words - "Optional
# system library" - and its default is the same shape as every other flag in
# this block: use_ozone && (is_linux || is_chromeos), a platform-family list
# reaching this OS because is_linux is true here.
#
# There is no such library on this machine and no checkout of one: DEPS has
# no third_party/libxkbcommon at all, so what a build gets on Linux is the
# distribution's. This OS's keyboard layout is its own (kernel/drivers,
# M129's input suite), and ui/events' xkb translation would be a second
# answer to a question that already has one.
#
# It is a flag rather than a patch for M157's reason: an optional system
# library that is not there is exactly the case the flag exists for.
use_xkbcommon = false

# Pango and Cairo, which are the other optional system libraries an X11 or
# Wayland desktop has and this machine has not. Same shape as use_xkbcommon
# above and as every flag in this block:
#
#   build/config/linux/pangocairo/pangocairo.gni
#     use_pangocairo = is_linux && !is_castos
#
# What it compiles here is ui/base/ime/linux/composition_text_util_pango.cc,
# which converts a Pango attribute list into Chromium's own composition
# text - the underlines under the characters an input method has not
# committed yet. There is no Pango on this machine and DEPS has no checkout
# of one; this OS's input method, such as it is, is M85's line discipline.
use_pangocairo = false

# WebRTC. M160 turned this off and M161 turns it back on, and the reason is
# worth keeping because it is a correction rather than a change of mind.
#
# M160's argument was that WebRTC is real-time COMMUNICATION - a peer
# connection carrying live audio and video from capture devices - and that
# this machine has no camera and no audio input, so building it would be
# M65's "a feature with nothing behind it". Two things are wrong with that.
#
# The first is that it is not a pretence. A browser on a machine with no
# camera does not claim to have one: getUserMedia fails with the error the
# specification defines for exactly that case, which is what every real
# browser does on a laptop with the lid shut on its webcam. And a data
# channel needs no capture device at all - it is a peer connection carrying
# bytes, over the TCP and UDP M66 built. The media half degrades honestly and
# the rest works.
#
# The second is that the deferral was not available. Chromium has no
# supported configuration of Blink without WebRTC:
# //third_party/blink/renderer/modules lists mediastream, peerconnection,
# webrtc and breakout_box unconditionally, and blink/renderer/platform/p2p
# includes services/network/public/mojom/p2p.mojom-blink.h - which
# is_p2p_enabled is what generates. Turning it off did not remove those
# sources, it removed what they include, and a -k 0 build of //third_party/
# blink/public:blink then failed in about forty places that were all one
# missing header. A flag that removes a dependency without removing its
# users is not a configuration; it is a broken build.
#
# What M160 got right stays and is below: webnn_use_litert, which really is
# a GPU path on a machine with no GPU.

# WebNN's LiteRT back end, which is the same sentence about the same machine.
# WebNN is the Web Neural Network API and it has two back ends: TensorFlow
# Lite, which is arithmetic on the CPU and works anywhere, and LiteRT, which
# is Dawn - a GPU and NPU path. Both defaults are the platform list this port
# has now met a dozen times:
#
#   services/webnn/features.gni
#     webnn_use_litert = is_android || is_chromeos || is_linux || is_apple ...
#
# LiteRT is the ONLY thing left in Blink's dependency graph that brings Dawn's
# native library back, and SwiftShader and the Vulkan loader with it as data
# dependencies - the whole stack M158 and M159 removed, reappearing behind a
# machine-learning flag. Turning it off takes all three out and leaves the
# tflite back end, which needs no GPU and which M160's libm work is what made
# compile at all.
#
# The condition for turning it on is the one M158 set for SwiftShader and has
# not changed: real GPU hardware with a driver.
webnn_use_litert = false

# And WebNN's other back end, which is a different reason for the same
# answer. TensorFlow Lite is arithmetic on the CPU and would run here - but
# behind it are XNNPACK and ruy and behind those is cpuinfo, whose job is to
# tell them how many cores and caches this machine has. cpuinfo has three
# back ends - Linux's /sys/devices/system/cpu, FreeBSD's sysctl and Mach's
# sysctlbyname - and there is no fourth that is not a source written here.
# Its Linux one does not even compile for this target: struct
# cpuinfo_processor's linux_id member is behind #if defined(__linux__) in
# cpuinfo's own header while GN compiles the code that reads it, which is
# M144's distinction inside a third-party library.
#
# So WebNN is not available on this machine, and that is Chromium's own
# configuration for it rather than a hole: services/webnn builds with neither
# back end and reports no contexts, which is what the API is supposed to do
# where there is nothing to accelerate on.
#
# The condition for the CPU half is a cpuinfo back end for this platform -
# either /sys/devices/system/cpu, or a fourth back end upstream would take.
webnn_use_tflite = false

# WebXR, and the reason it is here at all. //device/vr on is_linux deps on
# //gpu/vulkan/init unconditionally, and that directory opens with
# assert(enable_vulkan) - so turning Vulkan off makes a directory this build
# never compiles stop the CONFIGURE. That is the hazard the paragraph above
# use_sysroot warns about, met for real.
#
# The fix is not to patch the assert. Chromium's own default says why in its
# own words:
#
#   # We enable VR on Linux even though VR features aren't usable because
#   # the binary size impact is small and allows many VR tests to run on Linux
#   enable_vr = enable_openxr || ... || (is_linux && !is_castos && ...)
#
# A feature switched on for bot coverage on a platform where it does not work,
# reaching this OS because is_linux is true. There is no headset, no GPU and
# no OpenXR runtime here.
enable_vr = false
enable_openxr = false

# Crash keys, which are the annotations a crash REPORT carries. Behind them
# is crashpad, whose Linux implementation is ptrace(2), PR_SET_DUMPABLE,
# /proc/<pid>/task, rt_tgsigqueueinfo and glibc's <features.h> - Linux's
# kernel again, and about twenty objects of it. There is no crash reporting
# service on this machine for a report to reach, so an annotation system
# with nothing behind it would be the same pretence M65 refused.
#
# Chromium already has the configuration for a platform in that position and
# it is one declare_args() away: use_crash_key_stubs turns crash_key.cc into
# crash_key_stubs.cc and drops the crashpad dependency entirely. Its default
# is is_fuchsia, and use_crashpad_annotation's is "(is_linux && !is_castos)",
# which is how this OS was reaching for crashpad's Linux half at all.
use_crash_key_stubs = true

# The vulnerability reward program's flags, and the only reason //sandbox is
# anywhere near the display compositor.
#
# M162 measured //components/viz/service and found it 22 targets wider than
# Blink's graph, of which nine are //sandbox - //sandbox/linux:seccomp_bpf
# among them, on a machine that has no seccomp and whose answer to "what may
# this process do" is a capability set assigned at spawn. That looked like
# the second of CLAUDE.md's two remaining conditions arriving five rungs
# early. It is not. `gn path --all` finds exactly ONE route:
#
#   //components/viz/service:service -> //components/vrp_flags:vrp_flags
#     -> //sandbox/policy:policy -> //sandbox/linux:seccomp_bpf
#
# and //components/vrp_flags is not a security feature. Its own README says
# what it is: "a target for controlled read and controlled write that can be
# sent to a renderer when the --vrp-flags argument is provided" - a memory
# read/write primitive built into the browser on purpose, so that reward
# program researchers start an exploit from a known position rather than
# from scratch. Its declare_args() default is the platform-family list this
# port has now met a dozen times, (is_win || is_mac || is_linux) with a
# 64-bit cpu, so it reached this OS the same way every other one did.
#
# Turning it off is not a build fix and would be right even if the sandbox
# behind it compiled perfectly. A machine whose whole security story is a
# capability set does not ship a browser with a deliberate read/write
# primitive in it behind a command-line switch.
#
# What this does NOT do is answer the sandbox question, and the condition
# for that is worth naming rather than leaving as a mood: it becomes real
# when //content launches a renderer, because that is the first process on
# this machine that will run somebody else's code and should hold strictly
# less authority than the thing that spawned it. The mechanism for that is
# already here - M65's capability set, assigned from the name a program was
# spawned by and able only to shrink - and Chromium's own seccomp is not.
enable_vrp_flags = false
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
