# lean_os

A desktop operating system for x86-64, written from scratch: bootloader,
kernel, drivers, filesystem, window system and applications. No GRUB, no
libc, no third-party code anywhere in the OS itself.

![100 milestones](https://img.shields.io/badge/milestones-100-informational)

```
UEFI firmware
  -> BOOTX64.EFI            hand-written PE32+ EFI app (kernel/boot/uefi)
  -> kernel.bin             long mode, paging, PMM/VMM/heap, SMP
  -> init (PID 1)
  -> compositor             owns the framebuffer, the cursor and the speaker
  -> desktop_icons, desktop_shell, and whatever you open
```

## What it does

- **A desktop.** Overlapping windows with a real z-order, titlebars with
  traffic-light buttons, drag and resize, snapping, minimize/restore with
  motion, a taskbar, a Spotlight-style launcher, context menus, toasts,
  drag and drop, four virtual desktops, and keyboard chords.
- **Applications.** A terminal with a real shell (arguments, quoting,
  `>`/`>>`, a pipe, tab completion, `cd`), a text editor (undo, redo,
  find, paste), a file manager (columns, sorting, rename/copy/delete), a
  task manager, a settings panel, a paint toy and a clock.
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
  See M90-M93 in [milestones.md](milestones.md).
- **A terminal that is a device.** A line discipline with canonical
  and raw modes, sessions, process groups and job control - and
  **pseudo-terminals**: `/dev/ptmx`, `/dev/pts/<n>`, `openpty` and
  `forkpty`, so a program can run another program on a terminal it
  cannot tell from the console. `^C` interrupts the foreground job and
  `^Z` suspends it, with `waitpid(WUNTRACED)` reporting the stop. See
  M85 in [milestones.md](milestones.md).
- **Unix-shaped enough to build against.** An environment inherited
  across a spawn and a real working directory, so a relative path means
  something; signals a program can *catch*, delivered through a frame on
  its own stack; `mmap`/`munmap` that hands pages back and reuses the
  holes; POSIX threads sharing one address space; and `<dirent.h>`,
  `<sys/stat.h>`, `<unistd.h>`, `<signal.h>`, `<pthread.h>`,
  `<sys/mman.h>` and `<setjmp.h>` over the top of it. See M75-M79 in
  [milestones.md](milestones.md).
- **A desktop that remembers.** Whatever was open when the machine
  stopped is open again where it was when it starts; an editor with
  unsaved changes can veto a shutdown; recently-opened files are in the
  launcher and the file manager.
- **A network.** An RTL8139 driver, Ethernet, ARP, IPv4, ICMP, UDP and
  **TCP** - the eleven-state machine, retransmission with a measured
  timeout and Reno congestion control - plus a DHCP client, and sockets
  programs open as ordinary file descriptors. `netconf` and an SNTP
  client (`nettime`) use them. See
  [docs/networking.md](docs/networking.md).
- **Settings that stick.** Wallpaper, colours, **screen resolution**
  (changed live, with a countdown that puts it back if you do not
  confirm), motion, and volume.
- **Sound.** A PC-speaker beep on errors and an AC'97 output stream.
- **A boundary around a program.** Every process carries a capability
  set the kernel assigns from a manifest at spawn time, and that set can
  only ever shrink. An ordinary application cannot paint on the screen,
  read the clipboard, list processes, open a socket or switch the machine
  off. See [docs/capabilities.md](docs/capabilities.md).
- **Somebody else's program.** The 1972 Whetstone benchmark, ported
  unmodified, running on an SSE-enabled kernel against a libc written
  here. See [docs/third-party-programs.md](docs/third-party-programs.md).
- **A compiler that knows this OS by name.** `x86_64-lean_os` is a real
  target triple in binutils and GCC, built by
  `tools/build-toolchain.sh` from a nine-edit port. `x86_64-lean_os-gcc
  hello.c -o hello` produces a program this machine runs with **no flag
  supplied by hand** — the load address, code model, red zone and startup
  files all come from the target description, because every flag invented
  by hand is a flag someone else's build system will not pass. `bzip2`
  and `GNU hello` are built with it, the second through
  `./configure --host=x86_64-lean_os && make`, and both run here. See M94
  in [milestones.md](milestones.md).
- **Somebody else's *userland*.** Toybox, built for this machine from
  the published tarball with no edit to its source, installed as one
  static binary and 143 command names in `/bin`: `find`, `grep`, `sed`,
  `sort`, `xargs`, `tar`, `ps`, `du`, `wc` and the rest. `find . -type f
  | xargs grep -l something | sort | uniq -c | sort -rn` is a boot
  self-test — five programs nobody here wrote, four pipes, five forked
  and exec'd processes. See M89 in [milestones.md](milestones.md) for
  the eight bugs running it found.
- **Somebody else's language.** CPython 3.12.7, cross-built with this
  project's own compiler from a three-edit port, with its standard
  library on the disk as `.py` source rather than frozen into the
  binary — so `import json` is a file this filesystem opens. A boot
  self-test runs a real script; `python3 -m test` runs **CPython's own
  regression suite** here and reports its own pass and fail counts,
  which is the only instrument in this project that neither wrote its
  own assertions nor chose what to assert. It found nineteen bugs in
  this OS, including a thread stack that had been entered misaligned
  since M79 and an `fstat` that refused every descriptor which was not
  a file — which is why `print()` on this machine used to write nothing
  and return successfully. See M99 in [milestones.md](milestones.md).
- **Somebody else's *libraries*.** zlib 1.3.1, libpng 1.6.44 and
  libjpeg 9f, built for this machine by this project's own compiler
  with **no edit to any of their source**, installed into the sysroot
  by their own `make install` so the next one links the last — libpng's
  `./configure` will not proceed until it finds `-lz` here. Each is
  graded by its own test suite on the machine, and libjpeg's is the
  sharpest instrument in this tree: it ships the PPM, GIF, BMP and JPEG
  files the IJG's own encoder and decoder produced in **1995**, and its
  `make test` requires byte equality with them. All seven comparisons
  pass here — baseline and progressive, decode and encode, and a
  `jpegtran` transcode that turns the progressive file back into the
  original file byte for byte. See M100 in
  [milestones.md](milestones.md).

- **A package manager.** `os install grep` puts **GNU grep 3.11** on
  this machine — built here by this project's own compiler from the
  published tarball with no edit to its source — and the machine runs
  it. A package is a manifest, a file table and a payload with **no
  install hooks at all**: installing is verify and copy, so what
  installing can do to your machine is a sentence rather than an audit.
  Three SHA-256s, written here, answer three different questions; every
  path is refused before anything is created; commands land in
  `/pkg/bin` and never in `/bin`, so a package cannot take over the name
  of a program this OS ships.

  And the kernel decides what an installed package may do. A program
  under `/pkg` never matches the shipped capability table whatever it is
  *called* — a package shipping a binary named `compositor` gets `0x0`,
  not `CAP_ALL` — and every write under `/pkg` needs a capability only
  `/bin/os` holds, so the registry that records those grants is not a
  file anything else can edit. See M111 in
  [milestones.md](milestones.md) and [docs/packages.md](docs/packages.md),
  which is also where the four C library bugs GNU grep's build found are
  written down.

## Build and run

Needs an `x86_64-elf` cross-toolchain, `nasm`, `clang`+`lld` (for the
EFI app), `mtools` and `qemu-system-x86_64`. See
[docs/toolchain.md](docs/toolchain.md).

```sh
./tools/run-qemu.sh          # builds everything, fetches OVMF the first time, boots
./tools/run-qemu.sh --selftests   # ...and runs the boot self-test battery on the way
```

Three optional steps put ported software into the image, each once per
image and none of them part of `all` — see the Makefile for why writing
into a fresh image is ordered rather than automatic:

```sh
make toybox                  # /bin/toybox and 143 command names
tools/build-packages.sh      # cross-build grep and bzip2 into .osp archives
make packages                # ...and write them into the image as /pkg/repo
```

The disk image is 2 GiB and sparse - a few megabytes on disk until
something fills it. `QEMU_MEM=128` boots the same kernel on a small
machine and `QEMU_DISK=ide` runs it through the ATA driver instead of
virtio; both are configurations the harnesses are expected to pass in,
not fallbacks.

## Tests

One command, three tiers. Each is a superset of the one above it.

```sh
./tools/run-tests.sh --fast    # host unit tests. No QEMU. Under a second.
./tools/run-tests.sh           # ...plus a graded boot and the quick input subset. Minutes.
./tools/run-tests.sh --full    # ...plus the whole input suite and the slow host tests.
```

`make test` and `make test-fast` are the same thing for people who type
that instead.

Four instruments, and none of them subsumes another:

- **Host unit tests** (`tests/`) compile kernel units - `libk`, the heap,
  leanfs, every network parser - for the machine you are sitting at and
  run them under ASan and UBSan in under a second. They exist to reach
  the error paths a booted machine cannot: a full disk, a failed
  allocation, a corrupt superblock, a malformed packet. It also
  compiles the **scheduler** (Q13) - 2,157 lines whose bugs have
  historically presented as "about one boot in ten hangs" - against a
  fake timer and a fake CPU, so a tick is a function call and a
  fairness property can be checked at every task count, and it learns
  the order locks are taken in so an inversion is an error rather than
  a comment (Q9). 247 tests.
- **The boot self-tests** (`tools/qemu-serial-test.sh`) boot the real
  image and grade the serial log against 112 markers and 32 performance
  budgets. They prove every subsystem still works from the inside.
- **The input suite** (`tools/qemu-input-test.sh`) drives real clicks and
  keys through QEMU's monitor and grades real framebuffer pixels. It
  proves the path a person's hands take - a distinction learned the hard
  way (M40). Most of its tests check that something *did* change; two of
  them check the opposite, that nothing else did, which is the only way to
  catch a flicker (Q7). Since Q19 it boots **once per image** and
  restores a snapshot of the painted desktop per test - 440 s to 337 s
  for fifty tests - with the snapshot keyed on a hash of the image, so a
  stale one is refused rather than quietly used.
- **Fuzzers** (`make fuzz-run`) feed the network parsers and the
  filesystem mount path arbitrary bytes. The network target manages about
  150,000 inputs a second.
- **A mutation harness** (`make mutate`) breaks the kernel on purpose,
  one small change at a time, and reports whether the tests noticed. It
  is the only instrument here that grades the *tests* rather than the
  machine, and the first thing it found was a file at 100% line coverage
  whose mutation score was zero.
- **Six differential tests** (`tools/sh-test.sh`,
  `tools/regex-test.sh`, `tools/scanf-test.sh`, `tools/printf-test.sh`,
  `tools/math-test.sh` and `tools/pkg-test.sh`) compile this project's
  own shell, regular-expression engine, `sscanf`, `printf`, libm and
  SHA-256 from the same source the machine runs, for the machine you are
  sitting at, and require every fixture to agree with the host's own —
  the last of them over 250 real files from this tree, with a package
  round trip decided by `cmp` and `diff -r`. Nothing in those
  fixtures says what the right answer is - a program nobody here wrote
  decides, which is the only useful standard for code whose whole job is
  to agree with every other implementation of itself. Every one of them
  found real bugs on its first run; the libm one (M99) found `fmod`
  returning the wrong sign and a result larger than its own modulus, and
  then, once it learned to ask about infinity, a `log` that did not
  return at all.
- **An image-tree test** (`tools/image-tree-test.sh`) has a host tool
  write a directory tree into a leanfs image, compares the image with an
  independent reader against the tree it came from, then boots it and has
  the machine walk that tree and hash every byte of it against what the
  host wrote down. It is how a source tree reaches this disk at all, and
  both halves are needed: the host half alone is a well-formed image
  nothing has opened.
- **Exhaustion** (`/bin/exhausttest`, graded by the `[q9]` boot marker)
  takes descriptors, pipes, shared-memory segments and sockets to their
  ceilings and requires each to refuse, recover, and work again - twice
  over, with a leak audit across 2,200 rounds either side of it.
- **A fault-injected disk** (`tools/disk-fault-test.sh`) boots the
  machine with QEMU's `blkdebug` refusing **every** write, through
  virtio and through ATA, and requires it to reach PID 1 anyway. Every
  device error in the block and network drivers used to be a `panic`;
  Q16 made them errors that propagate, and this is what says so.
- **A crash test** (`tools/crash-test.sh`) cuts the power mid-write with
  `SIGKILL`, reboots, and checks the filesystem with an independent
  reader. Sixteen cuts across the heaviest metadata window; the
  filesystem has survived all of them.

Booting the machine is no longer the same thing as testing it. `make run`
boots to the desktop in about eight seconds; the ~190-second self-test
battery runs only when something asks for it, which
`tools/qemu-serial-test.sh` does and `tools/run-qemu.sh` does not. The
image is identical either way - see `kernel/dev/fwcfg.h` for why the
switch comes from outside the image rather than from a `#ifdef`.

## Where things are

```
kernel/          boot, arch/x86_64, mm, sched, drivers, fs, ipc, net, kernel.c
system_api/      the syscall ABI: numbers, structs, the kernel/user contract
user_space/lib   the runtime every program links: crt0, syscalls, gfx, wmclient
user_space/libc  a C library subset, for programs written against standard headers
user_space/bin   the applications
third_party/     source nobody here wrote, kept clearly separate
tools/           build scripts, the QEMU harnesses, the font generator
docs/            per-subsystem design notes
milestones.md    the live record: where this is, what is open, what is next
milestones-archive.md   every milestone entry M0-M110 and Q1-Q20, frozen
```

**`milestones.md` is the real documentation.** It carries the current
state, every open box with the condition attached, and the order the work
goes in. Start there. Behind it,
[milestones-archive.md](milestones-archive.md) holds every milestone
entry ever written - what was added, what it cost, and, most usefully,
what went wrong and what that taught.
