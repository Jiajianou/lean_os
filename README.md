# lean_os

A desktop operating system for x86-64, written from scratch: bootloader,
kernel, drivers, filesystem, window system and applications. No GRUB, no
libc, no third-party code anywhere in the OS itself.

![93 milestones](https://img.shields.io/badge/milestones-93-informational)

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

## Build and run

Needs an `x86_64-elf` cross-toolchain, `nasm`, `clang`+`lld` (for the
EFI app), `mtools` and `qemu-system-x86_64`. See
[docs/toolchain.md](docs/toolchain.md).

```sh
./tools/run-qemu.sh          # builds everything, fetches OVMF the first time, boots
./tools/run-qemu.sh --selftests   # ...and runs the boot self-test battery on the way
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
  allocation, a corrupt superblock, a malformed packet. 123 tests.
- **The boot self-tests** (`tools/qemu-serial-test.sh`) boot the real
  image and grade the serial log against 80 markers and five performance
  budgets. They prove every subsystem still works from the inside.
- **The input suite** (`tools/qemu-input-test.sh`) drives real clicks and
  keys through QEMU's monitor and grades real framebuffer pixels. It
  proves the path a person's hands take - a distinction learned the hard
  way (M40). Most of its tests check that something *did* change; two of
  them check the opposite, that nothing else did, which is the only way to
  catch a flicker (Q7).
- **Fuzzers** (`make fuzz-run`) feed the network parsers and the
  filesystem mount path arbitrary bytes. The network target manages about
  150,000 inputs a second.
- **A mutation harness** (`make mutate`) breaks the kernel on purpose,
  one small change at a time, and reports whether the tests noticed. It
  is the only instrument here that grades the *tests* rather than the
  machine, and the first thing it found was a file at 100% line coverage
  whose mutation score was zero.
- **A shell differential test** (`tools/sh-test.sh`) compiles
  `/bin/sh` from the same source the machine runs, for the machine you
  are sitting at, and requires every fixture in `tests/sh/` to produce
  byte-identical output to the host's own `/bin/sh`. Nothing in those
  fixtures says what the right answer is - a shell nobody here wrote
  decides, which is the only useful standard for a program whose whole
  job is to agree with every other shell about what a script means.
- **An image-tree test** (`tools/image-tree-test.sh`) has a host tool
  write a directory tree into a leanfs image, compares the image with an
  independent reader against the tree it came from, then boots it and has
  the machine walk that tree and hash every byte of it against what the
  host wrote down. It is how a source tree reaches this disk at all, and
  both halves are needed: the host half alone is a well-formed image
  nothing has opened.
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
milestones.md    the living record: what was built, in what order, and why
```

**`milestones.md` is the real documentation.** Every milestone says what
was added, what it cost, and - most usefully - what went wrong and what
that taught. Start there.
