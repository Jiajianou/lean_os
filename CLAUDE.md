# lean_os

A desktop operating system for x86-64 written from scratch: bootloader, kernel,
drivers, filesystem, window system, applications. No GRUB, no libc, no
third-party code anywhere in the OS itself.

@README.md

## The goal

A Unix-interface-compatible OS that is stable enough and complete enough to
build and run somebody else's software against, on a machine that manages its
own CPU, memory and disk honestly. The desktop should take the best of macOS,
Windows, Linux and the web rather than imitating any one of them.

Most of that goal is built: gcc bootstrapped here (M98), Python built
here (M99), and a browser — **NetSurf, which landed 2026-09-10** and is
`/bin/netsurf`: HTML, CSS, a DOM, JavaScript and https, none of it
written here and none of it edited.

**A browser of Chromium's kind is still not next, and now there are
numbers rather than an opinion.** The same milestone measured what its
build asks of this machine — 100 GB of checkout, 535 repositories, clang
and libc++ only, and a seccomp sandbox naming 427 syscalls against this
kernel's 79 — and reduced the whole question to five conditions, of which
the first was `AF_UNIX` with `SCM_RIGHTS`: without it there is no Mojo and
so no Chromium at all. **M118 built that one**,
and in doing so corrected the framing: the same feature is in front of
WebKit, Gecko and Ladybird, none of which has a single-process mode
either. **M119 then built the second**: `epoll`,
`eventfd` and `timerfd`, which is `base`'s whole message pump — and which
gave this kernel write-readiness it had never had, so `EPOLLOUT` tells the
truth where `<poll.h>`'s POLLOUT still says "anything open". And **M120 built `memfd_create`** — shared memory a descriptor
names, passed over an M118 channel — so the three
pieces a multi-process program is made of are all here. **And M121 built
the third**: `x86_64-lean_os-clang`, `clang++` and libc++ — thirteen
anchored edits and three files, of
which eight edits are the compiler (against M94's nine for GCC) and five
are libc++, with one program built from *two* compilers as the check
that they agree about this target's ABI. It also found **27 missing
functions** in this libc (plus a `struct tm` declaration and six C99
`lconv` fields), which is the half that cost the milestone.

**M135 and M136 then found a condition that list never named, because
it was not true when the list was written: Rust.** 138
`rust_static_library` declarations across 78 of Chromium's `BUILD.gn`
files, 317 `cargo_crate`/`rust_target` invocations, and `base::FilePath`
partly written in it. **M137 built `core` and M138 built `std`** — a
fork of rust-src expressed as twenty-three anchored edits and five new
files, with `x86_64-lean_os` as a real `target_os` rather than a
`target_os = "linux"` that would compile and lie. What remains on the
Rust side is Chromium's own build integration (`rust_abi_target`, a
prebuilt std at the path `//build/config/rust.gni` expects, and the
`cargo_crate` templates), which is a build-system problem rather than a
language one.

**M139 built that**, and the result is `libstd_std.rlib` for
`x86_64-unknown-lean_os` out of Chromium's own ninja. It also cost the
fork its first three patches - M134 built the drift check for patches
that did not exist, and it is no longer vacuous - and measured where the
next wall is: **every Chromium Rust target depends on
`//build/rust/allocator`, which is PartitionAlloc, which is `base`.** The
two things in the way there are `ScopedClearLastError` and a missing
`link.h`, which is porting work rather than toolchain work.

**M140 through M145 walked up that ladder and `//base` runs here**:
PartitionAlloc, perfetto, abseil and ICU, the POSIX surface //base's own
sources ask for, the long double family, the Linux seams, and finally 428
objects linked into `/bin/chromiumbase`. **M148 got the layer above it**:
`//mojo` - message pipes, data pipes, shared buffers and a channel over a
socketpair, with Chromium's own `MessagePumpEpoll` on M119's epoll, ipcz
under the pipes and M120's memfd under the buffers. The one thing it
asked this kernel for was `/proc/<pid>/fd/<n>`, because a memfd has no
other name and a process that wants to hand out memory nobody can change
needs a second descriptor for the same pages with less access.

**M149 made it two processes**, which is the one thing a multi-process
design is and everything before it was not: `base::LaunchProcess` spawns
a second copy of the test program, the two nodes meet over a socketpair,
and a **mojom interface** - Chromium's own generator, a `mojo::Remote`
and a `mojo::Receiver` - carries a call and an asynchronous reply with a
read-only buffer in it. It needed no kernel change at all.

**M150 got //url and //net**, which is what a browser is before it is
anything else. Three more patches, all of them the same seam: inotify and
rtnetlink are system calls rather than a family, so `ProxyConfigServiceLinux`
and `AddressTrackerLinux` are gated on `has_linux_kernel` and //net's own
fallbacks - a direct proxy configuration, `getifaddrs(3)` for the interface
list - are what this platform gets. The libc grew the surface a network
stack asks for: `<resolv.h>` over M114's resolver, `<ifaddrs.h>`,
`<uchar.h>`, `<alloca.h>` from `<stdlib.h>`, a hundred socket option
numbers and an empty `libresolv.a`.

**M151 opened a connection**: two of net's own sockets over this kernel's
loopback, and a `net::URLRequest` fetching a page through the whole HTTP
stack. It cost four things - a `CAP_NETWORK` grant (the capability model
refusing `socket(2)` was the first wall), a truthful errno for that
refusal, `SYS_sockname` so `getsockname(2)` reports the port a bind to
zero was given, and `/proc/cpuinfo` so `sysconf(_SC_NPROCESSORS_ONLN)`
stops answering -1, which is the value a sandbox produces and which
Chromium reaches a `NOTREACHED` on.

**M152 got https**: BoringSSL builds for this target, assembly included,
and the test program generates a P-256 certificate, serves TLS with it and
fetches the page back through Chromium's own stack - TLS 1.3, CHACHA20,
and the certificate verified against a trust anchor added through
`CertVerifierWithUpdatableProc`. It needed no kernel change and no patch.

**M153 measured the next rung and M154 removed it.** The wall was
**bindgen**: it parses headers with libclang rather than compiling them,
builds its command line out of `{{cflags}}` alone, and so never saw the
`--target` and `--sysroot` this fork kept in `gcc_toolchain`'s
`extra_cflags` - which is the TOOL COMMAND STRING. M154's patch 0024 puts
them in a config (`//build/config:default_toolchain_flags`, last in
`default_compiler_configs` so the triple wins), and patch 0025 makes the
resource directory bindgen parses with an argument rather than
`clang_base_path`. `tools/chromium-bindgen-test.py` grades the result
against `x86_64-lean_os-gcc` over 92 sizes, alignments, offsets and
constants.

Two things M154 learned that will cost an afternoon each if forgotten.
**`toolchain_args` is ignored for the DEFAULT toolchain** - GN says so and
`custom_toolchain` makes `//lean_os/toolchain:x64` the default, so every
line in that block has always been decoration; `sysroot` is still `""` in
this build, which is why `gn path` and `gn ls` fail in `//build/modules`.
And **two different clang resource directories in one include path are
fatal**: `stdint.h` ends in `__has_include_next(<stdint.h>)`, so each
includes the other, the first one's include guard swallows the second, and
nothing declares `uint8_t` - in a header that is not wrong about anything.

**M155 built V8 and M156 finished running it.** It is the rung Blink cannot
be reached without and the first piece of Chromium here that writes machine
code at run time rather than at build time. It builds, links as a 51 MB
static EXEC with its snapshot inside, and `/bin/chromiumv8` is on the image
with `[m156]` grading **seven** checks on the machine: a platform, an isolate
and a context; JavaScript interpreted; a hot loop compiled to x86-64 and run
out of pages this kernel made executable; the regular expression engine; a
thrown `Error` reaching `v8::TryCatch` with `-fno-exceptions`; JavaScript
calling C++ through a `v8::FunctionTemplate`; and a garbage collector that
grew a heap of 200,000 objects and gave it back.

What stopped M155 was **this kernel's mmap region table**, a fixed array of
128 entries inside `task_t`. PartitionAlloc reserves one large range and then
commits, decommits and protects sub-ranges of it, so the count is not "how
many things has this program mapped" but "how many answers does one range
have". **M156 made it a heap allocation that doubles** from 32 to a ceiling
of 65536 - Linux's own neighbourhood, its `vm.max_map_count` default being
65530 - with every loop over it bounded by `mmap_capacity` rather than by the
constant. The invariant to not break is that the array is terminated by a
zero `pages`, so `scheduler_regions_reserve` returns 0 only when
`used + 1 < capacity`; `tests/test_mmap_regions.c` grades that directly and
`/bin/vmtest` splits one mapping into 512 regions on the machine.

The porting half cost nine patches, all of them the same distinction M144
drew - `v8config.h` had never heard of this platform, `mremap`,
`prctl`, `__NR_gettid`, `MAP_NORESERVE`, `MADV_DODUMP` and PKU are Linux's
KERNEL rather than the Linux family, and `<linux/auxvec.h>` was included by
files that use nothing from it. The tenth is different and is the sharpest
thing the milestone found: a **weak undefined TLS init function**, branched
to absolute zero, which fits in a PC-relative 32-bit field only if the image
is near zero. On Linux it always is; this OS loads programs at 512 GiB, so
the link failed on a function nobody calls. `constinit` on the declaration
says what was already true and the reference disappears.

**And V8 found four things wrong with this machine, all of them fixed here.**
Two are in the kernel and the C library rather than in the port:
`mprotect` refused to work on a page of a program's **own image**, which is
where V8 puts its read-only heap and which POSIX has always allowed - the
kernel checked only the mmap region, and `/bin/vmtest` grades the image case
now, including that a write to a page made read-only still faults. And
nothing could say where the **main thread's stack** is: `pthread_getattr_np`
knew only about threads this library created, so V8's `IsOnCentralStack`
check had no answer. `OS_MAIN_STACK_TOP` and `OS_MAIN_STACK_MAX_BYTES` are in
`system_api/include/process.h` now and the kernel derives its own names from
them, so the two cannot drift; `/bin/posixtest` required the answer to exist
rather than tolerating its absence, which is what had hidden it.

The other two are in this libc. `math_errhandling` and its two
constants did not exist, so llvm-libc's own ieee754 could not ask whether to
report an error at all; `<math.h>` says `MATH_ERREXCEPT` now and the error
paths **raise it**, graded by 28 error cases in `tools/math-test.sh` - 26
against the host's libm and 2 against C99 where the host is lax about
`fmod`. And the C99 float family was 16 functions of about 40: `truncf` and
`nearbyintf` are what V8 asked for, and the milestone added the other 37
plus twelve missing double ones, taking that harness from 56 graded
functions to 94.

**M157 got Skia, and the surface was small.** A `-k 0` build of `//skia`
failed in exactly two directories - 58 objects in fontconfig and 20 in Dawn -
against `//services/network`'s 1,276, because nothing here drags in the
graphics stack. Dawn went away with `skia_use_dawn = false` in args.gn and no
patch: that flag's `declare_args()` default is a list of platforms the Skia
team has *verified renders to a screen*, and it reached this OS only because
`is_linux` is true here. There is no GL, no Vulkan and no display a GPU
process could talk to. `/bin/chromiumskia` is graded by `[m157]` on eleven
**pixel values** - exact rectangle edges, a blend that lands on 0x80, an
anti-aliased circle with 142 partly-covered edge pixels, an even-odd hole, a
clip, a matrix, and a PNG round trip in which all 4,096 pixels come back
unchanged.

Three things it taught, all worth keeping. **A configure result checked into
a tree is a claim about the machine that ran configure**: fontconfig's
`meson-config.h` says "Autogenerated ... do not edit" and asserts
`HAVE_GNU_STRERROR_R`, `HAVE_RANDOM_R` and `HAVE_FSTATFS`, which are glibc and
Linux link tests nobody ran for this target - patch 0034 makes them
conditional and names this OS nowhere. **A `libs` entry is not evidence**:
`third_party/fontconfig/BUILD.gn` writes `if (!is_win) { libs = ["uuid"] }`,
and `nm -u` over all 58 fontconfig objects finds no `uuid_*` symbol at all, so
the sysroot's fifth empty archive is empty because nothing wants anything from
it. And **the Rust work was already done**: behind fontconfig sits Chromium's
fontations font backend and behind that the crates.io `libc` crate, which
Chromium vendors at **the same 0.2.189 rust-src does** - so M138's 948-line
`tools/rust-port/libc/lean_os/` is applied to both copies by `apply.py --libc`
rather than duplicated into a patch, and `chromium-test.sh` compares them byte
for byte. Chromium's Rust rules also refuse a source the GN target does not
list, so `apply.py` edits that `BUILD.gn` too.

The libc grew `<libintl.h>` (returning the msgid is what gettext(3) specifies
when no catalog exists, which here is always) and the 4.2BSD `timeval` macros.

**M159 answered the question M158 left open, and the answer was smaller
than the question.** M158 said gpu/config has no `gpu_info_collector` for a
platform with no system GPU information and that writing one would be
inventing a source. It does not need one: `gpu_info_collector_linux.cc`
compiles unchanged, and underneath it is **ANGLE's own `gpu_info_util`**,
whose first branch with no libpci, no X11 and no Vulkan is "no PCI devices
found" - it returns false with an empty GPUInfo, which is the truth about
this machine through a path ANGLE already ships.

**`//gpu/config` is the gate to everything above `//net`, which does not
sound like it.** `gn path` says //cc reaches it through
`//components/viz/common`, //media reaches it, //services/network's mojom
reaches it, and so does `//third_party/blink/renderer/platform/wtf` -
Blink's string library - through `//third_party/blink/public/common:headers`.
It drags //ui/base, //ui/ozone and //ui/gl in with it. A `-k 0` build of it
fails in **23** places against M158's 546 for //cc, and all 23 are four
things: ANGLE's own Vulkan switches, an optional system library, M144's
sentence in two more files, and two missing libc functions.

**ANGLE has its own flags and they are not Chromium's.** M158 turned off
`enable_vulkan`, `enable_swiftshader`, `enable_swiftshader_vulkan` and
`use_dawn`, and SwiftShader stayed in the graph anyway. Three more do it:
`angle_build_vulkan_system_info` (default `angle_has_build`, so ON in every
Chromium build - this is the one that pulls `angle_vulkan_icd`, which
*data*-deps SwiftShader, which is subzero, a second run-time code generator
to port after V8), `angle_enable_vulkan` (ANGLE's Vulkan back end, the second
route to the same ICD through libANGLE), and `angle_shared_libvulkan` (the
Vulkan **loader**, which //ui/gl data-deps so it sits beside the binary -
nothing links it and ninja builds it). `use_xkbcommon` is the fourth and its
own declare_args comment says what it is in two words: "Optional system
library."

**ANGLE's GL implementation is built and is in no binary**, and that
distinction is the milestone. `//ui/gl/init` names `libGLESv2_thin_static`
when `use_static_angle` is true, so libANGLE and the GLSL translator compile;
nothing calls `gl::init::InitializeGLOneOff`, so the linker takes nothing out
of those archives. `nm -C` on `/bin/chromiumgpu` finds `angle::GetSystemInfo`
and no `egl::Display`, no `sh::TCompiler`, no `gl::Context::`, no
`gl::init::`. Grade the program, not the build.

**And "blocklisted" is not "absent".** `ComputeGpuFeatureInfo` - the software
rendering list applied to the collected GPUInfo - **enables** WebGL, WebGL2,
accelerated GL, the 2D canvas and video decode and encode here, and that is
the rule list being right: `software_rendering_list.json` lists hardware
known to be BROKEN, and hardware that is not there matches none of it.
`ComputeGpuFeatureInfoWithNoGpu()` is the function that answers the other
question, and it turns all thirteen off while leaving the 2D canvas at
`kGpuFeatureStatusSoftware` - not "no drawing" but "drawing on the CPU",
which is M158's whole decision in one enum value. A port that ran only the
rule list would conclude this machine can do WebGL. `/bin/chromiumgpu` grades
both directions.

**M160 got `//cc` - the compositor - and the measurement M159 asked for was
worth making.** A `-k 0` build of it failed in **68** places against M158's
546, and the composition was entirely different: SwiftShader, crashpad and
libdrm were gone, and what remained was reached from the COMPOSITOR through
`//components/metrics/dwa` -> `//services/network/public/cpp:cpp_p2p` ->
`webrtc_overrides` -> WebRTC -> XNNPACK and tflite. Re-measure before
believing a number is not advice, it is the milestone.

**47 of the 68 were one missing declaration.** Eigen's
`SpecialFunctionsImpl.h` calls `lgammaf`, tflite includes it, and
`user_space/libc/src/math.c` had said since M99 that `lgamma` and `tgamma`
were "the one gap in C99's set this library has ... nothing in this tree has
asked for them". The condition that note named was met, so they are built:
Lanczos g=7 with nine coefficients, Euler's reflection for the left half
plane, and `G(x) = G(x+1)/x` near zero - which is the only form that survives
a subnormal, and is what gives `lgamma(DBL_TRUE_MIN)` 744.44 instead of an
infinity out of a sine that underflowed. `lgammal`/`tgammal` are still absent
and their condition is unchanged: a program on this machine that calls one.

**lgamma's relative error peaks at its ZEROS and that is not a bug.** There
is one at x=1, one at x=2 and one between every pair of negative poles, and
there the reflection and the Lanczos term cancel two values near 1.15 down to
0.0017. Measured: 3.54e-15 away from them, 6.71e-13 at the worst, on an
ABSOLUTE error of 1.1e-15. No formula avoids it - only more mantissa does -
and the long double this code computes in is 80-bit on the target (and on
an x86_64 Mac) but 64-bit on an arm64 Mac, which is M142's limitation about
that harness met a second time, on the hosts where it applies.
`tests/math/cases.tsv` says 1e-12 with all of that written beside it.

**WebRTC is off, and NOT because it does not build.** 761 of its 762 targets
and all 579 of XNNPACK's compiled for this target without a patch, which is
the interesting half of the measurement. `is_p2p_enabled = false` is a
decision about the MACHINE: WebRTC is real-time communication, this machine
has no camera and no audio input, and a peer connection with no media to put
in it is M65's rule. The condition for turning it back on is audio capture or
a camera. It took 1,392 targets out of the graph, cpuinfo among them - whose
Linux, FreeBSD and Mach backends have no fourth option that is not an
invented source.

**And a feature is only off when the LAST site turning it on is.** Patch 0041
gates `use_network_interface_change_listener` on `has_linux_kernel`, and the
first attempt gated one of the two places `services/network/public/mojom/`
enables it. The mojom parser was still handed the flag by the other and
stopped on a struct that same feature gates. That is M159's libsync edge
(patch 0040, two directories) in a different shape, and it cost a build both
times. Grep for every site before believing one edit removed something.

**What //cc asked this libc for**, besides lgamma: `rand_r` (libwebm),
`mlock`/`munlock` and `flock` (tflite). Two of those are answers rather than
implementations and the difference is the point - `flock` REFUSES with
ENOSYS, because there is no lock table in this kernel and an flock returning
0 would claim exclusive access to a file anybody can open; `mlock` SUCCEEDS,
because nothing here evicts, but it touches every page first, since this
kernel faults lazily and a bare `return 0` would promise residency for memory
that is not there.

**mlock is also how a kernel bug of unknown age was found.** `mincore` read
the page table, so it answered "not resident" for a range nobody had mapped -
and a page inside a live mapping that has never been touched is absent from
the page table for exactly the same reason. Two different answers collapsed
into one. mincore(2) says a range containing unmapped pages is ENOMEM, and
that half is the one a caller needs: it is how you ask "may I touch this"
without finding out by faulting. mlock asked and got a page fault. It looks
the mapping up in the region table now and asks the page table only about
residency.

**M161 got Blink**, which is the engine, and the measurement that decided
which target to aim at is worth keeping. `//components/viz/service` is NOT
the next rung after //cc: it IS the GPU service - //gpu/ipc/service and
//gpu/command_buffer/service - so it wants the GL implementation compiled,
which contradicts M158 and M159. And `//third_party/blink/renderer/platform`
declares `visibility = [ "//third_party/blink/*" ]`, which is Chromium saying
the unit an embedder links is the public one. So the program links
`//third_party/blink/public:blink` - the whole renderer, visibility "*" - and
includes the renderer's own headers to reach what it grades.

A -k 0 build of that failed in **74** places, then 23, then 6, then 4, then 1,
then none.

**M160's `is_p2p_enabled = false` did not survive contact with Blink, and
M161 reverses it.** About forty of those 74 were one consequence: the flag
removed WebRTC's headers without removing the sources that include them.
//third_party/blink/renderer/modules lists mediastream, peerconnection,
webrtc and breakout_box unconditionally and blink/renderer/platform/p2p
includes p2p.mojom-blink.h, which is_p2p_enabled is what generates. There is
no supported configuration of Blink without WebRTC. The M65 argument was also
too strong: a browser on a machine with no camera does not claim to have one -
getUserMedia fails with the error the specification defines - and a data
channel needs no capture device at all. What M160 got right and what stays is
`webnn_use_litert = false`, which really is a GPU path on a machine with no
GPU, and which is the only thing that brings Dawn's native library,
SwiftShader and the Vulkan loader back into Blink's graph.

**Two libraries had already decided this platform is not one they support,
and their BUILD.gn argued with them** (patch 0046). cpuinfo's src/init.c ends
its dispatch with `cpuinfo_log_error("operating system is not supported in
cpuinfo")` and returns false - its designed answer, older than this port -
while GN compiled its Linux back end, which reads a struct member cpuinfo's
own `#if defined(__linux__)` had removed. Dawn's SystemUtils.cpp has THREE
`#error "Implement X for your platform"` chains, so patch 0043's generic
POSIX arm in platform.h was not enough: a generic arm has to exist in each,
and the include chain has to be there or the other two compile without
unistd.h. Nothing is stubbed in either; what is left is the path each library
already takes on a system it does not know.

**Blink's startup is three things and nothing says so at the call site.**
Partitions, then WTF, then Oilpan. `InitializeWtf()` calls
`AtomicString::Init()` and `InitStringStatics()`, both of which allocate, and
does NOT bring the partitions up - so the first Blink allocation went to a
null PartitionRoot and faulted at offset 0x80 of address zero.
`blink::Partitions::Initialize()` is what Chromium calls first, from
BlinkInitializer, which a program outside the renderer does not get.

**And painting is behind BlinkInitializer, which is the next rung.**
blink::GraphicsContext takes a PaintController, whose constructor makes a
PaintArtifact with MakeGarbageCollected; Oilpan's cppgc::HeapBase needs a
cppgc::Platform; Blink builds that from blink::Platform::Current(). So a
paint needs a Platform implementation, a main thread scheduler and a
v8::Platform - the renderer starting up. Both faults were found on the
machine rather than by reading, which is what booting it is for.

**The patch reset did not reach sub-repositories, and that cost three
builds.** tools/build-chromium.sh and tools/chromium-test.sh both skipped
files the top-level checkout does not track - third_party/angle, dawn,
webrtc, cpuinfo, each with its own .git - on the grounds that they belong to
somebody else. The series is applied to a CLEAN tree every time precisely so
that "is this patch already applied" needs no answer, and a sub-repository
file that never got reset makes the question come back, as "does not apply to
this checkout" on a patch that is perfectly good. `git -C` finds the
sub-repository from the file's own directory and needs no list. M134's drift
check was reporting its own bookkeeping.

**What is in front of `//services/network` is porting work, and more of
it than the name suggests.** A `-k 0` build of `//services/network:network_service`
reached 1,276 failures in ANGLE, WebRTC, Vulkan, Dawn, XNNPACK, dav1d,
SwiftShader and fontconfig - the network service's mojom dependencies drag
in the graphics stack - plus real gaps in `//base`'s own neighbourhood:
PartitionAlloc's `ScopedClearLastError`, crashpad's Linux half (`features.h`,
`sys/ptrace.h`, `linux/futex.h`), fontconfig's `libintl.h`, and third-party
copies of `build_config.h` that say "Unsupported platform". That measurement
predates M158 and M159, which between them removed the whole GPU stack from
the graph and the ANGLE failures with it - re-measure before believing the
number. None of it is a toolchain problem any more.

**M171 made Chromium the desktop's Browser.** An ozone platform of this
project's own (`third_party/chromium/lean_os/ozone`, registered by
`ozone_extra.gni`, no patch) makes a compositor window Chromium's
`PlatformWindow`, wraps the shared segment in an `SkSurface` (zero copy)
and turns the event pipe into `ui::Event`s on the UI thread. `/bin/browser`
(`user_space/binaries/browser.c`) execs `/bin/chromiumshell` with the six
switches, `dup2(1, 2)` first - **this libc's stderr is descriptor 1 and
Chromium's LOG writes to 2**, so without it a browser's last words go
nowhere. That is how the memfd table's ceiling of 32 was found: base falls
back to `/dev/shm` when `memfd_create` fails, silently in release. The table
doubles to 65,536 now (M156's shape; objects arrive a page at a time because
kmalloc never splits a fresh growth). `make sysroot` no longer rewrites every
header's mtime - four tier scripts run it and each cost the next Chromium
build an hour. Still `--single-process` (M169's measurement); popups draw
nowhere until the compositor can place a client-positioned window.

**M172 fixed the four-core fork failure M170 named**, and it was a stale
READ, not a lost write: a copy-on-write break moved a frame with `invlpg` on
one core, and sibling threads on other cores kept a read-only translation of
the old frame - reads never fault. `virtual_memory_cow_break` now reports a
moved frame and the fault path shoots down the other cores when the address
space is shared. With that, M169-M172 came off the branch onto main.

**Two conditions remain and neither is a porting problem**: a machine
with 16 GB of RAM and 100 GB of disk (which is M110's hardware, and
held), and a sandbox story that is not a pretence (which is a design
question — Chromium calls seccomp and namespaces, and this OS's answer
to "what may this process do" is a capability set assigned at spawn;
M65's rule governs what may be built there). Chromium is no closer to
building here: 100 GB of checkout and 535 sub-repositories do not fit on
a 2 GiB image, and no amount of compiler work changes that. Google
Chrome itself is not a porting problem at all: it is proprietary, so
there is no source. Re-read this section before proposing any of it.

## Non-negotiables

When doing any work, check out a branch from main, then do the work and test from there. Rebase back to the main branch when everything is good and passing tests.

Violating any of these is a bug, not a tradeoff. They are the project.

- **No third-party code ships in the OS.** No libc, no newlib, no external
  library linked into any binary in the image. `third_party/` holds source
  nobody here wrote, kept separate and unmodified — it is ported *against*
  this system, never merged into it.
- **No third-party boot code.** No GRUB, Multiboot or Limine. The bootloader
  is a hand-written PE32+ EFI application (`kernel/boot/uefi/boot.c`).
- **Freestanding C, plus NASM** where C can't reach. `<stdint.h>`/`<stddef.h>`
  are fine — compiler-provided, nothing linked. Kernel objects link with
  `x86_64-elf-ld` directly, not through `gcc`, so no crt0 can slip in.
- **x86-64, UEFI only.** The legacy BIOS path was removed in M26.
- **Plain Makefiles and hand-written linker scripts.** No CMake, Meson,
  autotools, or package manager.
- The cross-toolchain (`x86_64-elf` gcc/clang, nasm, lld, mtools, qemu) is
  dev-time only. None of it ends up in the image.

## Build and run

```sh
./tools/run-qemu.sh              # build everything, fetch OVMF first time, boot
./tools/run-qemu.sh --selftests  # ...and run the boot self-test battery
make run                         # boots to the desktop in ~8s
```

Ported software goes into the image in separate, one-per-image steps —
never part of `all`, because writing into a fresh image claims inodes
kernel.c's M22 self-test has opinions about:

```sh
make toybox                      # /bin/toybox and 143 command names (M89)
tools/build-packages.sh          # cross-build the .osp package repository (M111)
make packages                    # ...and write it into the image as /pkg/repo
make browser                     # Chromium's content_shell, its launcher,
                                 #   home page and fonts, into the image (M171);
                                 #   tools/build-chromium.sh content/shell:content_shell
                                 #   builds it first, once, most of an hour
make netsurf                     # NetSurf, which was the browser M113-M170
```

**Run the payload steps in one chain with `make all`, not after a
separate `make`.** `$(IMAGE)`'s recipe recreates the disk whenever the
kernel is newer, which wipes leanfs — so a `make browser-if-built`
invoked on its own after a boot can silently discard every payload
installed before it. M121 lost a whole A/B measurement to this: the
image it compared had the browser on it and no `/bin/sqlite3`.

The two compilers for this target are dev-time and neither is part of
`make`; both install into `build/toolchain` on purpose, because clang
finds `x86_64-lean_os-ld`, `crtbegin.o` and `libgcc.a` there with no
configuration:

```sh
tools/build-toolchain.sh         # x86_64-lean_os-gcc + binutils (M94, M97)
tools/build-clang.sh             # x86_64-lean_os-clang (M121) - needs the above
tools/build-libcxx.sh            # libc++ and libc++abi for the target (M121)
```

The third toolchain is Rust, and none of it is built here: `rustc`,
`cargo` and **rust-src** come from Chromium's own pinned toolchain under
`build/chromium/src/third_party/rust-toolchain`, and
`tools/rust-port/apply.py` puts this project's fork of the standard
library onto a copy of that rust-src in `build/rust-src-lean_os` (M138).
`tools/rust-test.sh` does all of it.

**A cargo build does not notice a changed port.** `-Zbuild-std`
fingerprints the source tree it was handed, so an edit under
`tools/rust-port/` that reaches `build/rust-src-lean_os` after the first
build is an edit the next `cargo build` silently does not have — it
prints "Compiling std" and links the old rlib anyway. `rust-test.sh`
keeps a fingerprint of every port file beside the copy and re-unpacks
from scratch when it moves; do not work around that by hand.

`make browser` is the whole browser: it runs `tools/build-netsurf.sh`
when the port has never been built and `tools/install-netsurf.sh` either
way. **The install half is not optional bookkeeping** — `$(IMAGE)`'s
recipe recreates the disk whenever the kernel changes, which wipes
leanfs, so `tools/run-qemu.sh` and `tools/run-tests.sh` both reinstall
the browser through `make browser-if-built` after `make all`. That is
M113, and the bug it closed was real: the input suite's
`browser_renders_a_page` was grading whatever image was lying around.

`tools/build-netsurf-host.sh` builds the *same* NetSurf for this Mac
(M117) and `tools/build-netsurf-host.sh run <url>` says whether it
renders or crashes: if the twin renders a page the guest dies on, the
bug is under the browser (M117's was `malloc(0)`), and a guest crash
now prints its backtrace on the serial log.

`tools/build-netsurf.sh` needs two host tools beyond the list above -
`bison` 3.x (macOS ships 2.3, which cannot parse one of NetSurf's
grammars) and the host's `libpng`. Both are dev-time only.

`QEMU_RES=<w>x<h>` sets the screen size from outside the image (M114),
for the same reason the browser is reinstalled from outside it: a kernel
rebuild recreates the disk and `/etc/settings.conf` goes with it. The
compiled-in default stays 1024x768 — the interactive suite's coordinates
are measured against that framebuffer.

On macOS `tools/run-qemu.sh` launches QEMU through a generated
`build/qemu-app/lean_os.app` that declares itself not high-resolution
capable (M116). QEMU 11's cocoa display otherwise draws one guest pixel
per *physical* pixel and the window opens at half size on a Retina
screen; `tools/window-test.sh` measures the window in the default tier.
`QEMU_HIDPI=1` opts out. Link lines are one short line each; `make V=1`
prints the full commands.

**A libc change does not reach a ported program until it is relinked.**
NetSurf is a 20 MB static binary with this project's libc inside it.
`make all` builds `$(LIBC_A)` and `$(NETSURF_BIN)` depends on it, so
that happens on its own now; do not run `make sysroot` to force it,
because its recipe begins `rm -rf $(SYSROOT)` and would delete the
fifteen third-party libraries installed there.

`QEMU_MEM=128` and `QEMU_DISK=ide|ahci|nvme|virtio` are configurations
the harnesses are expected to pass — supported paths, not fallbacks. The
image is byte-identical across all four disks; the kernel picks a backend
by probing (`kernel/drivers/blk.c`, M107) and the self-test switch comes
from outside the image via fw_cfg, not a `#ifdef` (see
`kernel/dev/fwcfg.h`). The boot battery is expected green on each of the
four, and `[m107]` names the one it got so the four passes are
distinguishable in a log.

**Both kinds of Mac are meant to build this and run every harness, and
they are not the same machine underneath.** On an x86_64 Mac, QEMU can
run the guest on the host's own CPU: put
`-accel hvf -cpu host,xlevel=0x80000008` after a harness's ceiling
(`tools/qemu-serial-test.sh 1500 -accel hvf -cpu host,xlevel=0x80000008`)
and a battery that takes minutes under TCG takes a fraction of that. The
`xlevel` is for QEMU, not the kernel: under hvf a bare `-cpu host`
reports a highest extended CPUID leaf below 0x80000004, so `[inventory]`
finds no brand string where the laptop has one. It
is also a different CPU in the one place this project measures
arithmetic: under hvf the guest's x87 is real silicon, and under TCG it
is QEMU's softfloat, which rounds FYL2X correctly where Intel's does
not - `log2l`'s row in `tests/math/long_double_cases.tsv` claimed half an
ulp because softfloat was the only x87 that had ever answered it. A result
that differs between the two is a fact about one of them, not noise. An
arm64 Mac has no such option - its hvf runs arm64 guests - so TCG is the
accelerator there, and TCG is what `tests/budgets.tsv` was measured
under. Two harnesses pick hvf by themselves where they can:
`tools/memory-size-test.sh`, because what it grades is a number and not
a time, and `tools/nic-stream-test.sh`, because the freeze it exists to
catch - QEMU on macOS stopping the guest to fork SLIRP's guestfwd command,
about 4.3 s per GiB touched - only happens under hvf, and its budget row
holds under either.

`QEMU_CPUS` and `QEMU_MEM`, given per command, are how to boot a
laptop-sized machine: `QEMU_CPUS=8 QEMU_MEM=16384 tools/qemu-serial-test.sh
1500 -accel hvf -cpu host,xlevel=0x80000008` is the ThinkPad's shape. The harnesses'
defaults stay what they are - one core and a few GiB are what the budgets
and the battery's timings were measured on, and a default that followed
the host would make two runs of one commit different machines.

`LEANOS_QEMU_INPUT=usb tools/qemu-input-test.sh` runs the **whole** input
suite on a machine with no PS/2 controller at all (`-machine
pc,i8042=off`) and a USB keyboard and mouse on xHCI instead. Not one test
changes, because `kernel/drivers/xhci.c` delivers through the same
`keyboard_inject`/`mouse_inject` ring buffers the PS/2 handlers feed.

## Tests

Three tiers, each a superset of the one above:

```sh
./tools/run-tests.sh --fast   # host stages only, no QEMU, a few minutes
./tools/run-tests.sh          # + graded boot and the quick input subset
./tools/run-tests.sh --full   # + the whole input suite and slow host tests
```

`make test` / `make test-fast` are the same thing. Also: `make fuzz-run`
(network parsers and the mount path), `make mutate` (grades the *tests*),
`make coverage`, `tools/crash-test.sh` (SIGKILL mid-write, reboot, verify with
an independent reader), `tools/math-test.sh` (M99: this libm beside the
host's in one process, every declared function graded or explicitly not -
it is in `--fast`), `tools/python-test.sh` (M99: CPython's own regression
suite, on the machine, reporting its own counts), `tools/sh-test.sh` (M86: builds `/bin/sh` for the host
and requires every fixture in `tests/sh/` to agree with the host's own `/bin/sh`
byte for byte — it is in `--fast`), `tools/pkg-test.sh` (M111: this
project's SHA-256 against the host's `shasum -a 256` over 250 real files,
plus a package round trip decided by `cmp` — it is in `--fast`), `tools/iconv-test.sh` (M100: this libc's charset
conversion against the host's own over 2.58 million conversions — it is
in `--fast`, and the generated table is regenerated and compared on
every run so a hand edit to a generated file is a failure),
`tools/rust-test.sh` (M137, M138: the Rust port — the target
specification loads, `core` and then `std` build for `x86_64-lean_os`,
the two generated tables are regenerated and compared, and the two
programs it installs are graded by `[m137]` and `[m138]` **on the
machine**, because a wrong value builds and links perfectly well),
`tools/uchar-test.sh` (M150: this libc's char16_t and char32_t
conversions against **Python's own encoders**, over every code point
Unicode has - 1,112,064 of them, a million of which are a surrogate pair,
and the test fails if it ever sees fewer than that because a run without
pairs passes with the pairing broken - it is in `--fast`),
`tools/chromium-bindgen-test.py` (M154: what bindgen believes this
target's types are, against `x86_64-lean_os-gcc` folding the same headers
at compile time - 92 sizes, alignments, field offsets and constants, with
the flags read out of the configured build's own `args.gn` rather than
copied here. Its last two checks grade the instrument: the pre-M154 flags
have to produce a different answer, and so does a build that does not
define `__lean_os__` - it is in `--fast`),
`tools/chromium-test.sh` (M134 onward: the Chromium fork - the
checkout is at its pinned revision, all forty-six patches still fit it, and
what a build produced is x86-64 ELF, up to and including **//base's own
428 objects** since M144 and the **//mojo** program since M148. It grades
artefacts rather than running a build, because a Chromium compile is
minutes and the battery is already 400 seconds),
`tools/math-long-double-test.sh` (M142: the long double
family, whose answers come from **MPFR through GCC** - every expected
value is a `__builtin_<name>l()` call folded at compile time at this
target's 64-bit mantissa, and the test requires the generated object to
reference no symbol at all, because a call left behind would be the
library under test answering its own exam. There is no host oracle for
this one that works on both Macs: on an arm64 one `long double` IS
`double`, and an oracle only an x86_64 one has would make the verdict
depend on which Mac ran it. The comparison runs on the machine as
`/bin/mathltest`),
`tools/cross-arch-test.sh` (the host-compiled test code - make's
unit-test binaries, make's host tools, and every compile the differential
tests above make - built and LINKED for the other Mac architecture as
well, arm64 on an Intel Mac and x86_64 on Apple Silicon, out of the
Makefile's own source lists and the scripts' own command lines. The host
tests had only ever been built on arm64 until an Intel Mac ran them, and
what that found had been invisible from the other desk; this catches the
part of such a difference that is a compile or a link. It runs nothing it
builds, so a difference that only shows at run time - x86 inline assembly
that only an x86 host compiles into a host test, which links cleanly and
faults - is not its to see. It is in `--fast`),
`tools/memory-size-test.sh` (the memory `[inventory]` reports, held to
the `-m` QEMU was given at 4 GiB and 16 GiB: never more, and at most
64 MiB less. Until it, the machine reported the span of its frame
bitmap - the highest usable address - so a 16 GiB guest said 17408 MiB
and the 16 GB ThinkPad was written down as 18. It is in the default
tier),
`tools/clang-test.sh` (M121: the clang port, graded in four ways — the
preprocessor's macros, `clang -###` against every decision in
`tools/clang-port/LeanOS.cpp`, a compile with nothing on the line, and
**one program linked from a clang object and a gcc object** calling each
other across twelve ABI shapes in both directions), and
`tools/image-tree-test.sh` (M93: a host tool builds
a tree into the image, the host compares it to the directory it came from, and
the machine walks and hashes it back from inside — `--no-boot` for the host
half alone).

**A performance budget measured on a busy host is not a measurement.**
M121 lost two hours to a `sqlite_fixture_ms` "regression" that was two
things on the desk: VS Code's `cpptools` indexing the 120,000-file LLVM
tree the milestone had just unpacked into `build/` at 98% of a core, and
a second session running this same 400-second disk-heavy battery from
another worktree. The instrument that settles this kind of question is a
metric in the *same* run that the diff provably cannot touch:
`disk_1mib_write_through_us` is a raw `blk_write` to a fixed LBA, and it
swung 53,196 -> 126,406 us at one commit. Before believing a number, run
`ps -Ao pid,ppid,command | grep qemu-system` and read the **parents** -
the count alone does not say whose VM it is - and `ps -Ao %cpu,comm -r |
head`. Two worktrees on this machine is normal; two VMs during a timing
run is not.

**And never `pkill -f` a pattern that can match more than your own
process.** `pkill -f "qemu-system-x86_64.*OVMF"` in an A/B loop matched a
sibling worktree's VM and the loop's own command line. Kill by PID read
from `ps`, with the parent checked.

**A test must leave the tree exactly as it found it** - timestamps
included. `tools/tree-stamps.py` runs around every stage of
`run-tests.sh` and fails one that changes a tracked file. `git status`
cannot see this: M116 found `iconv-test.sh` regenerating a checked-in
file in place, which made the next `make` relink every program, print
135 KB of link lines and recreate the disk image after every test run.

**No instrument here subsumes another.** Host tests reach error paths a booted
machine cannot; the boot self-tests prove subsystems from the inside; the input
suite grades real framebuffer pixels because that's the path a person's hands
take (learned the hard way in M40). Booting the machine is not the same thing
as testing it.

**Since M159, `chromium-test.sh` also grades the GPU configuration**, and
the absences it checks are **namespaces rather than words** on purpose:
`nm -C | grep SwiftShader` finds three symbols in a binary with no
SwiftShader in it - two are //ui/gl's table of implementation NAMES and the
third is `features::IsSwiftShaderAllowed`, the predicate that answers no. A
check written against the word fails on a build that is right. What the
absence is asked of is the BINARY rather than args.gn, which is the stronger
claim: `gl::init::` being absent says nothing ever initialises a GL context,
where a flag only says what was configured.

There is no CI. Every tier is run by hand, on this machine, before a commit -
which is the arrangement the first ninety-three milestones were graded under.
Nothing in this tree is allowed to depend on a hosted runner.

## The loop

Work is **milestone-driven**: one milestone at a time, taken to completion
before the next one starts. Don't run ahead, and don't leave partial work
spread across several milestones.

**The git history is the record.** There is no `milestones.md` and no
`docs/` - both were deleted deliberately, and nothing should recreate
them. What a milestone bought, what it cost and what it taught goes in
its **commit message**, which is why the commit messages here are long.
Before touching a subsystem, read its code and read what the history
says about it:

```sh
git log --oneline -- kernel/file_system/       # what has happened here
git log --grep='leanfs' --format='%H %s%n%b'   # and why it happened
```

Each milestone:

1. **Read the history for the thing you are about to change** before
   writing anything. It often says what an earlier attempt got wrong.
2. **Build it**, respecting the non-negotiables above.
3. **Add tests for it.** New code arrives with new tests; a milestone whose
   tests are all pre-existing isn't finished. Pick the instrument that actually
   grades the thing - host tests for error paths, a boot marker for a
   subsystem, an input test for anything a person's hands touch.
4. **Run them.** `--fast` while iterating, the default tier before committing,
   `--full` when the milestone touches input or the slow paths. `make mutate`
   over new tests whose passing could be vacuous. Nothing red gets committed.
5. **Write the commit message as the milestone entry** - what changed,
   what it cost, what went wrong and what that taught. That is part of
   the milestone, not bookkeeping after it, and it is the only place
   that record now lives.
6. **Commit** as `M<n>: <what changed>`, on `main`. The next `<n>` is one
   past the highest already in the history:

   ```sh
   git log --all --oneline | grep -oE ' M[0-9]+:' | grep -oE '[0-9]+' | sort -n | tail -1
   ```

Then start the next one.

**Decide and proceed.** Don't stop to ask. Where this file doesn't settle
a question, take the reading most consistent with it and record the
assumption in the commit message.

**Never push.** `git push` is not part of this loop, ever. Commits stay local;
what leaves the machine is the user's call.

**End every report with the commands, every time.** Whenever work is
finished that the user is to try - above all anything for the ThinkPad -
the last thing in the reply is the exact commands to run, in order, one per
line, copy-pasteable, with real values rather than placeholders (the stick
is `/dev/disk4`; never write `diskN`). Build whatever does not need the
user (`make all`, `tools/make-hardware-image.sh`) before replying, so what
is left is only what needs their hardware or their password:

```sh
diskutil list external
tools/write-usb.sh /dev/disk4
sudo tools/read-usb-log.py /dev/disk4 --last > thinkpad.log
```

`write-usb.sh` stops to ask for the device path again - say so, because a
pasted block that stopped at that prompt once booted an old stick. Say what
to do on the machine between writing and reading (what to click, what to
type), and which log lines will answer the question.

## How work is decided here

- **Measure before optimizing.** M69 set this: performance work on an
  unmeasured path doesn't get done. `syscall`/`sysret`, window scaling, SACK
  and Nagle are all deferred on exactly this ground.
- **Don't build a thing that pretends to enforce something.** M65 refused a
  permission model with no users behind it; `chmod` is a truthful failure
  rather than a no-op returning 0. A machine with one principal reports one
  principal.
- **100% line coverage is not a passing grade.** The mutation harness exists
  because the first file it examined had full coverage and a mutation score of
  zero.
- **A third-party toolkit is for applications, not for the desktop
  itself.** M125 drew that line and M127 built on it: LVGL is linked
  into user-space programs, and the compositor, window manager and
  desktop shell stay first-party because they own the framebuffer, the
  cursor and the z-order. Every LVGL application lives in **one**
  multicall binary (`user_space/binaries/desktop_applications.c`) that
  `/bin/settings` and `/bin/task_manager` are seeded as symbolic links
  to, because 600 KB of toolkit per application does not fit in a
  kernel that `incbin`s every program. Adding an application means a
  name in that table and a link in `kernel.c`'s `PROGRAM_ALIASES`, not
  a new `USER_PROGRAM`.
- **Deferrals need a condition, not a mood.** Deferring something means
  naming what would have to become true before it is built, in the commit
  that defers it. Standing deferrals: multi-user, a journalling
  filesystem, dynamic linking, USB boot. Find the condition with
  `git log --grep='deferred'` before proposing any of that work.

## Definitions of the vague words

When "stable", "secure" or "fast" need to mean something, they mean:

- **Secure** → the capability model the kernel enforces. Every process
  carries a set assigned from a manifest at spawn; it can only ever shrink. An
  ordinary app cannot paint the screen, read the clipboard, list processes,
  open a socket or power the machine off.
- **Stable** → the crash test, the mount check, and no regression in the boot
  markers.
- **Fast** → the five performance budgets graded by the boot self-tests. A
  number, or it isn't a claim.

## Where things are

```
kernel/              boot, architecture/x86_64, memory_management, scheduler,
                     drivers, file_system, inter_process_communication,
                     network, process, device, library, acpi, power, kernel.c
system_api/          the syscall ABI: numbers, structs, the kernel/user contract
user_space/library   the runtime every program links: crt0, syscalls,
                     graphics, window_manager_client
user_space/libc      a C library subset for programs written against std headers
user_space/binaries  the applications
user_space/shell     the shell
user_space/loader    the dynamic loader
third_party/         source nobody here wrote — do not modify
tools/               build scripts, QEMU harnesses, the font generator
tests/               host unit tests, fakes, fixtures, budgets, coverage floors
```

There is no `docs/` and no `milestones.md`. Design notes belong in the
commit that introduces the design; the state of the project is the tree
plus `git log`.

## Conventions

- **The commit history is the source of truth for where the project is.**
  `git log --grep='<subsystem>' --format='%H %s%n%b'` is the instrument;
  a milestone's body says what it cost and what it taught, and that last
  part is the most valuable thing in it. Write commit bodies that will
  still answer somebody's question in a year. Do not recreate
  `milestones.md` or `docs/` to hold this instead.
- Commits: `M93: leanfs v3 - a filesystem sized for a source tree`. Milestone
  number, colon, what changed - then a body with the reasoning.
- Match the surrounding code's style, comment density and idiom. Comments here
  explain *why* a constraint exists, not what the line does.
