# lean_os

A desktop operating system for x86-64, written from scratch: bootloader,
kernel, drivers, filesystem, window system and applications. No GRUB, no
libc, no third-party code anywhere in the OS itself.

![63 milestones](https://img.shields.io/badge/milestones-63-informational)

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
  drag and drop, and keyboard chords.
- **Applications.** A terminal with a real shell (arguments, quoting,
  `>`/`>>`, a pipe, tab completion, `cd`), a text editor (undo, redo,
  find, paste), a file manager (columns, sorting, rename/copy/delete), a
  task manager, a settings panel, a paint toy and a clock.
- **A system underneath.** Pre-emptive multitasking across multiple
  cores, a per-process address space, a custom filesystem with
  directories and files up to 8 MiB, pipes, shared memory, signals,
  an orderly shutdown, and a clock that knows the date.
- **Settings that stick.** Wallpaper, colours, **screen resolution**
  (changed live, with a countdown that puts it back if you do not
  confirm), motion, and volume.
- **Sound.** A PC-speaker beep on errors and an AC'97 output stream.
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
./tools/qemu-serial-test.sh  # boots headless, grades the serial log: 50 boot self-tests
./tools/qemu-input-test.sh   # drives real clicks and keys, grades real pixels: 44 tests
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
