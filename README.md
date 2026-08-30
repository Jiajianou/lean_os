# lean_os

A desktop operating system for x86-64, written from scratch: bootloader,
kernel, drivers, filesystem, window system and applications. No GRUB, no
libc, no third-party code anywhere in the OS itself.

![79 milestones](https://img.shields.io/badge/milestones-79-informational)

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
  directories and files up to 8 MiB, pipes, shared memory, an orderly
  shutdown, and a clock that knows the date.
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
```

## Tests

Two harnesses, and neither subsumes the other. Run both.

```sh
./tools/qemu-serial-test.sh  # boots headless, grades the serial log: 69 boot markers
./tools/qemu-input-test.sh   # drives real clicks and keys, grades real pixels: 45 tests
```

The first proves every subsystem still works from the inside. The second
proves the path a person's hands take to reach it - a distinction that
was learned the hard way (see M40 in
[milestones.md](milestones.md)).

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
