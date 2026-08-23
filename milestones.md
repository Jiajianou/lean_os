# lean_os — Milestones

This file is the living progress record for the project. Update the status
and notes on a milestone as soon as it's reached — this is the source of
truth for "where are we" across sessions.

Status legend: `[ ]` not started · `[~]` in progress · `[x]` done

## Ground rules / assumptions

- **Architecture:** x86_64, UEFI boot only (the original legacy-BIOS boot
  path was removed in M26 - see below).
- **Bootloader:** written entirely from scratch (`kernel/boot/uefi/boot.c`,
  a hand-written PE32+ EFI application), no GRUB/Multiboot/Limine or any
  third-party boot code.
- **Language:** freestanding C for kernel/system_api/user_space, with x86_64
  assembly (NASM — decided in M0) for boot code, context switches, and other
  spots C can't reach. No libc, no newlib, no external libraries linked into
  any binary the OS ships. (Revisit if you'd rather use Rust/Zig instead of
  C — nothing below depends on the choice, only M3 onward's code does.)
  `<stdint.h>`/`<stddef.h>` are fine to `#include`: they're compiler-provided
  freestanding headers (type definitions and macros resolved at compile
  time), not part of any linked C library — nothing from them ends up
  linked into the binary. Kernel objects are also linked with `x86_64-elf-ld`
  directly (not via `gcc` as the link driver), so there's no libc/crt0 for
  a stray implicit link to pull in even by accident.
- **Toolchain (dev-time only, not shipped):** a cross-compiler targeting
  `x86_64-elf` (clang with `-target x86_64-elf` or a self-built
  `x86_64-elf-gcc`/binutils), `ld` for linking with custom linker scripts,
  and QEMU (`qemu-system-x86_64`) for fast iteration. These build/test the
  OS; none of their code ends up in the OS image. Real-hardware testing
  (USB boot) is a manual fallback once QEMU boots reliably.
- **Repo layout:**
  ```
  kernel/          # boot code, arch/x86_64, mm, sched, drivers, fs, kernel.c
  system_api/      # syscall ABI: numbers, structs, calling convention —
                   # shared contract between kernel and user_space
  user_space/      # crt0/runtime, init, shell, utilities
  tools/           # toolchain build scripts, qemu run/debug scripts
  docs/            # design notes per subsystem
  milestones.md    # this file
  ```
- **Build system:** plain Makefiles + hand-written linker scripts. No
  CMake/Meson/autotools, no package manager.

---

## M0 — Project scaffolding & toolchain ✅

- [x] Create `kernel/`, `system_api/`, `user_space/`, `tools/`, `docs/` dirs
- [x] Decide and document assembler (NASM — see [docs/toolchain.md](docs/toolchain.md))
- [x] Build/install `x86_64-elf` cross-compiler + binutils (via Homebrew)
- [x] Install QEMU, verify `qemu-system-x86_64 --version` (10.1.3, already present)
- [x] Top-level `Makefile` that can (currently) build nothing but runs
- [x] `tools/run-qemu.sh` script stub

## M1 — Boot sector (stage 1) ✅

- [x] 512-byte MBR boot sector in 16-bit real mode
- [x] BIOS `INT 13h` disk read to load stage 2 into memory
- [x] Print a message via BIOS `INT 10h` teletype to prove it's alive
- [x] Boots in QEMU and shows the message

## M2 — Stage 2: protected mode → long mode ✅

- [x] Enable the A20 line
- [x] Build and load a minimal GDT, enter 32-bit protected mode
- [x] Query memory map via BIOS `INT 15h, EAX=E820` and stash it for the
      kernel
- [x] Set up identity-mapped paging structures, enable long mode (64-bit)
- [x] Jump into the kernel's entry point with a defined handoff (memory
      map pointer, etc.)

## M3 — Minimal freestanding kernel ✅

- [x] `kernel/` linker script + entry stub in assembly
- [x] First C code runs (freestanding: `-ffreestanding -nostdlib`, no libc)
- [x] VGA text-mode driver for `kprintf`-style debug output
- [x] Panic handler (halt + message)

## M4 — CPU fundamentals in the kernel ✅

- [x] Kernel-owned GDT/TSS setup (bootloader's GDT was temporary)
- [x] IDT + interrupt/exception handlers (divide-by-zero, GPF, page fault,
      double fault at minimum)
- [x] PIC remap (or APIC if you want to skip legacy PIC)

## M5 — Memory management ✅

- [x] Physical frame allocator, seeded from the E820 map
- [x] Virtual memory manager (page table manipulation, map/unmap)
- [x] Kernel heap allocator (`kmalloc`/`kfree`)

## M6 — Timer & core drivers ✅

- [x] PIT or APIC timer, tick counter, `sleep`-style busy wait
- [x] Serial port (COM1) driver for debug logging (parallel to VGA)
- [x] PS/2 keyboard driver

## M7 — Multitasking basics ✅

- [x] Task/thread control block struct
- [x] Context switch (assembly) between kernel threads
- [x] Simple round-robin scheduler driven by the timer interrupt

## M8 — System call interface (`system_api/`) ✅

- [x] Define syscall ABI: numbering, register convention, `syscall`/`sysret`
      (or `int 0x80`) entry
- [x] `system_api/include/` headers shared by kernel and user_space
- [x] Kernel-side syscall dispatch table
- [x] First syscalls: `write`, `exit`, `getpid`

## M9 — User mode & process loading ✅

- [x] Ring 3 transition, TSS configured for privilege-level switches
- [x] Minimal ELF64 loader in the kernel
- [x] User stack + address space setup per process

## M10 — User-space runtime ✅

- [x] `user_space/lib` crt0 (`_start`) for user binaries
- [x] Thin syscall wrapper functions (built from `system_api/` headers)
- [x] Bare string/mem helpers (`memcpy`, `strlen`, ...) — hand-written, no
      external libc

## M11 — First user program end-to-end ✅

- [x] "Hello world" built against `user_space/lib` + `system_api`
- [x] Kernel loads and runs it, output visible via syscall `write`
- [x] This is the milestone that proves the whole stack (boot → kernel →
      syscall → user space) works together

## M12 — Storage & filesystem ✅

- [x] ATA (PIO mode) disk driver
- [x] Minimal custom filesystem format (or a simple FAT-like layout) + VFS
      layer in the kernel
- [x] Load user binaries from disk instead of an embedded/in-memory blob

## M13 — Init & shell ✅

- [x] `fork`/`exec`-equivalent syscalls, `wait`
- [x] `user_space/init` as PID 1
- [x] Minimal shell (`user_space/shell`)
- [x] A couple of coreutils-style programs (`ls`, `cat`, `echo`)

## M14 — IPC & process management ✅

- [x] Pipes
- [x] Signals (basic set: kill, term, etc.)
- [x] Process groups / more complete `wait` semantics

## Path to a desktop environment (M15+)

Everything through M14 is text-mode only — no framebuffer, no pointing
device, no windowing, no user-space heap. This is the ordered path from
there to "boot into a graphical desktop and run my own custom apps,"
zero dependencies preserved throughout (BIOS VBE calls are dev-time boot
handoff, same category as the existing E820/disk-read BIOS calls — not a
runtime dependency).

## M15 — Filesystem headroom ✅

- [x] Current cap (`LEANFS_MAX_FILE_SIZE` = 16 direct blocks x 512 B = 8 KiB)
      is already tight: existing user ELFs are ~7 KiB. Raise it before
      anything bigger (fonts, a UI toolkit, GUI apps) needs to land on disk.
- [x] Add indirect blocks (or grow direct block count / block size) to
      leanfs; grow the data region accordingly
- [x] Re-verify M12/M13's persistence tests still pass at the new limits

## M16 — Linear framebuffer & graphics primitives ✅

- [x] Extend stage2 to set a VBE/VESA linear framebuffer mode (`INT 10h
      AX=4F02h`) and query mode info (`AX=4F01h`) for physical base/pitch/
      width/height/bpp before the protected-mode transition
- [x] Pass framebuffer info through the existing boot handoff struct
      (alongside the E820 pointer)
- [x] `kernel/drivers/fb.c`: map the framebuffer physical region, pixel
      plot / fill-rect / blit primitives
- [x] Retire VGA text mode as the primary display (keep serial/klog for
      debug logging either way) — deferred the actual text-mode retirement
      to M17 (framebuffer console), since VGA text is still the only
      logging surface until then; klog/serial are untouched either way

## M17 — Framebuffer text console + font rendering ✅

- [x] Embed a fixed-width bitmap font in the kernel image
- [x] Software glyph renderer + scrolling text console drawn over the
      framebuffer
- [x] Route existing klog output through it, so all prior boot logging
      stays visible without touching every call site again

## M18 — PS/2 mouse driver & cursor ✅

- [x] IRQ12 driver, 3-byte packet decode (dx/dy/buttons) — same shape as
      M6's keyboard driver
- [x] Cursor sprite draw/erase over the framebuffer (save/restore pixels
      underneath; no compositor yet to own this properly)

## M19 — User-space heap allocator & shared memory ✅

- [x] `SYS_sbrk`-style syscall to grow a process's address space on demand
- [x] `user_space/lib` malloc/free (mirrors the kernel heap's design) so
      apps can allocate dynamically — nothing in user space can today
- [x] A shared-memory syscall (compositor <-> app pixel buffers) — pipes
      are the wrong tool for whole-frame pixel data

## M20 — Windowing compositor ✅

- [x] User-space compositor process owns the framebuffer exclusively
- [x] IPC protocol for apps to create a window (request a shared pixel
      buffer), submit damage/redraw, and receive input events — damage/
      redraw and input receipt landed in a narrower form than originally
      scoped; see the progress log for exactly what and why
- [x] Compositor blits windows to the real framebuffer in z-order, draws
      borders/title bars — z-order exists structurally (a window list,
      painted in order) but is unproven beyond one window; see M21

## M21 — App UI toolkit + input routing ✅

- [x] `user_space/lib` drawing API (rects, lines, text) targeting an app's
      window buffer
- [x] Compositor routes keyboard/mouse events to the focused window;
      focus-follows-click
- [x] 1-2 demo GUI apps (clock, simple paint) proving the full pipeline
      end to end

## M22 — Desktop shell ✅

- [x] Taskbar/dock listing running apps, click to focus/minimize
- [x] App launcher reading the filesystem for available executables
- [x] This is the milestone where "desktop environment running custom
      apps" is genuinely true, not aspirational

## M23 — Desktop icon + GUI terminal ✅

- [x] `wm_create_request_t.desktop`: a chrome-less, full-screen, always-
      on-*bottom* window kind (compositor.c's mirror image of M22's
      panel) - the actual "desktop" background a real icon can be drawn
      on and double-clicked through gaps in other windows
- [x] `desktop_icons.c`: one double-click-to-launch "Terminal" icon on
      that background (`WM_EVENT_MOUSE_BUTTON` timestamp pair via
      `sys_uptime_ms`, no drag/select/rename - launching is the only
      interaction a single-icon desktop needs)
- [x] `SYS_dup2` + `SYS_wait_nb`: the two missing fd-table/process
      primitives a real in-window shell needs that no earlier milestone
      did - pointing a client's own stdout at a pipe before `SYS_spawn`
      so a *child's* output can be captured instead of hitting the global
      console, and polling a spawned child non-blockingly so a GUI event
      loop never stalls waiting for it
- [x] `gui_terminal.c`: a real line-editing shell in its own window
      (own text-grid renderer over `WM_EVENT_KEY`, not `user_space/shell`'s
      fd 0/1 console path) - runs the same coreutils the panel launcher
      already spawns, with their real stdout captured and rendered in
      the window instead of racing the compositor for the framebuffer
- [x] `init.c` now also spawns `desktop_icons` alongside the compositor
      and desktop shell, torn down and respawned together

## M24 — UEFI boot path ✅

- [x] `kernel/boot/uefi/{efi.h,efi_proto.h}`: hand-written UEFI base types
      and the handful of protocols needed (Simple Text Output, Boot
      Services, Loaded Image, Block I/O, Graphics Output) - no GNU-EFI or
      edk2 headers linked in, same "write it ourselves" ethos as
      everything else this project ships, just matching a public spec's
      ABI instead of a BIOS one
- [x] `kernel/boot/uefi/boot.c`: the UEFI counterpart to stage1+stage2 -
      GetMemoryMap → the exact e820_entry_t handoff format kernel/mm/
      e820.h already defines, GOP → the exact fb_boot_info_t format
      kernel/drivers/fb.h already defines, raw EFI_BLOCK_IO_PROTOCOL
      reads (not a filesystem) to fetch kernel.bin from the same fixed
      LBA stage2.asm reads it from, then ExitBootServices and a raw-asm
      jump into kernel_main with the identical RDI/RSI register handoff
      - producing byte-compatible structures is what let kernel.c/pmm.c/
      fb.c stay completely unmodified; this milestone is 100% new files
      plus a 16-byte partition-table patch to stage1.asm
- [x] `kernel/boot/stage1.asm`: the disk's boot sector is now also a
      valid legacy MBR (one partition entry, type 0xEF, sitting in the
      gap between the existing boot blob and leanfs's start) - UEFI reads
      that table and finds the ESP; BIOS still just runs the boot code
      above it and never looks at the table at all. One disk image, two
      independent boot paths, no GPT needed since the kernel is fetched
      by raw LBA rather than from inside the ESP's filesystem
- [x] Toolchain: `clang -target x86_64-unknown-windows` + `lld-link`
      (PE32+ output - a UEFI application has to be PE, not ELF) and
      `mtools` (formats the ESP directly inside the disk image via
      `mformat -i image@@byteoffset`, no loopback mount needed) - see
      docs/toolchain.md
- [x] `tools/build-ovmf.sh` + `tools/run-qemu-uefi.sh`: OVMF firmware
      built from source (tianocore/edk2, CLANGPDB toolchain) rather than
      installed, because the only prebuilt OVMF available on this
      machine (a 2021 build from a third-party tap) reliably crashes
      under current QEMU - a firmware bug with nothing to do with this
      project's own boot code, confirmed by reproducing it against a
      trivial "print a string" EFI app before writing any real logic
- [x] One real bug worth remembering: boot.c's own scratch pool
      allocations were originally tagged with a custom EFI_MEMORY_TYPE
      value in the UEFI spec's OS-reserved range (0x80000000+). That
      value doesn't fit EFI_MEMORY_TYPE's signed-int underlying
      representation, got sign-extended to garbage crossing the ms_abi
      call boundary into the firmware, and corrupted DxeCore's own state
      badly enough to crash *inside the firmware*, several calls later -
      diagnosed by rebuilding OVMF as a DEBUG build to get real PDB
      symbol attribution on the crash. Fixed by just using the always-
      spec-safe `EfiLoaderData` instead; there was never a real need for
      a custom type

## M25 — Package/build tooling for third-party user programs ✅

- [x] `tools/build-user-program.sh`: compiles a standalone third-party
      `.c` file against `user_space/lib` (crt0 + syscall_wrappers +
      str/malloc/gfx/font8x16/wmclient) with the exact flags this
      project's own programs build with, pulled live from the Makefile
      (`make print-USER_CFLAGS`/`print-USER_LIBOBJS` - a small new
      generic `print-%` target) rather than a second hardcoded copy that
      could drift
- [x] `tools/leanfs-put.c`: a *host* program (ordinary hosted C, built
      with the system `cc`, never runs as part of the OS) that
      duplicates kernel/fs/leanfs.c's on-disk structs and allocation
      logic byte-for-byte and writes a file straight into an
      already-built disk image's leanfs region - the missing host-side
      half of M12's filesystem, letting a third-party program get onto
      disk without a kernel rebuild at all. Verified both via the
      kernel's own boot self-tests passing and an independent
      byte-for-byte round-trip check (including the indirect-block path)
      written from scratch against the format, not reusing leanfs-put's
      own code
- [x] `make preseed`: found a real interaction the hard way - leanfs-put
      writing onto a disk image that's never booted claims the first
      free inode, and if that's inode 0 it shifts kernel.c's M22 boot
      self-test's hardcoded "launcher slot 0 is hello, the first file
      ever seeded" assumption right out from under it, panicking on the
      very next boot. `make preseed` writes every built-in program via
      leanfs-put itself, in seed order, before any third-party program
      is added - not part of `all` and changes no default behavior (the
      kernel's own first-boot seeding already skips anything already
      present), just makes the third-party workflow safe by construction
      instead of by instruction
- [x] `docs/third-party-programs.md`: the end-to-end workflow

## M26 — UEFI-only boot, BIOS path removed ✅

- [x] Deleted `kernel/boot/stage1.asm` and `stage2.asm`;
      `kernel/boot/uefi/boot.c` is now the only boot loader - it already
      did the whole job itself (M24), nothing in the kernel proper needed
      to change
- [x] `kernel/boot/mbr.asm`: LBA 0 is now pure partition-table data (no
      executable boot code) - UEFI firmware still needs it to find the ESP
      (same legacy-MBR-with-a-0xEF-entry trick M24 introduced), nothing
      ever executes this sector as code anymore
- [x] Kernel blob moved from LBA 9 to LBA 1 (no more stage1+stage2 gap to
      reserve); `boot.c`'s `KERNEL_START_LBA` updated to match
- [x] `tools/run-qemu.sh` now builds (`make all`) and boots (via OVMF) in
      one command, auto-building OVMF firmware on first run if
      `build/ovmf/` is missing; `tools/run-qemu-uefi.sh` removed (merged
      into `run-qemu.sh`); the `Makefile`'s `run-uefi` target removed
      alongside it
- [x] `tools/qemu-serial-test.sh` updated to boot via OVMF too (had been
      implicitly BIOS-only, which would no longer boot at all)
- [x] `docs/flow.md` rewritten to walk through the UEFI boot flow in place
      of the removed BIOS stage1/stage2 deep-dive

## M27 — Networking stack + NIC driver ✅

- [x] `kernel/drivers/pci.h/.c`: brute-force PCI config-space enumeration
      (bus/slot/function, port 0xCF8/0xCFC) - just enough to find a device
      by vendor/device ID, read its I/O-mapped BAR0, and enable bus
      mastering + I/O space. No bridge walking, capability lists, or
      memory-mapped BARs - one device (the NIC below) is all this kernel
      has ever needed to find.
- [x] `kernel/drivers/rtl8139.h/.c`: RTL8139 Fast Ethernet driver (QEMU's
      `-device rtl8139`) - chosen over e1000/virtio-net as the smallest
      real register interface of the three (one I/O BAR, a fixed 8K RX
      ring the NIC DMAs into directly, 4 fixed TX descriptor slots, no
      descriptor-ring bookkeeping). PCI interrupt line hooks into the
      existing PIC-based `irq_register_handler` infra (M6) unchanged - a
      legacy `pc`-machine NIC's routed IRQ is just another PIC line, SMP's
      LAPIC/IOAPIC bring-up notwithstanding. `kernel/mm/pmm.h/.c` gained
      `pmm_alloc_contiguous`/`pmm_free_contiguous` for the RX ring's
      physically-contiguous DMA buffer - the single-frame allocator alone
      can't promise that.
- [x] `kernel/net/{ethernet,arp,ip,icmp,net}.h/.c`: Ethernet II framing,
      RFC 826 ARP (request/reply, small fixed cache, opportunistic
      learning from any received ARP or IP packet), minimal IPv4 (no
      fragmentation, on-link-vs-gateway next-hop resolution, standard
      Internet checksum), and ICMP echo request/reply in both directions.
      Static IP config (10.0.2.15/10.0.2.2, matching QEMU usermode
      networking's fixed DHCP lease and gateway) rather than a DHCP client -
      out of scope for "driver + ARP + ICMP", and the only network this
      kernel actually boots on.
- [x] `tools/run-qemu.sh` / `tools/qemu-serial-test.sh`: both now attach
      `-netdev user -device rtl8139` - QEMU's built-in SLIRP NAT, no root/
      tap setup required.
- [x] Boot self-test (`kernel/kernel.c`, after the SMP self-test): sends a
      real ICMP echo request to the gateway and blocks on a matching reply
      arriving asynchronously via the RX IRQ handler - proves NIC TX/RX,
      PCI IRQ routing, ARP resolution, and ICMP matching all work against
      a real peer (SLIRP), not a kernel-side loopback stand-in. Verified
      passing twice in a row via `tools/qemu-serial-test.sh`.
- [x] `rtl8139_init`/`net_init` return 0 rather than panicking when no NIC
      is attached - the one driver in this kernel where "not present" had
      to become a real, non-fatal outcome rather than every other
      driver's "assume it's there" panic: an RTL8139 is a legacy chip
      real machines (the separate "port to real hardware" stretch goal,
      below) essentially never have, so a hard panic here would have
      blocked every real-hardware boot at this exact self-test. Degrades
      the same way the keyboard/mouse self-tests already do for present-
      but-unexercised hardware. Verified both ways: boots and pings the
      gateway with a NIC attached, boots cleanly through to init with none.

## M28 — Port to real hardware (USB boot test) [~]

Preparation and tooling are done and verified as far as anything without
a physical machine and USB port can go; the actual boot-on-real-hardware
step is a manual one - see [docs/real-hardware.md](docs/real-hardware.md)
for the full runbook. Leaving this `[~]` rather than `[x]` until that
manual step has actually been run and reported back.

- [x] Fixed a real bug this stretch goal's own premise would have hit
      immediately: `kernel/boot/uefi/boot.c`'s `find_whole_disk_block_io`
      picked "the first non-partition Block I/O handle" - correct by
      accident under QEMU (exactly one disk ever attached) but a coin
      flip on any real machine with more than one drive (an internal SSD
      plus the USB stick this is meant to boot from). Rewrote it to walk
      UEFI device paths instead: get the handle this app was actually
      loaded from (`EFI_LOADED_IMAGE_PROTOCOL.DeviceHandle`), find its
      device path, and match the *disk* by finding another handle whose
      path is exactly that path's prefix (everything but the trailing
      partition node) - the standard technique real UEFI OS loaders use,
      newly added `EFI_DEVICE_PATH_PROTOCOL` support in `efi_proto.h` to
      make possible. Verified in QEMU by attaching a second, decoy disk
      and confirming the kernel still boots from the real one - the exact
      scenario the old code would have gotten wrong, made reproducible
      without needing real multi-disk hardware to catch it.
- [x] `tools/write-usb.sh`: writes `build/os-image.bin` onto a real USB
      drive (macOS via `diskutil`/raw `dd`, Linux via `lsblk`/`dd`),
      refusing obviously-wrong targets (the system disk, non-removable
      media) and requiring a typed confirmation of the exact device path
      before doing anything destructive.
- [x] `docs/real-hardware.md`: the full runbook - firmware settings
      needed (Secure Boot off, UEFI not CSM/Legacy boot mode), what
      normal boot output looks like, and which self-tests are *expected*
      to report "not found" on real hardware rather than indicating a
      real failure.
- [ ] The actual manual step: write the image to a drive, boot a real
      x86_64 UEFI machine from it, and confirm the framebuffer console
      comes up and boot self-tests run to completion (network/PS2 caveats
      in the runbook notwithstanding).

## Path to a usable OS (M29+)

Everything through M28 proves the stack end-to-end (boot → kernel → user
space → GUI apps → network) but the desktop itself is still a demo: windows
can be focused and minimized but not closed, dragged, or resized, there's
one hardcoded desktop icon, and there's no app you'd actually use day to
day. M29 is a robustness/cleanup pass first - closable windows and process
teardown need to share one reclaim path, and that path needs to be solid
before more UI leans on it - then M30+ builds out the window-manager UX and
app baseline toward something worth calling a usable OS.

M29 through M33 are now all done, save M28/M29's one shared manual step
(actually booting a physical machine from USB - needs real hardware this
environment doesn't have). Every window can be closed, minimized, maximized,
dragged, and resized; the desktop has a real multi-icon grid, Alt-Tab, and
clipboard; and there's a real text editor, file manager, and settings panel
to use once you're in. What's still true: click/drag/keychord-*reaching* the
right code (as opposed to the code itself being correct, which every
kernel-side self-test above does verify) is manual/interactive-only,
same boundary this project drew starting at M18's mouse driver.

## M29 — Robustness & foundational cleanup ✅ (except the one manual step)

- [x] Client-crash cleanup: `SYS_task_alive` (`system_api/include/syscall.h`,
      non-reaping, distinguishes a clean `SYS_exit(0)` from a nonzero/
      signal death) + `SYS_pipe_reset` (recycles a dead window's event
      pipe for the next connection without misdelivering stale events) are
      new syscalls; `compositor.c`'s `reap_dead_clients`/`reclaim_window`
      poll every connected client once per main-loop iteration and free
      its window slot - for real, for reuse - the instant it dies
      *unexpectedly*. Caught a real regression via testing: M20's wm_demo
      self-test draws one frame and exits(0) *on purpose*, which an
      earlier version of this reclaimed immediately, wiping the frame
      before the kernel's own pixel-check could run - the crashed-vs-
      clean-exit distinction above exists specifically because of that.
      Deliberately does not free the shm segment behind a reclaimed
      window (no SYS_shm_free exists, and freeing it out from under this
      process's own still-live vmm mapping would alias physical memory
      to whatever's allocated next - worse than the leak); MAX_WINDOWS/
      MAX_SHM_SEGMENTS' existing headroom absorbs it, and a slot that's
      out of segments simply fails cleanly instead of corrupting anything
- [x] Fixed-size table audit: task table, fd table, named-pipe table, shm
      segment table, and the compositor's own window-slot table all
      already failed cleanly (return -1/NULL) at their syscall/request
      boundary before this milestone touched anything - verified by
      reading every allocator, not assumed. PCI/NIC rings (rtl8139.c)
      aren't reachable from user space at all yet (no raw-socket syscall
      exists), so there's no unprivileged path to audit there today
- [x] OOM handling: `pmm_try_alloc_frame` (`kernel/mm/pmm.c`) is a new
      non-panicking sibling to `pmm_alloc_frame` - `SYS_sbrk` and
      `shm_create` (the only two allocation paths a user program's own
      request size can drive to genuine physical exhaustion) now use it
      and fail the syscall (-1) instead of panicking the whole kernel;
      `sbrk` leaves whatever it already mapped in place (still valid,
      just more than this one call needed), `shm_create` unwinds exactly
      what it allocated. `kmalloc`/`pmm_alloc_frame` elsewhere still
      panic on purpose - every other call site is kernel-internal
      bookkeeping with no per-caller failure path of its own to hand
      "out of memory" back through, the same category as `pmm_init`
      finding no usable memory at all
- [x] leanfs error-path audit: full-disk and racing-a-crash-mid-write
      (partial block allocation) were already rolled back/failed cleanly.
      Found and fixed a real one while reading the load path: on-disk
      superblock fields (`data_blocks`, `inode_table_sectors`,
      `bitmap_sectors`) were trusted unchecked and directly size fixed
      buffers (`leanfs_init`'s stack-allocated inode-table read, the
      static bitmap array) - a corrupted field would have overrun them.
      Now validated against their compile-time-expected values at load
      (any mismatch reformats, the same recovery path a bad magic already
      used) and a new `block_valid` bounds-checks every block number
      pulled from an inode's `direct[]`/`indirect` fields or an indirect
      table before it reaches `bitmap_clear` or an ATA read - a corrupted
      inode now fails the read/skips the block instead of walking off the
      bitmap array or reading an arbitrary disk LBA
- [x] `tools/qemu-serial-test.sh` is now a real pass/fail regression
      harness: boots for `SECONDS` (default bumped 2 -> 20, enough to
      carry a from-scratch boot through every self-test into the M22/M23
      desktop handoff), then greps the capture for `*** KERNEL PANIC:`
      and 18 required per-self-test "passed"/"verified" boot markers
      (one per milestone from M4 through M27's own self-test, plus the
      M13 init handoff as the final "reached steady state" checkpoint),
      printing exactly what's missing and exiting nonzero on any failure
      - closes the actual gap behind `docs/real-hardware.md`'s
      pre-existing claim that this script already "grades automatically"
- [x] Sweep of every `panic()` call site under `kernel/drivers/`,
      `kernel/mm/`, `kernel/fs/`, `kernel/ipc/`, `kernel/net/`,
      `kernel/acpi/`, and `kernel/arch/`: found and fixed one real
      "assumes present, panics if not" gap directly relevant to M28's
      still-open real-hardware step - `kernel/acpi/acpi.c`'s `table_at`
      panicked if any ACPI table (RSDT/XSDT/MADT) sat above the 1 GiB
      identity-mapped region, which real firmware placing tables in high
      reserved memory (never an issue on QEMU, routine on real hardware)
      would have hit at boot, on real hardware, with no way to recover -
      exactly the class of bug M27's own notes flagged for RTL8139/PS2.
      Now returns NULL and every caller falls back to "continuing
      single-core," the same degraded-but-booting outcome ACPI already
      had for a missing RSDP or MADT. Every other panic reviewed is
      either kernel-internal bookkeeping with no user-reachable trigger
      (pmm/heap double-free checks, vmm invariant checks) or a
      genuinely-required-hardware path (ATA disk, framebuffer) where
      "absent" isn't a real outcome to degrade into
- [ ] Close out M28's one remaining checkbox: the actual manual USB-boot-
      on-real-hardware step - needs a physical x86_64 UEFI machine and a
      USB drive, so it's still the one item in this milestone (and M28's)
      that has to happen outside this environment; everything the runbook
      (`docs/real-hardware.md`) needs from the software side is done

## M30 — Window chrome: close / minimize / maximize ✅

- [x] Titlebar close/minimize/maximize hit-boxes: three flat-colored
      14x14 squares, right-aligned in the titlebar (`draw_titlebar_buttons`/
      `titlebar_button_rect`, `user_space/bin/compositor.c`), hit-tested in
      `handle_mouse` before the existing focus/hit-test logic so a button
      click never also falls through to a focus-changing click on
      whatever's underneath
- [x] `system_api/include/wm.h` protocol: `WM_ACTION_CLOSE` (SIGTERMs the
      owning client; the process's own termination is what reclaims the
      slot, via M29's `reap_dead_clients` - the literal "closed and
      crashed share one path" M29's intro promised, not a second copy),
      `WM_ACTION_MAXIMIZE`/`WM_ACTION_RESTORE` (saves/restores x/y/w/h;
      maximize targets full-screen-minus-panel but clamps to the
      window's own shm-backed buffer size - there's no resize protocol
      yet, M31's job, so this repositions rather than ever reading past
      what a window's buffer actually holds)
- [x] `apply_window_action()` is the single function both a titlebar
      click and an external `WM_ACTION_PIPE` request (`accept_pending_action`)
      call - the existing panel click-to-minimize (M22) and the new
      titlebar minimize button drive the exact same code, not two copies
- [x] `wm_send_action()` needed no signature change (already generic over
      any `wm_action_type_t`) - what "free" close/minimize/maximize
      buttons actually required was the compositor drawing/handling its
      own chrome, not any client-side code; gui_clock/gui_paint/
      gui_terminal are unmodified and get all three
- [x] New kernel-side self-test (`kernel/kernel.c`, `[wm30]`): drives a
      real running gui_clock purely over `WM_ACTION_PIPE` (maximize,
      restore, minimize, un-minimize, close) and confirms each step via
      real framebuffer pixel reads plus `SYS_wait` catching the closed
      client's SIGTERM exit code - 6/6 checks passed. What this doesn't
      (and can't, headlessly) prove is a real mouse click landing on a
      button's exact pixels; that's manual/interactive verification, the
      same category M21's own self-test already deferred focus-follows-
      click to

## M31 — Window dragging & resizing ✅

- [x] Mouse-down-on-titlebar (excluding the M30 buttons) starts a real
      move-drag state machine (`drag_mode`/`drag_window` and friends,
      `user_space/bin/compositor.c`) - once active, every further mouse
      event updates the dragged window's position directly instead of
      going through the normal hit-test/focus/content-event-forwarding
      path at all, until button-up ends it
- [x] Resize via 5px edge/corner hit zones around each ordinary window's
      outer (border-inclusive) rect (`resize_hit_mask`) - checked ahead
      of the move-drag check so a corner wins over "click the titlebar."
      Minimum size 60x40 (`MIN_WIN_W`/`MIN_WIN_H`). Like M30's maximize,
      clamped to never exceed the window's own fixed shm-backed buffer
      size (`buf_w`/`buf_h`) - there's still no protocol for a client to
      grow its own buffer, so this can shrink a window from its native
      size but never stretch it past what its buffer actually holds; the
      opposite (undragged) edge is always kept fixed by deriving the
      dragged edge's new position from the clamped new width/height,
      not by tracking and separately patching it
- [x] Move is bounds-clamped: at least 40px of a dragged window's
      titlebar must stay on-screen horizontally, its top can't go above
      the screen's own top edge, and its titlebar bottom can't dip below
      a docked panel's top edge - so a window can never be dragged fully
      off-screen or parked unreachably behind the panel
- [x] Not automated, by design: whether a real mouse click/drag at the
      *right pixel coordinates* actually reaches this state machine needs
      real hardware-shaped input this headless build has no way to
      fabricate - the same boundary M18's mouse driver, M21's focus-
      follows-click, and M22's click-to-minimize already drew between
      "self-test proves the logic" and "manual/interactive QEMU-monitor
      verification proves a real click reaches it." The build is clean
      and the full boot regression suite (`tools/qemu-serial-test.sh`)
      still passes with zero behavior change to anything upstream of
      this.

## M32 — Desktop & input polish ✅

- [x] Multiple desktop icons with a real grid layout: `ICONS[]`
      (`user_space/bin/desktop_icons.c`) now lists Terminal/Clock/Paint,
      laid out top-to-bottom-then-wrap-to-a-new-column by `layout_icons`
      from the window's own actual height (never hardcoded) - leaves
      `PANEL_MARGIN` clear at the bottom so a last-row icon can never end
      up drawn underneath (and unreachable behind) desktop_shell.c's
      always-on-top panel. Double-click detection is now per-icon
      (`last_click_icon`) instead of assuming there's only one
- [x] Alt-Tab window switching (`alt_tab_cycle`, `compositor.c`):
      intercepted in `handle_keyboard` before any client ever sees it,
      cycling forward through every alive ordinary window and reusing
      `apply_window_action(..., WM_ACTION_FOCUS)` - the same M30 focus/
      un-minimize logic a titlebar or panel click already drives, not a
      third copy of it. Needed real Ctrl/Alt tracking that didn't exist
      in this kernel at all before this milestone (`kernel/drivers/
      keyboard.c` only ever tracked Shift internally) - new
      `keyboard_modifiers()` plus a `SYS_kbd_modifiers` syscall expose it
      as a live bitmask (`system_api/include/input.h`'s `KBD_MOD_*`),
      since the buffered ASCII stream alone can't tell a plain Tab from
      Alt+Tab
- [x] Copy/paste: a new single kernel-owned global clipboard
      (`kernel/ipc/clipboard.h/.c`, `SYS_clipboard_set`/`SYS_clipboard_get`)
      - same "one flat global, no per-process scoping" simplicity
      `kernel/ipc/shm.h` already uses for shm ids. `gui_terminal.c` wires
      Ctrl+C to copy the current input line (there's no text-selection UI
      to copy *from* instead) and Ctrl+V to paste at the end of it -
      works both within one terminal and between two independently
      spawned terminal windows, proving the clipboard is genuinely
      cross-process and not per-window state
- [x] New kernel-side self-test (`[clipboard]`): round-trips real bytes
      through `SYS_clipboard_set`/`SYS_clipboard_get` directly from
      kernel context, no process spawning needed - same shape as M14's
      own `SYS_pipe` self-test. Whether a real Alt+Tab or Ctrl+C/V key
      chord actually reaches this logic from real keyboard hardware is,
      like M31's drag/resize, left to manual/interactive verification -
      the same boundary M18/M21/M22/M31 already drew for click- and
      key-driven behavior this headless build has no way to fabricate

## M33 — Core usable-OS app baseline ✅

- [x] Text editor (`user_space/bin/text_editor.c`): real multi-line,
      cursor-addressable editing (Up/Down/Left/Right/Backspace/typing
      all work anywhere in already-loaded text) built on a new kernel
      capability this needed and didn't have - arrow-key decoding
      (`kernel/drivers/keyboard.c` only ever decoded the plain scancode
      block before this; the four arrow keys are the first 0xE0-prefixed
      extended scancodes this driver understands) and a new
      `SYS_writefile` syscall (`vfs_write` already existed and was
      already used internally, just never reachable from user space -
      the missing write half of `SYS_readfile`). One real, documented
      simplification: Enter always appends a new line at the *end* of
      the file rather than splitting the current line at the cursor
      (that needs each line to be its own growable slot, not a fixed
      `MAX_LINES` table of fixed-size buffers) - you can fix a typo
      anywhere, new lines only ever get added at the bottom. Ctrl+S saves
      to the filename the process was spawned with (`untitled` if none)
- [x] GUI file manager (`user_space/bin/file_manager.c`): lists every
      file via `SYS_listfiles` (leanfs is flat - this genuinely is the
      whole namespace), Up/Down + Enter or a mouse double-click opens the
      selection in `text_editor.c` (no per-file type metadata exists to
      make a smarter guess from), refreshes every second so a file just
      saved from a concurrently open editor shows up on its own
- [x] Settings/control panel (`user_space/bin/settings.c`): two real,
      live controls, not a static info readout - a desktop background
      color picker (new `WM_SETTINGS_PIPE`/`wm_set_bg_color`, the
      compositor's first global non-per-window setting - `BG_COLOR` in
      `compositor.c` was a compile-time constant before this) and a
      clipboard viewer + Clear button (M32's `SYS_clipboard_*`). Display
      resolution and uptime are shown too, read live via `SYS_fb_info`/
      `SYS_uptime_ms`, not hardcoded
- [x] All three are reachable, not just present on disk: added as three
      new `desktop_icons.c` grid entries (Editor/Files/Settings,
      alongside the existing Terminal/Clock/Paint)
- [x] Two new automated self-tests: `SYS_writefile`/`SYS_readfile` round-
      trip real bytes through a real file (`[vfs]`), and `WM_SETTINGS_PIPE`
      is driven directly (no GUI client needed) with a framebuffer pixel
      check proving the desktop background genuinely changed (`[settings]`,
      same shape as M30's own `WM_ACTION_PIPE` self-test). Whether real
      typing/clicking in text_editor.c or file_manager.c's UI reaches the
      right pixels is, like every other keyboard/mouse-driven behavior
      since M18, left to manual/interactive verification - this headless
      build has no way to fabricate real hardware input

## Path to a polished desktop UI (M34+)

M0-M33 built a genuinely usable desktop, but every app hand-rolls its own
widgets straight out of `gfx.h`'s five primitives (pixel/rect/outline/
line/text) - there is no menu of any kind anywhere in the codebase, no
dialog/prompt primitive (`text_editor.c` can only ever save to the
filename it was spawned with), no visible scrollbar (file_manager.c
scrolls a list with no on-screen indicator), and no text selection
outside of gui_terminal.c's own line buffer (M32 explicitly flagged
this as a known gap - Ctrl+C copies "the current input line" for lack
of anything to select instead). The single 8x16 bitmap font (M17) is
also still the only font in the system, used unscaled and unstyled for
every label, title, and button anywhere. This arc is about closing
those specific, already-identified gaps rather than expanding scope
into new subsystems.

M34 through M38 are now all done. Every gap named above is closed: a
shared widget layer (buttons, menus, a scrollbar) backs settings.c,
file_manager.c, desktop_icons.c, and text_editor.c instead of each
hand-rolling its own; text_editor.c has a real File menu, a Save As
prompt, and a confirm-discard dialog on both its own Quit item and the
titlebar close button (a new opt-in, backward-compatible WM protocol
field); text_editor.c and gui_terminal.c both support real click-drag
text selection wired into the clipboard; and the compositor now draws
window titles at all (a gap only found while scoping M38, not one this
arc set out looking for), plus a drop shadow, edge-aware resize
cursors, and a second (accent) theme color. Two deliberate, documented
scope trims recur throughout: no compositor-owned modal/popup surface
(M35's menus and M36's dialogs are drawn client-side, inside the
owning app's own window, not overlaying the whole desktop), and
gui_terminal.c's output view still has no real scrollback buffer to
put a scrollbar on (M37) - both real, separate features this arc
didn't also take on.

## M34 — Reusable widget primitives ✅

- [x] `user_space/lib/gfx.{h,c}` gained `gfx_point_in_rect` (pure
      geometry - no `gfx_ctx_t` needed) and `gfx_draw_button` (filled
      rect + 1px border + centered label) - the two pieces that were
      actually duplicated verbatim across this codebase, not a larger
      speculative widget set added ahead of a real consumer (checkbox/
      scrollbar widgets are added in M37/M38 alongside their first real
      use, same "not written speculatively" principle wmclient.h's own
      header comment already follows)
- [x] Real dedup, not a parallel unused library: `settings.c`'s
      hand-rolled `point_in_rect` and Clear-button
      `gfx_fill_rect`+`gfx_draw_text` pair are gone, replaced by
      `gfx_point_in_rect`/`gfx_draw_button`; `compositor.c`'s identical
      `point_in_rect` is gone too, replaced the same way for its
      titlebar-button hit test; `desktop_icons.c`'s `point_in_icon`
      now expresses its box+label hit region as one `gfx_point_in_rect`
      call instead of four hand-written comparisons
- [x] Deliberately did *not* touch `compositor.c`'s own drawing
      (`draw_titlebar_buttons` still calls this file's own clip-rect-
      aware `fill_rect`, not `gfx_fill_rect`) - `gfx.c`'s primitives
      only clip to a `gfx_ctx_t`'s own 0..width/height, with no idea
      compositor.c's `clip_x0..clip_y1` partial-redraw rect (M20/M21's
      whole reason a full-screen recomposite doesn't happen on every
      mouse-move) even exists; routing button *drawing* through it
      would silently undo that optimization on every cursor tick. Pure
      hit-testing (`gfx_point_in_rect`) has no such cost - O(1)
      comparisons, not a per-pixel loop - so it was safe to share as-is
- [x] Verified via `tools/qemu-serial-test.sh`: 22/22 required boot
      markers, no panic, zero behavior change to anything upstream of
      this milestone

## M35 — Menus (menu bars + context menus) ✅

- [x] Scope trim, decided up front rather than discovered mid-
      milestone: a compositor-owned popup surface (a new WM protocol
      primitive, floating above every window regardless of which one's
      focused) is real, separate work - a new request pipe, a new
      overlay z-order layer, modal-vs-not input routing decisions -
      disproportionate to what "add a menu" actually needed here. Both
      menus below are drawn and hit-tested entirely inside the owning
      client's own window buffer instead (`gfx_draw_menu`/
      `gfx_menu_hit_test`, `user_space/lib/gfx.{h,c}` - two real,
      simultaneous consumers is what earned this its own helper rather
      than each call site looping over M34's `gfx_draw_button` by
      hand). Trade-off, same as this project's other documented scope
      trims (text_editor.c's M33 Enter-at-end note is the closest
      precedent): a dropdown can't visually extend past its own
      window's edges, and dismissing one is click-elsewhere-inside-
      that-window, not a system-wide popup grab
- [x] File menu bar in `text_editor.c`: a new top strip (`MENU_ROWS`,
      shrinking `TEXT_ROWS` by one row - the same "reserve a row" shape
      `STATUS_ROWS` already used) with a "File" label that toggles a
      Save/Quit dropdown. Kept to the two items that need no new
      infrastructure - a filename-prompting "Save As" and a data-
      safe "New"/close both need M36's dialog primitive first, so
      they're deferred there rather than half-built here
- [x] Right-click context menu on the desktop background
      (`desktop_icons.c`): `mouse_event_t.buttons` bit1 (right button)
      has been decoded end-to-end since M18's mouse driver but never
      read by any client until now - opens a "Terminal"/"Settings"
      quick-launch menu at the click point (clamped to stay fully
      on-screen), reusing the exact same `gfx_draw_menu`/
      `gfx_menu_hit_test` pair the File menu uses. (Desktop *icons*
      themselves keep plain double-click-to-launch - a context menu on
      a single-purpose icon with no rename/delete/properties yet
      wouldn't have anything real to put in it)
- [x] Verified via `tools/qemu-serial-test.sh`: 22/22 required boot
      markers, no panic - this milestone touches no kernel code at all,
      so the regression suite is confirming zero behavior change to
      every self-test that came before it. Whether a real right-click
      or a File-label click actually lands on these pixels is, like
      every mouse-driven behavior since M18, manual/interactive-only
      verification this headless build has no way to fabricate

## M36 — Dialogs & the file save flow ✅

- [x] Same scope trim as M35, for the same reason: no compositor-owned
      modal surface. Both prompts below are a centered overlay drawn
      into `text_editor.c`'s own window buffer that owns that window's
      input stream until answered (mouse-click-anywhere or Y/N key) -
      modal to this one app, not the whole desktop
- [x] Save As prompt: File menu gained a third item (menu is now New/
      Save/Save As/Quit) that opens a filename input box seeded with
      the current filename; Enter saves under the typed name
      (`sys_writefile`), a click anywhere cancels. Closes the gap M33
      flagged - Ctrl+S could previously only ever save to the filename
      the process was spawned with
- [x] Confirm-discard dialog: "New" and "Quit" both route through one
      shared `request_action()` that checks `dirty` first and only
      prompts (`Y = discard, N/click = cancel`) when there
      genuinely are unsaved changes to lose - a clean file goes
      straight through with no interruption
- [x] The titlebar close button needed a real protocol change to reach
      this dialog at all: M30's `WM_ACTION_CLOSE` was an unconditional
      SIGTERM with no way for a client to intervene. New opt-in field
      `wm_create_request_t.confirm_close` (`system_api/include/wm.h`) -
      0 for every existing client (`wm_connect`/`_panel`/`_desktop` all
      pass it explicitly, so nothing about their behavior changes) - and
      a new `wm_connect_confirm_close()` only `text_editor.c` calls.
      When set, `compositor.c`'s `apply_window_action` sends
      `WM_EVENT_CLOSE_REQUEST` to the window's own event pipe instead
      of `sys_kill`, and leaves the client running until it exits on
      its own - the same "app that doesn't implement it just doesn't
      respond to this event" contract X11's `WM_DELETE_WINDOW` uses,
      not a new way to force a close
- [x] One deliberate, documented convention: a self-chosen close always
      calls `sys_exit(1)`, not `sys_exit(0)` - M29's `reap_dead_clients`
      only reclaims a window slot on a *nonzero* exit (a clean exit(0)
      is treated as "meant to still be showing something," per
      wm_demo's own self-test), so an exit(0) here would leave a
      stale, unclosable window on screen despite the process actually
      being gone
- [x] New kernel-side self-test `[wm36]`: spawns compositor + a real
      `text_editor` (untitled, unedited - so it takes the immediate,
      not-dirty branch), sends `WM_ACTION_CLOSE` over `WM_ACTION_PIPE`
      (same headless-protocol-driving shape as M30's own self-test),
      and checks three things real framebuffer pixel reads plus
      `SYS_wait` can prove without any simulated keyboard: the window
      showed its own background before the close request, the slot was
      genuinely reclaimed after, and the exit code was text_editor's
      own `1` - not `128+SIGTERM` - proving the new event-based path
      fired instead of the old direct kill. The dirty-and-prompts path
      itself needs a real keypress to ever get dirty in the first
      place - like every keyboard/mouse-driven behavior since M18,
      that's manual/interactive-only verification this headless test
      can't fabricate. Verified via `tools/qemu-serial-test.sh`:
      23/23 required boot markers, no panic

## M37 — Text selection & scrollbars ✅

- [x] Click-drag text selection with highlight rendering in both
      `text_editor.c` (over its `lines[]` buffer, grid coordinates
      derived the same way `clamp_cursor`'s own row/col math already
      does) and `gui_terminal.c` (over its fixed `grid[ROWS][COLS]`
      output buffer) - `sel_dragging` (button physically held) vs.
      `sel_active` (survives release so the highlight and a later
      Ctrl+C still have something to act on) is the same small state
      machine in both files. A plain click with no drag in
      `text_editor.c` just moves the cursor there - mouse-driven cursor
      placement it never had before this, not only selection
- [x] Ctrl+C in both apps now copies a real selection
      (`copy_selection_to_clipboard`, row-major, trims trailing padding
      off `gui_terminal.c`'s space-filled grid rows) when one is
      active, replacing M32's "copy the whole current input line"
      stand-in - `gui_terminal.c`'s own header comment named this
      exact gap. Falls back to the old line-copy behavior when nothing
      is selected, so existing muscle memory (copy the line you're
      typing) still works unchanged
- [x] Visible scrollbar (`gfx_draw_scrollbar`, `user_space/lib/gfx.{h,c}` -
      a track plus a thumb sized/positioned from `total_items`/
      `visible_items`/`scroll_top`) wired into `file_manager.c`'s file
      list, which previously scrolled with no on-screen indicator of
      position or extent at all
- [x] Scope trim: `gui_terminal.c`'s "scrollback" isn't a real
      milestone target here - `grid_scroll()` (M13-era) discards a row
      the instant it scrolls off the top; there is no history buffer
      behind the visible `ROWS` at all to put a scrollbar on. Adding
      one is a real, separate feature (a growable output history plus
      a viewport into it) this milestone doesn't also take on -
      selection above already works over whatever's currently visible
- [x] Verified via `tools/qemu-serial-test.sh`: 23/23 required boot
      markers, no panic - this milestone touches no kernel code, so
      the suite confirms zero regression to everything upstream.
      Whether a real click-drag lands on the right pixels is, like
      every mouse-driven behavior since M18, manual/interactive-only
      verification this headless build can't fabricate

## M38 — Visual chrome polish ✅

- [x] Window titles, drawn on the titlebar for the first time ever -
      a real, pre-existing gap found while scoping this milestone:
      `wm_create_request_t.title` has been tracked per-window and
      echoed through `wm_query_windows` since M22, but `compositor.c`
      itself never actually painted it anywhere before this; a
      titlebar was a flat colored strip plus three buttons and nothing
      else. Drawn bold - a faux weight synthesized from the one 8x16
      bitmap font this project has (`bits | (bits >> 1)` per glyph
      row; no floating point exists anywhere in this toolchain per
      `gfx.h`'s own build-flag note, so there's no vector/anti-aliased
      path to a real second weight, only this kind of cheap integer
      trick) via a new clip-rect-aware `draw_text_clipped` - same
      reason `gfx_draw_button` couldn't be reused for titlebar buttons
      back in M34, now applying to titlebar text too. `fit_title`
      truncates to whatever fits before the leftmost button, so a
      window shrunk near M31's `MIN_WIN_W` can't draw a title straight
      through the close button
- [x] Window drop shadow (`fill_rect_shadow`) - blends toward black
      (1/3 opacity, plain integer math) rather than a flat offset
      rect, which would just look like a second window. Drawn before
      each window's own border/titlebar/content, offset down-right, so
      only the sliver the window itself doesn't cover ends up visible
- [x] Edge-aware resize cursors: four new hand-authored 8x8 sprites
      (horizontal/vertical/both diagonals) alongside the existing
      arrow - `hovered_resize_mask()` reuses M31's own
      `resize_hit_mask` (or the in-progress `drag_resize_mask`, so the
      shape doesn't flicker back to the arrow mid-drag if the cursor
      drifts outside the exact edge margin) to pick which one
      `draw_cursor` shows, every redraw
- [x] Accent color theming: `wm_settings_request_t` gained
      `accent_color` (the compositor's second global setting, replacing
      the compile-time `TITLEBAR_FOCUS_COLOR` constant a focused
      titlebar always used before this) alongside the existing
      `bg_color` - `wmclient.h`'s `wm_set_bg_color` became
      `wm_set_theme(bg, accent)` (settings.c was and still is its only
      caller, so no back-compat shim needed). `settings.c` gained a
      second swatch row and tracks its own current choice of both
      colors so picking one never resets the other back to default.
      Scoped to the titlebar only, not "buttons" as first drafted -
      the titlebar close/maximize/minimize colors are a semantic
      convention (red/green/gray), not decoration, and there's no
      live way for an arbitrary client (menus, etc.) to read the
      compositor's current theme back out to recolor itself with -
      that's a real, separate query-protocol addition this milestone
      doesn't also take on
- [x] New kernel-side self-test `[wm38]`: two of the four additions
      above are real framebuffer pixel effects checkable headlessly -
      a probe point just past gui_clock's own outer border against
      the exact expected shadow-blend value, and a titlebar pixel
      confirming a `WM_SETTINGS_PIPE` request with a new `accent_color`
      actually repainted the focused titlebar (same "drive it
      directly, no GUI client needed" shape as M33's own settings
      self-test). The bold glyphs and the resize-cursor sprites are,
      like every font/pointer-shape detail since M17/M18, left to
      manual/interactive verification - no single pixel headlessly
      distinguishes "bold" from "regular," and there's no way to
      fabricate a real mouse hovering a resize edge. Verified via
      `tools/qemu-serial-test.sh`: 24/24 required boot markers, no
      panic

## M39 — Sharper system text everywhere ✅

The premise this milestone started from, found while scoping it: the
reason text looked soft across this OS was never the renderer, it was
the glyphs. Every text path in the project - `console.c`'s kernel
console, `gfx_draw_char`, and `compositor.c`'s own `draw_char_clipped` -
was already a correct, exact 1:1 blit of one byte-per-row bitmap onto a
pixel grid. No scaling, no resampling, no filtering anywhere in that
path that *could* blur anything. What all three were faithfully
blitting was a malformed `font8x16[128][16]`: 'A' had no clean apex and
two stems of different weights, 'o' was a lopsided ring with stray
pixels above and below, 'i' and '1' each carried a doubled two-row
baseline bar no other glyph had, and - the one that dominated
everything else - glyphs routinely used all 8 columns of their cell, so
adjacent letters physically touched. Fixing the table fixed text in
every one of those places at once, with no change to any drawing code.
That's what made "everywhere" one milestone instead of a per-app sweep.

- [x] Re-authored all 95 printable glyphs (0x20-0x7E) on one enforced
      metric: shared baseline (row 11), shared cap line (row 2), shared
      x-height line (row 5), shared descender row (row 14), uniform 2px
      stems. The old table held none of these consistently - glyphs
      started and ended on whatever row they happened to, which is why a
      line of text read as visually noisy even though each glyph was
      blitted perfectly
- [x] **Column 7 of every cell is now reserved blank** - the single
      biggest legibility win, and not part of the original plan. It
      turned out the old font had no inter-character gap at all: text
      drawing advances exactly `FONT_WIDTH` with no tracking of its own,
      so the gap has to live *inside* the cell, and glyphs that used
      column 7 simply collided with the next letter. Restricting every
      glyph to columns 0-6 gives uniform 1px letter spacing everywhere
      for free
- [x] Both copies generated from one source. `tools/gen-font.c` is a
      *host* program (ordinary libc, built with `$(HOSTCC)`, never runs
      as part of the OS - the same arrangement `tools/leanfs-put.c`
      already used) holding the glyph art plus the metric checks, and
      emitting all four files. The duplication itself is unchanged and
      unavoidable (a user program still can't link against the kernel
      image); what's gone is the hand-sync. Worth restating: the two
      tables were verified in sync *before* this milestone changed
      anything - byte-identical glyph data - so this prevented a
      divergence rather than repairing one
- [x] `make font-check` wired into the build via a stamp file both
      `font8x16.o` objects depend on, so neither can compile while a
      checked-in table disagrees with the generator. Verified by
      hand-editing a byte and confirming the build fails with a pointed
      message rather than silently shipping. A stamp rather than a
      `.PHONY` prerequisite so this stays incremental and doesn't
      relink the kernel on every build
- [x] Real generated bold weight replacing M38's runtime faux-bold.
      M38's `bits | (bits >> 1)` thickened rightward *within the byte*,
      so ink already in column 7 shifted off the end and was silently
      dropped - the old 'A', whose bottom rows were `0xE7`, lost its
      thickening on exactly the stem that most needed it. With column 7
      now reserved, that same one-column dilation is lossless, so
      `font8x16_bold` is generated once by `gen-font.c` and
      `draw_char_clipped` became a table lookup instead of per-pixel
      arithmetic on every redraw. Deviation from the plan, stated
      plainly: this is a generated dilation, not a separately
      hand-drawn second weight - the reserved column is what makes it
      correct, and hand-authoring 95 more glyphs bought nothing on top
      of that
- [x] Scope trim held as planned: no anti-aliasing. It's the obvious
      thing to reach for under "sharper" and it's the wrong tool at
      this size - it trades edge contrast for perceived smoothness,
      which on an 8px-wide cell means blurrier text, not sharper. It
      would also need a higher-resolution glyph source to downsample
      from (8x16 bitmaps carry no sub-pixel information to recover).
      Crisp, hinted bitmaps aligned to the pixel grid are the correct
      answer for a fixed-size bitmap font
- [x] Scope trim held as planned: no scalable/multi-size text. Every
      consumer hardcodes `FONT_WIDTH`/`FONT_HEIGHT` for layout
      (`console.c` derives `cols`/`rows` from them, `gfx_draw_button`
      centers labels with them, `gui_terminal.c`'s fixed
      `grid[ROWS][COLS]` is sized by them), so a second size is a
      layout-system change across ~9 files, not a font change
- [x] Two pre-existing self-tests needed re-aiming, which is worth
      recording rather than burying: M21's and M22's pixel checks each
      probe a specific pixel *inside* a caption glyph ('C' in
      "CLOCK"/"Clock", 'P' in "PAINT"), and those coordinates are
      coupled to the letterform by construction. Re-authoring 'C' and
      'P' inverted them - the probes now sit on the left stem (ink) and
      the bowl interior (background) of the new shapes. Not a
      regression in either test, and their neighbouring probes
      deliberately sample flat fills for exactly this reason
- [x] New self-test `[font39]`, and unlike every GUI-facing milestone
      since M18 it needs no manual/interactive caveat - glyph geometry
      is exact data, not a mouse hover or a "does it look bold"
      judgement call. Two halves. The table half proves what actually
      shipped inside the kernel image is what `gen-font.c`'s checks
      passed on (column 7 clear in all 128 entries, every printable
      codepoint non-blank, control codes blank, `font8x16_bold` exactly
      the lossless dilation). The rendered half draws "Axg" through the
      real console blit path and reads it back out of the framebuffer:
      'A' must start on the cap line, 'x' on the x-height line, both
      must land on the same baseline, 'g' must reach the descender row,
      and column 7 of all three cells must stay background - M39's
      whole premise measured in real pixels rather than asserted.
      Confirmed non-vacuous by deliberately lifting 'x' one row off the
      baseline and watching it panic with `expected 0000000B got
      0000000A`
- [x] Verified via `tools/qemu-serial-test.sh`: 25/25 required boot
      markers, no panic. Also confirmed visually with a QEMU
      screendump of the booted desktop - the icon labels
      (Terminal/Editor/Files/Settings/Clock/Paint) now sit on one
      baseline with even spacing, where the old font ran them together
      into a single connected mass

## Stretch goals (unordered, orthogonal to the desktop path)

- [x] SMP (multi-core) support
- [x] UEFI boot path as an alternative to BIOS (M24, above; BIOS itself
      later removed in M26, leaving UEFI as the only path)
- [x] Package/build tooling for third-party user programs (M25, above)
- [x] Networking stack + NIC driver (M27, above)
- [~] Port to real hardware (USB boot test) - prep/tooling/runbook done
      (M28, above); the manual boot-on-real-hardware step itself isn't yet

---