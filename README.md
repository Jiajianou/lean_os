# lean_os

A desktop operating system for x86-64, written from scratch: bootloader,
kernel, drivers, filesystem, window system and applications. No GRUB, no
libc, no third-party code anywhere in the OS itself.

```
UEFI firmware
  -> BOOTX64.EFI            hand-written PE32+ EFI app (kernel/boot/uefi)
  -> kernel.bin             long mode, paging, physical/virtual memory, heap, SMP
  -> init (PID 1)
  -> compositor             owns the framebuffer, the cursor and the speaker
  -> desktop_icons, desktop_shell, and whatever you open
```

## What it does

- **A desktop.** Overlapping windows with a real z-order, titlebars with
  traffic-light buttons, drag and resize, snapping, minimize/restore with
  motion, a taskbar, a Spotlight-style launcher, context menus, toasts,
  drag and drop, four virtual desktops, and keyboard chords.
- **Window chrome that is not from 2005.** A 28px titlebar with its own
  vertical gradient rather than a flat fill, the title centred and bold,
  and an accent hairline along the top of the focused window - so focus
  is shown the way a modern desktop shows it rather than by painting the
  whole bar blue. The traffic lights are anti-aliased 12px circles in
  macOS order, coloured on the focused window and grey on every other
  one, with their glyphs appearing on hover. Windows are rounded on all
  four corners - the bottom two are cut out of the client's own blit,
  which the compositor can do because it already has what is behind the
  window in its back buffer - and they sit on a real drop shadow: eight
  concentric rounded strokes whose alpha falls off with distance, rather
  than one offset rectangle at a flat ratio. The launcher, the toasts
  and the window menu are on the same rounded primitives.
- **Artwork with an alpha channel.** The desktop icons are RGBA, drawn by
  a first-party vector rasteriser in `tools/gen-icons.c` rather than typed
  as hex: closed contours filled with a scanline rasteriser at four
  sub-scanlines a row, vertical gradients, and a rounded tile with a
  hairline highlight. The same description renders at 48px for the
  desktop and 24px for the taskbar, and the generator refuses a face
  whose corners are not transparent, whose centre is not opaque, whose
  edge has no partly-covered pixel, or whose two sizes disagree about the
  silhouette. The taskbar is 44px of vertical gradient with rounded
  translucent hover and focus states, an accent bar under the focused
  app, a dimmer dot under the ones merely running, and each slot showing
  the icon its window title maps to.
- **Applications.** A terminal with a real shell (arguments, quoting,
  `>`/`>>`, a pipe, tab completion, `cd`), a text editor (undo, redo,
  find, paste), a file manager (columns, sorting, exact sizes, free
  space, new file, new folder, rename, copy, and a delete that takes a
  folder with things in it - behind a confirm that counted them), a
  task manager, a settings panel, a paint toy and a clock. The task
  manager and the settings panel are drawn by a real toolkit now; the
  rest still draw themselves.
- **A system underneath.** Pre-emptive multitasking across multiple
  cores, a per-process address space, a custom filesystem with
  directories and files up to 4 GiB, pipes, shared memory, an orderly
  shutdown, and a clock that knows the date.
- **A machine, not a demo.** Physical memory sized from what the
  firmware reports rather than from a constant, an identity map built
  from the same table, and a per-process address space measured in
  hundreds of gigabytes with a real NX bit, `mprotect`, `MAP_FIXED` and
  a stack that grows when it is touched. A virtio block driver with the
  ATA one kept as the fallback, and a cache in front of both: the same
  megabyte costs 95 ms through PIO, 5.6 ms through DMA and 2.5 ms warm.
- **A terminal that is a device.** A line discipline with canonical
  and raw modes, sessions, process groups and job control - and
  **pseudo-terminals**: `/dev/ptmx`, `/dev/pts/<n>`, `openpty` and
  `forkpty`, so a program can run another program on a terminal it
  cannot tell from the console. `^C` interrupts the foreground job and
  `^Z` suspends it, with `waitpid(WUNTRACED)` reporting the stop.
- **Unix-shaped enough to build against.** An environment inherited
  across a spawn and a real working directory, so a relative path means
  something; signals a program can *catch*, delivered through a frame on
  its own stack; `mmap`/`munmap` that hands pages back and reuses the
  holes; POSIX threads sharing one address space; and `<dirent.h>`,
  `<sys/stat.h>`, `<unistd.h>`, `<signal.h>`, `<pthread.h>`,
  `<sys/mman.h>` and `<setjmp.h>` over the top of it.
- **A desktop that sleeps.** Every window used to be a process that never
  left the run queue - its event loop polled and yielded - so an idle
  desktop cost 22% of a host core under QEMU and 44% with eight windows
  open. Clients block in `SYS_waitfds` now and tell the compositor when
  they have drawn; 11% and 23%, and a click's result is composited the
  moment the client presents it.
- **A desktop that remembers.** Whatever was open when the machine
  stopped is open again where it was when it starts; an editor with
  unsaved changes can veto a shutdown; recently-opened files are in the
  launcher and the file manager.
- **A network.** An RTL8139 driver, Ethernet, ARP, IPv4, ICMP, UDP and
  **TCP** - the eleven-state machine, retransmission with a measured
  timeout and Reno congestion control - plus a DHCP client, and sockets
  programs open as ordinary file descriptors.

  **And a resolver that does not trust one server.** `/etc/resolv.conf`
  is read, every nameserver in it is asked at once alongside the one
  DHCP handed over, and the first correct answer wins - because a
  nameserver that accepts queries and answers none is a common failure,
  and a resolver with one server cannot tell that apart from a network
  that is down.
- **The devices a real machine has.** AHCI, **NVMe** and **xHCI with USB
  HID** - three controllers found by PCI *class* rather than by vendor,
  which is what lets one driver drive everybody's silicon. The block
  layer probes NVMe, AHCI, virtio and ATA in that order and nothing above
  `block_device_read` can tell which answered; `QEMU_DISK=nvme|ahci|virtio|ide`
  are four supported configurations of one byte-identical image.

  **And the keyboard is the same keyboard.** The USB driver delivers
  through the ring buffers the PS/2 IRQ handlers have always fed, so
  there is no second input path - which means the entire existing input
  suite grades it unchanged, on a machine booted with **no PS/2
  controller at all**.
- **Settings that stick.** Wallpaper, colours, **screen resolution**
  (changed live, with a countdown that puts it back if you do not
  confirm), motion, and volume.
- **A toolkit under the applications.** LVGL 9.2.2, nobody here's and
  unedited, rasterising straight into each window's own shared memory -
  anti-aliased Montserrat, a flex layout that places things rather than
  a list of hardcoded pixel coordinates, rounded cards, a volume slider
  you drag and a switch that slides. **Settings** and **Tasks** are
  built on it, and they are the *same binary*: one multicall program
  that `/bin/settings` and `/bin/task_manager` are symbolic links to,
  the way `/bin/ls` is a link to toybox - because a converted desktop
  cannot afford a 600 KB copy of the toolkit per application. **The
  capability model survives that**, and not by accident: the kernel
  assigns capabilities from the name a program was *spawned by*, so one
  binary reached through two links still gets two different sets.

  Their colours are not `#define`s any more. The desktop colour and
  accent picked in Settings derive the whole palette - window ground,
  card, border, dim text, and the ink on an accent button, which flips
  from near-white to near-black at the contrast ratio where it should.
  A host test grades all thirty-six combinations the Settings panel can
  produce against WCAG, because "is this readable" is a number.

  The compositor, the window manager and the desktop shell stay
  first-party. They own the framebuffer, the cursor and the z-order;
  the toolkit is for applications.
- **Sound.** A PC-speaker beep on errors and an AC'97 output stream.
- **A boundary around a program.** Every process carries a capability
  set the kernel assigns from a manifest at spawn time, and that set can
  only ever shrink. An ordinary application cannot paint on the screen,
  read the clipboard, list processes, open a socket or switch the machine
  off.
- **A compiler that knows this OS by name.** `x86_64-lean_os` is a real
  target triple in binutils and GCC. `x86_64-lean_os-gcc hello.c -o hello`
  produces a program this machine runs with **no flag supplied by hand** -
  the load address, code model, red zone and startup files all come from
  the target description, because every flag invented by hand is a flag
  someone else's build system will not pass.
- **Somebody else's *userland*.** Toybox, built for this machine from
  the published tarball with no edit to its source, installed as one
  static binary and 143 command names in `/bin`: `find`, `grep`, `sed`,
  `sort`, `xargs`, `tar`, `ps`, `du`, `wc` and the rest.
  `find . -type f | xargs grep -l something | sort | uniq -c | sort -rn`
  is a boot self-test - five programs nobody here wrote, four pipes, five
  forked and exec'd processes.
- **Somebody else's language.** CPython 3.12.7, cross-built with this
  project's own compiler, with its standard library on the disk as `.py`
  source rather than frozen into the binary - so `import json` is a file
  this filesystem opens. `python3 -m test` runs **CPython's own
  regression suite** here and reports its own pass and fail counts, which
  is the only instrument in this project that neither wrote its own
  assertions nor chose what to assert. It found nineteen bugs in this OS.
- **Somebody else's *libraries*.** zlib, libpng and libjpeg, built for
  this machine by this project's own compiler with **no edit to any of
  their source**, installed into the sysroot by their own `make install`
  so the next one links the last. Each is graded by its own test suite on
  the machine, and libjpeg's is the sharpest instrument in this tree: it
  ships the files the IJG's own encoder and decoder produced in **1995**,
  and its `make test` requires byte equality with them.
- **A web browser.** **NetSurf 3.11**, built for this machine by this
  project's own compiler, on the desktop as **Browser** - libhubbub
  parsing HTML5, libdom, **libcss** doing the cascade, **Duktape**
  running JavaScript, freetype rasterising the text, and **libcurl over
  mbedtls over this project's own TCP** for `http` and `https`. Fifteen
  third-party projects and **no edit to any of their source**: the
  display port is *one file*, because libnsfb registers its surfaces at
  runtime and NetSurf picks one by name, and the pixels are zero copy -
  libnsfb's XRGB8888 is byte-for-byte this compositor's own word layout.

  **It reaches the real web.** `https://www.google.com/` loads and
  renders in about six seconds, verified against certificate authorities
  that arrive as a *package*. Getting there found a bug in this project's
  `malloc` that had been present for ninety-five milestones: it returned
  8-byte-aligned memory where x86-64 requires 16, and no program had ever
  noticed because none had been built by a compiler that vectorised a
  store into the heap.

  It holds `CAP_FS_WRITE | CAP_NETWORK` and nothing else. **Not**
  `CAP_FRAMEBUFFER`: twenty megabytes of somebody else's C and C++,
  running a JavaScript engine on bytes from a machine nobody here
  controls, with no more authority over the screen than the clock has.
- **`AF_UNIX` with `SCM_RIGHTS`.** `socketpair`, names and abstract
  names, `sendmsg`/`recvmsg`, and a descriptor - a pipe end, an open
  file, a socket - crossing to another process as a reference to *the
  same kernel object*, sharing its file position. It needs **no
  capability**, and that is the point rather than an omission: a renderer
  process is the one program on this machine that must hold no
  `CAP_NETWORK` and the one that cannot work without this call.
- **A message pump.** `epoll`, `eventfd` and `timerfd` - a set of
  descriptors the kernel remembers, a counter another thread can poke to
  be heard, and a deadline that is a descriptor like any other. It also
  gave this kernel something it had never had: **write-readiness**.
  `EPOLLOUT` means a pipe with room, a socket with send-buffer space, a
  counter below saturation - where `poll` reported POLLOUT for anything
  open.
- **`memfd_create`.** Anonymous shared memory that a *descriptor* names,
  sized with `ftruncate`, mapped `MAP_SHARED`, and passed to another
  process over a Unix-domain socket. **Nobody can name it and anybody
  holding the descriptor can map it.** Sealable, so a sender can hand
  over memory the receiver can *verify* is read-only rather than trust.
- **A second compiler.** `x86_64-lean_os-clang`, `clang++`, and
  **libc++**. It links against **GCC's** libgcc rather than LLVM's own
  compiler-rt and libunwind, and that is a decision rather than a
  default: libgcc's exception machinery keeps a static registry of the
  `.eh_frame` tables it has been told about, so two unwinders in one
  program means a throw that crosses between them finds no handler and
  aborts with no message.

  The sharpest thing it proves is **one program built by two compilers**:
  one translation unit from clang and one from GCC, linked together and
  calling each other in both directions across twelve shapes the x86-64
  ABI argues about. Every archive in this sysroot was built by GCC, so
  two front ends that disagreed there would produce programs that run and
  are wrong, and no single-compiler test can see it.

- **A third language, with its standard library.** `x86_64-lean_os` is a
  Rust target, and **`std`** is built for it: `std::fs` on leanfs,
  `std::thread` on this libc's pthreads, a `Mutex` and a `Condvar` that
  block on this kernel's futex, both clocks, the environment, and a
  `HashMap` seeded from `getrandom`. The fork is a set of *anchored
  edits* to rust-src rather than a patch series - twenty-three of them,
  plus one new module in the `libc` crate and one in `std::os`. Most are
  a line adding `lean_os` to a list of operating systems.

  **It is lean_os, not Linux wearing a hat.** This `struct stat` carries
  three `timespec`s and puts `st_mode` first; `O_RDONLY` is 1; `sigset_t`
  is 32 bits. A target that claimed `target_os = "linux"` would compile
  and produce plausible wrong answers, so `/bin/ruststd` carries **two
  tables of the same 301 facts** - every struct size, alignment and field
  offset - one computed by `x86_64-lean_os-gcc` from the C headers and
  one by `rustc` from the Rust module, in one program, compared on the
  machine. Reordering two fields in the Rust `struct stat` makes the boot
  battery print which offsets disagree and panic.

  Building it found two things missing from this libc and added them:
  `fcntl(F_DUPFD_CLOEXEC)`, which `File::try_clone` needs, and
  `posix_memalign`, which an over-aligned `Box` needs.

  **And Chromium's own build system builds it.** `x86_64-unknown-lean_os`
  is a target rustc resolves by *name* rather than by a path to a JSON
  file, which needs no fork of rustc, so `//build/rust/std` compiles the
  forked standard library for this machine - 9.3 MB of `libstd_std.rlib`,
  x86-64 ELF, out of ninja. Three patches to Chromium make that work and
  the first mentions this OS nowhere: it adds a declare_arg for the case
  Chromium has no name for, an out-of-tree platform that poses as Linux
  for GN's sake while having a Rust target of its own.

- **A package manager.** `os install grep` puts **GNU grep 3.11** on this
  machine - built here by this project's own compiler from the published
  tarball with no edit to its source. A package is a manifest, a file
  table and a payload with **no install hooks at all**: installing is
  verify and copy, so what installing can do to your machine is a
  sentence rather than an audit. Commands land in `/pkg/bin` and never in
  `/bin`, so a package cannot take over the name of a program this OS
  ships.

## Build and run

Needs an `x86_64-elf` cross-toolchain, `nasm`, `clang`+`lld` (for the
EFI app), `mtools` and `qemu-system-x86_64`.

```sh
./tools/run-qemu.sh          # builds everything, fetches OVMF the first time, boots
./tools/run-qemu.sh --selftests   # ...and runs the boot self-test battery on the way
```

Three optional steps put ported software into the image, each once per
image and none of them part of `all` - see the Makefile for why writing
into a fresh image is ordered rather than automatic:

```sh
make toybox                  # /bin/toybox and 143 command names
tools/build-packages.sh      # cross-build grep and bzip2 into .osp archives
make packages                # ...and write them into the image as /pkg/repo
make browser                 # cross-build NetSurf, libcurl and 14
                             #   libraries, and write the browser in
```

The two compilers for this target are separate again, and neither is part
of `make` - both take most of an hour, once:

```sh
tools/build-toolchain.sh     # x86_64-lean_os-gcc and binutils
tools/build-clang.sh         # x86_64-lean_os-clang, into the same prefix
tools/build-libcxx.sh        # ...and libc++/libc++abi for the target
```

`QEMU_RES=1440x900 ./tools/run-qemu.sh` boots with a bigger screen. The
size lives in `/etc/settings.conf`, which a kernel rebuild deletes along
with the rest of the filesystem, so it is re-applied from outside the
image on every run - the compiled-in default stays 1024x768 because
every coordinate in the interactive suite is measured against it.

The disk image is 2 GiB and sparse - a few megabytes on disk until
something fills it. `QEMU_MEM=128` boots the same kernel on a small
machine and `QEMU_DISK=ide` runs it through the ATA driver instead of
virtio; both are configurations the harnesses are expected to pass in,
not fallbacks.

## Tests

One command, three tiers. Each is a superset of the one above it.

```sh
./tools/run-tests.sh --fast    # host unit tests. No QEMU. Under a second.
./tools/run-tests.sh           # ...plus a graded boot and the quick input subset.
./tools/run-tests.sh --full    # ...plus the whole input suite and the slow host tests.
```

`make test` and `make test-fast` are the same thing for people who type
that instead.

Several instruments, and none of them subsumes another:

- **Host unit tests** (`tests/`) compile kernel units - the kernel
  library, the heap, leanfs, every network parser - for the machine you
  are sitting at and run them under ASan and UBSan in under a second.
  They exist to reach the error paths a booted machine cannot: a full
  disk, a failed allocation, a corrupt superblock, a malformed packet. It
  also compiles the **scheduler** - whose bugs have historically
  presented as "about one boot in ten hangs" - against a fake timer and a
  fake CPU, so a tick is a function call and a fairness property can be
  checked at every task count. It also compiles the three pieces of the
  device drivers whose failure mode is a *plausible wrong answer* rather
  than a loud one - the USB boot-report decoder, the xHCI ring's cycle
  bit, and the NVMe request splitter, **which is the one path QEMU cannot
  reach at all**: its namespace has 512-byte blocks, so a 4Kn drive's
  read-modify-write path is dead code here and would first execute on
  somebody's real filesystem. 564 tests.
- **The boot self-tests** (`tools/qemu-serial-test.sh`) boot the real
  image and grade the serial log against 134 markers and 44 performance
  budgets. They prove every subsystem still works from the inside.
- **The input suite** (`tools/qemu-input-test.sh`) drives real clicks and
  keys through QEMU's monitor and grades real framebuffer pixels. It
  proves the path a person's hands take. Most of its tests check that
  something *did* change; two of them check the opposite, that nothing
  else did, which is the only way to catch a flicker.
- **Fuzzers** (`make fuzz-run`) feed the network parsers and the
  filesystem mount path arbitrary bytes, about 150,000 inputs a second.
- **A mutation harness** (`make mutate`) breaks the kernel on purpose,
  one small change at a time, and reports whether the tests noticed. It
  is the only instrument here that grades the *tests* rather than the
  machine, and the first thing it found was a file at 100% line coverage
  whose mutation score was zero.
- **Chromium's `//base` links and runs here.** 428 objects out of
  Chromium's own ninja, linked into `/bin/chromiumbase` - 2 MB, ET_EXEC,
  entry `0x8000000040`, no interpreter: a lean_os executable in exactly
  the shape every other program on this machine has, whose C++ is
  Chromium's foundation library and whose C library is this one. It takes
  a path apart with `base::FilePath`, writes and reads a file through
  `base::File` on leanfs, measures a sleep with `base::TimeTicks`, and
  reaches this libc's `getrandom` through `base::RandBytes`. Getting there took the same
  distinction three times over, and the patch that draws it the third
  time is 804 lines across nineteen files in which the word `lean_os`
  does not appear: `is_linux` answers "does the build gate the right
  things", and `has_linux_kernel` answers "is Linux's own system call
  surface there". They are not the same question, and `getdents64`,
  `clone`, `tgkill`, `exit_group`, `prctl`, `inotify`, the futex and
  `<linux/magic.h>` are all on the far side of it.
- **And `//mojo` runs here too.** The layer every multi-process piece of
  Chromium is made of: message pipes that carry bytes and each other's
  endpoints, data pipes, shared buffers, ipcz underneath them, and two
  `mojo::core::Channel`s over the two ends of one socketpair carrying a
  message and a descriptor - driven by Chromium's own
  `base::MessagePumpEpoll` on this kernel's epoll. The Linux fast path is
  *not* compiled: `ChannelLinux` is `memfd_create` through `syscall(2)`,
  `eventfd` and `futex(2)`, so what decides between it and `ChannelPosix`
  is whether Linux's kernel interfaces are there rather than which family
  the build gates this platform as.

  The one thing it asked this kernel for was **`/proc/<pid>/fd/<n>`**. A
  memfd has no name but that one, and a process that wants to hand out
  memory nobody else can change needs a second descriptor for the same
  pages with less access. Reopening a descriptor there can only ever
  shrink its rights - a read-only one cannot be mapped writable,
  truncated, or reopened for writing - which is the same rule the
  capability set follows, and the difference between a boundary and a
  claim.
- **Two processes, one mojo connection.** `/bin/chromiummojo` spawns a
  second copy of itself with `base::LaunchProcess`, hands it one end of a
  socketpair, and the two mojo nodes meet over it - and then they talk
  through a **mojom interface**, which is how Chromium describes every
  one of its own: a `.mojom` file, Chromium's own generator, a
  `mojo::Remote` here and a `mojo::Receiver` there, a call and an
  asynchronous reply delivered to a callback on a run loop. The reply
  carries a read-only shared memory region the *child* created, which
  this side unwraps - and unwrapping it read-only is base's own assertion
  that the descriptor which crossed the channel reports `O_RDONLY`. The
  answer also carries the answerer's process id, because every other part
  of the check would pass just as well if the work had quietly happened
  here.

  It needed nothing new from this kernel. `fork`, `execve`, `waitpid`,
  descriptor remapping across the exec, `SCM_RIGHTS` and memfd were all
  already here, and Chromium's launcher found them where POSIX says they
  are.
- **A network stack.** Chromium's **//url and //net** build for this
  machine and run on it: GURL's parser and canonicaliser, `net::IPAddress`,
  `net::HttpUtil`, 594 objects in `libnet.a` and 26 in `liburl.a`. What
  makes it interesting is not the C++ but what it asked a C library for -
  and got: `<resolv.h>`'s `res_ninit(3)`, so Chromium reads **this
  machine's own nameservers**, the ones M114's resolver assembles from
  `/etc/resolv.conf` and from DHCP; `getifaddrs(3)`, so
  `net::GetNetworkList` finds `eth0` without netlink; `<uchar.h>`, whose
  `char16_t` conversions are graded against Python's own encoders over
  **every code point Unicode has**; and something over a hundred socket
  option numbers.

  The Linux-only halves are not compiled and that is a decision rather than
  a gap: `ProxyConfigServiceLinux` is inotify, `AddressTrackerLinux` is
  rtnetlink, and //net's own fallbacks - a direct proxy configuration and
  the getifaddrs interface list - are what a POSIX platform without them is
  supposed to get.
- **And it opens a connection.** A `net::TCPServerSocket` and a
  `net::TCPClientSocket` meet over this kernel's loopback - a non-blocking
  `connect(2)` whose completion arrives on Chromium's own
  `MessagePumpEpoll` - and then a **`net::URLRequest` fetches a page**:
  the host resolver, the socket pool, `HttpNetworkTransaction` and the
  response parser, against a server that is eleven lines of POSIX in a
  thread of its own, so that everything being graded is on Chromium's
  side.

  The first thing that stopped it was not the kernel but **the capability
  model**: `/bin/chromiumnet` was spawned with `CAP_APP_DEFAULT`, and
  `socket(2)` refused. It holds `CAP_NETWORK` now because it was given it
  by name, which is the whole design working. The second was
  `getsockname(2)`, which used to answer out of the machine's network
  configuration with a port of zero - right about the address and a lie
  about the port, and useless to anything that binds to port zero and then
  needs to say where it is.
- **And https.** **BoringSSL** builds here, x86-64 assembly and all, and
  `/bin/chromiumnet` uses both halves of it: it generates a **P-256
  certificate** when it starts - key, serial, validity, a subjectAltName
  for `127.0.0.1`, signed with SHA-256 - serves TLS on this kernel's
  loopback with it, and then fetches `https://127.0.0.1/` through
  **Chromium's own stack**, which negotiates **TLS 1.3 with
  CHACHA20-POLY1305** and verifies the certificate against a trust anchor
  added through `CertVerifierWithUpdatableProc` - the interface a browser
  uses for enterprise roots, not a test hook.
- **A JavaScript engine, most of the way.** **V8** builds for this machine out
  of Chromium's own ninja - 51 MB, ET_EXEC, entry `0x80000b9600`, with the
  startup snapshot linked into the file rather than sitting beside it,
  because a program on this machine is one file the kernel maps. It starts
  here: a platform, an isolate and a context on this kernel's threads, and it
  **interprets JavaScript** - arithmetic, strings, arrays, JSON and a
  recursive Fibonacci.

  It does not finish. PartitionAlloc calls `mprotect` on sub-ranges of one
  large reservation often enough to exhaust this kernel's fixed 128-entry
  mmap region table, and a fixed table is the wrong shape for what a browser
  does to an address space. That is the next piece of kernel work rather than
  more porting, and until it is built there is no `/bin/chromiumv8` on the
  image and no boot marker claiming one.

  Nine of the ten patches it cost are the same sentence: **being in the Linux
  family is not having Linux's kernel.** `mremap`, `prctl`, `__NR_gettid`,
  `MAP_NORESERVE`, `MADV_DODUMP` and memory protection keys are Linux's own,
  and V8 reaches for all of them behind `V8_OS_LINUX`. The tenth is about
  this machine rather than about Linux: a **weak undefined TLS init function**
  is branched to absolute zero, which fits in a 32-bit PC-relative field only
  when the image is near zero - and this OS loads programs at 512 GiB, so a
  function nobody calls broke the link.

  **It found two holes in this libc too.** `math_errhandling` was missing, so
  the one question a caller can ask about how errors are reported had no
  answer; `<math.h>` says `MATH_ERREXCEPT` now and the error paths raise the
  flags that makes true, graded against the host's libm. And C99's float
  family was sixteen functions out of about forty - `truncf` was the one V8
  named, and the other thirty-seven came with it.

- **A libm for the format the hardware has.** `long double` on x86-64 is
  the x87's 80-bit extended type, and this libc has the C99 set for it -
  fifty-two functions, graded to the unit in the last place of a 64-bit
  mantissa. The answers come from **MPFR**, through GCC, which folds
  `__builtin_atan2l(...)` on constant arguments at compile time at the
  target's own precision; the test requires the object holding those
  12,840 values to reference no symbol at all, because a call left behind
  would be the library under test marking its own exam.

  The sharpest thing it found is about the machine rather than the
  library. `fsin`, `fcos` and `fptan` came back wrong by up to 2,500,000
  units in the last place while every other x87 instruction was within
  one - which is exactly what rounding the *argument* to a double costs.
  So the circular functions are not instructions here: they are a
  five-piece reduction of pi/2 and an eleven-term polynomial, in this
  format, and they are right wherever addition is.
- **Differential tests** (`tools/sh-test.sh`, `tools/regex-test.sh`,
  `tools/scanf-test.sh`, `tools/printf-test.sh`, `tools/math-test.sh`,
  `tools/pkg-test.sh` and `tools/iconv-test.sh`) compile this project's
  own shell, regular-expression engine, `sscanf`, `printf`, libm and
  SHA-256 from the same source the machine runs, for the machine you are
  sitting at, and require every fixture to agree with the host's own.
  Nothing in those fixtures says what the right answer is - a program
  nobody here wrote decides, which is the only useful standard for code
  whose whole job is to agree with every other implementation of itself.
  Every one of them found real bugs on its first run; the iconv one found
  a sentinel that shared a value with real data, in thirteen charsets at
  once - and, before that, taught this project that **the host is not
  always one oracle**: macOS's iconv transliterates by default, which
  made 144,589 of its first run's "disagreements" nothing of the kind.
- **An image-tree test** (`tools/image-tree-test.sh`) has a host tool
  write a directory tree into a leanfs image, compares the image with an
  independent reader against the tree it came from, then boots it and has
  the machine walk that tree and hash every byte of it against what the
  host wrote down.
- **Exhaustion** (`/bin/exhausttest`) takes descriptors, pipes,
  shared-memory segments and sockets to their ceilings and requires each
  to refuse, recover, and work again - twice over, with a leak audit
  across 2,200 rounds either side of it.
- **A fault-injected disk** (`tools/disk-fault-test.sh`) boots the
  machine with QEMU's `blkdebug` refusing **every** write, through virtio
  and through ATA, and requires it to reach PID 1 anyway.
- **A crash test** (`tools/crash-test.sh`) cuts the power mid-write with
  `SIGKILL`, reboots, and checks the filesystem with an independent
  reader. Sixteen cuts across the heaviest metadata window; the
  filesystem has survived all of them.

Booting the machine is no longer the same thing as testing it. `make run`
boots to the desktop in about eight seconds; the ~190-second self-test
battery runs only when something asks for it, which
`tools/qemu-serial-test.sh` does and `tools/run-qemu.sh` does not. The
image is identical either way - the switch comes from outside the image
via fw_cfg rather than from a `#ifdef`.

## Source conventions

The source carries **no comments**, in any language, and **no abbreviated
names**: `file_system` rather than `fs`, `virtual_memory_map_page` rather
than `vmm_map_page`. Two things keep their spelling, because both are
contracts rather than style - the C and POSIX names in
`user_space/libc/include` that ported software links against, and the
names of standards, hardware and protocols (PCI, NVMe, xHCI, ELF, TCP and
the rest). `third_party/` is exempt from all of it, being nobody here's
to edit. See [CLAUDE.md](CLAUDE.md).

## Where things are

```
kernel/          boot, architecture/x86_64, memory_management, scheduler,
                 drivers, file_system, inter_process_communication, network,
                 process, device, library, acpi, power, profile, kernel.c
system_api/      the syscall ABI: numbers, structs, the kernel/user contract
user_space/library   the runtime every program links
user_space/libc      a C library subset, for programs written against std headers
user_space/binaries  the applications
user_space/shell     the shell
user_space/loader    the dynamic loader
third_party/     source nobody here wrote, kept clearly separate
tools/           build scripts, the QEMU harnesses, the font generator
tests/           host unit tests, fakes, fixtures, budgets and coverage floors
```
