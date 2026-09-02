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
- [x] **Repaired after M79** (see the note below): this tool had been
      broken since M53 and nothing noticed, because nothing runs it -
      `make preseed` is optional and no self-test uses it.

### The drift M25's tooling was found in (repaired after M79)

`tools/leanfs-put.c` duplicates leanfs's on-disk format, and duplication
drifts. Found still speaking the M15-era format: an inode with a `name`
field (M53 moved names into directory records), no `mtime` or
`dindirect` (M59 added both), `used` where the format now has `type`, a
32-inode cap the kernel left behind at M53, no concept of a directory at
all, and a 256-byte out-of-bounds stack read. A file it wrote onto a
filesystem that has a `/bin` was unreachable even when the write
reported success.

Rewritten to read its geometry from the *superblock* rather than from
constants recomputed here - that is what a superblock is for, and it
means the tool cannot disagree with the kernel about where the inode
table is - with every remaining compile-time constant checked against
the superblock at runtime, so a mismatch is a refusal naming the file to
fix rather than a corrupt filesystem. It resolves absolute paths through
directory records with `mkdir -p` semantics, and mirrors the kernel's
own `map_block` and `format_fresh`.

The last bug in it is the one worth writing down. Everything above was
correct and the kernel still discarded the result: `LEANFS_MAGIC` here
was M12's `LFS1`, and the kernel has bumped it twice since (M53 `LFS2`,
M59 `LFS3`) precisely so that a disk in an older layout is reformatted
rather than misread. Every write succeeded, the tool reported success on
all 47 programs, and the next boot printed `no valid leanfs superblock
found - formatting fresh` and re-seeded every one of them. It cost three
instrumented boots to find, because a successful write and a silently
discarded filesystem look identical from the host. So a magic that is
neither ours nor a blank region is now an **error** naming both values
and the file to update, not a reformat.

The verification is end-to-end and is the reason any of this is known to
work: preseed an image, boot it under `tools/qemu-serial-test.sh`, and
require **zero** `seeding disk with` lines - the kernel seeds only what
is missing, so zero means it found all 47 programs already on the disk.
Both paths are covered: a fresh format, and a second `make preseed` over
an image that already has one (no duplicate directory records, no leaked
blocks - the bitmap matches block reachability exactly). Both boot
69/69.

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

## Path to a polished Windows/macOS hybrid desktop (M40+)

A reported bug started this arc: double-clicking the "Editor" or "Clock"
desktop icons does nothing, while Terminal/Files/Settings/Paint all work.
Investigation traced leanfs storage, `SYS_spawn`/`elf_load`, and
`desktop_icons.c`'s own hit-test/layout math (hand-simulated at the real
1024x768 boot resolution) and found no bug in any of those layers - both
files are stored and spawned identically to the working icons, and the
six icons' hit-rects don't overlap, wrap, or collide with the panel.
That's a real, if uncomfortable, finding in its own right: it means this
project's long-standing gap (every click/drag/double-click feature since
M18 has only ever been proven by a self-test calling
`apply_window_action()`/`WM_ACTION_PIPE` directly, never by a real mouse
click at real pixel coordinates) has finally hidden a bug behind it
instead of just a theoretical caveat. M40 closes that gap and uses it to
actually find and fix this. M41 through M44 then push the desktop's UX
and visuals further.

M41 shipped a macOS-style top menu bar. Direct design feedback after M41
landed retargeted the rest of this arc: this project is a deliberate
Windows/macOS hybrid, not a macOS clone. An early, uncommitted M42
attempt at a macOS-style Dock (icon tiles, centered, growing/shrinking
around the middle as apps open and close) was built far enough to prove
out and then discarded before ever landing, once it became clear that
wasn't the wanted direction - see M42's own entry for exactly what was
kept from that attempt and what wasn't. M42 as redefined moves the bar
back to the *bottom* and makes it behave like a Windows taskbar (a
Start/launcher button, running-app buttons, a system tray) instead of a
Dock, and moves each app's own File/Edit menu back to being drawn
in-window (Windows convention, and M35's original shape) rather than
living in a shared top bar. M43 adds a Spotlight-style launcher - opened
from the taskbar's Start button or a global keychord - and window
edge-snapping. M44 remains a final visual-consistency pass across
whatever M41-M43 actually shipped.

## M40 — Robustness pass + real interactive-input test harness ✅

- [x] QEMU-monitor-driven input-injection harness: real
      `sendkey`/`mouse_move`/`mouse_button` HMP commands against a booted
      image, with results read back from real framebuffer pixels via
      `screendump`. Landed as `tools/qemu-input-test.sh` (plus
      `tools/qemu_input.py`, the engine, and `tools/qemu_input_suite.py`,
      the tests) alongside the existing boot-marker check - run both
      before every milestone from here on. Two things about QEMU had to
      be learned rather than assumed, both documented where they bite:
      the guest's mouse is *relative*, so `move_to` homes into the
      top-left clamp and steps from there and then **verifies against the
      pointer actually drawn on screen** (it has to - an unverified move
      silently landed 100px short once and read as a guest bug); and
      HMP's `mouse_button` help string ("1=L, 2=M, 4=R") is simply wrong
      about which bit is the right button
- [x] The Editor/Clock bug: reproduced, root-caused and fixed. It was
      never about those two programs - it was **whichever two apps you
      launched third and fourth**, which is why static reading of
      `leanfs.c`/`proc.c`/`elf.c`/`desktop_icons.c` found nothing. Every
      user process inherits its parent's whole fd table, and PID 1
      descends from the kernel's boot task, which permanently held five
      pipe fd-pairs its own self-tests (M14/M30/M33/M36/M38) had opened
      and - there being no `SYS_close` - never released. Those ten fds
      rode into `init` and then into the compositor, which starts with
      12 of its 32 fd slots already spoken for; after its six protocol
      pipes and the desktop+panel event pipes, exactly two remained.
      App three onward got a failed `sys_pipe_open`, a `window_id` of -1,
      and no window at all. Fixed at the root (`sched_reset_fds_to_std`,
      called before `init` is spawned), with `MAX_FDS` 32 -> 64 so an
      8-window compositor isn't a to-the-last-slot fit, and a compositor
      that refuses a window now says so on its own stdout instead of
      failing silently
- [x] Four more real bugs the harness found or the audit uncovered along
      the way, none of which any protocol-level self-test could have seen:
      - **`.bss` was never zeroed.** `objcopy -O binary` drops NOBITS, so
        the ~176 KiB of `.bss` past the end of `kernel.bin` was whatever
        the firmware had left there; C's "statics start zeroed" guarantee
        was being provided by luck. Growing `task_t` moved `tasks[]` into
        a dirtier stretch and the next spawn panicked. `entry.asm` now
        clears it
      - **`process_spawn` published a task before initializing it.** The
        heap/shm cursors were assigned *after* `task_spawn_in` returned,
        with a comment arguing the gap was too short to matter. It
        wasn't: the task is schedulable the instant the lock drops, and
        gets a whole quantum - long enough to reach `malloc` with
        `heap_brk` still zero and map a page at virtual address 0. They
        are now arguments to `task_spawn_in`, set under the same lock
        that makes the task READY
      - **Right-clicks never reached the window under the pointer.**
        Events go to the focused window, and only a *left* press ran the
        focus hit-test - so M35's right-click desktop menu simply never
        opened from a fresh desktop, because the panel holds focus after
        boot. `focus_window_under_cursor` is now shared by both
      - **Alt+Tab dropped its own modifier.** `SYS_kbd_modifiers` sampled
        *live* key state, but every caller asks just after reading a
        buffered character - a full compositor frame later, by which
        point Alt can be released. The modifier bitmask is now captured
        in the IRQ handler and travels with each character
- [x] Double-click detection made latency-independent: `mouse_event_t`
      and `wm_event_t` now carry a `time_ms` stamped by the PS/2
      interrupt handler, and `desktop_icons.c` times the events instead
      of its own two trips round the event loop. It was measuring
      delivery latency, so a full-screen redraw between two clicks
      stretched a real double-click past its own 500ms window - failing
      more often the more windows were open, which is precisely what
      "double-clicking that icon doesn't work" looks like from outside
- [x] `SYS_spawn`'s failure paths audited end to end. The worst finding:
      `elf_load` **panicked the kernel** on any malformed image, and
      `SYS_spawn` has been able to hand it any file on disk since M13 -
      so `sys_spawn("some_text_file", "")` from an ordinary user program
      took the machine down. It also trusted every offset in the headers
      (a truncated file could read past the image buffer; a crafted
      `p_vaddr` could map a segment at a kernel address). It now
      validates everything - and validates it all *before* allocating
      anything, so a refused image costs no frames, no address space and
      no task slot. Out-of-memory mid-load uses `pmm_try_alloc_frame`
      rather than panicking, and a full task table is checked before an
      address space is built rather than after
- [x] Retroactive coverage for already-shipped interactive features:
      titlebar close click (M30), titlebar drag-move (M31), Alt+Tab
      (M32), right-click context menu (M35), single-vs-double click, and
      all six desktop icons - the first real evidence for or against
      everything deferred as "manual/interactive-only" since M18. Two of
      those six were broken (Alt+Tab and the context menu) and had been
      for several milestones
- [x] Stress test: launch/close cycles through real clicks, asserting
      M29's reclaim path holds and that the compositor never starts
      refusing windows
- [x] New boot self-tests for both root causes - the fd-table
      inheritance invariant (checked before *and* after the reset, so it
      fails loudly in both directions) and `SYS_spawn`'s failure paths,
      the latter comparing task count and free-frame count across a
      failed spawn so "returned -1" also has to mean "leaked nothing"
- [x] Both regression scripts now run the disk with `snapshot=on`: they
      take no write lock (so they can run concurrently, and alongside a
      `make`), and every run is genuinely the from-scratch,
      unformatted-disk boot `qemu-serial-test.sh`'s time budget is
      written against. Before this, only the first run after a rebuild
      was - every later one booted the disk the previous run had already
      formatted, quietly skipping the format path it claims to cover
- [x] Full `tools/qemu-serial-test.sh` pass (27/27 markers, zero
      regressions) and `tools/qemu-input-test.sh` pass (7/7)

## M41 — macOS-style top menu bar ✅

- [x] `user_space/bin/menu_bar.c`: an always-on-top, screen-*top*-docked
      panel showing the focused app's name on the left. Reuses
      `compositor.c`'s existing `is_panel` plumbing rather than inventing
      a second mechanism - `wm_create_request_t.panel` went from a 0/1
      flag to `WM_PANEL_NONE/BOTTOM/TOP`, numbered so every existing
      caller and every existing `if (req.panel)` keeps meaning what it
      always did
- [x] `text_editor.c`'s File menu is now the *shared bar's* menu rather
      than a second copy drawn inside its own window - M35's in-window
      menu row is gone and the reclaimed height went back to the text.
      A new three-channel protocol (`WM_MENU_PIPE` to declare,
      `WM_MENU_QUERY_PIPE` to ask what the focused window has,
      `WM_MENU_CMD_PIPE` for the pick coming back as
      `WM_EVENT_MENU_COMMAND`) draws the split as narrowly as it can:
      the owning app still defines its menus and still decides what an
      item *does*; only a list of labels goes out and an index comes
      back. Neither the bar nor the compositor knows what "Save As" means
- [x] The clock moved from the bottom dock's corner to the top bar's
      right edge, taking `format_clock` with it rather than leaving a
      second copy behind - and freeing that corner of the dock for M42
- [x] A dropdown is taller than the bar and narrower than the screen, so
      a panel can now declare a docked height smaller than its buffer
      (`panel_dock_h`) plus one *overhang* rect below it
      (`WM_ACTION_SET_PANEL_OVERHANG`) that the compositor also blits and
      routes clicks into. Both halves matter: growing the panel outright
      would have shoved every window on the desktop down and pulled them
      back up as menus opened, and blitting full width would have painted
      an opaque band across everything either side of the menu (which the
      first version did - caught by looking at it)
- [x] Every `connected_panel_height()` clamp is now edge-aware, behind
      one `content_top_limit()`/`content_bottom_limit()` pair so window
      placement, maximize and drag bounds can't drift apart on which
      bars they remember
- [x] `MAX_WINDOWS` 8 -> `WM_MAX_ROUTABLE_WINDOWS`, itself 10 -> 12 (the
      event-pipe name grew a second digit), and `MAX_SHM_SEGMENTS`
      16 -> 32. A fourth always-on desktop client meant the old caps left
      too few slots for the six apps a desktop icon can launch - M40's
      harness caught it the same day the bar landed, which is exactly
      what it was built for. The compositor holding fewer windows than
      the protocol can route to was never deliberate; the two are tied
      together now
- [x] New boot self-test (`[m41]`, 6/6 checks): the bar really is docked
      at the top edge and only its declared height tall, a maximized
      window's titlebar starts immediately below it while the bar stays
      on top of it, the menu query round-trips the editor's declared File
      menu, and File > Quit reaches the editor as an event it acts on
      (its own clean exit code, not a kill)
- [x] Two new interactive tests through M40's harness - the bar follows
      focus, and a real click on "File" opens the dropdown and picking
      Quit closes the app (including that the dropdown doesn't paint
      outside its own rect)
- [x] `make` now fails with a readable message if a name in
      `USER_PROGRAMS` has no `incbin` block in `embed_programs.asm`.
      Adding `menu_bar` without one produced a wall of
      undefined-reference lines several steps from the cause, and the
      previous good image stayed on disk and kept booting - so every test
      kept passing against a kernel that no longer existed. An hour lost
      to a one-word list
- [x] `tools/qemu-serial-test.sh`'s capture budget 24s -> 40s: M40's two
      new self-tests and M41's fourth desktop client pushed a
      from-scratch boot past it (32s measured failing, 34s passing)
- [x] Full `tools/qemu-serial-test.sh` pass (28/28) and
      `tools/qemu-input-test.sh` pass (9/9)

## M42 — Windows-style bottom taskbar (redesigned; replaces M41's top bar) ✅

Originally scoped as a macOS-style Dock (icon tiles, centered, growing/
shrinking around the middle). Direct design feedback redirected this
mid-flight: no macOS dock, and the bar itself moves from top (M41) back
to the bottom and behaves like a Windows taskbar - this OS is a
deliberate Windows/macOS hybrid, not a clone of either. The Dock attempt
got far enough to be real, uncommitted work (`user_space/lib/appicons.c`/
`.h`, a dock-shaped rewrite of `desktop_shell.c`, matching
`compositor.c`/`kernel.c`/test changes) before being discarded outright -
git history was never touched by it, since none of it was committed.

- [x] `menu_bar.c` (M41) is deleted. `desktop_shell.c` is the single
      system bar again, bottom-docked via the pre-M41 `wm_connect_panel`,
      laid out left to right: a Start button (a 2x2 tile glyph plus the
      word, since this project has no icon-image format), running-app
      buttons (labeled rectangles - the pre-M41 shape, not icon tiles)
      shifted right to clear it, and a right-aligned system tray +
      clock. `format_clock` came back here with the clock rather than
      being left behind as a second copy - it has never existed in two
      places at once
- [x] Each app's own menu (`text_editor.c`'s File menu) is drawn
      in-window again - M35's original shape, `gfx_draw_menu`/
      `gfx_menu_hit_test` called directly, no cross-process round trip.
      That closes out M41's whole `WM_MENU_PIPE` protocol (the four
      pipes, the compositor's per-window declared-menu registry,
      `WM_EVENT_MENU_COMMAND`, `wm_declare_menus`/`wm_query_focused_menus`/
      `wm_send_menu_command`, and `str.h`'s `strlcpy`, which had no other
      caller left) - about 250 lines removed across five files
- [x] `wm.h` simplified back toward its pre-M41 shape: `WM_PANEL_TOP`
      removed (a single `WM_PANEL_BOTTOM` again), `panel_dock_h`/
      `WM_ACTION_SET_PANEL_OVERHANG` and its three payload fields
      removed, `wm_window_info_t.panel_edge` removed. In the compositor
      that took `connected_panel_height`'s edge parameter with it
      (`content_top_limit` is a constant again), collapsed
      `blit_window_rect` back into `blit_window` (the overhang was its
      only other caller), and dropped `apply_window_action`'s
      now-unused request pointer
- [x] Kept from the discarded Dock WIP: `compositor.c`'s panel-hover-
      routing fix - a panel no longer takes focus (neither on a click nor
      at connect time), and mouse events route to whichever panel the
      cursor is over rather than only to the focused window, so the bar
      can hover-highlight its buttons and receive clicks while never
      being focused. The cursor *leaving* a panel sends it one last move
      event, or the button it was over would stay lit forever
- [x] A new `WM_ACTION_TOGGLE_LAUNCHER` (same `WM_ACTION_PIPE` every
      other compositor-owned action already uses, but the first one with
      no `window_id` - handled before the window validation the rest go
      through) is what the Start button sends. M42 owns the toggle and
      the compositor-drawn overlay surface it shows; M43 fills that
      surface with the search field and results
- [x] **Two real bugs found by making the tests stricter, neither about
      this milestone:**
      - **A task blocked inside a syscall could not be killed at all.**
        `sys_kill` promises delivery "at the next syscall entry or
        scheduler tick"; a task parked in `pipe_read`, `SYS_read` on the
        console, or `SYS_wait` is already past the first and, spinning on
        `schedule()` from inside an interrupt gate with IF clear, never
        gets the second while it is the current task. The signal sat
        pending forever. New `sched_deliver_pending_signal()`, called
        from all four blocking loops. Found the moment the self-tests
        started actually *waiting* for what they killed - the first
        client that blocks on its event pipe instead of polling it
        (`gui_paint.c`) hung the whole boot
      - **The self-tests' own teardown was racing M40's leak audit.** A
        SIGKILLed compositor hands back every window's shm segment plus
        its full-screen back buffer when it finally exits, which - left
        unwaited - landed at an arbitrary later moment, sometimes inside
        M40's free-frame comparison, which then panicked the kernel for a
        leak that never happened. All eight teardowns go through one
        `selftest_reap` (kill *and* wait) now
- [x] Rewritten boot self-test (`[m42]`, 9/9 checks) replacing M41's:
      the bar is bottom-docked, the Start button is drawn at its left
      edge, a running app's button is drawn focused at its new offset,
      the tray is right-aligned, a maximized window's titlebar starts at
      the top of the screen with the bar still on top of it, and
      `WM_ACTION_TOGGLE_LAUNCHER` both shows and hides the overlay.
      M22's own taskbar probes moved with the running-app row
- [x] Four new interactive tests through M40's harness, replacing M41's
      two: hovering the Start button lights it and clicking it toggles
      the launcher (which is also the only proof a panel receives input
      at all now that it never holds focus), clicking the taskbar leaves
      the running app focused, a running-app button focuses then
      minimizes its window, and the editor's in-window File menu opens
      and runs File > Quit
- [x] Full `tools/qemu-serial-test.sh` pass (28/28, run three times for
      the intermittent panic above) and `tools/qemu-input-test.sh` pass
      (11/11)

## M43 — Spotlight-style launcher + window snapping ✅

- [x] Global launcher keychord (Ctrl+Space, reusing M32's
      `keyboard_modifiers()` infra already added for Alt-Tab) *or* a
      click on M42's taskbar Start button both open the same centered
      search overlay - compositor-owned, the one place in this project a
      compositor-level (not per-client) modal input grab is actually
      justified, unlike M35/M36/M42's deliberate client-side-only menu
      scope trims. It lists every file via `SYS_listfiles` (leanfs is
      flat, so that is the whole namespace), type-to-filter by
      case-insensitive *substring* rather than prefix (the word you think
      of - "terminal", "editor" - is in the middle of `gui_terminal` and
      `text_editor`), Enter or a click on a row spawns the selection,
      Escape dismisses without launching. Hovering a row selects it, so
      the pointer and the keyboard always act on the same thing
- [x] Window edge-snapping: dragging a window's titlebar until the
      *cursor* reaches the screen's left/right edge and releasing there
      resizes+repositions it to exactly that half. The cursor rather than
      the window's own edge, deliberately - M31's drag clamp keeps
      `MOVE_MIN_VISIBLE` px of the window on screen, so its edge can
      never actually reach x=0, but the pointer can. Same
      "never exceed the window's own buffer" clamp `WM_ACTION_MAXIMIZE`
      already follows, plus `MIN_WIN_*` floors so a display too small for
      two usable halves doesn't clamp windows to nothing
- [x] Two new protocol actions (`WM_ACTION_SNAP_LEFT`/`RIGHT`) rather
      than gesture-only handling, following M30's precedent exactly: the
      drag calls the same `apply_window_action` an external caller
      reaches, so the gesture and the protocol cannot drift, and the snap
      geometry becomes something a boot-time self-test can drive
- [x] Snap preview: a translucent outline of exactly where the window
      will land, shown while the cursor is in an edge zone and before
      release. M38's `fill_rect_shadow` was generalized into
      `fill_rect_blend` (blend toward any color, not only toward black)
      rather than a second blend loop that differed only in what it mixed
      with - the shadow is now a one-line call into it. Preview and
      result both come from the one `snap_rect`, so what you see before
      releasing is what you get after
- [x] New boot self-test (`[m43]`, 7/7 checks): both snap actions with
      `text_editor` as the subject, whose 640x384 buffer is wider than
      half this display and shorter than the full height, so one snapped
      rect exercises both sides of the clamp at once; plus the launcher
      overlay opening, drawing its first result selected, and closing
- [x] Four new interactive tests through M40's harness: Ctrl+Space with
      an app focused (the chord must not be swallowable by a client),
      type-to-filter and Enter; the Start button, type-to-filter and a
      real click on a result row; Escape dismissing without launching
      anything; and the snap gesture, checked *mid-drag* for the preview
      and after release for the result. That last one needed the harness
      to grow `press`/`move_held`/`release` - `drag()` is now those three
      in a row, and `move_held` skips `move_to`'s position verification
      because verifying re-homes the pointer against the top-left clamp,
      which mid-drag would drag the window there with it
- [x] Full `tools/qemu-serial-test.sh` pass (29/29) and
      `tools/qemu-input-test.sh` pass (15/15)

## M44 — Desktop visual polish ✅

- [x] The flat desktop background is now an integer-interpolated vertical
      gradient (`user_space/lib/wallpaper.c` - no floating point, same
      constraint M38/M39 worked within: each style is a pair of
      percentages applied to the chosen background color, and every row
      is one straight-line interpolation between the two). Deriving both
      ends from the *picked* color is what makes the new picker compose
      with the old one instead of superseding it
- [x] **The background color picker had been doing nothing for eleven
      milestones.** `settings.c` set the *compositor's* background, and
      desktop_icons.c's full-screen window has covered that completely
      since M32 - so the control was live, correct, and invisible. Found
      while deciding where the gradient belonged. The desktop client now
      paints the background from the compositor's settings, which fixes
      the old control and gives the new one somewhere to live
- [x] That needed the read side of a channel that had only ever been
      write-only: `WM_SETTINGS_QUERY_PIPE`/`_RESP`, same
      request/response-over-two-named-pipes shape as M22's window query.
      desktop_icons.c polls it; settings.c calls it once at startup, which
      fixes a second, quieter bug - it had been *assuming* the three
      defaults since M33, so reopening it showed the wrong swatch
      selected and its next click sent those assumed values back for the
      controls you hadn't touched
- [x] Translucent taskbar and launcher overlay, both through M43's
      `fill_rect_blend` (itself M38's shadow blend, generalized) rather
      than a third blend implementation. The taskbar needed one protocol
      field (`wm_create_request_t.translucent`) and a blended path in
      `blit_window`; it is opt-in because it costs that window the
      memcpy fast path, and because a translucent *ordinary* window would
      just mean reading one app's text through another's
- [x] Consistent corner and spacing pass: one radius (`gfx.h`'s
      `GFX_CORNER_R`) and one dialog inset (`GFX_PAD`) shared across
      taskbar buttons, desktop icons, the launcher and its search field
      and selection, the settings swatches and wallpaper buttons, the
      editor's dialogs, and window titlebars. The rounding *table* is
      shared too (`gfx_corner_inset`) rather than copied, because
      compositor.c has to draw its own clip-aware version and two tables
      would eventually disagree. Window frames get rounded **tops only**:
      the content area is a straight memcpy of the client's buffer, so a
      rounded bottom corner would just show that client's square pixels
      poking through the curve
- [x] Wallpaper option in `settings.c` - four built-in styles (Flat,
      Gradient, Deep, Grid), each button previewing its own ramp behind
      its name, since eight pixels of the real thing says more than the
      word "Deep" does. No image file format exists in this project and
      adding one is real, separate scope this milestone doesn't take on
- [x] A real bug in the new rounding, caught by a pixel self-test rather
      than by eye: the first `gfx_draw_rect_rounded` drew a *full-width*
      line on any row where the corner inset changed, which turned the
      top two rows of every taskbar button into a solid bar of border
      color. The fix computes, per row, how far the outline has to reach
      horizontally to close the gap its more-inset neighbour leaves
- [x] New boot self-test (`[m44]`, 6/6 checks), the first to run a real
      `desktop_icons.c`: the gradient at two rows (hand-derived from the
      style's own percentages, not read back and blessed), the taskbar's
      translucency **over that gradient** - the one check here that would
      still look reasonable if the blend were reading the wrong buffer -
      switching to `WALLPAPER_FLAT` and watching the ramp go away, and
      the settings query round-tripping what was just set
- [x] `tools/qemu_input_suite.py` learned the same two pieces of math
      (`desktop_px`, `panel_px`), computed rather than tabulated, so
      "is this bare desktop" is a question about the row as well as the
      pixel and a change to either ratio fails with both numbers named
- [x] Full `tools/qemu-serial-test.sh` pass (30/30) and
      `tools/qemu-input-test.sh` pass (15/15)

## Path to a daily-usable desktop (M45+) ✅

M40-M44 got the desktop looking right, and proved for the first time
that real clicks and keystrokes reach the code meant to handle them.
This arc is about the gap between "looks like a desktop" and "can be
used as one" - the things whose absence you notice inside the first
minute of sitting down at it. Three of them are large enough to be
milestones on their own:

- **Nothing can be stopped.** An app that hangs, ignores its close
  button, or opted into M36's confirm-close and never answers stays on
  the screen until the machine is reset. `SYS_kill` has existed since
  M14, and M42 even fixed its delivery into blocked tasks - but no
  *user* can reach it. There is no process list, no right-click, no
  Force Quit.
- **The machine cannot be turned off.** No shutdown, no restart, no
  ACPI S5 path in the kernel at all (`kernel/acpi/acpi.c` walks the
  tables only far enough to find the MADT for SMP). Every session so
  far has ended by killing QEMU.
- **Failure is silent.** A spawn that fails does nothing visible -
  which is exactly the shape M40's Editor/Clock bug wore for four
  milestones before the input harness caught it. This system has no way
  to tell its user anything.

M45 took the first, M47 the second, M48 the third. M46 was the
interface-detail pass the desktop had earned once the layout stopped
moving - circular macOS-style titlebar buttons chief among them. M49
finished the input surface (a scroll wheel; the keyboard chords a
desktop is expected to have). M50 was the robustness and resource pass
this arc's new syscalls and new long-lived UI needed, in the shape M29
and M40 already set - including `SYS_close`, which this project had
never had.

**All six landed.** What the arc actually cost, beyond the features
themselves, was six bugs that had been true for a while and had no way
to announce themselves:

- ACPI had found nothing at all since M26 made UEFI the only boot path,
  so SMP had been silently single-core for twenty-one milestones (M47)
- every window this OS composited leaked an shm segment, so roughly
  thirty window opens exhausted a fixed 32-slot table for the life of
  the machine (M50)
- `MAX_TASKS` was exhausted by the boot self-tests plus six apps, which
  is why the seventh stopped opening (M48)
- the Makefile had no header dependency tracking, so editing a struct
  recompiled nothing that used it (M48)
- a tall window cascaded under the always-on-top taskbar, and every
  window's shadow tinted it (M45)
- `vmm_unmap_page` panics on an address it doesn't find, which is right
  for kernel mappings and wrong the moment a user process supplies one
  (M50)

Three of the six were found *by* this arc's own new features - the task
manager, the spawn error codes and the toast surface between them - which
is the argument for having built them.

## M45 — Process control: task manager, Force Quit, app context menus ✅

The headline gap: there was no way for a person using this OS to stop a
program. Everything needed underneath already existed - `SYS_kill`,
M29's crash-reclaim path, M42's fix for signalling a task blocked in a
syscall. What was missing was entirely above the kernel.

- [x] Kernel: a per-task name. `task_t` (`kernel/sched/sched.h`) grew a
      `char name[TASK_NAME_MAX]`, copied in by `task_spawn_common` inside
      the *same* critical section that publishes the task as schedulable
      - for exactly the reason M40 moved `heap_brk`/`shm_next_vaddr`
      there: a `SYS_taskinfo` running on another CPU can read the table
      the instant the lock drops. `process_spawn` takes the name as an
      argument rather than deriving it, because it is handed an in-memory
      image, not a path: `sys_spawn` and kernel.c's own self-tests are
      the two callers that actually know it
- [x] New `SYS_taskinfo` (30): `(buf, max_entries) -> count`, filling an
      array of `task_info_t` (the new `system_api/include/proc.h`) - pid,
      parent pid, pgid, state, exit code, name, plus the two numbers
      that make a leak visible from user space at last (open fd count,
      mapped shm segments; `shm_count_by_owner` is the new kernel-side
      half of the second). A whole-shot read-only snapshot, the same
      shape `SYS_listfiles` already has for files - no iterator, no
      handle, nothing to leak. No filtering by owner
- [x] `MAX_TASKS` moved from a `#define` inside sched.c to sched.h,
      because a caller enumerating the table now has to size a buffer for
      it and a second hand-picked number that merely happened to be big
      enough is the exact shape of bug M40 and M41 each shipped once
- [x] `WM_ACTION_KILL`: SIGKILL, and unlike `WM_ACTION_CLOSE` it
      deliberately ignores `confirm_close`. That distinction is the whole
      reason this action exists rather than reusing the old one - M36's
      contract explicitly permits a client to never answer
      `WM_EVENT_CLOSE_REQUEST`. Close stays the polite verb; kill is the
      one that always works. Neither touches the window slot: both let
      M29's `reap_dead_clients` notice the death, so force quit, ordinary
      close and a real crash converge on one teardown path
- [x] Right-click a running-app button in the taskbar -> Restore/Minimize,
      Close, Force Quit. Right-click a window's own titlebar -> the same
      three items, drawn by the compositor. Both drive
      `apply_window_action`, and item 0's label is read from the window's
      live minimized state in both rather than hardcoded in either
- [x] The taskbar's menu needed the panel overhang back, as planned:
      `wm_create_request_t.panel_dock_h` (the panel's window is now
      `PANEL_OVERHANG_MAX + PANEL_HEIGHT` tall but docks only the bottom
      32 rows) plus `WM_ACTION_SET_PANEL_OVERHANG`, which asks the
      compositor to composite and click-route N rows above the dock line.
      One extra field on `window_t` (`buf_y0`) makes `blit_window` cover
      both regions with a single rect and a single source-row expression,
      rather than a second blit path. `win->h` stays the docked height
      throughout, so window placement and maximize - both of which
      reserve room for a panel - are indifferent to a menu that is up for
      a second. desktop_shell.c draws the bar through a `bar_gfx`
      context over just the docked rows, which is why not one coordinate
      in its existing drawing had to move
- [x] `wm_action_request_t` grew one `value` field for the overhang's row
      count, and `wm_send_action_value` alongside `wm_send_action` (which
      is now that call with 0) - the one action in the protocol that
      carries a number
- [x] `user_space/bin/task_manager.c`: a real window listing every
      process from `SYS_taskinfo` - pid, name, state, parent, and a
      combined `fd/sh` column - polled on a timer the way
      `file_manager.c` refreshes its listing, with End Task (SIGTERM) and
      Force Quit (SIGKILL) acting on the selection, Up/Down + Enter or
      click-to-select. Terminated tasks stay listed and dimmed rather
      than vanishing: their slots are never recycled, and watching what
      just died - and with what code - is most of the value of having the
      list open
- [x] The four processes that *are* the desktop are listed but refused,
      with a status line saying why rather than a silently ignored click.
      The guard is in the task manager, not in `sys_kill`: the kernel
      stays exactly as permissive as it was, because a real rule there
      needs a permission model this project doesn't have
- [x] Reachable three ways: a new desktop icon, M43's launcher (it is a
      file on disk, so it already was), and Ctrl+Shift+Esc intercepted in
      `handle_keyboard` next to Alt+Tab and Ctrl+Space
- [x] **Two placement bugs the seventh desktop icon exposed**, both about
      the taskbar and neither previously reachable. A tall window far
      enough down the cascade was placed with its bottom *under* the
      always-on-top panel (unreachable, not just crowded), and every
      window's drop shadow reached under the bar - which, since M44 made
      the bar translucent, meant the taskbar's own colors depended on
      which windows happened to be open. Placement now clamps at the
      bottom as well as the top, and the shadow stops at
      `content_bottom_limit()`. Found by the input harness, whose window
      count is read from taskbar pixels and so broke outright
- [x] New boot self-test (`[m45]`, 8/8 checks): `SYS_taskinfo`'s count
      and names checked by pid against tasks the test spawned itself;
      `WM_ACTION_CLOSE` **failing** to remove a `confirm_close` client
      that ignores `WM_EVENT_CLOSE_REQUEST` (the load-bearing half - it
      is what fails the day the two verbs collapse into one) and
      `WM_ACTION_KILL` succeeding; and the reclaim afterwards, checked by
      connecting a *second* client into the slot the first gave up and
      watching it draw there, which only works if that slot's shm segment
      and event pipe both came back
- [x] `user_space/bin/wm_stubborn.c`, a self-test-only client in the same
      role wm_demo.c has held since M20: it opts into `confirm_close` and
      deliberately never answers, because every app this project ships
      answers properly and the one case `WM_ACTION_CLOSE` cannot handle
      needs a client that really doesn't. It also creates one shm segment
      it never uses, which is what gives the kill path a reclaimable
      resource to measure - `shm_free_by_owner` had never been driven
      from a *signal* death with a segment outstanding
- [x] Four new interactive tests through M40's harness: a real
      right-click on a taskbar button raising the menu out of the panel
      and Force Quit removing the window (plus the overhang going back
      down); the same menu from a titlebar right-click; Ctrl+Shift+Esc
      opening the task manager; and End Task against a victim spawned for
      it. That last one first failed for a real harness reason worth
      recording: 90 queued keystrokes exceed a 1024-byte event pipe, the
      compositor blocks part-way through forwarding them, and its main
      loop services the mouse before the keyboard - so a click issued
      during the backlog lands in the middle of it. The test now waits on
      the scrollbar thumb reaching the bottom of its track, which is a
      direct read of "the selection is on the last row" rather than a
      proxy for elapsed time
- [x] The ESP moved from LBA 1024 to past the end of the filesystem: the
      kernel grew into it at exactly 1025 sectors, which is the build's
      size guard doing its job but also a ceiling one milestone away.
      Nothing wanted the ESP *before* leanfs, and moving it turns a
      1024-sector limit into a 2047-sector one
- [x] Full `tools/qemu-serial-test.sh` pass (31/31) and
      `tools/qemu-input-test.sh` pass (19/19)

## M46 — Window chrome & control details ✅

The layout had stopped moving, so the details were worth paying for. The
house rule for this milestone, and the answer to "which OS is this
copying": **macOS shapes, Windows positions.** The buttons became
circular traffic lights, but they stayed right-aligned in
minimize/maximize/close order where every window in this project has
always had them - so `titlebar_button_rect` is untouched and M30's
hit-test, M42's tests and every user's muscle memory all still apply.

- [x] Circular titlebar buttons replacing M30's three flat 14px squares:
      close is a red circle carrying an ×, maximize an amber circle with
      a +, minimize a grey-green circle with a −. The × is what the
      request was really about - a red square says "something", a red
      circle with an × in it says "close". The glyphs are dark rather
      than white, macOS's own choice and the right one here: all three
      fills are light and a white mark on amber is illegible at 6px
- [x] Glyphs are drawn on the focused window and on whichever window the
      cursor is over, and omitted otherwise - macOS's own rule, and what
      stops three saturated dots shouting out of every unfocused window.
      The circles are always drawn: a titlebar with no buttons at all
      would be worse than a quiet one
- [x] Hover feedback at all, which no titlebar button had ever had. The
      compositor already receives every mouse move for hit-testing, so
      this was new state, not new plumbing - and the highlight and the
      click now share one `titlebar_button_at`, where before there were
      two identical loops. A lit button that wasn't the button that acted
      would be a particularly annoying way to discover that
- [x] One shared circle table, `gfx_circle_inset`, mirroring exactly what
      M44 did with `gfx_corner_inset`. No `gfx_fill_circle` alongside it:
      nothing drawing into an app's own window wants a circle, and the
      shared thing that mattered was the table, not a primitive with no
      caller
- [x] Double-clicking a titlebar toggles maximize/restore, through
      `apply_window_action` like everything else. M40 already made
      double-click detection latency-independent by timestamping events
      in the PS/2 handler; this is that machinery's second user, and its
      first inside the compositor itself
- [x] Focused and unfocused windows now differ by more than titlebar
      color: the title text dims, and M38's drop shadow is deeper on the
      focused window (1/2 rather than 1/3 of the way to black). Depth and
      contrast are the cues that survive a user changing the accent color
      out from under the design
- [x] **Cursor shape over the resize zones was mostly already done**, by
      M38 - `compositor.c` has had edge/corner cursor bitmaps and picked
      between them from `resize_hit_mask` since that milestone. What was
      actually missing was the *move* cursor over the titlebar band, so
      that is what this milestone added. M46's plan called for a
      `SYS_cursor_shape` and a kernel bitmap set; that would have been a
      second cursor implementation for no gain, since
      `kernel/drivers/cursor.c`'s arrow is invisible the whole time the
      compositor owns the framebuffer. Recorded rather than done
- [x] A real pressed state for buttons: `gfx_draw_button_state` darkens
      the fill and nudges the label a pixel down and right, and
      `gfx_draw_button` is that with `pressed = 0`. Wired into the two
      files that actually have `gfx_draw_button` dialogs - `settings.c`'s
      Clear and `task_manager.c`'s End Task / Force Quit, both of which
      also un-press when the pointer leaves while held. `text_editor.c`
      and `file_manager.c` are named in the original plan but have no
      such buttons: the editor's prompts are keyboard-driven and the file
      manager has none at all
- [x] New pixel self-test (`[m46]`, 9/9): the close button's bounding-box
      *corner* is titlebar color while its middle is button color - which
      is exactly what says a circle got drawn and not a square, and the
      one check a milestone that only changed the fill color would fail -
      the × present on a focused window and gone once a second client
      takes focus, both shadow ratios read at the same probe point either
      side of that focus change, and the title text's bright/dim pixel
      counts
- [x] M38's own shadow expectation moved from the 1/3 blend to the 1/2
      one, since its window is the focused one. Updated rather than
      loosened - it still asserts the exact arithmetic the compositor
      does
- [x] The input harness learned every cursor shape. `move_to` verifies
      where the pointer landed by matching the arrow's bitmap, so it
      could not locate a pointer parked on a resize edge or a titlebar at
      all - which had been latently true since M38 and only surfaced when
      a test first tried to put the pointer there. `find_cursor_shape`
      matches all six shapes and returns which one, so "which cursor is
      drawn here" is now a question a test can ask
- [x] Three new interactive tests: hovering a titlebar button lights it
      (and the close button still closes, checked in the same test);
      double-clicking the titlebar maximizes then restores; and the
      pointer moving from window content to a resize edge to a corner to
      the titlebar gets the arrow, the horizontal resize cursor, the
      diagonal one and the move cursor in turn
- [x] Full `tools/qemu-serial-test.sh` pass (32/32) and
      `tools/qemu-input-test.sh` pass (22/22)

## M47 — Session lifecycle: shutdown, restart, persistent settings ✅

Every session of this OS before this milestone ended by killing QEMU.
There was no way to turn the machine off from inside it, and nothing it
remembered between boots - including the wallpaper M44 had just added a
picker for.

- [x] **ACPI had not worked at all since M26.** `acpi_find_madt` scans
      the legacy EBDA/0xE0000 ranges for the RSDP, but under UEFI the
      RSDP is published in the system table's configuration array and
      firmware owes nobody a copy in low memory - so from the moment the
      BIOS path was removed, every boot logged "no RSDP found" and SMP
      silently fell back to single-core. It surfaced here because M47
      needs the FADT and the log said it wasn't there. The loader now
      reads the RSDP out of the configuration table before
      ExitBootServices and hands it over in RDX (`kernel_main` grew a
      third parameter; `acpi_set_rsdp` still signature-checks it and
      falls back to the legacy scan). The MADT and the FADT are both
      found now
- [x] ACPI S5 poweroff: `acpi_find_power` parses the FADT for the
      PM1a/PM1b control ports and the ACPI-enable handshake - the same
      RSDT/XSDT walk the MADT lookup already did, factored into a shared
      `find_table`. The `\_S5` sleep type properly lives in AML in the
      DSDT and **writing an AML parser is not in scope**, so `power.c`
      supplies the well-known values (0 first, which is what QEMU's own
      tables define, then 5) and says in the log that they are well-known
      rather than read. QEMU's documented port 0x604 and Bochs's 0xB004
      are the last-resort tiers. Every tier announces itself before it
      fires, so the last line in the log is always the mechanism that was
      actually tried last
- [x] Reboot in three tiers: the FADT's reset register (SystemIO only - a
      memory-mapped one would need a vmm mapping for a single byte, and
      the next tier is a perfectly good answer), the 8042 pulse, then a
      deliberate triple fault. On this QEMU the FADT reports no reset
      register, so the 8042 tier is the one that actually restarts the
      machine - which the harness now observes end to end
- [x] `SYS_shutdown(mode)` (31) performs an *orderly* stop:
      `power_orderly_stop` SIGTERMs every task except the caller and the
      parentless idle ones, waits a bounded ~1s counted in real PIT ticks
      (not a spin), SIGKILLs whatever is left and waits again, then
      `vfs_sync` and the platform tier. It returns how many tasks needed
      SIGKILL, which is what makes the escalation testable, and it is
      deliberately separate from `power_shutdown` so a self-test can
      drive it without the machine turning off underneath it
- [x] `vfs_sync()` exists and is honest: leanfs is write-through, so
      there is nothing buffered for it to push out, and it says so in the
      log rather than pretending to do work. It exists because the day
      leanfs grows a write cache, the fix belongs there and not in a
      shutdown path that would otherwise have to learn what a filesystem
      is
- [x] A Power row in the launcher: Shut Down and Restart, both behind a
      confirm step. Confirming is keyboard-only (Y/Enter; anything else,
      including a click, cancels) - these sit one click from a search
      field, and two more click targets over the two that raised them is
      how a mis-click becomes a double mis-click. `LAUNCHER_ROWS` went
      12 -> 10 to make room
- [x] `shutdown` and `reboot` as programs on disk rather than shell
      builtins, like every other command this project ships - which makes
      them reachable from `gui_terminal.c` and from the launcher as well
      as from the text shell, and gives the tests a non-graphical way
      into the same syscall
- [x] Settings persist through `user_space/lib/settings_file.c` - plain
      `key=value` text, shared by the one writer (`settings.c`, via a new
      `apply_theme` so the live compositor and the file can't drift) and
      the one reader (`compositor.c`, once at startup before any client
      connects). Loading is all-or-nothing: a half-parsed file would hand
      the compositor one real color and two zeros, which is a black
      desktop that looks like a compositor bug rather than a damaged file
- [x] **Persistence broke every GUI self-test, which is the most
      interesting thing this milestone found.** Eleven milestones of
      pixel tests assert against the compositor's *compiled-in* defaults,
      which was safe only while those were the only thing a fresh
      compositor could start with. Pick a flat wallpaper and reboot, and
      `[m44]` panics on the way back up. Caught the first time the input
      harness rebooted a guest that had just changed its wallpaper -
      exactly the scenario the feature exists for. The fix is that the
      whole self-test phase runs against pinned defaults with the user's
      own file held aside and handed back before PID 1 starts; "the tests
      should tolerate any settings" is not the fix, because a pixel test
      whose expected values depend on what somebody clicked last week
      isn't a test
- [x] New boot self-test (`[m47]`, 4/4): the settings file round-trips
      through a real write, a real compositor and a real
      `desktop_icons`, graded on the pixel the desktop actually paints -
      the whole chain, not just the parser - and a deliberately corrupted
      file falls back to the defaults rather than to garbage colors. Then
      the orderly stop driven directly, twice: with no grace period both
      victims must die of the *SIGKILL* that follows (exit code 137) and
      the killed count must be 2; with a real one they must all be gone
      before the SIGKILL round runs at all (143, and a count of 0). Two
      independent measurements of the same escalation
- [x] The harness learned "the guest exited on its own":
      `Machine.wait_for_exit`. Nothing here had ever observed a clean
      guest exit - every test ends by killing QEMU - so "did S5 actually
      fire" had no way to be answered from either side. QEMU's own
      process terminating is the only real proof
- [x] Three new interactive tests: Start > Power > Shut Down through real
      clicks, asserting QEMU exits with status 0 and that the log shows
      the orderly stop ran; the same up to Cancel, asserting the machine
      is still running four seconds later; and the end-to-end
      persistence proof - pick the Flat wallpaper, restart, and watch the
      desktop come back flat. That last one is deliberately a *restart*
      rather than two boots: two boots would only prove `SYS_writefile`
      works, which M33 already covers
- [x] Full `tools/qemu-serial-test.sh` pass (33/33) and
      `tools/qemu-input-test.sh` pass (25/25)

## M48 — System feedback: notifications, errors, no silent failures ✅

M40 spent an entire milestone chasing a bug whose only symptom was
"double-clicking that icon does nothing." The bug was fixed; the class of
symptom was not. This system had no way to tell its user anything.

- [x] A compositor-owned notification surface: transient toasts stacked
      down from the top-right corner, auto-dismissing on their own
      deadline, click to dismiss early. Compositor-owned like M43's
      launcher rather than a client, and for a sharper version of the
      same reason - the two things that most need to speak are the
      compositor itself (a window it had to refuse) and a client that has
      just *died*, neither of which can be asked to draw its own. Drawn
      above even the launcher, since a failed launch is one of the things
      that raises them
- [x] Top-right rather than above the taskbar: the bottom-right corner is
      the tray, and a toast that covers the clock is a toast in the way
- [x] `WM_NOTIFY_PIPE` plus `wm_notify(level, title, body)` in
      `wmclient.h` - one-way, fire-and-forget, the same shape
      `WM_SETTINGS_PIPE` already has. Levels info/warn/error differ only
      in the accent stripe's color. No buttons: that is a dialog's job,
      and M36 already built dialogs
- [x] `system_api/include/spawn_error.h`: distinct negative codes plus
      one shared message table. `SYS_spawn` had returned the same -1 for
      a missing file, a malformed ELF, a full task table and
      out-of-memory since M13 - M40 audited all four into existence and
      they stayed indistinguishable to the caller, which is exactly why a
      desktop icon that couldn't launch could only ever "do nothing".
      `SPAWN_ERR_NOT_FOUND` stays -1 so every existing `pid < 0` test
      keeps meaning what it meant. `sys_spawn` asks `elf_validate` and
      `sched_has_free_task_slot` itself, duplicating checks
      `process_spawn` makes internally, because process_spawn's NULL is
      the ambiguity being removed and a spawn reads a whole file off disk
      first
- [x] Every silent failure got a voice: a spawn that fails from a desktop
      icon (M40's exact symptom) or from the launcher, a window the
      compositor had to refuse (M40 made it print to stdout - nobody
      reads stdout on a desktop), and a client that *crashed*, which
      M29's `SYS_task_alive` 0-vs-2 split has been able to distinguish
      since that milestone and had mentioned to nobody
- [x] **"Nonzero exit code" turned out not to be the same question as
      "did this surprise us."** A SIGTERM death is 143, so an ordinary
      titlebar close arrives at `reap_dead_clients` looking exactly like
      a crash - wiring the toast straight to M29's existing test would
      have announced every deliberate close as a failure. `window_t`
      grew a `close_requested` flag, set by `WM_ACTION_CLOSE`/`KILL`, so
      only a death the compositor did not ask for is news
- [x] An in-window status line for the app that owns its own failures:
      `file_manager.c` threw away `sys_spawn`'s result, so a file that
      couldn't be opened looked exactly like a double-click that didn't
      register. `text_editor.c` already had one (its "SAVE FAILED"
      status), so this is the second user of the same idea rather than a
      new one - a toast in the far corner is the wrong place for
      something about the window you are looking directly at
- [x] New boot self-test (`[m48]`, 9/9): a toast's accent stripe and
      background where they should be, still there two seconds in, and
      gone once its own 4s deadline passes - the last of which is the one
      worth having, since a notification surface that only ever appears
      is one that eventually covers the screen. Plus each distinct spawn
      cause asserted against its own code (a missing name, an ordinary
      text file, and a deliberately truncated ELF built from a real
      compositor's first 64 bytes), and a check that two distinct codes
      don't share one message - which is what makes the codes buy
      anything
- [x] Two new interactive tests: launching a non-program from the
      launcher raises a toast that then expires on its own, and clicking
      a toast dismisses it well inside its deadline (so the dismissal can
      only be the click) without leaving anything behind. The click is
      consumed rather than falling through, which matters because a toast
      lands exactly where a maximized window's close button is
- [x] **The new error codes found a real bug within an hour of
      existing.** The seventh app launched from a desktop icon stopped
      opening - and because a spawn failure now has a reason and a voice,
      the desktop said "Too many programs are running." on screen instead
      of doing nothing at all. `MAX_TASKS` (64) was exhausted: task ids
      are assigned sequentially and slots are never recycled, so every
      throwaway compositor and victim client the boot self-tests spawn is
      charged against that table for the life of the machine. This
      milestone's own extra self-test was the one that tipped it over -
      the third time this project has shipped a bug that was really an
      exactly-sized cap, and the first time the machine diagnosed itself
- [x] So `kernel_main` now logs `[sched] task table at handoff: N of
      MAX_TASKS` - which measured 54 of 64, leaving 4 for the session and
      6 for apps. `MAX_TASKS` went to 128 from that measurement rather
      than from a guess: 54 + 4 + 10 concurrent app windows = 68 for a
      full desktop *once*, and the remaining 60 are the part that
      matters, since closing an app does not give its slot back. M50's
      cap re-derivation starts from this line rather than from nothing
- [x] **The Makefile had no header dependency tracking**, found while
      fixing that: raising `MAX_TASKS` in a header recompiled nothing and
      produced a kernel that still logged the old cap. Every translation
      unit that wasn't independently touched had been linking against
      whatever struct layout it last saw - and `task_t` has changed size
      twice in this arc. `-MMD -MP` plus an `-include` of the generated
      .d files fixes it; touching `sched.h` now rebuilds the 36 objects
      that actually depend on it
- [x] Full `tools/qemu-serial-test.sh` pass (34/34) and
      `tools/qemu-input-test.sh` pass (27/27)

## M49 — Input completeness: scroll wheel, keyboard chords, drag & drop ✅

- [x] PS/2 wheel support: the IntelliMouse 4-byte protocol negotiated at
      init with the 200/100/80 sample-rate knock, falling back to the
      3-byte packet when the device doesn't answer 0x03. `mouse_event_t`
      grew a signed `wheel`, zero on hardware without one - which is
      exactly what a wheel-less device reports, so no reader ever has to
      ask whether the hardware has one. The knock happens inside the same
      polled window as the existing F6/F4 handshake and before IRQ12
      generation is enabled, for precisely the reason mouse_init's own
      comment already gave: any part of the handshake done inside the
      interrupt path desynchronizes packet framing from the first real
      packet. Byte 3's high nibble (buttons 4/5) is masked off rather
      than misread as a huge scroll
- [x] Routed as `WM_EVENT_MOUSE_WHEEL` to whatever the *pointer* is over
      rather than to whatever holds focus - what every desktop with a
      wheel does, and the only behavior that makes scrolling a background
      window's list possible at all. Handled in the file manager's list,
      the editor's text, the task manager's process list and the
      launcher's results, one row per detent, which is the same unit
      Up/Down already move
- [x] The wheel scrolls the *view* and not the selection everywhere it
      would matter: End Task acts on the task manager's selection and
      Enter opens the file manager's, so a wheel that dragged the
      selection with it would be a gesture that can act on the wrong
      thing. The launcher is the exception and moves its selection,
      because its list has no other meaning
- [x] **`gui_terminal.c` has no scrollback to scroll.** Its `grid_scroll`
      moves the live grid up and discards the top line - there is no
      history buffer behind it, so there is nothing for a wheel to reveal.
      Recorded rather than faked; a scrollback buffer is real, separate
      scope
- [x] The chords, all dispatched from one table
      (`system_api/include/shortcuts.h`): Alt+F4 (close focused, honoring
      `confirm_close` - it goes through `WM_ACTION_CLOSE`, so it is the
      polite verb), Ctrl+Alt+Left/Right (M43's snap actions),
      Ctrl+Alt+Up/Down (maximize / restore-then-minimize, so holding the
      chord walks a maximized window down rather than doing nothing to
      it), and Shift+Alt+Tab to cycle backward. Every one already had an
      action to call; what was missing was only the binding
- [x] F1-F12 had no representation at all - `keyboard.c` decoded
      printable ASCII plus M33's four arrows, and every function-key
      scancode fell out of the table-bounds guard. `KBD_KEY_F1`..`F12` are
      14..25 rather than continuing 5..16 from the arrows, because 8, 9,
      10 and 13 are `\b`, `\t`, `\n` and `\r` and 27 is Escape - the
      numbering has a gap in it for a reason
- [x] Somewhere to discover them: a Shortcuts pane in `settings.c`,
      generated by looping over the same `SHORTCUTS[]` the compositor
      dispatches from. Not a second copy that would drift within two
      milestones - a chord cannot exist without being listed, and nothing
      can be listed that isn't wired up
- [x] `shortcut_lookup` matches on (character, modifiers) with an
      explicit `mods_forbidden`, which is what keeps Alt+Tab and
      Shift+Alt+Tab distinct - they share a key and differ only by a
      modifier one of them must not have
- [x] Drag and drop, scoped to the one case worth having: dragging a file
      out of `file_manager.c` onto the desktop or onto the editor opens
      it there. Two pipes rather than three -
      `WM_DRAG_PIPE` carries the payload to the compositor, which holds it
      until the button comes up and then writes it to `WM_DRAG_DATA_PIPE`
      *before* sending `WM_EVENT_DROP`, so a client that reads the data
      the instant it sees the event finds it already there. A single
      well-known data pipe is safe because exactly one drop can be in
      flight and only the dropped-on client is ever told there is one
- [x] The payload deliberately does **not** ride inside `wm_event_t`: 28
      bytes would more than double every event this system routes and cut
      what an event pipe buffers from 42 events to 19 - a cost paid on
      every keystroke and mouse move, for a field one event type uses once
      per gesture
- [x] There is no "drag end" message. The left button coming up is the
      end, and the compositor sees that itself - so a source that crashes
      mid-drag cannot leave one stuck, and a client cannot forget to send
      it. A drag label follows the cursor while one is in flight, which is
      what makes the gesture something a person can see they are doing
- [x] Dropping onto the editor goes through its existing
      confirm-discard prompt (a new `PENDING_DROP` beside the File menu's
      two) rather than loading over the top: losing unsaved work to a
      mis-drop is exactly the accident M36's prompt exists to prevent
- [x] New boot self-test (`[m49]`, 22/22): every chord resolved through
      the same `shortcut_lookup` the compositor calls, including three
      near-misses that must *not* be chords (a plain Tab, a plain space,
      Ctrl+an ordinary letter) - a table that got `mods_forbidden` wrong
      would silently make Shift+Alt+Tab cycle forward. Plus every row
      having an id, a chord name and a description, which is the property
      that makes one table worth having. Then a real drag announced on
      `WM_DRAG_PIPE` producing a real drag label at the pixel the cursor
      is parked at
- [x] The harness learned to inject a wheel (`Machine.wheel`, QEMU's
      `mouse_move dx dy dz`) - one monitor command per detent, because
      the PS/2 wheel field is a 4-bit signed value and a large dz is not a
      bigger scroll, it is a wrapped one
- [x] Four new interactive tests: three wheel detents move the file list
      by exactly three rows - checked by capturing row 3's pixels before
      and requiring row 0 to be those same pixels after, which is
      content-independent and off-by-one-proof in a way that counting
      rows by eye is not - plus Alt+F4 closing the focused window,
      Ctrl+Alt+Right/Left/Down snapping and minimizing, and a file
      dragged from the file manager onto the desktop opening in the
      editor
- [x] Full `tools/qemu-serial-test.sh` pass (35/35) and
      `tools/qemu-input-test.sh` pass (31/31)

## M50 — Robustness & resource hygiene ✅

The pass this arc's new syscalls, new long-lived UI and new ways to kill
things needed - M29 and M40's shape, applied to what M45-M49 added.

- [x] `SYS_close` (32) finally exists. `SYS_dup2`'s own comment had
      admitted there wasn't one since M21, and M40's root-cause bug was
      five never-released pipe fd-pairs riding into every process on the
      system. It releases the caller's own fd-table slot and nothing
      more, deliberately: pipes here are not reference-counted, a named
      pipe is *meant* to outlive every fd that has ever pointed at it -
      that is the whole rendezvous mechanism the window protocol is built
      on - and an anonymous pipe's other end is usually held by a child
      spawned with a copy of this table. Freeing anything would turn "I
      am done with this descriptor" into "everyone else's is dangling".
      Slots are what were actually leaking
- [x] `wm_connect` hands five slots back per client: the two
      request/response pipes are a handshake nothing touches again, and a
      client will never write to its own event pipe, so its write end was
      a slot claimed and abandoned at birth
- [x] The compositor keeps a reclaimed slot's event pipe rather than
      closing it - M50's plan said to close it, and reusing it is
      genuinely better fd hygiene, since `accept_pending_window` resets
      it in place instead of spending two fresh slots on a reopen.
      Recorded as a deliberate departure rather than done
- [x] **`SYS_shm_free` (33), which turned a documented leak into a
      documented bound.** `reclaim_window`'s own comment had explained
      since M29 why it *couldn't* free a dead window's pixel buffer:
      there was no such call, and freeing frames out from under this
      process's own still-present mapping would alias live memory. Both
      true - and the second is exactly why this unmaps before it frees
      rather than the other way round. Without it, every window this OS
      ever composited consumed one of `MAX_SHM_SEGMENTS`'s 32 slots
      forever: **about thirty window opens exhausted the table for the
      life of the machine, and the thirty-first silently got no window.**
      The headroom that comment leaned on was never headroom, it was a
      countdown
- [x] `shm_free(id, owner)` refuses a segment owned by anybody else,
      which is the ownership invariant stated as code rather than as a
      convention. The address to unmap comes from the caller because
      nothing here tracks which address spaces a segment is mapped into,
      and adding that registry would be real bookkeeping for one caller
      that already knows the answer
- [x] `vmm_unmap_page_in`, which had to exist for any of that:
      `vmm_unmap_page` walks the *kernel* PML4 and panics on anything it
      doesn't find - right for kernel mappings and exactly wrong for an
      address that arrives from a user process, where "not mapped" is
      ordinary bad input. Found by the first boot after wiring
      `SYS_shm_free` up, as a panic
- [x] New boot self-test (`[m50]`, 13/13), in three parts. 24 shm
      create/free cycles that must return every frame exactly; a double
      free that must be refused; a **16-round kill storm** - each round a
      whole client connecting to a real compositor, drawing, and being
      SIGKILLed, which is M45's Force Quit driven far faster than a person
      could - requiring every window slot and every segment back, and one
      more client to still be able to connect *and draw* afterwards; and
      nine deliberate garbage arguments to the syscalls this arc added
- [x] The shm cycle can't map what it creates, and that is worth
      recording: task 0 is a kernel thread, and a kernel thread's
      `SYS_shm_map` cursor is zero (`task_t` says these are "meaningless,
      left zeroed" for anything not spawned through `process_spawn`), so
      asking would map at virtual address 0 - inside the identity-mapped
      low 2 MiB - and panic. Found by writing the test. The mapped path is
      what the kill storm covers, through a real compositor mapping and
      freeing real window buffers sixteen times
- [x] The kill storm's frame bound is derived, not picked. A dead process
      leaks its whole address space (page tables, stack, argument page,
      image) because this kernel has no `vmm_destroy_address_space` -
      measured at 15 frames per round and **logged every boot**. One
      leaked window pixel buffer would be 24 frames on its own, so the
      bound is 20 per round: loose enough for what genuinely leaks, tight
      enough to fail if even one buffer didn't come back
- [x] Caps re-derived from measurement rather than guessed, which is what
      the boot log now carries: `[sched] task table at handoff` (added in
      M48, when it caught `MAX_TASKS` being exhausted) and `[m50]
      compositor after the storm`, which reports what a compositor that
      has filled every window slot actually holds
- [x] `MAX_FDS` 64 -> 128 from that line. It measured **52 of 64**, and
      the derivation is 2 (stdin/stdout) + 22 (eleven well-known protocol
      pipes) + 24 (two per window slot ever created, since event pipes
      are kept and reset rather than reopened) = 48. Twelve spare is not
      headroom, it is the state M40 was in when this broke the first time
- [x] `MAX_SHM_SEGMENTS` left at 32 deliberately: the number was never
      the problem. With `SYS_shm_free` a compositor's live usage is
      exactly 1 + one per live window = 13 at `WM_MAX_ROUTABLE_WINDOWS`,
      which is now a bound instead of a countdown
- [x] `SYS_cursor_shape`, which M50's plan also wanted garbage-tested,
      does not exist - see M46: the compositor draws every cursor shape
      itself, so the syscall was never added. Recorded rather than
      quietly dropped
- [x] A soak run as a new interactive test: the clock redrawing every
      second, the taskbar refreshing every 300ms (a `WM_QUERY_PIPE` round
      trip each time), and the file manager re-reading the whole
      namespace once a second, for three minutes - thousands of pipe
      round trips - with the pointer moved across the taskbar throughout.
      "Nothing grew" is asserted as *launch one more app and require it
      to open*, which is not a proxy for a leak but the actual symptom
      one produces: exhausting any of these caps presents as "the next
      thing you launch silently doesn't", exactly how both M40's bug and
      M48's `MAX_TASKS` bug showed up. The log is checked for a window
      refusal too
- [x] **One more near-duplicate cap, found by the regression runs
      themselves.** `task_manager.c` kept a private `MAX_ENTRIES` of 64
      with a comment arguing that a constant "at least as large" as the
      kernel's was fine - which it was, until M48 raised `MAX_TASKS` to
      128. The task manager then silently listed only the *first 64*
      processes, so End Task acted on a long-dead boot self-test task and
      the live one you were looking at was not on screen at all. sched.h's
      own comment had warned about exactly this shape of bug; the warning
      was right and the mitigation wasn't. `TASK_INFO_MAX` in the shared
      ABI header is now the single definition, and `MAX_TASKS` is spelled
      as it
- [x] Full regression on both suites, three consecutive runs - M42's
      precedent, set after an intermittent teardown panic a single green
      run would have hidden. 36/36 serial markers and 32/32 interactive
      tests, three times each

### Known gaps and things still unverified after M50

Recorded rather than left implicit - each of these is a real limit of
what M45-M50 actually shipped, and several are load-bearing for whatever
comes next.

- **A dead process's address space is never reclaimed.** M29 documented
  the tradeoff and M50 measured it: ~15 frames per dead process (page
  tables, stack, argument page, and the frames its ELF image was copied
  into), logged every boot by the kill storm. There is no
  `vmm_destroy_address_space`. At 128 task slots this is bounded, but
  the bound is "the machine's whole uptime", not "concurrently running"
- **Task slots are never recycled.** Ids are assigned sequentially and a
  terminated task's slot stays valid forever, which is what makes
  `SYS_wait`'s poll-the-exit-code approach work without zombie
  bookkeeping. It also means `MAX_TASKS` is a budget for *launches over
  the machine's lifetime*: the boot self-tests alone now spend 73 of 128
  before PID 1 starts, and that number has grown every milestone this arc
- **Real-hardware boot is still unverified.** Everything in M45-M50 was
  developed and tested against QEMU/OVMF. M47's ACPI work in particular
  is the kind that behaves differently on real firmware - the FADT on
  this emulator reports no reset register at all, so the 8042 tier is
  what actually restarts the machine here and the ACPI reset path has
  never once fired
- **The S5 sleep type is guessed, not read.** `\_S5` lives in AML in the
  DSDT and this project has no AML parser, so `power.c` tries the two
  well-known `SLP_TYP` values and says in the log that they are guesses.
  Correct on QEMU; unknown on anything else
- **No scrollback in the terminal.** `gui_terminal.c` discards the top
  line when the grid scrolls, so M49's wheel has nothing to reveal there
- **`gui_terminal` never restores its own stdout.** It `dup2`s a pipe
  onto fd 1 before spawning and leaves it there deliberately (it reads
  its own output back), but that means the terminal's stdout is a pipe
  for the rest of its life
- **`SYS_close` does not close a pipe**, only the caller's slot - pipes
  are not reference-counted, so a reader blocked on a pipe whose last
  writer "closed" waits forever rather than seeing EOF. Nothing depends
  on EOF today; anything that ever does will need refcounting first
- **Panic paths from *older* syscalls were not re-audited.** M50's
  garbage-argument pass covers what M45-M50 added (`SYS_taskinfo`,
  `SYS_shutdown`, `SYS_close`, `SYS_shm_free`, `SYS_kill`). The other
  twenty-eight syscalls have never had the same treatment, and
  `vmm_unmap_page_in` exists because the first one that reached
  `vmm_unmap_page` with a user-supplied address panicked
- **User pointers are still trusted.** `sys_write`'s own comment has said
  so since M8: nothing validates that a ring-3 pointer is mapped and owned
  by the caller. Every syscall that writes through one (`SYS_taskinfo`,
  `SYS_fb_info`, `SYS_mouse_read`, ...) would fault the kernel on a bad
  address rather than returning an error
- **The desktop clients are unsupervised except for `desktop_shell`.**
  init waits on the taskbar and restarts the whole session if it exits,
  but a compositor or `desktop_icons` that dies on its own is not noticed
  until the taskbar happens to die too
- **The input harness is single-threaded against wall-clock time.** Two
  tests have already failed for host-load reasons rather than guest bugs
  (M45's key backlog, M48's slower run), and each was fixed by asserting
  on a guest-side state signal instead of elapsed time. The pattern
  works, but nothing enforces it for new tests


## Path to a dependable desktop (M51+)

M45-M50 closed the gap between "looks like a desktop" and "can be used
as one". This arc is about the gap between "can be used" and "can be
*relied on*" - and the ordering below is a judgment about what most
damages the product, not what is most interesting to build.

Three things decide it:

- **Overlapping windows are broken, and it is the first thing anyone
  will notice.** This compositor has never had a z-order. It paints in
  creation order, and clicking a window changes its titlebar color
  without bringing it forward - so a window you can see is behind one
  you don't want, permanently. Worse, the click hit-test walks the array
  backwards and takes the first *match* rather than the topmost
  *visible* one, so a click can be delivered to a window that is
  completely covered. Every hit-test in `compositor.c` carries a comment
  admitting this ("occlusion-unaware - a real fix is z-order work, out of
  scope here"). It is in scope now.
- **Any user program can take down the kernel with a bad pointer.**
  `sys_write`'s own comment has said so since M8. M40 fixed exactly one
  instance of this class (`elf_load`) and called "a user program can take
  down the kernel by spawning a text file" a bug at any threat model -
  which it is, and the other thirty syscalls that dereference a ring-3
  pointer have never been audited. This is the difference between a
  buggy app and a machine you have to reset.
- **Nothing that dies is ever fully reclaimed.** M50 measured it: ~15
  frames per dead process, forever, plus a task slot that is never
  recycled. Both are bounded only by the machine's total uptime, and the
  boot self-tests alone now spend 73 of 128 task slots before PID 1
  starts. This is the ceiling on how long the machine can be left on.

M51 takes the first, M52 the second, M54 the third. M53 is the one
structural limit that shows up in three separate apps at once - a flat
filesystem - which is why it sits above resilience and app depth rather
than in the stretch list where it started. M55 is the supervision this
session has needed since init learned to watch exactly one of its three
children. M56 is depth in the apps people actually spend time in.

## M51 — Z-order: raising, occlusion-correct hit-testing, focus that means something ✅

The compositor's own header comment has described this as a
"simplification" since M20 and every hit-test in the file repeats the
apology. It is the largest remaining behavioral difference between this
and a real window manager, and unlike most of them it is *wrong* rather
than merely absent: a click can land on a window nobody can see.

- [x] An explicit z-order (`zorder`/`z_count` in `compositor.c`) - an
      array of window indices, topmost last - rather than paint order
      being `windows[]`'s own order. Everything that walks windows for
      painting, hit-testing or event routing walks that instead. The
      three z-order *classes* this project already had (desktop
      background at the bottom, ordinary windows, panels always on top)
      are bands within that array: it is kept sorted by band, insertion
      goes to the top of the inserting window's own band, and a raise can
      only ever move a window within its band. A window's band is fixed
      at connect time, so nothing but a bug in that one block can break
      the invariant
- [x] Raise on focus - but in `set_focus`, **not** in
      `apply_window_action`'s `WM_ACTION_FOCUS` branch, which is where
      this milestone's plan put it and where it turned out to be wrong.
      That branch is the funnel for a taskbar button, Alt+Tab and an
      external `WM_ACTION_PIPE` request; a plain click on a window's
      body (`focus_window_under_cursor`), a titlebar move-drag and a
      resize-edge grab all call `set_focus` directly. Putting the raise
      in the action branch left exactly the three gestures a person uses
      most not raising anything - which is what the new boot self-test
      caught on its first run, before any of this reached a human
- [x] The raise sits *above* `set_focus`'s "already focused, nothing to
      do" early return: a window can be focused and still not be topmost
      (something else was raised while it kept focus), and clicking it
      must then still bring it forward
- [x] Hit-testing walks the z-order top-down and the first window whose
      painted frame contains the point is the only one allowed to answer.
      Five near-copies of the same backwards loop - the window pick, the
      titlebar-button pick, the resize-edge pick twice (click and cursor
      shape) and the move-drag titlebar pick - are one `z_hit_test` that
      takes a class mask and a region. `HIT_RESIZE` is the one documented
      exception: a resize handle reaches `RESIZE_MARGIN` pixels *outside*
      its own frame, so a window's grab halo is checked before that window
      is asked whether it occludes the point, which keeps a top window's
      halo winning over a lower window's frame
- [x] A window that is raised does not steal the click it was raised
      *by*. The raise happens during the press's own focus change and
      event routing then goes to the (now front) focused window, so the
      press that raises is the press the window receives
- [x] Alt+Tab cycles in z-order. Since focus raises, that order *is* the
      most-recently-used order: Alt+Tab steps one window back down it,
      Shift+Alt+Tab one forward, both wrapping past panels and the
      desktop background. Tapping Alt+Tab repeatedly therefore swaps the
      front two rather than touring every window, which is what a
      tap-without-holding does on Windows too and is the honest
      consequence of raising on focus
- [x] The taskbar's running-app order stays *stable* - by window id, for
      a window's whole life. `wm_window_info_t` grew a `z_index` so the
      shell can show frontmost-ness without reordering, and
      `desktop_shell.c` draws the frontmost non-minimized app's button
      with a half-lit accent border. The case that makes the field worth
      having is the one where focus and frontmost differ: minimizing the
      focused window leaves nothing focused at all, and the bar can still
      say which of the rest is in front
- [x] Shadows are correct now, and were wrong in a way nobody had
      noticed. `fill_rect_shadow` draws each window's shadow immediately
      before that window, so whatever is painted after covers it - which
      is only correct if "after" means "in front of". Painting strictly
      back-to-front fixes it with no new code
- [x] New boot self-test (`[m51]`), 7/7 checks - and the first boot-time
      test in this project to deliver a **real mouse click**. Everything
      mouse-driven before it was left to `tools/qemu-input-test.sh`,
      because the kernel had no way to move a pointer; `mouse_inject`
      (`kernel/drivers/mouse.h`) pushes a synthetic event onto the same
      ring the IRQ handler feeds, so a reader cannot tell it from a real
      packet. Deliberately not a syscall - a user program that could
      forge input could click any other program's window
- [x] `user_space/bin/wm_zorder.c`, the self-test-only client the above
      needs: a flat fill color on argv (so one pixel in the overlap says
      which of two windows is in front) and a row of click "ticks" in its
      own bottom-right corner, one per press it is routed. The
      bottom-right corner is the one part of the *lower* of two cascaded
      windows the upper never covers, which is what lets the test assert
      one window received a click *and that the other did not*
- [x] The checks: two 300x200 windows on the cascade at (100,100) and
      (140,140); the second in front to begin with; a click inside the
      first only, after which the overlap pixel must be the first
      window's, its tick must be lit and the other's must not; then a
      click in the region both cover, which must reach exactly the front
      one. Plus `wm_window_info_t.z_index` reporting the raised window as
      frontmost through the same query `desktop_shell.c` uses
- [x] Three new interactive tests: `clicking_a_window_raises_it` (both
      directions, so it is an order rather than a one-time swap),
      `overlap_click_reaches_the_front_window`, and
      `alt_tab_visits_windows_in_use_order`. They read the *compositor's
      own border color* down each window's edge column rather than
      whatever an app happens to draw - Files' right border and Tasks'
      left border each fall inside the other window, so each is visible
      exactly when its own window is in front, giving a positive answer
      in both directions instead of one probe that only says "something
      changed"
- [x] One test-harness lesson, worth recording because it will recur:
      the raise and the taskbar's focus accent must be asserted in **one
      predicate**, not one after the other. The compositor reorders and
      repaints immediately while the taskbar only learns about the new
      focus on its next poll, so a shot taken the instant the window
      moved forward can legitimately still show the old button lit. This
      is the same "assert on a guest-side state signal, not on elapsed
      time" pattern M45 and M48 each had to learn

## M52 — Kernel hardening: validated user pointers, no user-triggerable panic ✅

`sys_write`'s comment - "buf is trusted as-is for now; validating that a
ring-3 pointer is actually mapped and owned by the caller is left for
whenever a genuinely untrusted program needs to run here" - had been true
for forty-four milestones. The threat model argument was always right and
is also beside the point: the programs this OS runs are *its own*, and the
bug this prevents is a buggy app taking the machine down rather than
itself.

- [x] `vmm_user_range_ok(pml4, virt, len, need_write)` in `vmm.c` - the
      primitive everything else here is built on. It answers the question
      the hardware would answer by faulting, by doing the same walk the
      hardware does: PRESENT and U/S required at *every* level, which is
      how x86 paging computes access rights, so a leaf entry that looks
      fine under a kernel-only PDPT is correctly refused. Huge pages are
      refused outright (nothing a user process owns is one), and a length
      that wraps the address space is false rather than wrapped
- [x] `user_range_ok` in the syscall layer applies two rules: the range
      lies inside the caller's own private region (`USER_REGION_BASE`/
      `USER_REGION_LIMIT`, new in `proc.h` and exactly what PML4[1]
      covers) and every page of it is present and user-accessible.
      Failing either is `-1`, never a fault
- [x] Three shapes, for three genuinely different cases, rather than one
      primitive pretending they are alike: `copy_to_user` for a
      fixed-size payload the kernel produces, `copy_str_from_user` for a
      NUL-terminated string whose length nobody knows until it is read
      (validated page by page as the copy crosses into each one), and a
      plain range check plus in-place use for a bulk buffer whose length
      the caller chooses. There is no bounded bounce buffer for an
      arbitrary length, and allocating one per call would turn every
      large write into an allocation that can fail
- [x] In-place use is safe here for a reason worth writing down rather
      than assuming: the only way a mapping in the private region can go
      away is `SYS_shm_free`, and a process has exactly one thread of
      control - so while it is blocked inside a syscall there is nobody
      who could unmap the buffer it just passed. The day this project
      grows threads within a process, that comment is what stops being
      true
- [x] **No `copy_from_user`**, deliberately, and this is a correction to
      the plan above. No syscall in this project takes a fixed-size
      struct *from* the caller - the window-manager protocol carries its
      structs over pipes, not arguments - so it would have been a
      primitive with no user, which this project has refused to write
      before for the reasons `pipe.h` gives about `pipe_named`. It is
      four lines the day one is needed, and the rule it would enforce is
      already written down
- [x] The audit, and the real number: **fourteen** syscalls dereference
      at least one pointer the caller chose, not the thirty-one the plan
      above guessed. `SYS_write`, `SYS_read`, `SYS_spawn` (two strings),
      `SYS_readfile`, `SYS_writefile`, `SYS_listfiles`, `SYS_pipe`,
      `SYS_fb_info`, `SYS_mouse_read`, `SYS_pipe_open`, `SYS_kbd_read`,
      `SYS_taskinfo`, `SYS_clipboard_set`, `SYS_clipboard_get`. The rest
      take numbers, or *return* addresses without taking any
      (`SYS_fb_map`, `SYS_shm_map`, `SYS_sbrk`)
- [x] Truncation is refused, not performed. A path longer than
      `LEANFS_MAX_NAME` or a pipe name longer than `NAMED_PIPE_NAME_LEN`
      is an error: a truncated path names a *different file*, and a
      truncated pipe name is a rendezvous with a program that never meant
      to talk to you. `NAMED_PIPE_NAME_LEN` moved from `pipe.c` into
      `pipe.h` so the buffer and the cap are one definition rather than
      the near-duplicate cap this project has shipped three bugs behind
- [x] `SYS_shm_free`'s `vaddr` had a real hole and it is closed: any
      address was accepted, so one in PML4[0] would have unmapped a page
      from the *shared kernel map* - the same subtree every address space
      uses. That is a user-triggerable way to take the machine down, and
      it is one of the [m52] checks
- [x] `SYS_taskinfo`'s count cap is `TASK_INFO_MAX` rather than a
      hand-picked 4096, and an out-of-range count is an error return,
      never a silent clamp: a caller that asked for more than exists has
      a bug, and quietly answering a different question hides it
- [x] Null is refused from *any* ring, before the kernel-thread
      exemption. Address 0 is inside the identity map, so a kernel caller
      passing it would not fault - it would quietly scribble on physical
      page 0 - and M50's own "SYS_taskinfo with a null buffer" row is
      exactly that case asserted from `kernel_main`
- [x] **A kernel thread is exempt, by design, and this shaped the
      testing.** A task sharing the kernel's address space is passing
      kernel pointers, which is what it is supposed to do - the boot
      self-tests have driven `SYS_pipe_open` and `SYS_writefile` that way
      since M13. Told apart by which PML4 it runs on. The consequence:
      a garbage-argument matrix run from `kernel_main` would prove
      nothing, because every row would take that early return
- [x] `isr.c` splits fatal exceptions by *ring*, which is the whole
      milestone in one branch. A fault in ring 3 terminates that task
      with exit code `128 + SIGSEGV` (139), logged with the task's name
      and pid; a fault in ring 0 still panics, because that is a kernel
      bug and should be loud. Safe from interrupt context for the same
      reason the SIGKILL path already is - it is the identical
      `task_exit_with_code` call from inside a handler
- [x] Every fatal exception, not only `#PF`. A divide-by-zero, an invalid
      opcode and a `#GP` from ring 3 are the same kind of event and there
      was nothing to gain from leaving three of them able to stop the
      machine while the fourth could not
- [x] M48's crash toast finally has something real to report: a client
      that faults dies exactly the way a SIGKILLed one does, and the
      compositor already announces precisely that
- [x] `user_space/bin/wm_faulter.c` - a program that dereferences null on
      purpose, in the same self-test-only role `wm_stubborn.c` holds. It
      connects, paints, and creates a 64 KiB segment of its own *before*
      faulting: a process that crashes owning nothing would only prove
      the fault handler runs, while one that crashes holding a window, a
      segment and an event pipe proves the whole reclaim chain still
      works
- [x] `user_space/bin/badptr.c` - the garbage-argument matrix as a table,
      and a **user program** rather than a block in `kernel.c`, for the
      exemption reason above. Fourteen rows crossed with six shapes of
      bad pointer: null, a kernel address, one byte below the private
      region, an unmapped address inside it, a mapped byte whose range
      straddles into an unmapped page, and a valid pointer with a length
      that overflows. It derives all six from ring 3 - the region base is
      its own load address with the low 39 bits (one PML4 entry) masked
      off - rather than restating kernel constants a user program has no
      business knowing. Exit code is the number wrongly accepted, so
      adding a syscall without adding a row is a visible omission
- [x] New boot self-test (`[m52]`), 7/7: the faulter's window painted,
      then gone; the task terminated with 139; its segment's frames back;
      `SYS_shm_free` refusing a kernel address and an unaligned one while
      still accepting the legitimate free; and the matrix reporting 0
      accepted. The assertion that this code *keeps running* is the one
      that matters, and the serial harness's own "no kernel panic" grade
      is the other half of it
- [x] New interactive test
      (`a_crashing_program_only_takes_itself_down`): launch the faulter
      from the launcher, watch the crash toast appear and its window get
      reclaimed, and then **launch something else and require it to
      open** - deliberately not "the screenshot still looks like a
      desktop", since a hung guest would keep showing its last frame
- [x] Full regression: 38/38 serial markers and 36/36 interactive tests

## M53 — Directories in leanfs, and a namespace that isn't a junk drawer ✅

The filesystem was flat, and it showed up in three apps at once: the file
manager listed this OS's own executables next to your text files, the
launcher offered to "launch" `settings.conf`, and `SYS_listfiles` was
documented as "the entire namespace" because it had no choice. The boot
self-tests' own fixtures (`m33test`, `m48trunc`, `fstest`) sat in the
same list.

- [x] Directory inodes in `leanfs.c`: a directory is a file whose
      contents are `leanfs_dirent_t` name/inode records - the smallest
      change that is a real directory rather than a prefix convention,
      and one that reuses every block-allocation path a file already had.
      `inode_read_data`/`inode_write_data` were lifted out of
      `leanfs_read`/`leanfs_write` for exactly that reuse, which is what
      makes "a directory is a file whose contents are records" true of
      the code and not only of the header comment
- [x] **Inodes carry no name at all any more.** A name lives in exactly
      one place, its parent directory's records - which makes "two
      disagreeing answers to what this file is called" structurally
      impossible rather than merely unlikely. The `used` flag became a
      `type`, so a free inode and a file and a directory are one field
- [x] A record is 32 bytes exactly, checked by `_Static_assert`, so 16
      fit in a block and none ever straddles one. That is what lets every
      directory operation be a whole-block read/modify/write rather than
      needing byte-level addressing this driver does not have
- [x] Path resolution (`/bin/ls`), and every whole-file call taking a
      path. `vfs_read`/`vfs_write` keep their shape exactly - this is a
      resolver in front of them, not a new API
- [x] **No `.` or `..` in the resolver**, deliberately. Neither is stored
      on disk and this format has no parent link, so honoring them would
      mean either inventing a link or rewriting the path textually -
      which is the near-correct shortcut that turns into an escape from
      the root. The file manager's own `..` is a caller-side string
      operation on a path it already holds, which is honest about being
      exactly that
- [x] `SYS_mkdir` (34), and `SYS_listfiles` *becoming*
      `SYS_listdir(path, buf, maxlen)` on the same number (7). One repo,
      every caller converted in the same commit - leaving a second call
      that only ever meant `/` would just have been a way for the two to
      drift. A listed name that is itself a directory comes back with a
      `/` appended, so a caller knows what entering it would mean without
      a second call
- [x] `LEANFS_MAX_INODES` 32 -> 96, and it took a count to see why: this
      project ships 24 programs, which with four directories, the
      self-test fixtures and `settings.conf` came to *exactly 32*. That
      is the fourth exactly-sized cap in this project's history (M40,
      M48, M50), and the first one caught before it shipped
- [x] **A stack overflow found on the way**, not caused by this
      milestone. `save_inodes` and `leanfs_init` declared the whole inode
      table as a local - 3584 bytes of an 8 KiB kernel task stack, which
      a 15-sector table would have taken to 7680. Both are static
      scratch now; leanfs has no concurrency of its own, so one shared
      buffer is safer as well as smaller
- [x] A layout worth having: `/bin` for programs, `/home` for user
      files, `/etc` for `settings.conf`, `/tmp` for the self-test
      fixtures - which is the whole requirement for them, "somewhere that
      is not /home". `system_api/include/paths.h` is the one place both
      halves of the system agree on it
- [x] `/home/readme.txt`, seeded on a fresh disk. Not decoration: the
      file manager used to open on a namespace that always had two dozen
      things in it, and now opens on a directory that would otherwise be
      empty on first boot - which reads as "broken", not as "new"
- [x] The launcher lists `/bin` and skips anything that is itself a
      directory, so a data file is no longer something it can offer to
      run *at all*. That retires M48's "That file is not a program."
      toast for the common case, leaving it for the genuinely broken one
- [x] The file manager gets navigation: a path bar (right-aligned when
      the path outgrows the header, since the tail says where you are and
      the head only says how you got there), double-click to enter, and a
      `..` row pinned to index 0 so leaving is always in the same place.
      The root has no `..` row - an inert one would be a row that does
      nothing - which is an asymmetry the interactive tests name rather
      than encode as a literal
- [x] Absolute paths in `SYS_spawn` everywhere: init, every desktop icon,
      the launcher, the file manager's "open in the editor". The shell
      and `gui_terminal` get the smallest thing that deserves to be called
      a search path - a command with no `/` in it is looked up in `/bin`,
      anything with one is taken as the path it plainly is
- [x] `SYS_spawn` passes the **basename** to `process_spawn` as the
      task's display name. A task-manager row reading
      `/bin/desktop_icons` says nothing `desktop_icons` doesn't, spends
      six of `TASK_NAME_MAX`'s 24 characters saying where every program
      on this system lives, and would break every self-test that asks the
      scheduler what a task is called. The path is what gets loaded; the
      name is what gets shown
- [x] The superblock magic is bumped with the layout, so the reformat
      path M29 already had does double duty as the format migration -
      there is no in-place upgrade from a flat filesystem with no root
      directory, and the kernel re-seeds everything it ships anyway
- [x] New boot self-test (`[m53]`): the layout present; `/bin` holding
      exactly the programs this build ships and nothing else; a directory
      created, filled past one block (17 records where 16 fit), listed
      and read back by path; **the same name in two directories staying
      two different files**, which a prefix convention cannot do and is
      the cleanest single statement of "these are real directories"; and
      eight malformed or escaping paths refused as a table - a relative
      path, an empty component, a trailing slash, `.`, `..`, `..` out of
      the root, walking *through* a regular file, and a component longer
      than a name may be
- [x] New interactive test (`file_manager_navigates_directories`), and
      two existing ones rewritten around the new layout. All three assert
      on *how many rows have anything in them* rather than on names -
      content-independent, and enough to tell a directory holding two
      things from one holding two dozen
- [x] `missing_program_raises_a_toast` became
      `launcher_does_not_offer_data_files`: it used to type a data file's
      name into the launcher and require an error toast, and the right
      assertion now is that there is **no result and no toast**, which is
      a better outcome than a good error message about something that
      should never have been offered.
      `clicking_a_toast_dismisses_it` sources its toast from M52's
      faulting client instead - a real crash rather than a synthetic
      failure
- [x] One harness change this milestone paid for the hard way:
      `tools/qemu-serial-test.sh` and `tools/qemu_input.py` both take
      `LEANOS_IMAGE`. A `make` rewrites `build/os-image.bin` under any
      guest still reading it, which silently invalidated a 35-minute
      suite run mid-flight. Snapshot the image, export the variable, and
      a long check and ongoing development stop fighting over one file
- [x] **A bug the interactive suite caught and no boot test could.**
      `desktop_icons.c` has two tables of program names - the icon grid
      and the right-click menu - and only the first was converted to
      `/bin` paths. Nothing about the boot self-tests touches the desktop
      context menu, so 39/39 markers passed with "Open Terminal" silently
      launching nothing. Exactly the class of failure M40's harness was
      built for, six milestones later
- [x] Full regression: 39/39 serial markers and 37/37 interactive tests

## M54 — Reclaiming what dies: address spaces, task slots, real uptime ✅

M29 documented the leak, M50 measured it, and neither fixed it: ~15
frames per dead process, forever, plus a task slot that was never
recycled. Both were bounded only by total uptime, and the boot
self-tests alone spent **79 of 128 slots** before PID 1 started.

- [x] `vmm_destroy_address_space(pml4_phys, owned, n)`: walks the
      PML4[1..511] subtree, frees every page table, and frees a *leaf*
      frame only when its virtual address falls inside one of `owned`.
      PML4[0] is untouched - it is the shared kernel map, the same
      subtree in every address space, and freeing it would take the
      machine with it
- [x] **The `owned` list is the whole design, not a parameter.** A
      process's address space contains mappings whose frames are
      emphatically not its own: an shm segment created by *another*
      process (which somebody else may still have mapped) and the linear
      framebuffer, whose physical addresses are device memory that was
      never a `pmm` frame at all and would have poisoned the free list.
      `process_destroy_address_space` in `proc.c` supplies the two ranges
      that genuinely are the process's - the image/stack/argument page,
      and the sbrk heap - because that is `proc.h`'s layout knowledge and
      `vmm.c` has no business holding it
- [x] The ordering, which was the stated risk: a task cannot free the
      address space it is running on. Solved by switching that CPU to the
      *kernel's* address space first, which is safe from exactly one
      place - `task_exit_with_code`, where the executing code and the
      stack under it are both in PML4[0] and therefore identical in every
      address space, so the switch changes nothing the function can
      observe. `loaded_pml4_phys` is updated to match, or `schedule`
      would skip the next task's CR3 reload
- [x] Reclaimed on the failure paths too, which had leaked ~15 frames
      each with a comment arguing a full task table was "an already
      degraded state not worth recovering from". That was only true
      because there was nothing to recover *with*; being under pressure
      is exactly when leaking is worst
- [x] Task slots recycled, with a generation counter. A pid is now
      `(slot, generation)` packed into the same `int` every existing
      caller passes around - 8 slot bits, the rest generation, and task 0
      is still literally pid 0. `sched_task_by_id` refuses a pid whose
      generation has moved on, which is the difference between "no such
      task" and a *plausible wrong answer about somebody else*
- [x] `sched_task_by_slot` split out for the callers that enumerate the
      table (`SYS_wait(-1)`, `SYS_taskinfo`, the shutdown path). Those
      two questions stopped being the same one the moment a pid stopped
      being an index, and conflating them is precisely how a recycled
      slot gets mistaken for its previous occupant
- [x] **Reaping, not exiting, is what frees a slot** - and that is a
      decision, not an implementation detail. A terminated-but-unreaped
      task is exactly what the compositor's `SYS_task_alive` polling
      reads to tell an orderly exit from a crash, and recycling under it
      would turn "this client died" into "this pid is unknown", which
      M29's `reap_dead_clients` and M48's crash toast both depend on
- [x] The kernel stack (8 KiB a task) goes back at reap too - at exit the
      task was still running on it. Detached under `sched_lock` and freed
      after the unlock, because `task_spawn_common` takes its heap lock
      *before* `sched_lock` specifically so the two are never nested the
      other way anywhere in this kernel
- [x] `user_space/lib/children.h` - "reap what you spawn", for the two
      long-lived processes that launch things and then forget about them.
      Without it the kernel half of this milestone would have fixed the
      boot self-tests (which always waited) and done nothing at all for
      the desktop, where an app launched from an icon had no parent that
      ever called wait. **Only the parent reaps**: it would have been
      less code for the compositor to reap every client whose window it
      reclaims, since it already notices those deaths, but it is not
      their parent - and a non-parent consuming an exit status races with
      whoever is, which is a slot handed to somebody else between a
      `SIGKILL` and the `SYS_wait` that named the same pid
- [x] The compositor treats `SYS_task_alive <= 0` as dead, not `== 0`.
      There is a third way to say "not running" now - `-1`, "the kernel
      has no such task", which includes one whose slot was reaped - and a
      window whose client the kernel has never heard of is a dead window
      either way. `2` (terminated cleanly) still keeps its window, which
      is M20's `wm_demo`
- [x] **`MAX_TASKS` stays 128 and finally means what it says.** The boot
      log's own line is the evidence, and it is the headline result of
      this milestone: `[sched] task table at handoff` went from **79 live
      of 128** to **6 live of 128, high-water mark 9**. The high-water
      mark is printed next to the live count deliberately - the
      difference between the two is exactly how much recycling did
- [x] Three boot self-tests had to be corrected, and each was wrong in
      the same instructive way: they asked about a task *after* reaping
      it, which only ever worked because nothing was ever fully reclaimed.
      The `[pgid]` test queried a child the `SYS_wait(-1)` test above had
      already reaped; `[wm]` waited on `wm_demo` and then read the window
      it left behind, which the compositor correctly tore down once the
      kernel no longer knew that pid; `[m45]` read `SYS_task_alive` after
      its own wait. All three now ask while the task is still there and
      reap afterwards
- [x] New boot self-test (`[m54]`), 5/5, and the test that could not be
      written before this milestone: **384 spawn/reap rounds - three
      times `MAX_TASKS`** - with both free frames and live task slots
      required to come back to exactly where they started. Under the old
      rules the 50th round would have failed outright and all 384 address
      spaces would still have been resident. Plus a stale pid (one whose
      slot has since been recycled) required to be *refused* rather than
      answered about, which is the check that would fail loudly if the
      generation counter were dropped
- [x] `launch_close_stress` 5 -> 60 rounds, and the number is chosen
      rather than picked: the boot self-tests used to leave 49 of
      `MAX_TASKS` unspent, so the 50th launch would have failed with
      `SPAWN_ERR_NO_TASK_SLOT` and simply not opened a window. 60 is past
      that and comfortably under the 119 a post-M54 boot leaves - a test
      that passes now and could not have before, rather than one that
      merely takes longer. It needs both halves of the milestone: the
      kernel recycling a reaped slot, and the desktop actually reaping
- [x] Full regression: 40/40 serial markers and 37/37 interactive tests

## M55 — Session resilience: supervise every client, survive a compositor crash ✅

init waited on `desktop_shell` and restarted the whole session if it
exited. A compositor or `desktop_icons` that died on its own was not
noticed until the taskbar happened to die too - and for the compositor
that meant *never*, since nothing else in the session exits when the
screen goes away. And if the compositor went, every client went with it:
not killed, but wedged, because a window was the only thing holding them.

- [x] init supervises all three desktop clients, with the same "restart
      the session" response it already had. The old arrangement was not a
      design, it was `sys_wait` taking one pid. Polled with
      `SYS_task_alive` rather than waited on, because there is no
      wait-for-any primitive here that says *which* child went, and this
      loop has to know which pid to stop watching
- [x] `wm_create_response_t.compositor_pid` - the mirror image of M29's
      `client_pid`, and it exists for the mirror-image reason. Without
      it, "my events stopped arriving" is indistinguishable from "nothing
      is happening", which is the normal state of an idle desktop; a
      client would have to guess, and the only safe guess is to do
      nothing forever
- [x] `wmclient.c` reconnects on its own. `wm_window_t` keeps everything
      the handshake needs - geometry, flags, title - because a client
      supplied all of it in the first place and redraws its pixels every
      frame anyway. The check happens inside `wm_poll_event`, the one
      call every GUI main loop already makes every iteration, so a client
      gets crash survival without a line of its own
- [x] `wm_wait_event` became a poll loop rather than a blocking read. It
      used to sit in `sys_read` on the event pipe forever, which is fine
      right up until the process on the other end dies - at which point
      nothing will ever write to it again and the client is wedged with
      no way to notice. `gui_paint` was the one client in this state
- [x] `SYS_shm_unmap` (35): removes a mapping without freeing what it
      points at - the half of `SYS_shm_free` that makes sense when the
      segment is somebody else's and, in the case this exists for, no
      longer exists at all. A reconnecting client's window buffer points
      at frames the kernel handed back the instant the compositor died,
      and leaving that mapping would alias whatever those frames become
      next. Bounded to the caller's own shm window, so it cannot reopen
      the hole M52 closed next door in `SYS_shm_free`
- [x] While a client is between windows its `gfx` context is given zero
      dimensions, not merely a null pixel pointer. That is what actually
      makes it safe: every `gfx.c` primitive clips to `width`/`height`
      before touching `pixels`, so a client that redraws on a timer in
      that window draws nothing instead of dereferencing null
- [x] `WM_EVENT_EXPOSE`, synthesized by `wmclient.c` rather than sent by
      the compositor - which has no idea this window is a *re*-connection;
      from its side it is an ordinary new client. Every GUI program in
      this project handles it, one line each next to whatever already
      sets its redraw flag. `gui_paint` is honest about coming back as an
      empty canvas: the pixel buffer *was* its model of the drawing
- [x] A fresh compositor resets the eleven well-known pipes at startup. A
      named pipe deliberately outlives every fd that ever pointed at it -
      that is the whole rendezvous mechanism - so a compositor that died
      mid-`sys_write` leaves a partial struct at the head of the stream,
      and the next one's first read would be misaligned against every
      message after it forever
- [x] **Which then broke reconnection, and the fix is the interesting
      part.** A client notices the death and queues its request the
      instant it happens; the replacement clears the pipes on startup;
      the two race, and a client that lost blocked in `sys_read` on a
      response nobody would ever write. So `connect_common` sends, waits
      with a deadline, and sends again - four attempts at 1.5s. The
      deadline is long enough that a live compositor (a tight poll loop,
      answering in milliseconds) always replies inside it, so a re-send
      only happens when the first request genuinely went nowhere
- [x] `task_manager.c`'s protected list rewritten in both directions.
      **"kernel" and "cpu-idle" added** - a real hazard nobody had
      noticed, since `SYS_taskinfo` reports the scheduler's own tasks and
      End Task on the first row would have SIGKILLed task 0, which is not
      an app crashing but the machine stopping. **compositor,
      desktop_shell and desktop_icons removed** - they were guarded
      because killing one took the screen away and nothing brought it
      back, and a guard against something that no longer happens is a
      guard that is lying
- [x] `user_space/bin/wm_crash.c`, a self-test-only program that finds
      the compositor by name in `SYS_taskinfo` and SIGKILLs it. It exists
      because M55's claim is only worth something if it can be triggered
      the way a person would, and there was no such way - killing by name
      rather than by pid precisely because the launcher can start
      programs but not hand them a number somebody had to look up
- [x] New boot self-test (`[m55]`), 5/5: a compositor SIGKILLed out from
      under two live clients, replaced, and both windows back on screen
      **with their own pixels**. Checked as pixels deliberately - "the
      client is still running" was true before this milestone too, in the
      sense that nobody killed it, and is not the claim
- [x] New interactive test (`desktop_survives_losing_the_compositor`):
      open an app, kill the compositor from the launcher, and require the
      desktop back *and that app's window with it*. The proof that the
      machine survived is launching something else afterwards, not the
      screenshot still looking like a desktop - a frozen framebuffer keeps
      showing the last frame it was given, which is exactly how the first
      version of this test passed while the session was in fact dead
- [x] Boot budgets raised in both harnesses: `SECONDS_TO_RUN` 96 -> 180
      and the interactive suite's boot timeout 90 -> 240. Five milestones
      added five self-tests and several are the slow kind for an
      unavoidable reason - proving something about a *process* means
      starting one and waiting for it. 90 was measured failing during
      M55, which presents as every test erroring out at once rather than
      as anything resembling a real bug. The final numbers are
      deliberately several times a healthy boot, which costs nothing - the
      wait returns as soon as the marker appears - and keeps a loaded host
      from being reported as a bug in whatever change is under test. Both
      went up again in M56 (180 and 300) once boot time turned out to be
      *variable* rather than merely larger: 65s on a quiet host and past
      180 on a busy one, because most of the added time is self-tests
      waiting on real processes, and those wait on the scheduler rather
      than on a clock
- [x] **The reconnect retry, and why it is 400ms.** A client notices the
      compositor died and queues its request immediately; the replacement
      clears the well-known pipes on startup; the two race, and a client
      that lost blocked forever on a response nobody would write. The
      first fix used 1.5s and passed - by luck, on timing - and then
      failed the moment M56's own self-tests shifted the boot around it.
      400ms x 12 is the honest version: a live compositor answers in
      milliseconds, so the interval is a hundredfold margin against
      re-sending into a merely-slow answer, and what it actually buys is
      that a reconnecting window is blank for a moment rather than for
      long enough to look like the crash
- [x] Full regression: 41/41 serial markers and 38/38 interactive tests
      (36 in one pass, plus two re-runs - see M56's note on the orphaned
      QEMU processes those two "hangs" turned out to be)

## M56 — Depth where people actually spend time ✅

Every app in this project is a demonstration of a mechanism. Three of
them are things a person would genuinely use, and each was one feature
short of being usable for real work.

- [x] **Undo in `text_editor.c`** - a bounded ring of edit *records*, not
      snapshots. A snapshot-per-keystroke editor is one that stops working
      on a large file: this buffer is 600 x 80 characters, so even ten
      levels of undo would be half a megabyte of copies of something that
      barely changed between them. A record is six bytes and the ring
      holds 512 of them; when it fills, the oldest is overwritten, which
      is the trade a *bounded* history makes on purpose
- [x] There are exactly three mutations in that file - insert a
      character, delete a character, append an empty line - which is what
      makes records practical here at all. Enter deliberately does not
      split a line and Backspace deliberately does not merge one, so
      there is no structural edit more complicated than "the buffer grew
      by one row" to invert. Each inverse is performed through the same
      primitives as the edit, so undo cannot drift away from what it is
      undoing
- [x] `group` is what makes a paste undo as one action rather than as
      eighty. Every user-visible action bumps a counter and undo pops
      until the group changes - one group per keystroke, which is what
      every editor that does not try to be clever about coalescing runs
      does, and being clever about it is how undo starts surprising
      people
- [x] **And paste, which did not exist.** This editor had `Ctrl+C` since
      M32 and no `Ctrl+V` at all - the clipboard was a one-way street
      between it and the terminal. The milestone's own framing ("a paste
      is currently unrecoverable") assumed a feature that had to be
      written first
- [x] **Scrollback in `gui_terminal.c`.** `grid_scroll` discarded the top
      line outright, which is why M49's wheel had nothing to reveal here
      and why a command whose output was longer than the window was a
      command whose output you could not read - `ls /bin` alone is more
      lines than the grid has rows. 200 retained lines in a *ring*: at 70
      characters a line, shifting an array on every scrolled line would
      be 14 KiB of memcpy per line of output, which is exactly the case
      this exists for
- [x] The visible rows come from one combined sequence - scrollback then
      the live grid - through a single `view_row`, so neither the drawing
      nor the selection code has to know which half a row came from. A
      reader scrolled back stays on the same *content* while new output
      arrives rather than watching it slide upward; typing snaps back to
      live, because a keystroke appearing somewhere off screen is the
      worst possible way to find out you were scrolled up; and the cursor
      is only drawn when the live grid is what is showing, since a cursor
      in the middle of old output would be claiming you could type there
- [x] `SYS_unlink` (36) and `SYS_rename` (37). The write half of this
      filesystem stopped at "create or overwrite a whole file", which is
      how a filesystem that had never had to remove anything ended up
      with `m48trunc` on it forever. Unlink frees blocks, then the inode,
      then the name - each step making the one before it unreachable, so
      an interruption leaves a name pointing at a free inode (which the
      resolver refuses) rather than a live inode pointing at blocks
      somebody else now owns
- [x] A rename moves *records*, not data - which is only true because M53
      stopped storing a name in the inode; it would have been a copy
      before that. The new record is added before the old one is removed,
      because both halves rewrite a directory through the same block
      allocator a file uses and either can fail on a full disk: a
      duplicate name is recoverable, an unreachable inode is not
- [x] A directory is refused by unlink rather than recursed into or
      emptiness-checked, and a rename over an existing name is refused
      rather than silently replacing it. Both are the same judgment:
      guessing what someone meant is how a file gets lost
- [x] File operations in `file_manager.c` - `R` rename, `C` copy, Delete
      delete - as single keys rather than a menu, since this window has
      no menu bar and a modifier chord would collide with the
      window-manager ones the compositor swallows before a client sees
      them. Copy is a read and a write, not a filesystem operation:
      leanfs has no notion of one, and a whole-file pair is exactly what
      this OS's file API offers
- [x] **Delete behind M36's confirm, and nothing else.** There is no
      trash and inventing one would be scope this does not need - which
      makes the confirm the only thing between a keystroke and a file
      that is gone, so it is the one operation that gets one. Rename and
      copy are both recoverable by doing them again
- [x] **An icon image format, finally** (`system_api/include/icon.h`).
      Every icon in this project was hand-placed rectangles in whichever
      client needed one, apologised for in three separate files - and
      that is not fixable by drawing *better* rectangles, because what
      was missing was a way to say "here is a picture" at all
- [x] Deliberately the smallest thing that is genuinely a format: an
      eight-byte header, a palette of up to sixteen RGB triples, and
      4-bit indices two to a byte. Index 0 is always transparent, which
      is what lets an icon sit over a wallpaper without an alpha channel
      this compositor could not blend anyway. A 24x24 icon is 344 bytes
- [x] `icon_draw` takes an integer scale, so one 24x24 blob serves both
      the 48px desktop icon and a 24px one elsewhere - two sizes of the
      same picture rather than two pictures that can disagree, which is
      precisely what the old per-call-site rectangles were
- [x] Not a file on disk yet, and the header says so: the loader takes a
      pointer and does not care where the bytes came from. Compiled in
      today costs a client no file I/O per redraw and no "the icon is
      missing" failure path; the day icons are user-replaceable the
      format does not change, only where they are read from
- [x] `keyboard_inject(ch, mods)` in the keyboard driver - the
      counterpart to M51's `mouse_inject`, and it carries an explicit
      modifier mask because without one a self-test could type letters
      but never a *chord*, and every interesting thing a client binds is
      a chord. Deliberately not a syscall, for the same reason: a program
      able to forge keystrokes could type into any other program's window
- [x] New boot self-test (`[m56]`), 9/9. `SYS_unlink`/`SYS_rename` round
      tripping across **64 unlink-then-rewrite cycles of the same 2000
      bytes** - a filesystem leaking even one block per unlink runs out
      inside that loop, which is the accounting a bare "the file is gone"
      assertion misses entirely. Then a real editor, driven by real
      injected keys through a real compositor: type, `Ctrl+V`, `Ctrl+S`,
      `Ctrl+Z`, `Ctrl+S`, and compare the **bytes on disk** - "AB\n"
      exactly, not "shorter than it was", which a paste that silently did
      nothing would also satisfy. Then `ls /bin` into a real terminal and
      the wheel scrolled back and forward, compared as lit pixels in the
      top row so the assertion does not depend on which filename leanfs
      returns first
- [x] Three new interactive tests: `editor_undo_restores_the_buffer`
      (the clipboard loaded from the *terminal*, since its `Ctrl+C` with
      nothing selected copies the input line - one keystroke instead of a
      drag whose pixel path the test would then also be asserting on),
      `terminal_scrollback_scrolls_with_the_wheel`, and
      `copying_a_file_shows_up_in_another_window` - copy rather than
      rename or delete, because it is the one of the three that *adds* a
      row, so the second window's list growing is a positive assertion
      rather than "something is missing now"
- [x] **Harness lessons, all of which cost real debugging time.**
      A suite that is interrupted must kill its guest. Python
      dies on SIGTERM without unwinding, so three QEMU processes
      accumulated over an afternoon of interrupted runs, each burning a
      quarter of a core - and the symptom was boots timing out at 240s in
      tests that pass in 70 on an idle machine. That reads exactly like a
      guest that has hung, which is the worst thing it could have looked
      like: it sent a real investigation after a kernel bug that was
      never there. `main` now installs a TERM/INT handler that raises, so
      the `with Machine()` unwinds and reaps its child
- [x] And it failed two tests before it was understood: a pixel
      *baseline* must be captured from a settled frame. "Something is drawn" fires as soon as the first character
      lands, and a baseline snatched between the A and the B is one a
      correct undo can never get back to - so the failure looks exactly
      like the feature being broken. `_settled_row` polls until two
      consecutive screenshots agree, which is the honest fix; a fixed
      sleep would be a guess that gets worse on a loaded host
- [x] **An intermittent boot hang, found by the suite and traced to a
      real deadlock this arc introduced.** About one boot in ten stopped
      producing output entirely, always after a self-test that kills a
      process, and it looked exactly like a wedged emulator - three
      orphaned QEMU processes from interrupted runs had already produced
      the same symptom for a completely different reason, which is how it
      stayed unexplained for a while
- [x] The cause is M54's own change, and the mechanism is worth stating
      plainly: `task_exit_with_code` now tears down the dying task's
      address space, so it takes `vmm_lock`. A fatal signal is delivered
      from the *timer tick*. So a `SIGKILL` landing while its target was
      inside `vmm_map_page_in` - holding `vmm_lock`, with interrupts on -
      re-entered `vmm_destroy_address_space` and spun forever on a lock
      its own interrupted stack was holding. One CPU, interrupts already
      off inside the IRQ handler: the whole machine
- [x] The fix is `spin_lock_irqsave`/`spin_unlock_irqrestore`, applied to
      `vmm_lock`, `pmm_lock` and `heap_lock`. This kernel already knew the
      rule and had written it down twice - `sched_lock`'s comment calls
      `cli` there "not optional hardening the way it might look", and
      `klog_lock`'s names the identical failure - it just had not applied
      it to the allocators, because until M54 nothing on the task-exit
      path took their locks. "Reachable from an interrupt handler" now
      includes anything the exit path touches
- [x] **A second race, found the same way and older than this arc.**
      `WM_RESPONSE_PIPE` is one shared stream - there has only ever been
      one, since M20, when there was only ever one client - so whichever
      client the scheduler wakes first reads whatever is at its head, which
      may be somebody else's window id and shm segment. That was
      survivable while every client *blocked* on the read and the
      compositor answered one request per loop iteration, so the orders
      lined up in practice. M55's reconnect retry stopped them lining up:
      a client can now have two requests in flight and two clients can
      poll the same pipe at once. It presented as "the desktop never
      finished painting"
- [x] Fixed by *addressing* the answer rather than by giving each client
      its own pipe: `wm_create_response_t` echoes the requesting
      `client_pid`, a client that reads someone else's keeps waiting for
      its own (the other will time out and re-ask, which is what the retry
      is for), and the compositor answers a request from a client that
      already has a live window with *that* window - which is what makes a
      retry idempotent instead of a way to end up with two windows, one of
      which nothing will ever reclaim because its owner is very much alive
- [x] **A third, and the reason the second's symptom was so confusing.**
      A client's create request is a `SYS_write` to a rendezvous pipe
      whose reader may not exist yet - and `SYS_write` to a *full* pipe
      blocks. A named pipe outlives every process that ever held it (that
      is the mechanism), so a compositor killed with somebody's unread
      request in flight leaves those bytes there; enough of them and the
      next client to connect blocks forever. Alive, supervised, never
      going to draw anything - which is precisely the one failure init's
      liveness polling cannot see. `SYS_PIPE_CAPACITY` is part of the ABI
      now (one definition, shared with `pipe.h`) so a client can ask
      `SYS_pipe_poll` whether there is room, wait for it with a deadline,
      and give up rather than block. A failed connect is loud, and init
      answers it by restarting the session
- [x] Two harness improvements that came out of chasing all three: a boot
      timeout now *preserves the guest's serial log* (a boot that never
      finishes is the one failure with no pixels and no assertion to
      report - and the log was being deleted by the teardown before
      anyone could read it), and the suite kills its guest on a signal so
      an interrupted run cannot leave a QEMU process burning a core. "The
      desktop never finished painting" also names *which* of its five
      probes was wrong now - one symptom with five causes, and which one
      it is decides whether to look at the compositor, desktop_icons or
      desktop_shell
- [x] **A self-test that made the machine look broken.** The first
      version of the block-accounting check wrote and unlinked the same
      2000 bytes sixty-four times and watched for the disk to fill - the
      honest test available when leanfs could not count free blocks. It
      cost about four thousand ATA sector writes, because every metadata
      update in this filesystem rewrites the whole inode table and bitmap
      (31 sectors since M53) and PIO writes are the most expensive thing
      this OS does. It added more than a minute to a boot that reboots,
      and presented as tests timing out. `leanfs_free_blocks` makes the
      same claim as one comparison, which is both faster and a stronger
      statement - "every block came back", not "we did not run out"
- [x] Full regression: 42/42 serial markers, and 41/41 interactive tests
      - 39 in one pass plus two re-runs. Both re-runs are boot timeouts
      at 300s in a suite whose other 39 boots took 73 seconds each, and
      both passed in 73-75s on their own immediately afterwards. That is
      the host, not the guest: this machine had been running QEMU almost
      continuously for ten hours by then, and a 41-boot suite is the
      point in the day where that shows. Recorded rather than papered
      over - see the known-gaps note below

### Known gaps and things still unverified after M56

Recorded rather than left implicit, the same way M50's list was - each of
these is a real limit of what M51-M56 actually shipped.

First, what this arc *closed* from that list, since half of it was the
plan for these six milestones: user pointers are validated (M52), a
ring-3 fault kills one task rather than the machine (M52), dead address
spaces and task slots come back (M54), the terminal has scrollback (M56),
and every desktop client is supervised (M55). What remains:

- **Real-hardware boot is still unverified.** Everything from M45 to M56
  was developed against QEMU/OVMF, and M47's ACPI work in particular is
  the kind that behaves differently on real firmware. Unchanged since
  M28; still the largest untested claim in the project
- **The S5 sleep type is still guessed, not read.** `\_S5` lives in AML
  and this project has no AML parser
- **`SYS_close` still does not close a pipe**, only the caller's slot -
  pipes are not reference-counted, so a reader blocked on a pipe whose
  last writer "closed" waits forever rather than seeing EOF. Nothing
  depends on EOF today
- **A task nobody waits on still holds its slot forever.** M54 recycles
  on *reap* deliberately (a terminated-but-unreaped task is what the
  compositor's liveness polling reads), and M55's `children.h` made the
  two processes that launch things reap them - but a child whose parent
  dies before waiting has no reaper at all. Killing the compositor with
  apps open leaks exactly that many slots. Bounded by MAX_TASKS, which is
  now a concurrency limit rather than a lifetime one, so this is a much
  smaller version of the old problem rather than a new one
- **A reconnected window comes back at the cascade position, not where
  you left it.** M55 restores a window's *identity* - size, title, flags,
  and its pixels via `WM_EVENT_EXPOSE` - because those are things the
  client supplied. Where it was on screen was the compositor's, and died
  with it. Nothing in the protocol lets a client ask for a position
- **`gui_paint` comes back empty after a compositor crash**, and says so
  in its own handler: the pixel buffer *was* its model of the drawing. It
  is the one client in this project with no state behind its window
- **`shm_free_by_owner` still frees a segment its creator owns even if
  another process has it mapped.** M50 documented this and M54's address
  space teardown deliberately does not make it worse (it never frees a
  leaf frame outside the two ranges a process genuinely owns), but the
  underlying "one owner, no refcount" rule is unchanged
- **No `rmdir`.** Directories can be created and never removed, which is
  the same shape of gap `unlink` just closed for files - deliberately not
  guessed at in M56, since nothing has asked
- **Undo has no redo**, and its ring is bounded at 512 records. Both are
  the bounded-history trade stated in the code rather than oversights
- **The file manager copies at most 16 KiB**, refused out loud rather
  than truncated. leanfs itself allows 72
- **Icons are compiled in, not files.** The format is a real format and
  the loader takes a pointer, so the day they become user-replaceable
  nothing about either changes - but today "install an icon" means
  rebuild
- **`gui_terminal` still never restores its own stdout** - it `dup2`s a
  pipe onto fd 1 and leaves it there deliberately, which means the
  terminal's stdout is a pipe for the rest of its life
- **The interactive suite takes about 45 minutes** and boots a fresh
  guest per test, which makes it sensitive to anything else using the
  host's CPU - and, apparently, to the host itself over a long session:
  a boot that takes 73 seconds all day has been measured taking past 300
  late in a run, with the guest log showing M54's spawn/reap storm
  crawling at 64 of its 384 rounds. Every such test has passed on a plain
  re-run. Nothing in the guest explains it and six consecutive serial
  boots found nothing, so it is recorded as a property of the harness's
  environment rather than diagnosed. `LEANOS_IMAGE` (M53) means a long run and ongoing
  development no longer fight over `build/os-image.bin`, and M56 made
  the suite kill its guest on a signal - but a `make` running alongside
  it will still stretch a 60-second boot past the timeout
- **Pixel baselines need settling, and nothing enforces it.** M56 added
  `_settled_row` after two tests failed on correct behavior because a
  baseline was captured mid-repaint. That is the third variant of the
  same lesson (M45's key backlog, M48's slower run), and like the others
  it is a pattern rather than something the harness makes hard to get
  wrong

## Path to a desktop someone would choose to use (M57+) ✅

M51-M56 made this desktop dependable. It raises and occludes correctly,
it survives a compositor crash, a bad pointer kills one task instead of
the machine, and what dies gets reclaimed. None of that is what a person
notices.

What a person notices is that the clock in the taskbar counts *uptime*,
because uptime is the only thing this machine can know. That every
titlebar, menu item, button and filename is the same 8x16 monospace cell,
because there is exactly one font at exactly one size and
`gfx_draw_text` advances a fixed eight pixels per character. That
pressing Enter in the text editor does not split the line. That `cp a b`
cannot exist, because `SYS_spawn(path, arg)` carries one string and
`run_line` splits the command on its first space. That windows appear and
vanish with no motion at all, so a minimized window gives no clue where
it went. And that nothing here has ever made a sound.

**Real hardware is deliberately deferred.** It is still the largest
untested claim in the project and the ordering here is a bet, not a
dismissal: proving this on metal is worth much more once it is something
people would want to run, and the work is the same work whenever it
happens. The cost of waiting is real and worth writing down - every
milestone below is one more subsystem authored against QEMU's behavior,
and M28's own gaps note has been accruing since. It comes back the moment
this arc lands.

**It landed.** All seven milestones below are done, and what the arc
actually cost is written into each one's own progress notes. Three things
are worth pulling out here because they are about the project rather than
about a milestone:

- **Every one of these found a bug in something older.** M57's measured
  text found a taskbar clock that had never been measured; M58's mode
  change found a `bar_gfx` that had been stale since M55 and nobody had
  reconnected a panel to notice; M59's descriptors found fifty-three
  `kmalloc(LEANFS_MAX_FILE_SIZE)` call sites that were fine at 72 KiB and
  absurd at 8 MiB; M62's speaker found two real interrupt-delivery bugs
  because it was the first device this kernel ever *waited* on an
  interrupt from. A milestone that touches everything is a milestone that
  audits everything.
- **The tests got harder to write, in a specific way.** Everything before
  M61 could be asserted on a settled screen. An animation is deliberately
  not settled, a sound cannot be looked at, and a benchmark's output is a
  number nobody can predict. Each needed a different answer - sample the
  path repeatedly and take the maximum, read the speaker's own gate bits
  back off port 0x61, assert the *shape* of the output rather than its
  value - and each of those answers is reusable.
- **What is still not proven is the same thing it was seven milestones
  ago**: none of this has run on metal. That is now the single largest
  claim in the project, and it has four known prerequisites rather than
  the one it started with.

The ordering below is dependency-honest rather than pure priority. M57
needs nothing and changes every window in the OS. M58 is the other half
of the same question - how big everything is - and is the one milestone
here whose cost is almost entirely downstream of its own driver. M59 is
the foundation three separate apps are visibly working around, so it
comes before the milestone that spends it. M60 finishes the two apps
people actually live in. M61 is motion, and the first thing here that
makes the compositor meet a deadline. M62 is a capability that has never
existed. M63 is the one the others make possible: running a program
nobody in this repo wrote.

## M57 — Type that isn't 8x16 ✅

M39 proved the property this milestone depends on: there is one font
table and every text path in the project blits it, so fixing the table
fixed text everywhere at once with no change to any drawing code. What
M39 did not touch is the deeper limitation - one font, one size,
monospaced, and a text renderer whose advance is a compile-time
constant. This is the largest remaining visual difference between this
desktop and one somebody would call finished, and it is the same
one-change-everywhere shape.

- [x] A proportional UI font: per-glyph advance widths in a table
      alongside the bitmap (`user_space/lib/uifont.h`'s `ui_font_t`, with
      `width[c]` for the glyph's box and `advance[c]` for how far the pen
      moves). M39's "column 7 of every cell is reserved blank" was exactly
      right for a fixed advance and is exactly what a variable one
      replaces - so the conversion of the 16-row face *is* that trim, done
      in the generator: each glyph shrunk to its own ink, with an advance
      of width + 1 that is where letter spacing comes from now
- [x] Three sizes on M39's shared baseline/cap/x-height metric, produced
      three different ways and each way chosen for a reason. **small**
      (12 rows) is hand-authored, because a 12-row cell has no room for a
      2px stem at a readable x-height - it is a different drawing, not a
      scaled one, and it is where the proportional metric shows most
      ('.' and '!' one column wide, 'i' and 'l' three, 'M' and 'W'
      seven). **ui** (16 rows) is M39's own glyphs trimmed, plus eight
      re-authored ones - 'M', 'W', 'm' and 'w' get the eighth column
      back, 'c', 'e', 's' and 'r' give one up. **large** (24 rows) is a
      3/2 nearest-neighbour scale of `ui`, which is *exact* for this
      design rather than approximate: every feature in it is a 2-pixel run
      and floor(2n/3) maps any 2-run onto a 3-run wherever it starts.
      Scaling a bitmap font is usually how you get mush; the ratio was
      picked so that it cannot be
- [x] **Tabular figures, which the milestone did not ask for and the
      taskbar clock demanded.** A proportional '1' is narrower than a
      '0', so the first thing the new font did was make the clock shuffle
      sideways once a minute. Every digit now gets the widest digit's box
      and is centred in it - what a real font's tabular-figure set does -
      and `ui_check` asserts it, because it is a defect that is obvious in
      motion and invisible in any single frame
- [x] `gfx_text_width(font, str)`, `gfx_text_width_n`, `gfx_text_fit`,
      `gfx_char_advance` and a `gfx_text_metrics_t`, **and every
      `strlen(s) * FONT_WIDTH` in the tree deleted.** The grep was the
      honest size of the milestone exactly as predicted: `settings.c`,
      `file_manager.c`, `desktop_icons.c`, `desktop_shell.c`,
      `text_editor.c`, `gfx.c` and `compositor.c` between them held every
      centring, right-alignment and truncation in the OS, and each was a
      different shape of the same wrong assumption. `gfx_text_fit` is what
      replaced "divide the space by eight" - with a variable advance,
      *how many characters fit* is a property of which characters they are
- [x] Monospace stays, and stays correct: `gfx_draw_text_mono` /
      `gfx_draw_char_mono` are M39's 8x16 cell unchanged, and
      `gui_terminal.c`'s grid and `text_editor.c`'s column arithmetic draw
      through them. Everything *around* the grid in those two programs -
      menus, dialogs, status lines - is ordinary UI text and moved to the
      proportional face. Two fonts with two jobs, as intended
- [x] The kernel console keeps `font8x16` untouched, and so does
      `kernel/drivers/font8x16.c`: the UI family is `user_space/lib` only,
      because a panic's job is legibility and it draws before any of this
      exists
- [x] The eight glyphs the UI kept faking, at codepoints 0x01-0x08 -
      arrows all four ways, a checkmark, a bullet, an ellipsis and a close
      X. They live in the control-code range because no string this OS
      draws contains one, so nothing real can collide with them. Four of
      them are already load-bearing: a truncated window title ends in the
      ellipsis rather than being indistinguishable from a window genuinely
      named "Untitled do", a directory in the file manager carries the
      arrow (until now the only thing that told one from a file was what
      happened when you double-clicked it), the selected launcher row
      carries one too, the selected colour swatch in Settings gets the
      checkmark, and `Save As` in the editor finally has the ellipsis that
      says it opens a dialog
- [x] **A dense-list size with real consumers.** `file_manager.c` and
      `task_manager.c` draw their list rows in the 12-row face and
      everything else in the 16-row one - six and four more visible rows
      respectively out of exactly the same window
- [x] Tests: `user_space/bin/fonttest.c`, spawned and waited on from
      `kernel_main` the way M19's `memtest` is, because the property worth
      proving is not "the table is well formed" (the generator's own
      `ui_check` covers that at build time) but that **`gfx_text_width`
      and `gfx_draw_text_font` agree**. It renders into its own buffer and
      measures the ink: nothing left of the pen, nothing past the advance,
      every glyph inside the box its width table promises, all three faces
      on one cap line and one baseline and one descender row measured in
      real pixels, `gfx_text_fit`'s contract checked at forty widths, and
      two clock readings measuring identically. Plus the M42 taskbar pixel
      test, which caught the tray separator moving five pixels the first
      time this booted - exactly the class of breakage `gfx_text_width`
      exists to make visible

**Progress notes.**

*What the milestone predicted correctly.* The `strlen * FONT_WIDTH` grep
really was larger than the font work, and it really was the part that
mattered - every one of those sites was silently wrong the moment a glyph
stopped being eight pixels wide, and two of them (the taskbar's
right-aligned clock and the compositor's title truncation) were wrong in
ways only a pixel test would have caught.

*What it did not predict.* Tabular figures. A proportional advance is
right for letters and wrong for anything that has to line up in a column
or tick over in place, and nothing about "make the font proportional"
suggests that the very first symptom would be a clock that walks.

*A build-size result that paid for the font.* Every user program links
`gfx.o`, and the UI family is about 18 KiB of tables, which would have
been 18 KiB in each of twenty-five programs - all of them embedded into
`kernel.bin`, which was already at 926 KiB against a 1 MiB ceiling.
`-ffunction-sections -fdata-sections` plus `--gc-sections` on the
user-program link was the fix and turned out to be worth far more than
the font cost: `hello.elf` went from 29 KiB to 5 KiB and `kernel.bin`
from 926 KiB to 402 KiB. The three faces are separate objects, so a
program that only uses one only carries one.

## M58 — Display settings: change the resolution without rebooting ✅

The resolution this desktop runs at was chosen by `boot.c` before the
kernel existed, out of a list the firmware offered: it prefers 1024x768
and takes the first BGR8888 mode it finds if that is not available.
Nothing since could change it, and the reason is worth stating plainly.
GOP is a *boot services* protocol - `build_e820_and_exit_boot_services`
is the last moment in this machine's life when anything can call
`SetMode`. So a resolution setting is not a Settings pane with a small
driver behind it. It is a native mode-setting driver with a Settings pane
in front of it, and most of this milestone was everything downstream that
had always been allowed to assume the screen never changes size.

- [x] **A mode-setting driver** (`kernel/drivers/dispi.{h,c}`). QEMU's
      default adapter is the Bochs/stdvga device and its VBE DISPI
      registers at 0x1CE/0x1CF are exactly the runtime interface GOP is
      not: disable, write width/height/bpp, re-enable with the
      linear-framebuffer bit, then **read back** - the geometry *and* the
      pitch, out of `VIRT_WIDTH`, rather than assuming `width * 4`. The
      probe answers id 0xB0C5 on QEMU 10.1
- [x] A curated, validated list rather than a pretend enumeration. DISPI
      has no mode table - it takes any geometry inside its own limits - so
      nine standard sizes, each checked against `MAX_XRES`/`MAX_YRES` and
      against the video memory the device *reports* (16 MiB on stdvga,
      read from `VIDEO_MEMORY_64K`, not assumed). Validation happens once
      at boot rather than at the moment somebody clicks, because that is
      the one place where a mistake costs them their desktop
- [x] **`fb.c` learned to be re-initialized.** `fb_remap` maps through to
      the new size and only ever *grows* the mapping (a smaller mode
      leaves pages mapped that simply stop being read; unmapping them
      would buy nothing and could race a draw already in flight), and
      `fb_mapped_bytes()` is what `SYS_fb_map` now maps for a client, so
      the two sides cannot disagree about how far the framebuffer reaches
- [x] **And it re-derives the text console before it logs anything**,
      which is not tidiness - it is the first thing a mode change breaks.
      `console.c` caches how many cells fit; on a mode that got smaller
      its cursor is instantly off-screen, and the very next `klog` line
      draws a glyph through `fb_put_pixel`, which panics on an
      out-of-bounds coordinate - from inside the logging path, so the
      panic's own message tries to draw too. The first mode change this
      project ever performed died mid-sentence in `fb_remap`'s own log
      line, which is how this was found
- [x] `SYS_display_modes` (38) and `SYS_display_set_mode` (39). The
      syscall changes the mode and re-maps the kernel's framebuffer and
      stops there; everything downstream belongs to the process that owns
      the screen
- [x] **A new event carrying the change** - `WM_EVENT_DISPLAY_CHANGED`
      (11), shaped so a client already handling `WM_EVENT_EXPOSE` handles
      this by falling into the same code. `wmclient.c` does the work (the
      re-handshake M55 already wrote, factored out and now called for two
      reasons instead of one) and hands the event on, so each of the ten
      GUI programs changed by exactly one `||`
- [x] **The shm buffer was the real constraint, named up front and
      confirmed.** `window_t.buf_w/buf_h` are set once at connect, so a
      panel allocated for 1024x768 cannot fill 1920x1080. The
      reallocation happens at the one safe moment: the client unmaps its
      copy, then asks again, and the compositor's *idempotent* answer
      (M56) is replaced by a real rebuffer for exactly the windows flagged
      by the mode change. Nothing is ever freed while another process
      holds a mapping of it
- [x] Panels re-span the screen, and the taskbar and the desktop
      background are the two clients that genuinely resize rather than
      merely being notified - their width was never the client's call
- [x] Windows that are now off-screen get clamped back on, and maximized
      windows re-maximize. Deliberately *position only*: cropping a
      window to fit would be a resize nobody asked for, and one this
      compositor could not undo when the screen grew back, since it keeps
      no memory of the size a window had before it was trimmed
- [x] The cursor is clamped too. The mouse driver turned out not to be
      where this lives - it has never had bounds of its own; the clamp is
      in `handle_mouse` against `fb_info`, so re-querying the geometry
      *is* updating the bounds
- [x] **A Resolution pane in `settings.c`**: the current mode marked, the
      offered ones as buttons, and - on hardware with no DISPI adapter -
      the honest empty state, "This display cannot be resized after boot."
- [x] **Apply, then revert on a timer unless confirmed.** Ten seconds
      (`WM_MODE_REVERT_MS`) with a countdown in the pane. The deadline
      that actually protects anybody is the *compositor's*, not
      settings.c's: the case this exists for is a screen nobody can read,
      and a timer living in a window you cannot see would be no
      protection at all - it also survives settings.c dying mid-trial.
      Nothing is written to disk until the mode is kept, because a mode
      saved the instant it was applied would come back on the next boot
      even if it was the mode that made the screen unreadable
- [x] Persisted through `settings_file.c` as two more keys in the same
      `/etc/settings.conf`, and applied by the compositor at startup
      before anything is allocated against the boot geometry - so the
      desktop comes up the right size rather than changing size a moment
      after it appears. Both save paths are read-modify-write now, so the
      theme writer and the display writer cannot erase each other's half.
      This is the point at which `boot.c`'s hardcoded 1024x768 stops being
      the policy and becomes the fallback: for the first boot, and for
      hardware the driver does not recognise
- [x] **Where this stops working, said out loud**, in `dispi.h`'s header
      and in `docs/real-hardware.md`: DISPI is a Bochs/QEMU *device*
      interface and no real GPU implements it. On real hardware the probe
      finds nothing, the `[m58]` self-test skips rather than fails, and
      the pane offers nothing. Real mode setting means a GPU driver per
      vendor and is not a thing this project will do
- [x] Tests, and there are two boot self-tests because there are two
      halves. `[m58]` in the kernel switches mode and asserts the geometry
      *and the device-chosen pitch*, then writes and reads back the last
      pixel of the new mode - which faults in ring 0 if the mapping did
      not grow and returns the wrong value if the pitch is wrong - then
      restores the boot mode. The second `[m58]` drives
      `WM_ACTION_SET_MODE` down the action pipe with a real compositor and
      a real taskbar, checks the bar re-spans the *new* width, then
      **confirms nothing and waits the revert out**, checking the desktop
      came back. Two interactive tests do the same through real clicks in
      the Settings pane, and one of them asserts on the *screenshot's own
      dimensions* - the one measurement in that whole suite that comes
      from the display device rather than from anything the guest says

**Progress notes.**

*A latent M55 bug this turned into a certain one.* `desktop_shell.c`
computed `bar_gfx` - a `gfx_ctx_t` over the bottom rows of its window
buffer - exactly once, right after connecting. Every reconnect since M55
has silently left it pointing at the previous compositor's segment; it
never showed because nothing had reconnected a panel. A resolution change
does, and the buffer is now both somewhere else *and* a different width,
so the panel drew into memory it had just unmapped and vanished. Derived
state has to be re-derived where it is used, and it is now.

*A test that passed for the wrong reason.* The first version of the
desktop self-test checked "the taskbar spans the screen" by comparing
pixels at both ends of the bar row. Bare desktop is uniform too, so it
passed while the taskbar was entirely absent. It now also compares
against a pixel well above the bar - the difference between "the bar
reaches both edges" and "there is a bar."

*The interactive harness had its own version of the same assumption.*
`app_origin()` computed the compositor's un-clamped cascade position.
Every window in the suite was short enough for that to be right until
this milestone grew the Settings window, at which point tests were
clicking 76 pixels below everything they meant to click. It mirrors
`content_bottom_limit()` now.

## M59 — Files without limits, and a machine that knows the date ✅

`SYS_readfile`'s own comment had said "no open/close/fd-table/lseek yet"
since M13 and scoped that to what M13 needed. Six arcs later it was the
limit three apps apologized for in three different ways: the editor was a
600x80 buffer because that is what fits in one whole-file read, the file
manager refused a copy past 16 KiB because its copy buffer was static
storage in every process that has one, and the terminal could not write
its output to a file at all. Separately and just as visibly, every file
in this OS was dated zero, because the machine had no clock.

- [x] `SYS_open` (40), `SYS_lseek` (41), `SYS_stat` (42), and
      `SYS_read`/`SYS_write` accepting a file-backed descriptor. The fd
      table really was already there - what was missing was a file entry
      in it, not the machinery. `FD_FILE` points at a **shared open-file
      entry** rather than carrying the inode and offset inline, and that
      is not ceremony: `SYS_dup2` exists, two descriptors made from one
      have to share a position, and shell redirection is the first thing
      that would have hit it
- [x] `SYS_readfile`/`SYS_writefile` stay, and stay the recommended call
      for a small whole file. Every current caller is one. This milestone
      makes them no longer the *only* option; rewriting nine working
      callers would be scope nothing has asked for
- [x] **`SYS_close` finally closes.** Pipes are reference counted now, so
      "the last writer went away" is knowable - and a task exiting
      releases every descriptor it holds, which is what makes it true
      without anyone having to remember. *Named* pipes deliberately keep
      the old never-auto-close behaviour and are marked `persistent`:
      that is not an oversight, it is the mechanism M55's
      compositor-replacement recovery is built on, and refcounting them
      would have meant a client exiting could give the compositor EOF on
      a rendezvous point forever
- [x] `SYS_rmdir` (43) - the same gap `unlink` closed for files in M56.
      Empty directories only, and the file manager's own message says so:
      a recursive delete is one keystroke from losing everything under a
      path and this OS has no trash to take it back out of
- [x] **Double-indirect blocks.** 16 direct + 128 indirect + 128 x 128 is
      16528 blocks, a shade over 8 MiB, up from a 72 KiB ceiling that was
      smaller than `compositor.c`'s own source. The part worth writing
      down is that this was only tractable because the direct/indirect
      split got collected into *one* `map_block(inode, logical, allocate)`
      first - it had been open-coded four times (read, write, free, and
      the write's own rollback), and a third level would have meant
      getting the same thing right four more times
- [x] **Block 0 is now reserved forever**, which is what lets a zero
      block pointer mean "nothing here" - and a freshly created file's
      inode is all zeros. Nothing needed the distinction before, because
      every block of a file was allocated in one pass and the pointers
      were only ever read back. The on-demand allocator tests them, and
      the first thing it did without this was hand every new file block 0
      over and over
- [x] **A CMOS RTC read** (`kernel/drivers/rtc.c`, ports 0x70/0x71, with
      the update-in-progress wait, the read-it-twice-and-compare that
      makes a sample coherent, BCD-or-binary, and 12-hour mode). About a
      hundred lines, and it turns three zeros into truths at once: a
      file's mtime, a date column in the file manager, and a taskbar
      clock that shows the time instead of how long the machine has been
      switched on. A machine with no readable clock is an ordinary
      outcome, not a failure - files stay dated zero and the clock falls
      back to uptime, which is exactly what it showed before
- [x] `klog_put_dec`, because a date in hex is not a date. Seventeen
      milestones of `klog_put_hex32` were right for addresses, masks and
      counts; `[rtc] 000007EA-00000008-00000019` was the line that earned
      base ten its place
- [x] File manager: name, size and date columns, clickable headings that
      sort by each, and a second click that reverses. Directories always
      sort ahead of files whatever the key is, and ".." stays at row 0 -
      it is this window's own invention rather than an entry, and "go up"
      belongs in the same place every time. Sizes are three significant
      figures and a suffix; an exact byte count is the wrong answer in a
      column nobody compares 1048576 to 999999 in
- [x] And its copy is a *loop* now. `FM_COPY_MAX` was how large a file
      this window would copy **at all**; the same constant is now
      `FM_COPY_CHUNK` and decides only how many times round the loop
      goes. Delete handles a directory too, and rename always could
- [x] **Save latency, which was a user-facing performance bug.** Every
      metadata flush wrote the superblock, the entire inode table and the
      entire 16-sector bitmap - 32 PIO sector writes whether one byte
      changed or seventy kilobytes did. Dirty-sector tracking is the fix,
      with exactly one rule holding it up: nothing mutates `inodes[]` or
      `bitmap[]` without marking the sector it landed in, and
      `mark_inode`/`mark_block_bit` are the only ways to do either. A
      one-byte edit now costs **one** sector write
- [x] `read_program()` in `kernel.c`, and `SYS_spawn` sizing its buffer
      from `SYS_stat`. Fifty-three call sites said
      `kmalloc(LEANFS_MAX_FILE_SIZE)`, which was a harmless 72 KiB
      over-allocation right up until double indirection made that ceiling
      eight megabytes - three of those at a time, per self-test, is not an
      over-allocation any more
- [x] New boot self-test (`[m59]`), six claims. A descriptor round trip
      with a real seek across the join between two writes; **a 200 KiB
      file written and read back byte for byte** with every block
      distinguishable, so a mis-mapped block is a content mismatch rather
      than an invisible one; every one of its ~400 blocks returned on
      unlink; a file dated within a minute of when it was written; `rmdir`
      refusing a directory that still holds something and accepting one
      that does not; and **the metadata write count for a one-byte
      change**, asserted as a count rather than as a latency, because
      latency is a property of the host and this is a property of the code

**Progress notes.**

*The one bug this arc's shape made inevitable, and it showed up
immediately.* Rewriting the fifty-three program loads mechanically left
each site's `if (!image) panic("out of memory")` check sitting **above**
the assignment that filled it in, so every one of them fired on the first
boot. Mechanical edits to fifty-three call sites need the compiler or a
boot to check them, and this got both.

*Where the metadata-cost assertion had to be loose.* The bound is four
sectors rather than one. An inode is 84 bytes and the table is packed, so
a single inode can straddle a sector boundary and legitimately dirty two -
and a create touches the parent directory as well. Four is tight enough
that a return to whole-table flushes (31) fails instantly and loose
enough not to be a tripwire on where an inode happens to land.

## M60 — The two apps people live in, finished ✅

M56 called these "one feature short of being usable for real work" and
shipped undo, paste and scrollback into them. Both were still one feature
short, and in both cases it was the feature you reach for without
thinking.

- [x] **Enter splits the line at the cursor, and Backspace at column 0
      joins with the line above.** M56 deferred both deliberately and said
      why: they are the only *structural* edits in the file, and undo has
      to invert them. That was the right call when undo did not exist. It
      exists now and works, which turned the argument around
- [x] Two new undo record types, `EDIT_SPLIT` and `EDIT_JOIN`, each
      inverse performed through the same primitive as the edit
      (`split_line` / `join_line`) under M56's own rule - which is why
      they needed no new machinery, only their own two lines in
      `apply_inverse`. And `EDIT_NEWLINE` is *gone*, because the primitive
      that produced it is: Enter appended an empty line at the end of the
      buffer regardless of the cursor, which is what "you cannot split a
      line" looks like from the inside
- [x] Redo. The ring already held what it needed; what was missing was a
      cursor into it (`redo_count`) and the rule that a fresh edit
      discards the forward half - one line in `undo_record`, and the line
      that keeps redo from replaying an edit against a buffer that has
      since diverged from it
- [x] Find (`Ctrl+F`) and find-next (`Ctrl+G`), searching forward from
      the cursor and wrapping - which is what makes repeated find-next
      walk every occurrence and stop where it started
- [x] **The editor's byte cap is gone.** `load_file` streamed a 16 KiB
      whole-file read, so the real limit was neither MAX_LINES nor
      MAX_LINE_LEN but "how much fits in one read", and a bigger file
      opened silently truncated. It streams through a descriptor now, and
      saves through one too. A file with more lines than the buffer holds
      still cannot be *held* - so it says so, and a plain save over the
      original is refused. Writing back 600 lines of a thousand-line file
      is how a person loses the other four hundred; Save As is the honest
      way out and clears the flag
- [x] **`argv`.** `SYS_spawn(path, arg)` carries a real vector: a
      NULL-terminated array copied into the argument page proc.c has
      always mapped, laid out as `argc`, then the pointers, then the
      strings. RDI still points at that page, so `enter_user_mode` and
      the `iretq` frame did not have to learn anything - the whole
      unpacking is two instructions in `crt0.asm`, and a program written
      as `int main(void)` keeps working untouched. The kernel supplies
      `argv[0]` itself (the path), because that is the one element it
      knows for certain and the one a program is entitled to assume
- [x] `user_space/bin/cp.c`, which exists *because* of the milestone
      rather than alongside it. `cp a b` is the example M59 and M60 both
      used for what this OS could not express, and the only honest way to
      write it before was not to
- [x] `cat` takes files, plural - and with no arguments reads standard
      input, which is what makes it the right-hand side of a pipe. That
      needed exactly one thing beyond a loop: a read on a pipe has to be
      able to *end*, and M59's refcount is what made "the last writer went
      away" knowable
- [x] The terminal parses a command line: multiple arguments, and quoted
      arguments containing spaces. An unterminated quote is refused
      rather than guessed at - guessing turns `rm "my file` into two
      arguments neither of which was meant. Deliberately not globbing, not
      `&&`, not variables: each is a shell feature with its own failure
      modes and none of them was what was missing. What was missing was
      being able to say two words
- [x] Redirection (`>`, `>>`) and a single pipe (`|`), both of which are
      exactly what M59's descriptors were for. **The step a pipe does not
      work without is the terminal closing its own copy of the write end**
      after spawning the left-hand command, so that command is the only
      writer left - otherwise nothing brings the count to zero, the
      right-hand command never sees EOF, and `ls | cat` hangs forever
- [x] Tab completion over `/bin` and the current directory: the first
      token completes against `/bin` because that is what a first token
      *is*, every other token against the working directory. One match
      completes; several print the shared prefix and the candidates
- [x] **A working directory, `cd` and `pwd`**, which the milestone's own
      "the current directory" turned out to require - this terminal had
      never had one. There is no working directory in the kernel (every
      path a syscall takes is absolute), so it is the terminal's own
      state, exactly as `file_manager.c` already does it
- [x] **And a trailing slash resolves.** `leanfs.c` refused `/bin/`
      outright, which was defensible while nothing produced such a path -
      and tab completion produces one every time it completes a
      directory, because that suffix is what tells you it is one. `/bin/`
      names the same directory `/bin` does; `/bin/ls/` is saying something
      untrue about `ls` and is still refused
- [x] The terminal closes its own fd 0 at startup. A GUI terminal has no
      standard input to give away - keys reach it as window events - and
      now that a command can be written to read one, a child that read fd
      0 would pull keystrokes out of the same kernel ring the compositor
      drains. That is a wedged desktop rather than a wedged command
- [x] New boot self-test (`[m60]`), five claims, each one a thing this OS
      could not express a milestone ago: `cp a b` with two real arguments
      (asserted on the copied bytes, since a `cp` that made an empty file
      would exit 0 too); `ls /bin > out` typed with real injected keys and
      read back; `ls /bin | cat > out` producing *the same bytes*, which
      is the assertion that says the pipe ended rather than merely ran;
      `Tab` completing `/b` to `/bin/`, checked by which directory the
      resulting listing is of; and a paragraph typed with `Enter` in the
      middle of a line, saved, undone and redone, compared byte for byte
      off disk each time

**Progress notes.**

*The conversion was the milestone.* Making `argv` real is fifty lines in
`proc.c` and four in `crt0.asm`; what it costs is every program written
as `int main(const char *arg)`, and there were eleven. A mechanical
adapter (`const char *arg = argc > 1 ? argv[1] : ""`) kept ten of their
bodies untouched, and the two that deserved the real thing got it - `echo`
now echoes every argument, and `badptr` takes the argument page's address
from `argv` itself rather than from a string in it, which is the only
version of that test that is still true for a program launched with no
arguments at all.

*Tab completion found a filesystem rule that had stopped being right.*
Completion appends `/` to a directory, which is what makes the next Tab
descend - and `ls /bin/` was refused by the resolver. The rule was
written when nothing produced such a path, and the test that asserted it
had to move from the refusal table to an assertion of its own, in both
directions: honoured on a directory, still refused on a file.

## M61 — Motion, and a compositor that meets a deadline ✅

Windows in this OS appeared and disappeared instantly. That was not a
missing polish detail so much as missing information: a minimized window
vanished and nothing on screen said where it went, which is precisely
what an animation toward its taskbar button exists to say. M44 already
added the blend primitive this needed. What was missing was a clock.

- [x] **A real frame clock in `compositor.c`**, driven independently of
      input. `REDRAW_INTERVAL_MS` (100) is a *fallback poll* for changes
      the compositor cannot otherwise notice - its own comment says so -
      and a fallback poll is not a clock. `FRAME_MS` (16) is the ask
      nothing here could previously make: "a frame in sixteen
      milliseconds because something is moving." It is checked *before*
      the dirty/fallback path, so a frame owed at 16 ms is never waiting
      on a 100 ms poll
- [x] Minimize and restore animate to and from the window's taskbar
      button. This is the one that carries information rather than
      decoration, which is why it is first
- [x] **The compositor is *told* where the buttons are**
      (`WM_ACTION_SET_TASKBAR_SLOT`), by `desktop_shell.c`, when its
      layout changes - which is when a window opens or closes, not per
      frame. That geometry is the taskbar's, and a compositor that
      recomputed it would be a second copy to keep in step. A window
      nothing has reported animates toward the bottom of the screen under
      itself, which is the honest answer when nothing has said better
- [x] Open and close: a short scale from and to the middle of where the
      window is. Close is started in `reclaim_window`, which is the one
      place every way of losing a window funnels through - so a window
      that vanished because its process faulted gets the same treatment
      as one that was closed on purpose. From the screen's point of view
      they are the same event
- [x] The snap preview fades in rather than appearing, and the launcher
      fades rather than materialising - both on the same clock, because
      "one more reason for the frame clock to tick" is a much smaller
      thing than a second timer
- [x] **An animation draws a ghost rectangle, not a scaled copy of the
      window.** That is a scope line rather than a shortcut: resampling a
      live client's buffer every frame is real work, sixteen milliseconds
      is not long, and the thing being communicated ("it went *there*")
      is carried entirely by the geometry
- [x] 140ms, and ease-out - motion that starts fast and settles reads as
      a thing arriving somewhere, where linear motion reads as a thing
      being dragged. Animations that announce themselves are the ones
      people go looking for a setting to turn off
- [x] **And that setting exists anyway**, in `settings.c`, on by default
      and persisted. Motion that cannot be disabled is a genuine
      accessibility problem for some people, not a preference - and "off"
      means instantaneous, exactly what this desktop did before M61,
      rather than merely faster
- [x] **The performance milestone, and the budget is written down.** A
      frame repaints the union of where each rectangle was and where it
      now is - a few hundred rows, not a screen - which is what makes 16
      ms reachable at all. The compositor times each frame, counts the
      ones that overrun, and prints a single line at the end of a run if
      any did. A run that met its deadline says nothing, and
      `tools/qemu-serial-test.sh` fails on the line's *presence*. "An
      animation that stutters is worse than none", turned into something
      a harness fails on rather than something you have to watch for
- [x] Tests: the interactive suite already knows how to compare settled
      pixels, and an animation is the one thing that is deliberately not
      settled - so `[m61]` asserts the endpoints **plus an intermediate
      frame that is neither**: a pixel somewhere on the path between the
      window and the taskbar, lit by the ghost, that is bare desktop at
      both ends. Then the same minimize with motion switched off, which
      must produce nothing on that path at any point

**Progress notes.**

*The test taught this milestone something about its own harness.* A
kernel self-test sleeps with `hlt` and comes back when the scheduler next
picks it, so "wait 60 ms" is a floor rather than a time - a single sample
can land after a 140 ms animation has already finished, which is exactly
how the first version of this test failed. Sampling the path repeatedly
and taking the maximum is what makes the assertion about whether motion
happened rather than about when the sampling task happened to wake up.
Every future test of something transient has the same problem and now has
the same answer.

*Where the shortcuts list went.* The Motion switch needed a row, and the
Settings window was already as tall as it can be without the compositor
clamping it away from its cascade position. The twenty-seven pixels came
from moving the nine-row Shortcuts list into M57's 12-row face - which is
what that face is for, and which nothing had used it for outside the file
manager and the task manager.

## M62 — The first sound this OS has ever made ✅

M48 built an entire notification system in which an error arrives in
complete silence. Nothing here had ever driven a speaker, which makes
even a beep a new capability rather than a refinement of one.

- [x] **The PC speaker** (`kernel/drivers/pcspk.c`): PIT channel 2 and
      port 0x61, a square wave at a frequency for a duration. Seventy
      lines, and it works on machines with no sound device at all.
      Channel 2 is a different counter from the system tick's channel 0,
      so programming it disturbs nothing
- [x] Nothing blocks. A tone records a deadline and the timer tick turns
      it off - one comparison per tick. A sound that held its caller for
      its own duration would be a sound that stops the compositor drawing,
      which is the wrong trade for something that exists to accompany a
      toast
- [x] **The speaker is owned by one process**, and the ownership is
      *enforced* rather than asked for - the same judgment M56 made about
      `keyboard_inject`, made enforceable. There is no permission model
      here, but "the first process to claim it holds it until it exits"
      needs none: `SYS_audio_claim` succeeds once, every audio call
      refuses anybody else, and a dead owner owns nothing. The compositor
      claims it at startup, before any client connects, for the same
      reason it owns the screen
- [x] **One sound that is not a test: an error toast beeps.** That single
      connection is what makes this a feature instead of a driver. Only
      errors - a beep on every informational toast is a machine people
      mute, and a muted machine is one that cannot tell them anything
- [x] **AC'97 output** (`kernel/drivers/ac97.c`) - the same shape of work
      `rtl8139.c` already is: PCI enumeration that exists, two I/O BARs
      (a mixer and a bus-master block, which is why `pci.h` grew a BAR1
      accessor), a descriptor ring, an IRQ, and a buffer of 16-bit stereo
      48 kHz PCM
- [x] A volume control in `settings.c` - five steps sharing a row with
      M61's motion switch, because those two are the settings about how
      this desktop *behaves* rather than how it looks. Persisted like
      every other setting
- [x] **And a mute honoured by the beep as well as the stream.** On the
      AC'97 mixer, zero sets the codec's explicit mute bit rather than
      maximum attenuation - "muted" and "very quiet" are different claims
      and only one of them is what a mute switch promises. On the speaker
      it is the *only* thing volume can honestly mean: that hardware is
      one bit, and a quiet beep is not something it can produce
- [x] Absent hardware degrades rather than panics, exactly as M27 decided
      for the NIC: no AC'97 device means `[ac97] no AC'97 audio device
      found - the PC speaker is the only sound this boot`, and the
      self-test skips its stream half rather than failing it
- [x] `SYS_audio_release`, which the design did not have and the test
      found: ownership released only by *exiting* is ownership a
      long-lived process cannot hand over, and `kernel_main` is exactly
      that - it claims the speaker for its own self-test, never exits,
      and would have silently kept it away from the compositor for the
      life of the machine. An error toast would have gone back to
      arriving in silence, which is the one thing this milestone exists
      to fix
- [x] New boot self-test (`[m62]`), and the interesting part is what is
      checkable at all when the output is sound. Not "it sounded right":
      port 0x61's gate bits are read back while a tone plays and after
      its deadline passes (real hardware state, not a flag this code
      set); the AC'97 device is asserted to have *finished a buffer it
      was given*; muting leaves the gate shut; and a second, real process
      (`audiograb`) is refused the claim, the beep and the volume -
      which is the only place the ownership rule can honestly be tested
      from - and then the owner handing it back, with a beep afterwards
      that must fail

**Progress notes.**

*Two real interrupt bugs, found because this is the first device this
kernel ever waited on an interrupt from.* `pic_clear_mask` unmasked a
slave-PIC line without unmasking the master's cascade input, so
unmasking IRQ 11 unmasked nothing - `mouse.c` had been working around
exactly this since M18 with a hand-written `pic_clear_mask(CASCADE_IRQ)`
next to its own, and that workaround is now the rule. And
`pci_enable_device` never cleared the PCI command register's Interrupt
Disable bit, which UEFI firmware routinely leaves set. `rtl8139.c` found
neither, because it registers a handler and then polls its status
register anyway.

*And one that is not a bug, written down rather than worked around.* Even
with both fixed, the completion interrupt does not arrive: the device
asserts it (`GLOB_STA`'s POINT bit), the PIC's masks show IRQ 11 and the
cascade both open, and the PIC's own *request* register is empty - so it
never reaches the 8259 at all. On this machine the firmware routes PCI
interrupts through the I/O APIC, which this kernel does not program; it
drives the LAPIC for SMP and nothing else. Wiring up an I/O APIC is a
subsystem, not a bullet on an audio milestone. So `ac97_completions()`
polls the device's status register - exactly what `rtl8139.c` already
does, for exactly the same reason - and the IRQ handler stays registered
because it is correct and costs nothing on a machine that does deliver
it. **An I/O APIC is now the honest prerequisite for any device this
kernel wants to be interrupt-driven by**, and it is not written down
anywhere else.

## M63 — Somebody else's program ✅

Every binary this OS had ever run was written in this repo. This is the
milestone that changed that, and it is the reason `argv` is in M60 and
descriptors are in M59 - both were prerequisites rather than
coincidences.

- [x] **Floating point, which did not exist here at all.** Both `CFLAGS`
      and `USER_CFLAGS` carried `-mgeneral-regs-only` and the context
      switch had no `fxsave`/`fxrstor`, because nothing had ever needed
      one. That was a correct and deliberate simplification and also an
      absolute wall: essentially no real C program compiles without
      `double`. CR0.EM is cleared and CR0.MP/NE set, CR4.OSFXSR and
      OSXMMEXCPT are on, every task carries a 512-byte 16-byte-aligned
      FXSAVE area, and the flag is dropped from `USER_CFLAGS` **only**
- [x] The kernel keeps `-mgeneral-regs-only`, and that is what makes this
      cheap rather than pervasive: kernel code cannot touch xmm
      registers, so an interrupt does not have to save them and neither
      does a syscall. **Only a task switch does** - and it saves the
      outgoing task's state and loads the *incoming* one's before
      switching, rather than restoring on the way back. A fresh task
      never returns from `context_switch` at all, so "on the way back" is
      not a moment that exists for it; loading ahead of the switch is
      what gives every task a floating-point state somebody chose rather
      than whatever the last one left behind
- [x] CR0/CR4 are per-CPU, so `fpu_init_cpu()` runs on the BSP and again
      on every AP. A core that missed it would fault the first time a
      task doing float work was scheduled onto it, which is a bug that
      presents as "sometimes"
- [x] **A libc subset - ours, not somebody else's.** `string.h`,
      `stdlib.h`, a `stdio.h` over M59's descriptors, `math.h`, `time.h`,
      `ctype.h`, `assert.h`. Writing them is the opposite of linking
      newlib, and the ground rules hold
- [x] `memcpy`, `memset`, `strlen` and `strcmp` are deliberately *not* in
      it - they come from `user_space/lib/str.c`, where they have always
      been. Two copies of `memcpy` in one binary is exactly the kind of
      thing that quietly diverges
- [x] **Pick the program first, and let it decide the surface.**
      `third_party/whetstone/whetstone.c` - the 1998 C translation of the
      1972 Whetstone benchmark - and its link errors were literally the
      specification: `sin`, `cos`, `atan`, `log`, `exp`, `sqrt`, `printf`
      with `%ld`/`%.1f`/`%12.4e`, `fprintf(stderr, ...)`, `atol`,
      `strncmp`, `time(0)`
- [x] **Changes made to the ported source: none.** It compiles and runs
      exactly as downloaded, with its licence's comment block intact.
      That is the actual claim: the program was not adapted to lean_os,
      lean_os was made able to run the program
- [x] It runs through the same path everything else does - built into
      `/bin`, spawned by `SYS_spawn`, stdout on an ordinary pipe. There
      is no special path for third-party code, which is the only way
      "this desktop runs somebody else's program alongside its own" is
      true rather than arranged
- [x] The build stays honest: `third_party/` is the boundary and nothing
      from it moves into `kernel/` or `user_space/lib/`;
      `THIRD_PARTY_CFLAGS` is the ordinary user flags with `-Werror` off
      (treating a 1998 program's warnings as errors would mean editing
      somebody else's program to make it build); and
      `docs/third-party-programs.md` now says what was ported, what was
      changed, which headers the port needed, and - just as usefully -
      what a ported program still cannot have
- [x] The memory budget was known before starting rather than discovered
      half-way: `SYS_sbrk` is growth-only, the address-space layout is
      fixed in `proc.h`, and Whetstone needs none of it. That same budget
      is why DOOM is not what got ported - see the notes
- [x] New boot self-test (`[m63]`). `user_space/bin/libctest.c` is
      written *as a program* - standard headers, nothing from this
      project - and checks the maths against values that are either right
      or not (`sin(pi/6)` is exactly a half, `log(e)` is exactly one,
      `sin(1000)` is a range-reduction test rather than a series test),
      the formatter against strings that are either right or not
      (`%12.4e` of 1234.5 is `"  1.2345e+03"`), and then runs a
      200,000-term float loop that spans many scheduler quanta - which is
      the only way to catch a broken FXSAVE/FXRSTOR, because a switch
      that lost xmm state would corrupt the sum *intermittently*. Then
      Whetstone itself, with its output read back through a pipe and
      checked: "it exited 0" would also be true of a program that printed
      nothing

**The number.** `C Converted Double Precision Whetstones: 83.3 MIPS`,
from the boot log, produced by code nobody in this repo wrote.

**Progress notes.**

*What got ported, and honestly why it was not DOOM.* The milestone named
`doomgeneric` as canonical and a smaller first step as a legitimate
alternative "if the memory budget turns out to be the wall". The wall
here was a different one and worth stating plainly: this work was done in
an environment where pulling forty thousand lines of third-party source
into the tree was not a reasonable thing to do, and a port whose source
could not be reviewed line by line would have been a worse artifact than
one whose could. Whetstone is 433 lines, it is unambiguously somebody
else's program, and it exercises exactly the two things that were
missing - floating point and a C library. What it does not exercise is
the framebuffer, the keyboard and a few megabytes of heap simultaneously,
which is what DOOM would have added. **The enabling work is done; the
remaining distance to DOOM is the WAD, the heap budget, and forty
thousand lines of somebody else's source.**

*Two bugs in one file, and they were different bugs.* `math.c` failed its
own self-test twice on `cos(-40)`. The first cause was precision -
`x - k * TWO_PI` loses a bit for every power of two in `k`, fixed with a
Cody-Waite split whose first term multiplies exactly. The second, which
survived that fix and looked identical from outside, was *range*:
reducing to [-pi, pi] and running an eight-term Taylor series is accurate
near zero and wrong by 5e-8 near pi. Reducing to quadrants puts the
argument under pi/4 where the same eight terms are good to 1e-16. A test
that had only checked `sin(0)` and `cos(0)` would have passed throughout.

## M64 — A network user space can reach ✅

M27 shipped a NIC driver, Ethernet, ARP, IPv4 and ICMP. For the
thirty-six milestones after it, the only thing that ever used any of it
was one boot self-test pinging the gateway. `sock` appeared nowhere in
`syscall.h`. This is the milestone that makes the whole subsystem
reachable from a program, and the shape of it was decided by work that
was already done for other reasons.

- [x] **UDP** (`kernel/net/udp.c`) - a length, two ports, and the
      checksum over RFC 768's pseudo-header. Computed on send and
      *verified* on receive rather than accepted, because the
      pseudo-header is the single easiest thing for a from-scratch stack
      to get wrong and a wrong checksum nobody checks is a bug that only
      appears on somebody else's machine
- [x] **Sockets as file descriptors**, not as a handle namespace of their
      own. `fd_slot_t` was already a tagged union with refcounting, a
      working `SYS_close` and inheritance across `spawn` - all four built
      in M59 for open files, and all four exactly what a socket needs.
      `FD_SOCKET` is one enum value, one union member and two `case`
      labels. A socket is closed by the `close` that already existed,
      inherited by the `spawn` that already existed, and duplicated by
      the `dup2` that already existed
- [x] Received datagrams queue in the kernel, eight per socket, because
      they arrive in the NIC's IRQ handler and the program that wants
      them is asleep. Each keeps its source address and port - that is
      the entire difference between `recv` and `recvfrom`, and a client
      that cannot tell who answered cannot check that the right server did
- [x] **Loopback**, three lines in `ip_send`: anything addressed to us or
      to 127-anything is handed straight back up instead of put on the
      wire. It builds a *real* header rather than taking a shortcut
      around one, so the path a loopback datagram takes is the path a
      real one takes. This is what makes the socket layer testable on a
      machine with no network at all
- [x] **A DHCP client** (`kernel/net/dhcp.c`) - DISCOVER, OFFER, REQUEST,
      ACK, and an xid that ties a reply to *this* exchange so another
      client's ACK on the same segment cannot configure us. `net.h`'s
      constants become the fallback rather than the answer. It does not
      renew, and that is stated in its own header rather than hidden: a
      machine that runs for a week on a two-hour lease loses its address,
      and this OS has never run for a week
- [x] `SYS_socket`, `SYS_bind`, `SYS_sendto`, `SYS_recvfrom`,
      `SYS_sockpoll`, `SYS_netconf`, `SYS_settime`. No
      domain/type/protocol arguments on `socket`: there is one kind of
      socket here, and three parameters that each take exactly one value
      are three ways to be wrong about an API with no choices in it
- [x] **SNTP, in user space** (`user_space/lib/sntp.c` and `nettime`) -
      "the second way to know the time", next to M59's CMOS clock. In
      user space deliberately: a kernel SNTP client would be a kernel
      parsing replies off the network, which is a far larger trusted
      surface than a sixty-line program needs, and the entire point of
      this milestone is that the ordinary syscalls are now enough
- [x] `SYS_settime` stores an offset applied on every clock read rather
      than reprogramming the CMOS registers. Rewriting a machine's
      hardware clock is something its owner asks a boot utility to do,
      and a program that got the network working is not the same thing as
      that owner's consent
- [x] `netconf`, which prints the configuration and says whether it came
      from a lease or a fallback. That last line is the whole reason the
      program exists: on QEMU the two configurations are byte-for-byte
      identical, so a DHCP client that did nothing at all would produce
      an indistinguishable `[net]` log line

### Progress notes

*This milestone found two ways to stop the machine, in the same eight
lines of M27 code, and both were invisible for thirty-six milestones for
the same reason.* `ip_send` called `panic("ip_send: ARP resolution timed
out")`. That was defensible for thirty-six milestones:
the only caller was a boot self-test pinging a gateway guaranteed to
answer. The moment `SYS_sendto` exists, "the destination is unreachable"
becomes an ordinary thing a program does by typing an address wrong - and
M52's rule is that user space does not get to halt the machine. M52 could
not have caught this, because in M52 there was no way for user space to
reach `kernel/net` at all. **A subsystem with no consumers has no
user-facing failure modes, and therefore no bugs, right up until it has
one consumer.** `nettest` asserts that specific call returns -1.

**And then the fixed version hung the machine anyway.** The bounded wait
that `panic` came after was a `hlt` loop - correct for every caller it
had, all of which ran in kernel context with interrupts on. `int 0x80`
goes through an *interrupt* gate, so IF is clear for the entire syscall:
a `hlt` there halts the CPU with nothing able to wake it, and the timer
that would advance the deadline is precisely the interrupt that cannot
fire. The boot log ended mid-self-test with no panic, no output and no
clue - which is a worse failure than the panic it replaced, and it is the
one that would never have been found by reading the code, because the
loop is obviously correct right up until you notice which gate it is
reached through. A `schedule()` instead of the `hlt` would have deadlocked
the same way for the same reason.

The fix is what BSD has always done with a packet whose neighbour is
unknown: send the ARP request, drop this packet, let the caller send
again. `resolve_neighbor` waits only when `IF` is actually set, and
otherwise returns immediately. **The real fix is a trap gate on vector
0x80** so interrupts stay enabled through a syscall, which is what every
production kernel does - and which is deliberately not being done inside
a networking milestone, because it makes every syscall in the system
preemptible at once.

*Five of `nettest`'s fourteen checks are about failing correctly*, which
is the right ratio for a surface that has just become reachable for the
first time: a bound port refusing a second bind, `sendto` on a
descriptor that is a pipe, `recvfrom` on stdout, an oversized datagram
refused rather than truncated, and the unreachable send above. And one is
about a table: open and close a hundred sockets in a loop, because
`MAX_SOCKETS` is 32 and M50's lesson was that an entry allocated and
never freed looks perfect until the thirty-third caller.

*The self-test asserts `net_config_is_leased()` explicitly, and that is
the one assertion here that took real thought.* QEMU's SLIRP hands out
10.0.2.15 with 10.0.2.2 as the gateway - which is precisely the fallback
this kernel has hardcoded since M27. So a DHCP client that never sent a
packet would produce an identical configuration, an identical log line,
and an identical successful ping. The only way to tell "we got a lease"
from "we fell back" on this network is to ask which one happened, so the
kernel tracks it and the test demands the first.

*What `nettime` asserts is a failure, on purpose.* The gateway on this
network does not run NTP, so the self-test's real claim is that the
program comes back, comes back non-zero, comes back inside the second its
own deadline promises, and says why - because failing cleanly against an
unreachable server is what almost every network program spends most of
its life doing, and a program that hung there would hang any desktop that
shipped it.

*Still not here, and deliberately: TCP, DNS, and a permission model.* TCP
has its own entry below and keeps it. DNS is why `nettime` takes an
address rather than a name, and why `netconf` prints the DNS server it
was handed instead of pretending to use it. And a network is exactly the
thing that makes the permission-model entry below stop being theoretical
- any process can now open a socket and talk to anything.

## M65 — A permission model, finally worth having ✅

This project declined to build one four times, in writing, and was right
each time. `SYS_shutdown`'s own comment names what the alternative would
have been: "the same fake check the task manager's own desktop-process
guard is careful not to make". For a single-user desktop running only
programs from this repo, "any process can do anything" is a defensible
position and saying so beats decorating it.

What changed is not the argument, it is the machine. **M63** made it
possible to run a program nobody here wrote. **M64** made it possible for
that program to open a socket and talk to anything. The stretch-goal
entry said to revisit this "the moment a ported program is something a
person downloads rather than something this repo builds", and both halves
of the sentence it was waiting on have now happened.

- [x] **A capability model, not a permission one**, because there are no
      users here. No login, no uid, no owner on a file - and inventing
      one would have been a far larger lie than the one being fixed, since
      a uid nothing sets and nothing checks is decoration. What this OS
      genuinely has is *a process has a parent, and the parent chose to
      start it*, and the whole model rests on that one relationship
- [x] **The set only ever shrinks.** `init` starts with everything; a
      child gets its parent's set minus what the manifest withholds; a
      process can drop its own and can never get them back. There is no
      call anywhere that sets a bit that was clear - `sys_dropcaps` is an
      `AND` and the spawn path is an `AND`. That single property is what
      makes this checkable rather than merely present: no code path has to
      be *trusted* to hand a capability back, because none can
- [x] **The manifest is applied by the kernel, in `process_spawnv`.** Ten
      capabilities, and a table in `system_api/include/caps.h` naming
      every shipped program that needs more than the default. Putting it
      in the kernel rather than in the compositor is the difference
      between a rule and a suggestion: a launcher that forgot to consult
      it would be a way around the entire model, and the way to not have
      that problem is to not give launchers the choice. The shell launches
      programs too
- [x] `CAP_APP_DEFAULT` is `CAP_FS_WRITE` and nothing else. An ordinary
      windowed program cannot paint on the screen, read the clipboard,
      enumerate processes, open a socket, change the resolution, set the
      clock, claim the speaker or switch the machine off
- [x] **Ten gates, and the ones that are deliberately absent.** Reading a
      file is not a capability - this OS has no secrets on disk, and
      claiming a read boundary nothing enforces would be exactly the fake
      check this milestone exists to stop making. Neither is `SYS_fb_info`
      (how big is the screen) or `SYS_netconf` (what is this machine's
      address): those are facts, and gating a fact is the kind of check
      that looks like security and is not
- [x] `SYS_kill` is the one gate with a relationship in it. **A parent may
      always end what it started**, capability or not, walking the whole
      parent chain rather than one level - a shell that spawned a program
      that spawned a program is still the reason all three are running.
      Anything else needs `CAP_KILL_ANY`, which two programs on this
      machine have
- [x] A denial is **logged once per process per capability**. Silent
      refusal is how a permission model becomes an unexplained bug: the
      program sees -1 from a call with ten other reasons to return -1, and
      whoever is debugging it has nothing to go on
- [x] `caps` and `caps -a` on the command line, for the same reason
      `netconf` exists: a rule nobody can see is a rule nobody can check

### Progress notes

*The grant table got shorter as it was written, and that is the finding.*
The first draft gave `desktop_shell` `CAP_POWER` - it has "Shut down" on
the Start menu, so obviously it needs to switch the machine off. It does
not. It asks the compositor to, over the WM protocol, and it turned out
that **almost every program on this desktop is already built that way**:
the taskbar, the file manager, the editor, the paint program and the
clock hold nothing beyond writing files, because everything they do to
the screen they do by asking another process rather than by doing it.
Fifteen years of "capabilities are hard to retrofit" and the retrofit was
small here for a reason that had nothing to do with capabilities - the WM
protocol had already drawn the line, and this milestone only wrote it
down. **The programs that needed a grant are the ones that talk to
hardware or to other processes directly, and there are eleven of them.**

*The pattern from M52 held again.* M52's rule was "no user-triggerable
panic", and it could only be applied to what user space could reach. M64
made `kernel/net` reachable and immediately found a `panic()` in it. M65
is the same shape one level up: `SYS_fb_map` was open to every process
for forty-five milestones and nothing was wrong with that, because every
process was one of ours. **A boundary is worth exactly as much as the
population it separates, and both of the milestones that changed that
population were the two immediately before this one.**

*What this is not.* It is not a defence against a kernel exploit, and the
header says so. Nothing here stops a process writing to a page it should
not have; that is `vmm`'s job and M52's. What it stops is a program doing
something its launcher never intended - which is the failure a downloaded
program actually presents, and the only one a model with no users can
honestly claim to address.

## M66 — TCP ✅

The stretch-goal entry that asked for this is one sentence long and it
set the terms: *"Retransmission, congestion control and an eleven-state
machine are not a bullet on somebody else's milestone."* So this is its
own milestone, and what got built is a TCP that would work against
somebody else's - not a loopback toy with the interesting parts left out.

- [x] **The eleven states of RFC 793's figure 6**, driven by segment
      arrival and by a 100 ms clock: CLOSED, LISTEN, SYN_SENT,
      SYN_RECEIVED, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT,
      CLOSING, LAST_ACK, TIME_WAIT. Including the two that get skipped in
      half of the from-scratch implementations on the internet -
      CLOSING, for a simultaneous close, and TIME_WAIT, which actually
      expires here rather than being a state nothing ever leaves
- [x] **Sequence arithmetic that wraps.** Every comparison is a
      subtraction cast to `int32_t`, never a comparison of the numbers -
      `a < b` on sequence numbers is wrong for exactly the connection
      that has run long enough for it to matter, which is the one that
      will be hardest to debug
- [x] **Retransmission with a measured timeout.** Jacobson/Karels per
      RFC 6298: `srtt` pre-scaled by 8 and `rttvar` by 4 so the estimator
      is shifts and no division, `rto = srtt + 4*rttvar` clamped to a
      one-second floor, exponential backoff, and Karn's rule that a
      retransmitted segment is never measured
- [x] **Reno congestion control** - slow start, congestion avoidance,
      fast retransmit on the third duplicate ACK, and fast recovery with
      the window inflated per departing segment and deflated on exit. A
      timeout resets `cwnd` to one segment because a timeout means
      congestion; three duplicate ACKs halve it because they mean one
      segment was unlucky and the rest arrived
- [x] **A window that means something.** Advertised from the space left
      in the 4 KiB receive buffer, re-advertised the moment a reader
      drains it, and respected by a sender that takes the smaller of it
      and `cwnd`
- [x] **MSS from the option on SYN**, clamped to what a 1500-byte MTU can
      carry, with the initial congestion window sized from it once known
- [x] `SYS_listen`, `SYS_connect`, `SYS_connstat`, `SYS_accept`,
      `SYS_send`, `SYS_recv` - and `SYS_socket` grows its first parameter
- [x] Closing the last descriptor on a connected socket is an **active
      close**, not a discard: the peer gets the FIN and whatever is still
      queued, and the kernel runs the connection down through FIN_WAIT
      and TIME_WAIT after the fd is gone
- [x] TCP's 100 ms clock is a **kernel thread**, not a second PIT hook.
      The scheduler owns that hook, and a networking milestone reaching
      into the scheduler to get a timer is a networking milestone with a
      scheduler change hidden in it

### Progress notes

*M64 argued in writing against a `type` parameter on `SYS_socket` -
"three parameters that only ever take one value each are three ways to be
wrong about an API that has no choices in it" - and that argument was
correct for exactly as long as its premise held.* TCP ends it: there is a
real choice now, so there is **one** real parameter and not three, and
`OS_SOCK_DGRAM` is 0 so every call written before this milestone still
means what it meant. Worth recording because the temptation was to
either defend the old decision or pretend it had always been provisional,
and it was neither - it was right, and then the world changed.

*The hardest thing to test here was the thing loopback makes impossible.*
Loopback never loses a segment. That makes it a superb place to test the
state machine - both ends of every connection in `tcptest` are this
stack, so every path runs twice per connection - and the worst possible
place to find out whether **retransmission** works. And "the
retransmission path is untested" is true of most from-scratch TCP
implementations and is essentially never written down, because the
transfer succeeds and the test passes. So `tcp_debug_drop_next()` throws
away the next N segments *after* they are built and after the sequence
numbers have advanced - so the stack believes exactly what it would
believe if the wire had eaten them - and the self-test asserts both that
the 16 KiB transfer completes anyway **and that a retransmission actually
happened**. The second half is the one that matters: without it, a stack
that silently never dropped anything would pass.

*The transfer is 16 KiB through a 4 KiB send buffer on purpose.* A
"hello world" over TCP proves the handshake and nothing else - not the
window, not the congestion window, not buffer compaction on ACK, not a
sender that has to stop when the buffer fills and resume when it drains.
Four times the buffer, at eleven segments per bufferful, with
position-dependent content compared byte for byte, is the smallest test
that touches all of them.

*The loopback shortcut M64 wrote was correct for UDP and wrong for TCP,
and the difference is one sentence.* M64's `ip_send` handed a packet
addressed to this machine straight back up the stack with a direct call -
fine for a protocol where delivering a datagram cannot produce another
one. TCP is not that protocol: a data segment delivered makes the
receiver ACK, the ACK delivered lets the sender send more, and the whole
16 KiB transfer would have run inside **one recursive call chain**, dozens
of frames deep on a kernel stack with no room for it, every level
overwriting the single static buffer the level below was still reading
out of. The fix is a queue and a draining flag: recursion becomes
iteration, stack depth is two no matter how much traffic one send sets
off, and a full queue drops a packet - which is what a congested
interface does and what every protocol above it already handles. Found by
reading the code with TCP in mind rather than by a test, which is worth
noting because the symptom would have been an intermittent stack
overflow, and that is the kind of bug a passing test suite hides.

*Deliberately absent, and each for a stated reason:* window scaling, SACK,
timestamps, PAWS, Nagle, urgent data - performance features on a stack
that has no consumer whose performance anyone has measured. And
**out-of-order reassembly**: a segment arriving ahead of a gap is dropped
rather than held, which RFC 793 explicitly permits, costs one round trip
when it happens, halves the size of the receive path, and is the reason
go-back-N retransmission is the right match for the receiver this has.

## Stretch goals (unordered, orthogonal to the desktop path)

- [x] SMP (multi-core) support
- [x] UEFI boot path as an alternative to BIOS (M24, above; BIOS itself
      later removed in M26, leaving UEFI as the only path)
- [x] Package/build tooling for third-party user programs (M25, above) -
      and finally used for its actual purpose in M63, which is the first
      time this project builds a program nobody here wrote
- [x] Networking stack + NIC driver (M27, above)
- [~] Port to real hardware (USB boot test) - prep/tooling/runbook done
      (M28, above); the manual boot step itself was **deliberately
      deferred until the M57-M63 arc landed**, and that arc has now
      landed. It is the one thing in this file that cannot be done by
      anything without hands: there is no physical machine or USB port in
      the environment this was built in. It is also still the largest
      untested claim in the project, and the bet that proving it on metal
      is worth more once this is something a person would want to run has
      been taken - what is on the other side of it is a desktop with
      proportional type, a resolution setting, real files, a real shell,
      motion, sound, and somebody else's program running in a window.
      **Four** things to know when it comes back, each one found by writing
      the arc above rather than by booting anything: it was never one
      manual step - `leanfs.c` calls `ata_read_sectors` directly, and a
      machine with no IDE controller has nothing for it to talk to, so a
      block-device indirection plus a bootloader-supplied ramdisk root is
      the real prerequisite. A panic on a machine with no serial port is
      currently a black screen, so panic needs to paint before anyone
      boots one. And M58's mode-setting driver is a Bochs/QEMU device
      interface, so on real hardware the Display pane can only ever show
      the mode the firmware picked - real mode-setting is a GPU driver
      per vendor, and that is not a thing this project will do.
      **M62 adds a fourth**: PCI interrupts do not reach this kernel's
      legacy 8259 on the machine it was developed against, because the
      firmware routes them through an I/O APIC nothing here programs -
      `ac97.c` polls instead, and any device that genuinely needs to be
      interrupt-driven needs that subsystem first
- [x] Directories in leanfs - promoted out of this list and scheduled as
      M53. It was always "the one structural limit that shows up in three
      different apps at once", which is a milestone, not a stretch goal
- [x] An icon image format - promoted to M56, where it landed as
      `system_api/include/icon.h`: an eight-byte header, a sixteen-entry
      palette and 4-bit indices, plus a loader that scales by an integer
      so one blob serves two sizes
- [x] Audio: a PC-speaker beep at minimum, an AC'97/HDA output stream at
      most - done as **M62**: both, plus an ownership rule so one program
      cannot seize the machine's only speaker
- [x] Window animations (minimize/restore, launcher fade) - done as
      **M61**, where the missing piece turned out to be exactly what this
      entry always said it was: a frame clock the compositor drives
      independently of input
- [x] Multiple virtual desktops - done, and the entry's own prediction
      held on both halves. It *was* cheap: an `int8_t workspace` on
      `window_t`, a `window_here()` predicate at the four places that
      already walked the z-order, and a `-1` for chrome that means "on
      every desktop" so the taskbar and the desktop background did not
      need a special case. And it *was* the payoff for M49's chords -
      four new rows in `shortcuts.h` and the Shortcuts pane listed them
      without being told, which is the whole reason that table exists.
      The one thing it cost was a bug the shape of the table itself:
      `Ctrl+Shift+Alt+Right` matched **Snap right** first, because the
      matcher takes the first row that fits and the snap rows did not
      forbid Shift. So a window sent to the next desktop snapped to the
      right half of this one instead - visible, wrong, and caught by the
      self-test's fourth assertion on the first boot. Fixed twice over:
      the four-modifier rows moved above the ones they extend, *and* the
      snap rows now forbid Shift, so re-sorting the table cannot bring it
      back. The self-test is pixels rather than protocol replies - a
      window's own pixel at (150,150) against bare desktop at (500,500) -
      because "the window is on workspace 2" is only interesting if it
      also means the window is not on the screen, and a stale rectangle
      left behind by a bad redraw would pass any reply-based check
- [x] **A network user space can reach** - done as **M64**, and every
      one of the four things this entry asked for landed: UDP, a socket
      surface in the fd table M59 made real, a DHCP client, and SNTP as
      the second way to know the time. The entry's own diagnosis was the
      useful part - "the one whole subsystem in the project with no
      consumers" - and it turned out to understate the consequence: a
      subsystem with no consumers also has no *bugs*, because nothing can
      reach its failure modes. The first thing that reached them found a
      `panic()` on an unreachable address
- [x] **TCP** - done as **M66**, separately and later exactly as this
      entry insisted. The sentence held up as a specification: the
      eleven-state machine, retransmission with a measured RTO, and Reno
      congestion control are what got built, and each one took the space
      the entry said it would need
- [x] **A permission model** - done as **M65**, on exactly the trigger
      this entry named. M63 ended the "only its own programs" half of the
      sentence and M64 gave those programs a network, so the deferral
      stopped being defensible and the milestone came next. What the
      entry did not predict is how *small* it turned out to be: the WM
      protocol had already made almost every program on this desktop ask
      another process for anything it could not do itself, so writing the
      boundary down was mostly a matter of noticing where it already was
- [x] **An AML parser**, or enough of one to read `\_S5` instead of
      guessing it. Named in every gaps list since M47 - and "enough of
      one" is exactly what got written, which is the part worth being
      precise about. `acpi_find_s5` scans the DSDT for the encoding of a
      `Name (\_S5_, Package)` and decodes the small integers in it. It
      does not evaluate anything, has no namespace, and would miss an
      `_S5` defined inside a method. On QEMU it reads SLP_TYPa=0,
      SLP_TYPb=0 - which is exactly the value power.c had been guessing,
      so the guess was right and is now *known* to be right, which is the
      whole difference. The guess stays as the fallback, because a DSDT
      this cannot read is still a machine that should switch off, and the
      boot log says which one was used
- [x] **Icons as files rather than compiled in.** M56 predicted that this
      day would change only where the bytes are read from, not the format
      or the loader - and that is precisely what it cost: `icon.h` gained
      one function (`icon_bytes`, because a file has a length and a
      compiled-in array had `sizeof`), and `desktop_icons.c` gained a
      forty-line `load_icons`. Nothing in the loader changed.
      The compiled-in blobs stay as the *seed*, for the same reason the
      kernel seeds `/bin` from blobs inside `kernel.bin`: a fresh disk has
      no icons on it and something has to put them there. Everything is
      read back from `/icons/NAME.icn` afterwards, so replacing an icon
      is replacing a file. Asserted by a self-test that does exactly that
      - writes a magenta palette entry into `/icons/Terminal.icn`,
      restarts the desktop, and requires the pixels to change
- [x] **A faster interactive suite.** By M63 it was 44 tests, a fresh
      guest each, and a full run had reached an hour and a half - which
      is a tax that gets paid on every commit, and the trend only ever
      went one way. Two of the three things this entry asked for landed;
      the third turned out to be the wrong idea.
      **Guests run in parallel** - a third of the machine's cores, capped
      at four - which took a full run to about twenty minutes. **A quick
      tier** (`--quick`): eight tests covering launching, closing,
      z-order, the editor, the terminal, the filesystem, settings
      persistence and crash recovery, in **5 minutes 38 seconds**
      measured. **Batching tests into one guest** was deliberately not
      done: a test that inherits eight open windows from the previous one
      is not testing what it says it is, and window/fd/shm exhaustion is
      exactly the class of bug this suite was written to catch.
      One thing learned in the doing and written into the runner: the
      failure parallelism produces is a *boot timeout*, which is the
      harness giving up rather than a verdict about the desktop. The
      allowance now scales with the job count, and the script's header
      says to re-run a timed-out test alone before believing it

---

# Where this is, and what M67+ is for

Sixty-six milestones answered one question over and over: *can this be
built from scratch?* Bootloader, long mode, paging, SMP, a filesystem
with directories, a compositor, fifteen applications, a TCP stack with
Reno congestion control, a capability model, and somebody else's program
running in a window. The answer is yes, repeatedly and in writing.

**The next arc answers a different question: would you leave this
machine switched on?** That is not the same question, and the gap
between them is not features. It is three things this project has never
had to face, each one invisible for exactly the reason M64's `panic()`
was invisible for thirty-six milestones — nothing has yet been in a
position to notice.

**One: nothing in this system ever waits.** `task_state_t` has four
values and none of them is blocked. A shell parked at a prompt is a
`while` loop in `sys_read` calling `schedule()` forever
([kernel/arch/x86_64/syscall.c:280-292](kernel/arch/x86_64/syscall.c#L280-L292)).
The compositor's main loop ends in `sys_yield()`
([user_space/bin/compositor.c:4408](user_space/bin/compositor.c#L4408)),
and so does every one of the eleven programs that link `wmclient`. An
idle desktop on this OS runs every core flat out, forever. In QEMU that
is a warm laptop and nobody notices. It is also the reason the 50 ms
quantum is a latency floor rather than a ceiling, and the reason there
is no honest thing to say about power.

**Two: the machine cannot tell you what happened.** There is no
`SYS_klog`. Every capability denial M65 was careful to log, every driver
message, every spawn failure, and the panic handler's last words all go
to a serial port and to a framebuffer console that the compositor paints
over the moment the desktop starts. On any machine without a serial
cable, this OS's entire diagnostic surface is a black screen. M65 wrote
that "a rule nobody can see is a rule nobody can check" and then shipped
its denials into a log user space cannot read.

**Three: nothing on this disk is crash-safe.** `vfs_flush` is called by
exactly one caller, the orderly shutdown path. There is no `fsync`, no
write ordering, no generation count, nothing that checks a filesystem on
mount. `SYS_writefile` truncates and then writes, so a power cut in the
middle of saving a document loses the document *and* the version it was
replacing. `SYS_rename` refuses an existing destination, which means the
one atomic-replace trick every editor uses is not available. This is
fine while the only files are test fixtures. M60 called the editor and
the file manager "the two apps people live in", and the moment that is
true, this is the thing that will cost someone their work.

M67–M74 close those three, in that order of foundation, and then spend
what they unlock. Nothing below needs a physical machine.

## The arcs

| | Milestones | The claim it earns |
|---|---|---|
| **Foundation** | M67–M69 | The machine rests when idle and stays responsive when busy |
| **Trust** | M70–M71 | It tells you what happened, and it does not lose your work |
| **Reach** | M72–M74 | It can be automated, it can reach a name, and it remembers |

---

## M67 — A kernel that can be interrupted ✅

M64 found this, named it, and deliberately refused to fix it inside a
networking milestone: *"The real fix is a trap gate on vector 0x80 so
interrupts stay enabled through a syscall, which is what every
production kernel does — and which is deliberately not being done inside
a networking milestone, because it makes every syscall in the system
preemptible at once."* That sentence is a milestone description. This is
it.

[kernel/arch/x86_64/idt.c:62](kernel/arch/x86_64/idt.c#L62) installs
`SYSCALL_VECTOR` as `IDT_GATE_INTERRUPT_RING3` (0xEE), so `IF` is clear
from the `int 0x80` until the `iretq`. That has been load-bearing in a
way nothing wrote down: **`IF=0` is currently the kernel's only mutual
exclusion.** Every syscall handler has been written, for sixty-six
milestones, against an implicit guarantee that no interrupt and no other
task can observe it half-done. This milestone removes that guarantee,
which is why it is a milestone on its own and not a one-line diff.

- [x] Change vector 0x80 to a **trap gate** (0xEF), so `IF` survives into
      the handler. One line, and then the entire milestone is the audit
      it forces
- [x] **Audit every syscall handler for re-entrancy**, one at a time,
      against the question "what breaks if a timer tick lands here and
      another task enters this same function". The known shapes:
      - static/global scratch buffers shared across callers (the TCP
        loopback queue in M66 already found one of these the hard way)
      - the fd table, mutated without a lock while another task on
        another core may be reading it
      - `sched_*` bookkeeping reached from both the tick and a syscall
      - the shm segment table, the socket table, the pipe ring buffers
      - `leanfs`/`vfs` state, which is the one where a race costs a file
        rather than a frame
- [x] Take a real lock where the audit finds one is needed, and **say in
      the comment what it protects and against whom** — a lock with no
      stated invariant is the next person's mystery
- [x] The bounded waits M64 had to make `IF`-conditional
      (`resolve_neighbor`) can stop being conditional and go back to
      being ordinary waits
- [x] SMP is not new here but it stops being theoretical: the audit must
      assume two cores, because M-something-ago it already was two cores
      and only `IF=0` was hiding it on the syscall path

**How we'll know.** A new serial self-test that runs several CPU-bound
tasks alongside a syscall-heavy one across cores and asserts no
corruption — plus the existing 50 boot self-tests and 44 interactive
tests, both of which are now testing something they were not testing
before, because every one of them was previously running against a
kernel with a global implicit lock.

**Explicitly not here:** `syscall`/`sysret` instead of `int 0x80`. That
is a performance change, and this project has never measured syscall
cost. Changing the entry mechanism and the interrupt discipline in the
same milestone would leave neither one bisectable.

### Progress notes

*The one-line half took one line, and the audit it forces found that most
of the work had already been done - by SMP, years of milestones ago, for
a different reason.* `pmm`, `vmm`, `heap`, `klog` and the scheduler
already had locks, because a second core had already forced the question
for the state those five own. What SMP never forced was the state only a
*syscall* touches, because a syscall could not be preempted and two cores
rarely land in the same handler at the same instant. So the six locks
this milestone adds - `fs_lock`, `pipe_lock`, `shm_lock`,
`openfile_lock`, `clipboard_lock`, `net_lock` - are precisely the
subsystems that are reached from ring 3 and nowhere else. **The boundary
that had no lock was exactly the boundary that had a hardware one.**

*`net_lock` is recursive and every other lock here is not, and the
difference is a real finding rather than a preference.* The rule for the
other five is "one subsystem, one lock, taken at its own entry points".
That does not work for `kernel/net`, because M66's loopback drain
re-enters the receive path from inside a send - so a lock taken
per-function deadlocks on itself the first time a program talks to its
own machine. The alternative to recursion was to prove that no path ever
nests, which is a proof that has to be redone every time anyone adds a
caller. Worse, the socket table is *also* reached from three places that
are not net entry points at all - `SYS_close`, `SYS_spawn`'s fd-table
copy, and `task_exit_with_code` - and none of them can know whether it
was called from inside a net syscall that already holds the lock. Owner
tracked by CPU rather than by task, which is only sound because the lock
is always held with interrupts off.

*Every new lock disables interrupts while held, and that is a deliberate
cost with a named successor.* The reason is M56's, which was written
about `vmm_lock` and turns out to be general: **a fatal signal is
delivered from the timer tick, so a task preempted while holding a lock
can be torn down by `task_exit_with_code` and never release it.** That is
a permanent deadlock, not a slow spin. Interrupts off for the duration is
the smallest thing that makes it impossible. It is also the wrong answer
for `fs_lock` specifically, where "the duration" is a run of synchronous
ATA PIO transfers - and the right answer is a lock a task can *sleep* on,
which is M68's, because there is no blocked state to sleep in until then.

*The self-test is four processes, not two, and every assertion is a
pattern read back rather than a call that returned.* The temptation with
a race is to write a timing loop and call "ran for N seconds without
crashing" a pass. That test is worth nothing: it fails intermittently on
a broken kernel and passes intermittently on one, so it can never be
believed in either direction. `racetest.c` writes a byte derived from its
own pid, the round number and the offset, and demands all three back -
so a process that read another's bytes has a deterministic failure with
a name. Four racers rather than two because with two an interleaving has
to be unlucky twice to be visible, and on a machine with more than two
cores two processes can run without ever contending at all.

*The thing that did not break is worth recording too.* All 55 pre-existing
boot markers passed on the first run after the gate flipped. The
prediction going in was that the resource-counting self-tests - which
compare a frame count before and after and assume nothing else is running
- would start failing intermittently now that `kernel_main` itself is
preemptible. They did not, and the reason is that those tests spawn a
child and `SYS_wait` for it, so the only other runnable task is the one
they are measuring. A self-test that had merely *slept* for a fixed
interval instead would have been the fragile one.


## M68 — Wait queues, and the end of the busy loop ✅ (second attempt)

The foundational milestone of this arc, and the one everything after it
gets cheaper because of. It needs M67 first: blocking correctly means
being confident about what a task can observe half-way through.

- [x] **`TASK_BLOCKED`** in `task_state_t`, and a scheduler that does not
      consider a blocked task runnable. Four states became five; the
      fifth is the one the other four have been faking since M7
- [x] **A wait queue primitive** — a list a task parks on and something
      else wakes. One mechanism, used by everything, rather than a
      bespoke spin per subsystem
- [x] Convert every spin the kernel currently calls waiting:
      `sys_read` on stdin, `SYS_wait`, pipe reads on an empty pipe,
      `SYS_recv`/`SYS_recvfrom` on an empty socket, and the TCP
      connect/accept paths
- [x] **An idle task that `hlt`s.** The `cpu-idle` identity already
      exists per-core ([kernel/sched/sched.c:182](kernel/sched/sched.c#L182));
      it just never gets to sleep, because there is always something
      runnable. This is the payoff line: on an idle desktop, every core
      halts until an interrupt arrives
- [x] **A blocking multi-wait for user space** — the thing the GUI stack
      actually needs. Every `wmclient` program and the compositor itself
      polls *n* pipes and then yields; what they want is "sleep until any
      of these has something, or until this deadline". One syscall
      (`SYS_waitfds` or similar) over fds and a timeout, replacing the
      `sys_yield()` at the bottom of eleven main loops
- [x] Keep `SYS_yield` and the non-blocking `*_poll` calls. They are the
      right primitive for a compositor mid-frame, and removing them to
      prove a point would be a regression dressed as cleanup

**How we'll know.** An idle-CPU self-test: boot to the desktop, touch
nothing for five seconds, and assert accumulated idle time is above a
threshold — a number that is currently zero on every core, and which no
amount of reading the code would have told you. The interactive suite
must pass unchanged: a desktop that sleeps correctly is indistinguishable
from one that spins, right up until you measure it or touch the case.

**Why this before anything else in the arc.** M61 built a frame clock so
the compositor could meet a deadline. It cannot meaningfully meet one
while every other process on the machine is permanently runnable and
round-robin gives each of them 50 ms. M69 is not possible without this.

### First attempt — what it found, and why it was reverted

**Status of the FIRST attempt: implemented in full, verified as a mechanism, and reverted.** It landed on the second, after M69 built the measurement and the condition-waits it needed. Both accounts are kept, because the first one is where the bugs are.
The work is preserved in a git stash rather than deleted. This entry is
the useful output, and it is longer than the milestone because what the
attempt learned is worth more than what it shipped.

*What was built and did work.* `TASK_BLOCKED` as a fifth task state; a
sleep/wakeup channel primitive of the V6/xv6 shape (an address used as an
identity, a 128-entry scan, no allocation); absolute-deadline wakeups
swept from the timer tick; a sequence counter closing the lost-wakeup
race for the three waiters that have no condition lock to hold;
`SYS_waitfds` and `SYS_idle_ticks`; per-CPU idle tasks; and a
`pit_sleep_ms` that leaves the run queue instead of halting inside it.
Every one of those was exercised and correct in isolation.

*Three real bugs, each of which only exists once something can sleep.*

1. **A deadlock that needs both sides asleep.** `pipe_write` parking on a
   full pipe woke nobody - its wake is at the *end* of the function,
   unreachable from the middle. So a compositor filling a client's event
   pipe slept holding a pipe full of events the client had never been
   told about, while that client slept waiting for them. Invisible before
   M68 because neither side could sleep.
2. **A `noreturn` under a lock.** Folding the fatal-signal check into
   `sched_block_on` quietly moved it to *before* the caller's lock was
   released. `sched_deliver_pending_signal` does not return when a signal
   is pending, so a task killed at that instant died holding `pipe_lock`,
   with interrupts off, and every pipe on the machine then spun on a lock
   whose owner no longer existed. **The bug is invisible in the diff** -
   both versions "check for a signal before sleeping", and only one
   survives the check finding something.
3. **A spin inside an interrupt handler.** Refusing to return from
   `schedule()` until something was runnable looked like the safe way to
   avoid running a blocked task. It is reachable from the timer tick,
   where `irq_restore` puts back `IF=0` - so it spun with interrupts off,
   waiting for a run queue that only an interrupt could change. A dead
   machine, no panic, no output, about one boot in two, and it *moved
   when anything changed the timing, including the klog added to
   diagnose it*. The fix is to return: a task that is BLOCKED and still
   running is on its way to `schedule()` and will park there.

*Why it was reverted, and this is the finding that matters.* With the
mechanism correct, the boot suite still failed - and the failures
**wandered**. M36's close handshake on one run, M55's crash recovery on
the next, `wm_demo`'s window creation on the one after. A failure that
moves between runs is not a bug in the thing being tested; it is a whole
suite's timing assumption being wrong at once.

**Roughly fifty boot self-tests are written against fixed
`pit_sleep_ms` budgets measured on a desktop where nothing ever
blocked.** The window-manager protocol *is* pipes - a window is created,
drawn, focused, closed and torn down through them - so any change to how
promptly a pipe write reaches its reader changes the timing of every one
of those tests at once. Narrowing the scope did not help: the failures
persisted with the client loops reverted, and again with pipe blocking
itself reverted, which says the perturbation is not one call site.

*The order was wrong, and that is the lesson.* This file put M68 before
M69 on the reasoning that you cannot tune latency you cannot leave the
run queue for. That is true and it is beside the point: **M69's first
bullet is "measure input-to-photon before changing anything", and
without that number there is no way to tell a scheduling change that
helped from one that broke fifty timing assumptions.** The attempt had
no before, so every after was a guess. M69 should come first, its
measurement should replace the fixed `pit_sleep_ms` budgets in the boot
suite with something that waits for a condition rather than a duration,
and M68 should follow - at which point it is a change with evidence
either side of it rather than a wander through somebody else's timeouts.

*Kept from the attempt, in M67 and already landed:* nothing of M68
itself. The stash holds the whole thing and is worth re-reading rather
than re-deriving - the three bugs above are the expensive part, and they
will all be waiting again.

### Second attempt — what actually made it land

Landed with **62/62 boot markers** and the interactive suite green,
including pipe blocking, which is the piece that destabilised everything
the first time. Three things changed, and only one of them was in M68.

**1. The suite stopped betting on durations.** M69 converted 25
`process_spawn("compositor"); pit_sleep_ms(N)` pairs into
`selftest_wait_for_compositor()`, which waits for the desktop to be
painted. That is the root cause the first attempt tripped over, fixed at
source.

**2. The verification loop got 3.4x faster,** which is not a footnote.
The boot now reports its own time (143 s) against a capture budget that
had been bumped to 820 s without ever being measured. Cutting it to 240 s
took a full run from 13.5 minutes to 4 - and the entire second attempt
was a *bisection*, which is only affordable at four minutes a step. The
first attempt failed partly because each experiment cost a quarter of an
hour and there were a dozen of them.

**3. `pit_sleep_ms` does NOT block, and this was the whole thing.**
Bisection found it in one step: with the blocking sleep disabled and
every other part of M68 in place, the boot ran clean to M68's own test.

The reason is worth stating because it inverts the milestone's own
assumption. **A blocked task gives up the CPU and has to queue to get it
back; a halted one is resumed in place by the interrupt it is waiting
for.** So blocking adds a full scheduling round-trip to every sleep, and
this boot performs 138 of them plus polling loops that call it hundreds
of times. Measured: a self-test polling `pit_sleep_ms(10)` 300 times took
3 seconds halting and **15.6 seconds blocking**. That is the "everything
got slower" the first attempt kept seeing, and it was never the pipes.

**Not every wait should become a sleep.** A wait that is already cheap -
one instruction, woken by the interrupt it is waiting for - gets *more*
expensive when you promote it to a scheduling decision. Blocking pays
when the alternative is spinning through `schedule()`, which is what
`SYS_waitfds`, the pipe waits, stdin and `SYS_wait` do. It does not pay
for a timed halt, and the idle accounting never needed it to: the
`sched_idle_enter/exit` brackets say "this CPU is halted waiting for
time", which is true of a `hlt` loop and is what the measurement counts.

*What is asserted, and what is only reported.* The self-test asserts the
mechanism - a task in `SYS_waitfds` is `TASK_BLOCKED` rather than
runnable, and a write to the pipe it waits on wakes it. It **reports**
the idle tick count rather than asserting a threshold, because the boot
is not an idle desktop: the tcp-timer and `kernel_main` are legitimately
halting while several self-test clients are legitimately spinning, and
asserting a number against that mixture would be asserting the shape of
the boot rather than the behaviour of the scheduler.

*Still deliberately not converted:* the compositor's and every wmclient
program's main loop, which still end in `SYS_yield`. `SYS_waitfds` exists
and is tested; adopting it across the desktop changes the latency of
every message on the WM protocol and belongs with the terminal/pty work
in M72's remaining half. That is also the change that would finally let
M69's priority classes work, since it is what gives the scheduler a
signal to classify.



## M69 — Latency you can feel ✅

**Reordered after M68's attempt.** This entry used to open "with M68
done"; it now goes first, and M68's attempt notes above explain why in
detail. The short version: M68 could not tell a scheduling change that
helped from one that broke fifty timing assumptions, because it had no
measurement to compare against - and the first bullet below is exactly
that measurement. Doing this first also gives the boot suite a way to
wait for a *condition* rather than a *duration*, which is the thing that
has to change before anything can safely alter how promptly a pipe write
reaches its reader.

The scheduler's job is to get the right task running fast when something
happens. That is a different number to optimise from "share the CPU
fairly", and nobody has ever measured it here.

The number this milestone owns: **input-to-photon** — the wall-clock
milliseconds from a keypress or click landing in the driver to the
changed pixel reaching the framebuffer. Nobody has ever measured it here.
The current floor is structural and bad: a 50 ms quantum
([kernel/sched/sched.c:23](kernel/sched/sched.c#L23)) means the
compositor may not run for 50 ms after the event that concerns it, then
the client may not run for 50 ms after that, then the compositor again.

- [x] **Measure it first, before changing anything.** A timestamp
      injected at the driver and read at the compositor's blit, reported
      in the serial log. A milestone that tunes a number it never
      measured is a milestone that cannot claim anything
- [x] **A shorter quantum**, and honest about the trade: more context
      switches for lower latency, which is the right trade for a desktop
      and the wrong one for a batch machine, and this is a desktop
- [x] **Two priority classes, not a full nice(2).** Interactive and
      batch, with a task that just woke from a wait queue getting the
      interactive class and a task that used its whole quantum decaying
      toward batch. This is the classic heuristic, it is about forty
      lines, and it is the entire reason a Whetstone run does not make
      the desktop stutter
- [x] **Anti-starvation, stated as an invariant**: a batch task always
      makes progress, because a "responsive" desktop that hangs a
      compute job is a worse machine, not a better one
- [x] Make the priority visible in the **task manager**, since M45 built
      the place for it and a scheduling class nobody can see is the same
      unfalsifiable claim M65 warned about

**How we'll know.** Input-to-photon under three conditions, asserted with
budgets rather than eyeballed: an idle desktop, a desktop with a
CPU-bound program running on every core, and a desktop with a large file
copy in flight. The second is the one that matters — it is the case that
is visibly broken today and cannot be fixed by making the compositor
faster.

### Priorities: three attempts, and what finally made them work

**They landed on the third, and the fix was not in the scheduler.**

The first two attempts are below. Both failed the same way from opposite
directions - whatever signal was used, **the compositor was
misclassified, because it never blocked.** So the third attempt started
by changing that: the compositor's main loop now ends in `SYS_waitfds`
(M68's call) instead of `SYS_yield`. That had been deferred twice as "a
WM-protocol change belonging with the pty work"; it turned out to be a
thirty-line change once M69 had already converted the suite off fixed
sleeps, and it is the thing everything else was waiting on.

With the compositor genuinely blocking, the signal works: **a task that
was `TASK_BLOCKED` and got woken is waiting on the world.** A spinning
task cannot claim it however often it polls, and a blocking one cannot be
denied it - immune to both inversions rather than to one.

*One more thing was needed, and M61 found it.* The compositor spins **on
purpose** while an animation runs, because a 16 ms frame budget cannot be
met by a 10 ms timer - so it burned its slices and demoted itself during
exactly the 140 ms when it most needed the CPU. The fix is
`last_block_tick`: demotion requires burned slices **and** no block
within the last second. Still unfakeable by a spinner - it has to have
actually blocked to set it - and now robust to a burst of honest work.

*And `need_resched`, because priorities alone changed nothing.* Waking a
task makes it READY; nothing reschedules until the running task's quantum
expires. So the compositor could be woken by a mouse interrupt, outrank
everything, and still wait out somebody else's slice - which is precisely
the latency the classes were meant to remove. A flag set by a promoting
wake and honoured by the next tick, rather than a `schedule()` inside
`sched_wake_all`, which is called from interrupt handlers and from inside
other subsystems' locks.

**What the numbers say, and what they cannot.** Input-to-photon is
essentially unchanged (39-41 ms idle, 38-41 ms loaded) - because the
2-tick quantum had already removed the load penalty, which was the large
effect. Raising the load to four CPU-bound tasks gave 99 ms *both with
and without* priorities, and that turned out to be **the probe reporting
on itself**: the observer polls a pixel through `schedule()`, never
blocks, is correctly classified batch, and with five batch tasks it runs
about every fifth slice. Measuring a genuinely loaded desktop needs a
probe that blocks rather than polls, and that is a different instrument -
recorded in the self-test rather than papered over.

So priorities are kept on correctness grounds, with the honest caveat
that this milestone's instrument cannot show them paying off. The
mechanism is right, the classification is now unfakeable in both
directions, and the measurement to judge a better probe against exists.

### Second attempt at priorities, after M68 — and the sharper conclusion

Retried once M68 gave tasks a real blocked state, on the theory that
**"this task was BLOCKED and something woke it"** is a signal a spinning
task cannot fake. That is true, and it was still not enough: it broke the
M30 window-chrome self-test on the first run.

The inversion came back by the *opposite* route. The wake-based signal
stops a spinning client from claiming to be interactive. It does nothing
about the **compositor**, which also never blocks - it busy-polls its
request pipes and ends its loop in `SYS_yield` - so it burns whole slices
and gets **demoted**. A client that blocks on a pipe then outranks the
compositor it is waiting for, which is exactly the shape that wedged the
first attempt.

So the conclusion is sharper than "the signal was wrong", and both
attempts reached it from opposite directions:

> **The compositor is the one process on this machine that must never be
> classified as batch, and it is the one process that never blocks. No
> behavioural classifier can work until it does.**

That is not a scheduler change. It is converting the compositor's main
loop - and every wmclient program's - from poll-and-yield to
`SYS_waitfds`. Priorities become possible the moment that lands and not
before, and the measurement to judge them by now exists.

### Progress notes

**The number, measured for the first time:**

| scheduler quantum | input-to-photon, idle | with one CPU-bound task | boot suite |
|---|---|---|---|
| 5 ticks (50 ms, as shipped since M7) | 19.5 ms | **98.5 ms** | pass |
| 2 ticks (20 ms) | 41.0 ms | **39.2 ms** | **pass 57/57** |
| 1 tick (10 ms) | 21.9 ms | **19.7 ms** | fail - animation frame budget |

*The idle column wanders and the loaded column does not, and only one of
those is a finding.* Idle input-to-photon depends on where in the
compositor's loop the event happens to land, so 19-41 ms is one number
with a lot of variance in it. The loaded column is the result: **at a
50 ms quantum, one CPU-bound task made the cursor five times slower;
at 20 ms the penalty is gone.** The quantum WAS the latency floor, which
this file had asserted since M61 without ever checking.

*The first measurement measured the wrong thing, and the mistake is
instructive.* The polling loop that watches for the pixel to change was
originally a tight spin - which makes the observer a CPU-bound task, so
the compositor could not run again until the observer had burned its
whole 50 ms slice. It reported 82 ms idle. Adding a `schedule()` to the
observer took the same measurement to 19.5 ms. **A latency probe that
competes with the thing it is timing is measuring itself**, and this one
was wrong by a factor of four before anyone looked.

*Two priority classes were built, measured, and removed - and the reason
is the most useful thing this milestone found.* The plan was the classic
heuristic: a task that gives the CPU up before its slice is done is
waiting and should run first. It produced a textbook **priority
inversion** that wedged the boot. `wm_demo` waits for its window in a
blocking `sys_read`, which on this kernel is a spin through `schedule()`
- so it re-declared itself interactive on every pass and stayed
permanently runnable. The compositor does real work, burned whole slices,
and was demoted. The client then outranked the server it was waiting
for, and a handshake that takes 60 ms had not completed after **fifteen
seconds**. Raising the demotion threshold from 2 slices to 10 did not fix
it, it only changed the victim: `kernel_main` halts inside
`pit_sleep_ms` and so also never "yields", and a `pit_sleep_ms(10)`
started taking 100 ms.

**The conclusion is specific: a scheduler cannot tell waiting from
computing until waiting is something a task can actually do.** Every
process on this desktop busy-polls - the compositor's loop ends in
`SYS_yield`, every client's does too, a blocking pipe read is a spin - so
the classifier's input signal is "everything, always", which is the same
as no signal. That is M68. So the ordering argued about after M68's first
attempt is genuinely circular, and the resolution is that only the
*measurement* half of M69 was ever the prerequisite. It exists now, so
M68 can be attempted again with numbers on both sides of it, and
priorities become possible for the first time immediately after.

*The root cause of M68's wandering failures, found and fixed at the
source.* There are 137 `pit_sleep_ms` calls in `kernel.c` - about fifty
seconds of pure waiting - and nearly every one is a **bet**: sleep long
enough that a client has probably connected, then read a pixel and
assert. The condition is never checked; the duration stands in for it.
That is why M68 broke the suite in three different places on three
consecutive runs. `selftest_wait_until` and `selftest_wait_for_pixel`
wait for the thing about to be asserted, so a scheduling change can no
longer break a test that was not about scheduling - and a timeout now
names the condition that never came true instead of surfacing as a wrong
pixel three lines later. M55 is converted as the archetype; **the other
136 are follow-up work and are the remaining blocker on M68.**

*Two things the suite was hiding.* The `[wm] animation missed its frame
budget` check is **intermittent** on this host - it fired on roughly one
baseline run in four, before any change, which means it had been adding
noise to every M68 diagnosis. And `wm_demo`'s "did not exit cleanly -
window creation failed" panic conflated three different outcomes; it now
prints which, and the answer turned out to be "still running past the
budget" rather than the failure the message named.

*What the TSC is here for.* Every timing question this project had asked
was answered with the PIT, at 10 ms. A 16 ms frame budget cannot be
judged with a 10 ms clock - the answer is "one tick or two". The TSC is
calibrated against the PIT at boot, reports whether the CPU claims an
invariant counter, and is used for measurement only: the scheduler,
timeouts and `pit_sleep_ms` all still run off the PIT, because replacing
the machine's timebase is a much larger change than measuring with a
second one.



## M70 — A machine that says what happened ✅ (log, Console, painting panic)

Cheap, and it unblocks the debugging of everything after it. This is the
milestone that stops the desktop being a machine you can only diagnose
with a serial cable attached.

- [x] **`SYS_klog`** — read the kernel ring buffer from user space.
      Read-only, with a cursor so a reader can follow rather than
      re-read. Gated on a capability (`CAP_SYSLOG`), because the log
      contains driver addresses and other processes' failures, and M65's
      whole argument is that the gate goes where the boundary really is
- [x] **A Console app** in `user_space/bin` — the log, live, filterable,
      with the capability denials M65 already emits shown as what they
      are. This is where M65's *"a rule nobody can see is a rule nobody
      can check"* finally becomes true rather than aspirational
- [x] **A panic that paints.** Currently a panic on a machine with no
      serial port is a black screen — named in the stretch-goal list
      above as a hardware prerequisite, but it is not hardware work and
      it does not need hardware to test. Panic takes back the
      framebuffer, paints the message, the vector, the faulting address
      and a stack trace, and does it without allocating or taking any
      lock, because the reason it is running is that something is already
      wrong
- [ ] **Symbolised stack traces** — a symbol table emitted at link time
      and embedded in `kernel.bin`, so a panic names functions rather
      than addresses. The panic screen is the one UI in this project a
      person reads under stress, and hexadecimal is not a message
- [ ] **A crash record that survives the reboot.** Write the panic to
      disk before halting, and have the desktop offer it on next boot.
      The bug you cannot reproduce is the one worth keeping

**How we'll know.** A self-test that triggers a deliberate kernel fault
in a QEMU guest with *no serial device attached*, screenshots the
framebuffer, and asserts the panic text is on it — because the entire
point is the case where the log has nowhere else to go. And a second one
that reboots afterward and finds the crash record.

### Progress notes

*What landed:* a 64 KiB ring in `klog.c` written under the lock that
already serialised the serial port and the console, so the ring can never
hold a half-line the serial capture does not; `SYS_klog` with an absolute
cursor and `SYS_klog_total`; `CAP_SYSLOG` and a `console` program that is
the only thing on the machine holding it; and a `panic_render` that takes
the screen back and writes on it.

*The capability argument, applied one more time and coming out the other
way.* M65 declined to gate `SYS_fb_info` and `SYS_netconf` because how
big the screen is and what address this machine has are **facts**, and
gating a fact is the check that looks like security and is not. The log
is not a fact about the hardware - it is a running account of what every
other process on the machine is doing, including the arguments of their
failed syscalls and every capability denial. So it gets a gate, and
`SYS_klog_total` deliberately does not: how *much* has been logged is a
length, not a content.

*Splitting the panic in two is what made half of it testable.* A test
that triggered a real panic could not report what it found - the machine
is stopped, which is the entire point of a panic. So `panic_render` paints
and returns, `panic` calls it and then halts, and the boot self-test
paints a band and reads the pixels back. It asserts two things, and the
second is the one with teeth: that the band is there **and** that there
are glyph pixels inside it, because a coloured rectangle with no message
in it would pass the first check and be worth nothing. The halt-and-
broadcast half stays exercised only by real panics, where it always was -
but one of the two is now checked instead of neither.

*The constraints on `panic_render` are unusual enough to be worth stating,
and they ruled out the obvious implementation.* It cannot allocate (the
heap may be why it is running), cannot take a lock (a lock may be held by
a task that will never release it - a panic that deadlocks is strictly
worse than one that prints nothing), and cannot use `console.c`, which
does both. So it is `fb_fill_rect` plus the raw 8x16 font and nothing
else. It paints a band rather than the whole screen: a full clear at
1024x768 is three megabytes through a possibly-uncached mapping, and
leaving the desktop visible around the band makes it unmistakably a
takeover rather than a repaint.

*Deliberately not done, and each is a real piece of work rather than an
oversight:* **symbolised stack traces**, which need a symbol table emitted
at link time and embedded in `kernel.bin` - a build-system change, and the
panic screen is legible without it; and **a crash record that survives the
reboot**, which needs M71's durable write path to be worth anything (a
crash record written through a filesystem with no write ordering is a
crash record that may not be there). Both are listed in M71's and the
stretch list's terms rather than half-built here.

*One number moved:* the serial harness's capture budget, 740 -> 820
seconds. M69's latency self-test is deliberately slow - ten
input-to-photon samples, half of them with a CPU-bound task per core -
and a latency measurement that hurried would be measuring the hurry. 740
was measured with only the very last marker missing, which is exactly the
one-slow-boot-from-a-false-failure margin that number exists to avoid.



## M71 — Files worth trusting ✅ (atomic replace, unclean-mount check)

The trust milestone, and the one with the clearest failure story: today,
a power cut while `text_editor` saves loses both the new document and the
old one, because `SYS_writefile` truncates before it writes. Everything
else in this list follows from taking that seriously.

- [x] **`SYS_rename` learns to replace.** Today it refuses an existing
      destination, and M56 argued for that: *"silently replacing a file
      is a way to lose one, and the caller can ask."* That argument was
      right for a file manager and is exactly wrong for a save: the
      write-temp-then-rename dance is the *only* way to replace a file
      without a window in which neither version exists. An explicit
      replacing variant, so the default keeps M56's safety and the save
      path gets its atomicity
- [ ] **`SYS_fsync`** and a `vfs` that means it. There is currently no
      way for a program to ask that its bytes be on the disk rather than
      in a cache, and `vfs_flush` has exactly one caller — the shutdown
      path. A save that returns success before the data is durable is a
      save that lies
- [ ] **Write ordering in leanfs**: data blocks before the inode that
      points at them, inode before the directory entry that names it.
      Not a journal — a journal is a much larger milestone and this
      filesystem has one writer — but the ordering that makes a crash
      leave a stale file rather than a file pointing at somebody else's
      blocks
- [x] **A generation count in the superblock, and a check on mount.**
      A filesystem that was not cleanly unmounted gets scanned: bitmap
      against inodes, inodes against directory entries, orphaned blocks
      reclaimed. It reports what it found in the log, which M70 has just
      made visible
- [ ] **Disk full, handled everywhere.** Audit every write path for what
      happens at capacity. The current answer is unknown, which is
      another way of saying it is probably a truncated file and a
      success return
- [x] **The editor stops losing work**: save through the atomic path,
      warn on close with unsaved changes, and keep a recovery copy so a
      crash costs the last few seconds rather than the session

**How we'll know.** The test this milestone exists for: **kill the QEMU
process mid-write, reboot the same disk image, and assert the file is
either entirely the old version or entirely the new one.** Run it in a
loop at randomised offsets. Nothing else in this file has ever tested a
failure the machine did not choose, and that is precisely the class of
failure a filesystem exists to survive.

### Progress notes

*The failure this milestone exists for, stated precisely.* `SYS_writefile`
opens with `OPEN_TRUNCATE` and then streams. From the instant the
truncate lands until the last byte is written, the file on disk is
**neither the old document nor the new one** - so a power cut in that
window loses both: the version being written and the version being
replaced. The editor did exactly this on every Ctrl+S.

*The fix is one directory record, not a journal.* `dir_repoint` changes a
single entry from one inode number to another. There is no instant at
which the name fails to resolve: it points at the old inode right up
until it points at the new one. That is what makes write-to-a-temp-then-
rename an atomic replace rather than a slightly shorter version of the
same window - and it is why this had to be a **new call** rather than a
loosening of `SYS_rename`. M56 refused to overwrite because "silently
replacing a file is a way to lose one", which is still true; the
self-test asserts that `SYS_rename` *still* refuses, alongside asserting
that the new one does not.

*What a crash on this filesystem actually leaves behind - and it is not
what a journal would fix.* leanfs is write-through with a single writer,
so a power cut does not interrupt a transaction, it interrupts a
*sequence*: allocate blocks, update the inode, update the directory. A
crash between the first and second steps leaves blocks the bitmap calls
used that nothing points at. They are invisible - the filesystem works
perfectly - right up until the disk is full of blocks belonging to files
that never existed. So the check rebuilds the bitmap from the inodes
rather than trusting it, and the superblock's old `reserved` word became
a clean/dirty flag to know when to bother.

*Reusing `reserved` rather than bumping the magic was the deliberate
choice, and it has a nice property:* an existing disk reads 0 there,
which is exactly `LEANFS_STATE_CLEAN` - so every filesystem written by
every earlier kernel is treated as cleanly unmounted the first time this
runs, which is correct by construction, since those kernels had no way to
leave it any other way. A magic bump would have reformatted them instead.

*The self-test simulates the crash rather than approximating it.*
`leanfs_debug_orphan` drops a file's directory entry and inode while
leaving its blocks marked used - which is precisely the leak, not
something that resembles it. Same spirit as M66's `tcp_debug_drop_next`:
the failure cannot be asked for on demand, so it is produced honestly.
The measurement is exact - a 3000-byte file is 6 blocks, and the check
reports 6 orphaned blocks reclaimed and the free count returns to where
it started.

*One input to `rename_replace` could destroy the thing it was asked to
preserve*, and it is checked: renaming a file onto itself. The naive
implementation unlinks the destination and then finds it has unlinked its
own source. It is a no-op here, and the self-test asserts the file
survives.

*Deliberately not done in this milestone, each for a stated reason.*
**`SYS_fsync`**: leanfs's writes are already synchronous - `vfs_write`
returns when the bytes are on the platter - so an fsync here would be a
call that does nothing, and this project has refused to ship those
before. It becomes real the day there is a write-back cache, and that
day it is the cache's milestone. **Write ordering inside leanfs**
(data before inode before dirent): the check above makes the current
ordering *survivable*, which is the property that mattered; making it
*correct* is a change to every write path and wants its own before-and-
after. **Disk-full auditing** and the **crash-consistency test that kills
QEMU mid-write**: the second needs the harness to stop using
`snapshot=on`, which every other test depends on, and that is a harness
change rather than a kernel one.



## Fixed — the editor could not paste, and the manifest was dead letter

Reported as "Ctrl+V does nothing in the text editor", failing on every
interactive run since M67 began and reproducing on `7311549`. The cause
was four programs away from the symptom, and the first two diagnoses were
both wrong - which is the part worth keeping.

**The actual bug: `desktop_icons` was not in the capability manifest.**
It launches programs the same way a shell does - a plain `SYS_spawn` -
and M65's rule is that a child's set is *its parent's set intersected
with the manifest*. Absent from the table, it held `CAP_APP_DEFAULT`. So
**every program launched by double-clicking a desktop icon was capped at
`CAP_FS_WRITE`, whatever the manifest said about it.** The editor could
not read the clipboard, the task manager could not list processes,
Settings could not change the resolution. The manifest was decoration for
the entire desktop.

*Why nothing caught it for five milestones.* The boot self-tests spawn
from `kernel_main`, which holds everything. The Start menu goes through
the compositor's launcher, which also holds everything. Only the icon
grid was affected - and the one test that used it did so to set up a
*different* assertion, so the failure appeared as a missing paste rather
than as a capability problem.

*Two wrong diagnoses, and why each was wrong.* First: "not a capability
denial, because no `[caps]` line appears in the boot log". The denial is
logged - it just never appears in the **serial** log, because that boot
never launches an editor. I checked the wrong artifact and wrote the
conclusion down as fact. Second: a genuine race in `sys_kbd_modifiers`,
which reports the modifiers of the character the *kernel* last handed
out - to the compositor, one pipe hop earlier - so a client asking is
asking about a keystroke that may not be its own. That is real, and is
fixed below, and it was not this. What settled it was instrumenting
`clipboard_get` and saving the guest's serial log from inside the failing
test, which said `text_editor was refused 'clipboard'` in one line.

**Fix:** `desktop_icons` joins `sh`, `gui_terminal` and `compositor` in
the manifest. What a launcher holds is a *ceiling*, not a grant to what
it starts - the kernel still applies the table to every child - so this
lets it hand out what the table already allows and nothing more. The
honest cost is that it can now call privileged syscalls itself, the same
widening already accepted for the shells. **The cleaner shape is for a
launch to be a request to the compositor**, the way the Start menu and
shutdown already are; that is a WM-protocol change and is the right
follow-up.

### Also fixed: modifiers now travel in the event

`wm_event_t` carried `time_ms` with a comment explaining exactly why - "a
client timing a gesture has to use this rather than calling
`SYS_uptime_ms` itself, or it ends up measuring its own scheduling
latency" - and carried no modifiers, so every client asked
`SYS_kbd_modifiers` about global state that had already moved on. The
same argument, never applied. It now carries `mods`, filled by the
compositor at the instant it read the key, and `gui_terminal` and
`text_editor` read it from the event.

### Also fixed: a redirect that only worked once per process

See M72's notes. `sys_open` returns the lowest free descriptor, and the
shell parked stdout *after* opening its redirect target - so the second
redirect's file landed on the parking slot and was immediately
overwritten. Parking first makes it impossible. The kernel-side cycle is
now a permanent self-test (`[fd]` marker), because this was blamed on the
kernel first and the test is what makes that blame checkable.

## M72 — One shell, and it can be scripted ✅ (/bin/sh, scripts, `#!`)

There are two shells in this repo and neither is a shell.
[user_space/shell/shell.c](user_space/shell/shell.c) is 109 lines, takes
one argument, and has no quoting. The one the README describes —
arguments, quoting, `>`, `>>`, a pipe, `cd`, tab completion — is roughly
four hundred lines living *inside*
[user_space/bin/gui_terminal.c](user_space/bin/gui_terminal.c), which
means it is not a program, cannot be spawned, cannot read a file, and
cannot be the thing that runs when something else needs a command run.

- [ ] **Extract the interpreter into `/bin/sh`**, a real program. The
      GUI terminal becomes what a terminal actually is — a window that
      draws a grid of characters and owns a pty-shaped pipe pair — and
      spawns the shell like anything else. The serial shell becomes the
      same binary on a different fd, which deletes the second shell
      rather than improving it
- [x] **Scripts.** A file of commands, run top to bottom. `#!` honoured
      by the spawn path so a script is a program as far as anything that
      launches one is concerned — the launcher, the file manager, the
      desktop icon
- [x] **Exit status that composes**: `$?`, `&&`, `||`, and a shell that
      returns its last command's status, because a script whose failure
      is invisible is worse than no script
- [ ] **Variables and `$VAR` expansion**, assignment, and an environment
      inherited across spawn — which this kernel does not have at all
      today, and which is the smaller half of why `argv`-only programs
      are awkward to configure
- [x] **Globbing** (`*`, `?`) in the shell, not the kernel, expanded
      against `SYS_listdir` before spawn — the Unix rule, and the reason
      every program on this machine gets it for free
- [x] Deliberately **not** job control, `&`, subshells, or functions.
      Each is a real feature with a real cost and none of them is what a
      one-person desktop is missing

**How we'll know.** The boot self-test fixtures that are currently C
programs get rewritten as scripts and still pass. That is the honest
test of a shell: not that it runs a command, but that something which
used to need a compiler now does not.

### Progress notes

*What landed.* `/bin/sh` is a real program - the file is
`user_space/shell/sh.c`, renamed from `shell.c` because `#!/bin/sh` is
what every script in the world says and a shell whose name does not match
that is a shell scripts cannot name. It reads commands from a file when
given a path and from stdin otherwise, which is the entire difference
between a prompt and a script. It has quoting, `;`/`&&`/`||` over a real
exit status, `$?`, `$VAR` and `NAME=value`, `>`/`>>`, `*`/`?` globbing
expanded before the spawn, `#` comments, and six builtins.

*`#!` is nine lines in `sys_spawn` and it is the whole milestone.* A file
starting with `#!` is not an image - it names an interpreter, and what
gets spawned is that interpreter with this file's path as its first
argument. Done at the syscall rather than in `process_spawn` because this
is the layer that has a *path* to hand over; `process_spawn` takes an
image and could not name it. The effect is that **every launcher on this
machine gets scripts for free** - the shell, the Spotlight launcher, a
desktop icon, the file manager - because none of them has to know. Same
reason M65 put the capability manifest in the kernel rather than in the
compositor: a rule only one launcher consults is a rule with a way round
it.

*The first version of `#!` produced a boot that stopped with no message,
and the cause is worth writing down.* `process_spawnv` takes a COMPLETE
argv - `sys_spawn` builds `argv[0]` itself and hands the whole vector
over - so `argv[0]` has to be the *interpreter's* path and the script
becomes `argv[1]`. Getting that off by one is not a subtle failure:
`/bin/sh` treats "given a path" as "run this script" and "given nothing"
as "read stdin", so every script spawned an interactive shell that
blocked forever on a keyboard nobody was typing at.

*A redirect that only worked once per process - and the diagnosis was
wrong the first time, which is the part worth keeping.*

`echo hello > file` was first implemented the obvious way: dup2 the file
onto fd 1, run the command, dup2 stdout back. The **first** redirect in a
shell landed in its file; every one after it silently went to the
terminal, with the parse verifiably correct, the open succeeding and no
error anywhere. That reads exactly like a kernel fd-table bug, and this
file said so - **incorrectly**.

The fix for that mistake was a kernel-side repro: the same
park / dup2 / write / restore cycle, driven twice from `kernel_main` with
no shell involved. It passed, both rounds, bytes in order. So the kernel
was fine and the bug was in the shell.

**It was an ordering bug, and a nasty one.** `sys_open` returns the
LOWEST free descriptor. The shell opened its redirect target and *then*
parked stdout at a fixed descriptor - and after one redirect had been set
up and torn down, that parking descriptor was the lowest free one. So the
second command's file was handed the parking slot, `dup2(1, PARK)` then
overwrote that freshly-opened file with stdout, and `dup2(rfd, 1)` copied
stdout onto stdout. The redirect became a silent no-op, and only from the
second one onward, which is exactly the shape that made it look like
state the kernel was failing to restore.

Parking **before** opening makes the collision impossible, because the
slot is occupied by the time `sys_open` looks for a free one. The
kernel-side cycle is now a permanent self-test (`[fd]` marker) so that
this stays a shell question if it ever comes back.

*The lesson is about diagnosis, not descriptors.* "The first one works
and the rest do not" is a shape that points hard at the layer holding the
state, and the layer holding the state was innocent. What settled it was
reproducing the mechanism *without* the program that found it - which
took one short kernel self-test and should have been the first move
rather than the last.

*The self-test is a script, which is the only honest test of this.* The
old shell could run a command; so could M13's. What it could not do was
be handed a file. So the fixture is a real script with a `#!` line,
spawned through the ordinary `SYS_spawn` a desktop icon would use -
nothing in the path knows it is not an ELF - and it has to demonstrate
variable expansion, a quoted argument holding a space, a real 127 for a
missing command, `&&` running after success and `||` running after
failure. Plus one negative: `&&` after a *failure* must run nothing,
without which a shell that ran every branch unconditionally would pass
all four positive checks.

*Two shells became one and a half, not one.* `gui_terminal.c` still has
its own interpreter, and unifying them is the remaining half of this
milestone. It is not a copy-paste job: that code is entangled with the
window it draws - it echoes as you type, completes against its own
display, and routes keys as window events rather than reading fd 0 - so
the honest version is a pty-shaped pipe pair and a terminal that spawns
`/bin/sh` like anything else. That is a milestone-sized change to the
program the interactive suite exercises most, and doing it badly would
cost the desktop its terminal.

*Deliberately absent, each with a reason:* job control, `&`, subshells,
functions, `|` (the GUI terminal has one; this does not, and a second
implementation of the trickiest part is not obviously better than none),
and an **exported environment** - which this kernel has no concept of at
all, since `SYS_spawn` carries a path and an argv and nothing else.
Variables are shell-local and the header says so, because quietly setting
them and having children not see them is the other kind of honesty.



## M73 — Names, not numbers ✅

M64 shipped `SYS_netconf` returning a DNS server address and wrote,
carefully, that it prints *"the DNS server it was handed instead of
pretending to use it"*. M66 shipped a TCP that would work against
somebody else's stack. What is missing between those two and anything a
person would do is a resolver, and after that, one program that proves
the whole column.

- [x] **A DNS resolver in user space**, in `user_space/lib` — following
      M64's own precedent and its stated reason: SNTP went to user space
      because *"a kernel parsing replies off the network is a far larger
      trusted surface than a sixty-line program needs"*, and a DNS
      response parser is a strictly nastier piece of untrusted input than
      an SNTP reply. A queries, CNAME following with a hop limit, a small
      cache honouring TTL, and the message-compression pointer loop that
      every from-scratch resolver gets wrong on the first try
- [x] **`nslookup`**, for the same reason `netconf` and `caps` exist:
      the thing that makes a subsystem checkable is a program that prints
      what it did
- [x] **An HTTP/1.1 client** — `fetch URL`. `Host:` header, status line,
      headers, `Content-Length` and chunked bodies, a redirect limit, and
      a hard cap on response size. Deliberately no TLS: a from-scratch
      TLS 1.3 is a project, not a bullet, and an `https://` that quietly
      wasn't would be a lie of exactly the kind this file keeps refusing
      to tell
- [x] **The first inbound path.** `fetch` writing to a file is the first
      time in sixty-odd milestones that something arrives on this machine
      without being compiled into its disk image. That is what makes M63
      ("somebody else's program") and M65 (a capability model, built
      because a downloaded program is a real category) stop being
      hypothetical about each other
- [x] `CAP_NET` already gates the socket, so a downloaded thing does not
      get a network by default. Check that this is actually true once
      there is a downloaded thing

**How we'll know.** A self-test resolving a name against a DNS server
QEMU's SLIRP provides and fetching a file from a server run on the host —
asserting the bytes match, and asserting the failure paths, which is
where M64 got its ratio right: a name that does not exist, a server that
refuses, a redirect loop, a body that exceeds the cap, and a truncated
response.

### Progress notes

*The whole thing passed on the first boot it was run on*, which is worth
recording because almost nothing in this arc did - and the reason is that
every layer underneath it had already been made to work and to say so.
The path a fetched byte takes is TCP's handshake (M66), the loopback
queue M66 had to add when it discovered a recursive receive, the socket
fd table from M64, and M71's filesystem at the end. None of that needed
touching.

*The self-test does not need the internet, and that is the design.* A
test that reaches out to a real name reports on whether the machine
running QEMU has a network, which is the weather rather than the code. So
it is split in two:

- **The parser** is fed responses built by hand, because a DNS reply is
  just bytes and that is where a resolver actually goes wrong. The cases
  are specific hostile shapes rather than variations on "it works": a
  **compression pointer** (which every real reply uses and a naive parser
  cannot read at all), a **CNAME chain**, a **pointer that points at
  itself** (a hang rather than an error - the nastiest thing a malicious
  reply can do, and the reason there is a jump budget), somebody else's
  reply arriving with the **wrong id**, an answer to a **different
  question**, **NXDOMAIN**, and a **truncated message**.
- **The fetch** runs against `httpd`, a fixture that serves one canned
  response on loopback and exits. Same reasoning M66 used for TCP: check
  this machine's client against a server whose every byte is known.

*Five distinct failure codes, not one.* "The name does not exist", "the
server never answered", "the reply was malformed" and "there is no DNS
server configured" want different things from a caller, and a resolver
that returns one error for all of them makes every one of them
undiagnosable. `nslookup` prints a different sentence for each, which is
the same reason `netconf` and `caps` exist.

*In user space, following M64's own precedent* - "a kernel parsing
replies off the network is a far larger trusted surface than a sixty-line
program needs". A DNS response is strictly nastier input than the SNTP
reply that argument was made about: length-prefixed labels, a pointer
format that can jump backwards into the message, and a record count the
sender chooses. It is exactly the parser that should not be in the
kernel.

*`https://` is refused by name rather than downgraded.* A from-scratch
TLS 1.3 is a project, not a bullet, and an `https` that quietly was not
encrypted would be the kind of lie this file keeps declining to tell.

**What this actually unlocks.** M63 ran somebody else's program and M65
built a capability model on the argument that a *downloaded* program is a
real category - both reasoning about something that could not yet happen
here. `fetch` writing a file is the first time anything has arrived on
this machine's filesystem without being compiled into its disk image. The
model was waiting for this, and `fetch` holds `CAP_NETWORK` while an
ordinary program does not.



## M74 — The session that remembers ✅ (second attempt)

The last one, and the only one on this list that is purely about how the
machine feels. It is last because it depends on M71 being trustworthy —
a session file is just another file that must survive a bad shutdown —
and because it is the payoff rather than the foundation.

- [x] **Windows come back.** Position, size, workspace and z-order,
      restored on next boot for the apps that were open. M47 built
      persistent settings and M53 built somewhere to put them; this is
      the same mechanism pointed at the compositor's own state
- [x] **Unsaved work blocks a shutdown.** M47's shutdown SIGTERMs
      everything and gives it a second. An editor with unsaved changes
      should be able to say "wait, ask the person" — which means the
      shutdown path needs one veto with a timeout, not a longer grace
      period
- [x] **Recently opened**, in the launcher and the file manager. The
      single highest-value-per-line feature on any desktop, and this one
      has never had it
- [x] **The desktop survives a settings mistake.** M58's resolution
      countdown is exactly the right pattern; apply it to anything else
      that can make the machine unusable from inside the Settings pane
- [x] **A first-boot state that is not empty.** A fresh disk currently
      boots to a desktop with seeded icons and nothing else. A README on
      the desktop, the sample files the file manager is worth opening —
      the difference between a demo and a machine somebody just got

**How we'll know.** An interactive test that opens three windows across
two workspaces, moves them, restarts the machine, and asserts the pixels
come back where they were — the same pixels-not-protocol discipline the
virtual-desktop test used, and for the same reason: a compositor that
*says* it restored a window and did not paint it has failed.

---

### First attempt — built, self-tested, and not shipped

**Status: window save/restore is implemented and its boot self-test
passes. It is not in the tree, because it breaks seven interactive tests
by stopping input from reaching windows, and that cause was not isolated.
The work is preserved in a git stash.**

*What was built and works.* The compositor writes a line per live
application window - program, geometry, workspace - and on start
relaunches them and places them. The program name comes from
`SYS_taskinfo` via the client's pid rather than from the WM protocol,
because a window knows its *title* (what a person reads) and the task
table already knows what each process was launched as. The file is
written only when a cheap signature of the layout changes, so it is
checked every pass and written almost never. The `[m74]` self-test drives
the whole path and passes: a hand-written session naming a program and a
position the cascade would never choose, a compositor started with the
gate argument, and the window required to appear **at those coordinates**
- graded on the pixel, so a compositor that read the file and placed the
window anyway cannot pass - plus a second window changing the layout and
the session rewritten naming both programs.

*What went wrong.* With the compositor's M74 diff applied, seven of the
eight interactive tests fail, and they all fail the same way: **input
does not reach windows.** "Clicking the titlebar close button did not
close", "clicking the covered window's titlebar did not raise", "nothing
was typed into the editor". Window *counts* are correct, so the
compositor is running and compositing; only the acting-on-input is gone.

*What is known, so the next attempt does not repeat it.*

- **It is not the session gate.** `init` was reverted to pass no
  argument, which leaves `session_enabled` false and every session code
  path a no-op, and the tests still failed.
- **It is the compositor's M74 diff specifically.** Stashing it and
  rebuilding made `titlebar_close_button` pass; restoring it made it fail
  again. That is a clean bisect to one file's changes.
- **It is not the size of the task-info array.** Reducing it from 128
  entries (6 KB of BSS) to 40 changed nothing.
- Two things in that diff are still unexamined: the `restored` pointer
  threaded through `accept_pending_window`, and the change of `main(void)`
  to `main(int argc, char **argv)` in a program that had never taken
  arguments. One of those, or an interaction with the M69 `SYS_waitfds`
  loop, is the cause.

*Why it is not shipped anyway.* Every one of those seven failures is
about a person clicking something and nothing happening, which is the
worst class of desktop bug there is - and a feature that brings your
windows back is not worth a desktop that ignores the mouse. It is one
bisect step from being understood, and that step is worth taking with a
clear head rather than at the end of a long session.

*Also not attempted, and listed here rather than pretended about:*
unsaved work vetoing a shutdown, "recently opened", and a non-empty
first-boot desktop. All three are independent of the restore mechanism
and none was started.

### Second attempt — and the milestone that made it work

**The gate stopped being an argument, and that was the whole fix.**

The first attempt's own notes named two unexamined suspects, and one of
them was "the change of `main(void)` to `main(int argc, char **argv)` in
a program that had never taken arguments". It had to take one because
the session had to be switched on for the *real* compositor and off for
the two dozen compositors the boot self-tests start — a self-test
compositor that saved a session would leave a file the real one then
restored, relaunching test fixtures onto a person's desktop.

M75 removed the need. An environment is inherited across a spawn now, so
the gate is `getenv("LEANOS_SESSION")`, PID 1 sets it once, and the
compositor's signature is untouched. The kernel's own `[m74]` self-test
passes an explicit `envp` to `process_spawnve` — the first caller in this
project to do that deliberately, and a fair test of whether M75 was worth
building.

That is the second time in this arc that a milestone landed because a
later one had made it cheaper, and it is worth being explicit that this
was not the plan: M75 was written for `getenv`, and paid for M74 by
accident.

**What is actually restored, and what is not.** Program, position, size
and workspace, per window, up to eight. Not z-order and not focus, and
that is a decision rather than a shortfall: both are properties of a
*stack* rather than of any window, so restoring them means replaying an
order against clients that connect whenever they happen to start — and
getting it half right (a window raised over one that should be above it)
reads as a bug in the window manager rather than as an imperfect restore.

**The program name comes from `SYS_taskinfo`, not from the protocol.** A
window knows its *title*, which is what a person reads and not what a
launcher can spawn. The task table already knows what each process was
launched as and the compositor already holds `CAP_PROCESS_LIST`, so
nothing new had to be invented and nothing had to be trusted to the
client — the same reasoning M65 used for putting the capability manifest
in the kernel rather than in whichever program did the launching.

**Written when it changes, checked every pass.** A cheap signature over
every live window's pid and geometry is compared twice a second; the file
is written only when it differs. Saving every frame would be a disk write
per frame, which on this machine's PIO driver is a desktop that stutters
because it is remembering itself. Saving at each of the dozen places
geometry changes would be a call one of them would eventually forget.

**The veto is one question with a deadline, asked before the stopping
starts.** M47's shutdown SIGTERMs everything and gives it a second, which
is the right shape for a machine that *is* stopping and the wrong shape
for one that is being asked to. An editor cannot save in that second, and
a longer grace period would lose the same work later. So
`WM_EVENT_QUERY_SHUTDOWN` goes out first, to the windows that opted into
being asked things (`confirm_close`), and 600 ms of silence is consent. A
`WM_ACTION_VETO_SHUTDOWN` abandons the shutdown outright rather than
delaying it, names the program that objected, and focuses its window —
because "something is unsaved" leaves a person opening windows to find
out which, and the window in front of them is the actual answer.

**"Recently opened" is a library, not a service**, and that is what makes
it worth its thirty lines: what makes recents useful is that *every* way
of opening a file records one, not that any one way records it well. The
editor records on load and on save, the file manager records on open, and
the launcher and the file manager both read the same
`/etc/recent.conf` — one path per line, newest first, plain text like
every other state file here.

*Where they surface cost a decision each.* In the launcher they are the
first entries of the existing result list, shown by basename, so
filtering, arrow keys, scrolling and clicking all carry on working
unchanged and only *launching* one is different. In the file manager they
are a button in the header rather than a row in the list, because a
synthetic row would shift every real one down by an index — which three
interactive tests measure and, more to the point, a person's hand
measures.

**The settings countdown turned out to be pointed at the wrong thing.**
The milestone asks for M58's revert pattern "for anything else that can
make the machine unusable from inside the Settings pane", and the honest
answer is that *nothing else in the pane can*: the six background
swatches are all dark and the six accents are all bright, and no pair of
them is unreadable. What can make this machine unusable is
`/etc/settings.conf`, which is plain text on purpose (M47) and which a
person can therefore set to `bg=FFFFFF` — white on white, on a desktop
with no other way in.

A countdown cannot help there, because the file is read at boot and
reverting it would need somebody watching a screen they cannot read. So
the protection is a contrast check at both doors — the file at startup
and the settings pipe at runtime — measured as BT.601 luma against the
white every label here is drawn in, with a threshold generous enough
(60/255, where the darkest offered swatch scores 22) that only a
genuinely unreadable choice trips it. A rejected theme falls back to the
compiled-in default rather than to the previous one, because "the colours
you can always read" is a fixed thing and the previous value on a first
boot is itself whatever the file said.

**How it is graded.** Two ways, and neither subsumes the other. The
`[m74]` boot self-test writes a session by hand naming a program and a
position the cascade would never choose, starts a compositor with the
environment `init` gives it, and requires the window to appear *at those
coordinates* — on the pixel, so a compositor that read the file and
placed the window anyway cannot pass — then deletes the file, changes the
layout with a second window, and requires it to be written again naming
both programs. The interactive suite's
`session_restores_windows_across_a_reboot` does the thing a person does:
opens two windows, drags one somewhere the cascade cannot reach, types
`reboot`, and asserts both come back with the dragged one's titlebar
pixel where it was left.

*One existing interactive test changed meaning and was rewritten rather
than worked around.* `behaviour_settings_persist` used to restart the
machine and reopen Settings to read its switches back; Settings now comes
back by itself, so reopening it would put a second window on top of the
first. It reads the restored one, which is a better test than it was.

**What turning it on actually cost, and what it found.** The feature
itself went in cleanly. Making the interactive suite green again took
four more rounds, and three of the four were bugs that had nothing to do
with a session - which is the argument for the suite existing, made
again.

*A handshake race fifty milestones old.* Every reboot test began failing
on its SECOND boot, panicking in the `[m20]` self-test with "wm_demo
still running past the budget". The chain took six instrumented runs to
walk: the scheduler dump said wm_demo was blocked on a pipe and the
compositor was alive and polling; a probe of both rendezvous pipes said
neither held anything; and a log line in `accept_pending_window` said the
compositor had never seen a create request at all.

The cause: a compositor *clears* the rendezvous pipes when it starts
(M55, and rightly - a dead compositor's leftovers are not the new one's
business). `wmclient.c`'s `connect_common` has had a retry for exactly
that since M55, with a comment saying so. `wm_demo` is the one client in
this project that hand-rolls the handshake instead of using wmclient -
deliberately, so the raw protocol is exercised by something - and
hand-rolling it meant hand-rolling the missing retry too. It was
invisible for fifty milestones because on a first boot the compositor
always won the race; it appeared the moment a second boot skipped the
"seeding disk" step and moved the timing by a few seconds.

*A self-test that ate the thing it was testing.* The `[m74]` self-test
writes its own `session.conf` fixture and deletes it afterwards, which is
correct for a fixture and catastrophic for the file a person's desktop
wrote before the machine was switched off. Every boot began by forgetting
what was open - and the self-test passed throughout, because from inside
it everything worked. `selftest_settings_install_defaults` has had to
preserve `settings.conf` for the same reason since M47; this now does the
same for `session.conf`.

*Two tests were asserting things a desktop that remembers is no longer
allowed to assume.* `boot()` checks that (500, 500) is bare wallpaper,
which was true of every desktop this suite had ever seen and is not true
of one with a restored window on it. And the new test compared a
*titlebar* pixel across the reboot - a colour that depends on which
window has focus, which is precisely the thing this milestone
deliberately does not restore. Both now grade window *content*, which is
a fact about the window rather than about the stack it is in.

*And one stale number.* `behaviour_settings_persist` allowed 120 seconds
for a restart on a machine that takes 146 to boot - a deadline a healthy
machine could not meet, which had simply never been run against a boot
this long. The boot prints its own time now, so the replacement is
measured rather than guessed.



## The next arc: Unix-shaped enough to run somebody else's software

Every milestone so far has been in service of a desktop that feels
finished. This arc changes what "finished" means: the target is no
longer "this desktop does everything it needs to," it is "a program
written by someone who has never heard of lean_os can be compiled and
run here." M63 proved that once, for one 1998 benchmark, by treating a
port's own link errors as the specification rather than guessing a
surface in advance. Everything below is the same method, aimed
deliberately at a bigger target: **Python**, unmodified, actually
running a script someone else wrote — and named honestly as a waypoint
toward the harder thing behind it, not the destination.

That harder thing is a real browser, because that is the actual ask
behind "Unix-compatible" for most people, and the "Deliberately not
next" note below still calls it a decade of work. Nothing changes about
that estimate. What changes is that it now has a name and a direction
instead of being purely hypothetical: every milestone here is chosen
because it is also load-bearing for that eventual goal, not only for
Python. A process model with real signals, a thread that shares memory
with another thread, a dynamic loader — Chrome needs every one of these
regardless of whether Python ever existed. Python is simply the program
whose link errors are cheap enough to learn from first.

What Python's absence actually exposes, concretely, in code already in
this tree: `SYS_spawn` (`system_api/include/syscall.h`) carries an argv
and nothing else, so there is no `getenv`; every path lean_os resolves
is absolute, so there is no working directory (`kernel/fs/leanfs.h`
says so directly); `docs/third-party-programs.md` lists "scanf, struct
tm, localtime, threads, signals, sockets: no" as the honest limits of
M63's libc; the only memory primitive is M19's growth-only `SYS_sbrk`;
and nothing on this machine has ever loaded code it did not statically
link. Each of the next five milestones closes exactly one of those
gaps, in the order a real interpreter would actually hit them.

**What the arc turned out to be worth, written after it.** All five of
M75-M79 landed, and the sixth did not - but the way it did not is the
useful part. M80's attempt notes have the detail; the headline is that
every one of CPython 3.11.9's 113 core translation units now compiles
against this OS's own libc, unmodified, with a cross-compiler and
`-ffreestanding`. Nothing links, and `posixmodule.c` - which is where
`fork` and `exec` live, and which this OS does not have - has not been
attempted at all.

Two things about the arc are worth recording because neither was
planned:

*The milestones paid each other back out of order.* M75 was written for
`getenv`, and what it actually bought first was M74 - a milestone from
the *previous* arc that had been reverted, in part, for a change it only
needed because there was no environment to gate on. M79's "deliberately
not condition variables, beyond whatever the next real program's link
errors ask for" and M63's "deliberately no struct tm... a program that
needs them will say so at link time" were both written as deferrals and
both were collected by the same program, milestones apart.

*How the arc was verified.* 69 of 69 boot markers, which is the 63 this
arc started with plus one per milestone including M74's; and 45 of 45
interactive tests, one of which needed re-running on its own after a boot
timeout under three concurrent guests - which is what the input harness's
own header says to do before believing a timeout, and which was right
here: it passes in 157 seconds alone.

*Letting the program name the surface produced a better libc than a
standard would have.* Thirteen headers and several hundred lines were
added, and not one of them was guessed: each exists because CPython's
own source refused to compile without it. That also meant every one of
them had to answer a real question honestly rather than plausibly -
`nl_langinfo(CODESET)` says ASCII rather than UTF-8, `localtime` is
`gmtime`, `fcntl` refuses to *set* a flag it cannot honour, and
`<wchar.h>` writes down that it is Latin-1 and wrong above U+00FF.
A header written from a standard would have said the comfortable thing
in all four places.

### M75 — Environment, and a place to stand ✅

- [x] `SYS_spawn` carries an `envp` alongside M60's `argv`, inherited by
      a child by default and overridable — the "environment inherited
      across spawn" that M72's own notes named as missing and
      deliberately left out of that milestone
- [x] `getenv`/`setenv`/`putenv`/`environ` in `user_space/libc`
- [x] A working directory: `SYS_chdir`/`SYS_getcwd`, and the kernel's
      path walker accepts a path relative to the caller's cwd instead of
      requiring an absolute one — `leanfs.h`'s own comment ("there is no
      working directory") is the bug report
- [x] `/bin/sh`'s `cd` and `$VAR` (M72) move from shell-local bookkeeping
      to the real exported environment and a real process-wide cwd, so
      `cd`-ing in the shell means something to a program it spawns
      afterward, not just to the next line typed at the prompt
- [x] Deliberately not a full `PATH`-search exec or shell globbing
      changes — M72 already owns those, and this milestone is about the
      kernel/libc primitives underneath, not the shell surface

**How we'll know.** Two children spawned with different `envp`/cwd do a
`getenv`/`getcwd`/`open` round trip using only *relative* names and
land on two different real files — a relative path resolving correctly
is the proof; an unchanged string coming back proves nothing.

#### Progress notes

*The working directory went in at the syscall layer, not in leanfs, and
that is the whole design.* `leanfs.h` says plainly that every call takes
an absolute path and that "." and ".." are refused, and both statements
are still true — they are true statements about a *filesystem*, and a
working directory is a property of a *caller*. So `copy_path_from_user`
is where every path-taking syscall now goes: absolute paths are
normalized, relative ones are joined onto the calling task's `cwd`, and
"." and ".." are resolved textually before leanfs ever sees them. Doing
it once at that door rather than in fourteen handlers is what makes
"relative names work" a property of the syscall layer instead of of
whichever handlers remembered.

*Textually, and that is the honest description.* With no symbolic links
on this machine, `/a/b/..` and `/a` name the same directory by
construction, so there is nothing a lookup could tell us that the text
does not already say. The day this filesystem grows links is the day that
stops being true, and the comment above `path_normalize` is where it
stops.

*`envp` sits immediately after `argv`'s NULL, in the same block.* Not an
invention: `envp = argv + argc + 1` is where every C runtime written
since V7 Unix looks for it, so crt0 finds it with one `lea` and needs no
second address. The argument region grew from one page to two to hold it,
which turned out to be the milestone's one real regression — M52's
garbage-argument matrix has a case that reads a byte straddling off the
END of that region into nothing, and with the region a page longer it was
straddling into a page that is now perfectly valid. It failed loudly on
the first boot, which is the self-test doing exactly its job.

*The kernel keeps a copy of each process's environment, and that is not
redundant.* `envp == NULL` means inherit, and the obvious implementation
— read the parent's own argument page — is wrong in a way that would not
show up for a long time: a process may write over that page, and an
environment a program can corrupt *for its children* is a footgun with no
upside. So `task_t` carries a packed block, and inheritance is a copy of
it.

*`sys_spawnv` passes `environ` rather than NULL, and the difference is
the one that matters.* The kernel's NULL case means "the environment this
process was started with", which is right for a raw syscall and wrong for
a program that has just called `setenv`. The wrapper every program
actually uses passes the live one, so a `setenv` before a spawn is seen
by the child — which is what a person means by an environment.

*The shell lost a hundred lines and gained the thing it was apologising
for.* `sh.c`'s own header said variables were shell-local "because
SYS_spawn carries an argv and nothing else", and its `resolve()` helper
carried a hand-rolled ".." with a comment apologising for inventing a
namespace leanfs does not have. Both are gone: assignments are `setenv`,
`$VAR` is `getenv`, `cd` is `SYS_chdir`, and paths are handed to the
kernel as typed. `export` is accepted and does nothing but assign,
because this shell has one namespace — the reason POSIX has two is
subshells and functions, and it has neither.

*PID 1 is where an environment starts, because the kernel deliberately
does not have one.* A kernel that knew what `HOME` meant would be a
kernel with an opinion about a user space it cannot see. So `init` sets
`HOME`, `PATH`, `TMPDIR` and `SHELL` and `chdir`s to `/home`, and
everything on the desktop descends from that.

### M76 — A signal a program can catch ✅

- [x] `SYS_signal`/`SYS_sigaction`: install a user-mode handler for
      `SIGINT`/`SIGCHLD`/`SIGTERM` and friends, invoked through a kernel
      trampoline the way M63's FXSAVE work already established a
      per-task saved context can be — this is the same shape of problem
      one level up
- [x] `kill()`/`raise()` as real syscalls, not just the kernel-internal
      delivery M14 and M47's shutdown path already use
- [x] Ctrl+C at the terminal delivers `SIGINT` to the foreground process
      instead of only ever being a keystroke `gui_terminal` reads as
      ordinary input
- [x] `SIGCHLD`, so a parent can be told a child exited instead of
      polling `SYS_wait` — M14's syscall comment already names "more
      complete wait semantics" as deferred, and this is that deferral
      coming due
- [x] Deliberately not a full realtime signal set or `sigprocmask`'s
      mask semantics beyond block/unblock — the ask is "a running program
      can be told something happened without being killed," not
      POSIX.1's entire signal model

**How we'll know.** A self-test program installs a `SIGINT` handler; a
driven Ctrl+C is asserted to run the handler and leave the process
alive — still listed in `SYS_taskinfo` afterward — rather than
disappearing the way every process on this machine does today.

### M77 — POSIX names for what is already here ✅

- [x] `<dirent.h>`: `opendir`/`readdir`/`closedir` over `SYS_listdir`'s
      newline-separated names (M53), which already tags a directory
      with a trailing `/` for a human reading a terminal — not a struct
      a program can walk
- [x] `<sys/stat.h>`: a real `struct stat` and `stat`/`fstat`/`lstat`
      over `SYS_stat` (M59), which today reports the three fields the
      file manager's columns needed and nothing a general-purpose
      program goes looking for
- [x] `access()`, `rmdir()` — rounding out M53's `SYS_mkdir`
- [x] Deliberately not permission bits that mean anything. M65's
      capability set is what actually gates a process here, not a unix
      uid/mode, and inventing an `st_mode` nobody enforces would be
      exactly the kind of invented fiction M65 already refused for users
      and uids

**How we'll know.** A program written against only `<dirent.h>`,
`<sys/stat.h>` and `<unistd.h>` — none of this project's own headers —
walks a directory tree it knows nothing about ahead of time and prints
what it finds, the way `find` or `du` would.

#### Progress notes

*`/bin/treewalk` is the milestone, and its include list is the point.*
There is no `paths.h` and no `syscall_wrappers.h` in it; every line of it
would compile unchanged on a Linux box. It walks a tree it was not told
the shape of, prints type, size and path per entry, and totals at the
end — `du`, which is the shortest real program that needs both halves of
this milestone at once.

*`st_mode` carries file-type bits and zero permission bits, on purpose.*
`S_ISDIR` and `S_ISREG` answer correctly because leanfs genuinely
distinguishes the two. The permission bits are 0 and a program that tests
them finds nothing set rather than a plausible `0644` — there are no
users on this machine (M65 argued that at length and refused to invent
one) and nothing here enforces a mode. `st_uid`, `st_gid` and `st_dev`
are 0 for the same reason; `st_nlink` is 1 because leanfs has no hard
links, which makes it a fact rather than a default.

*`lstat` is an alias for `stat` and that is the truthful
implementation*, not a stub: there are no symbolic links on this
filesystem, so there is nothing for it to decline to follow.

*`SYS_fstat` is the one genuinely new syscall*, and it exists because it
answers the one question a path cannot: *what is this open file*, asked
of a descriptor whose name may since have been renamed out from under
it — which is exactly what the self-test does. A pipe or a socket is
refused rather than described, because a size and an mtime for a pipe
would be two lies where `-1` is one honest answer.

*`opendir` reads the whole listing and `readdir` walks it*, which is a
design choice rather than laziness: `SYS_listdir` is whole-shot with no
iterator and no handle, and a directory here holds at most 96 entries. A
streaming `readdir` would be a second kernel interface built for a size
this filesystem cannot reach.

*`d_type` is real here, not always `DT_UNKNOWN`.* `SYS_listdir`'s
trailing `/` is exactly that information, so the cheap field a tree
walker relies on to avoid a `stat` per entry actually works — and
`treewalk` asserts that `d_type` and `S_ISDIR` agree on every entry,
because they come from two different kernel interfaces and a
disagreement between them is worth saying out loud.

*`access()` answers `F_OK` from something real and reports `R/W/X` as
granted for anything that exists.* That is not a stub either: on a system
where what a process may do is decided by its capability set and never by
a file mode, "may I write this" has no answer a `stat` could give. A
program that wants to know should try and read the error.

### M78 — Memory that can be given back ✅

- [x] `SYS_mmap(len, prot, flags)` for anonymous mappings, `SYS_munmap`
      — a second memory primitive next to M19's `SYS_sbrk`, inside the
      same fixed per-process layout M63's "know the budget before
      starting" already assumes
- [x] Freed pages are actually reusable — the one thing eleven
      milestones of sbrk-only heap structurally cannot do, since a bump
      allocator has no concept of a hole
- [x] `user_space/lib/malloc.c` routes large allocations through `mmap`
      instead of always extending the sbrk break, the same size-based
      split every real allocator makes
- [x] Deliberately not file-backed or `MAP_SHARED` mappings — anonymous
      and private, per process, until something concrete asks for the
      other kind

**How we'll know.** A program maps and touches 64 pages, frees half,
and a second mapping of the same size is asserted to land in the freed
range rather than growing the process further — proof the address-space
accounting tracks holes, not just a high-water mark.

#### Progress notes

*The whole milestone is one sorted array and one forward scan.* Each
process's live mappings are kept in `task_t` ordered by base address with
free slots at the end, and `mmap_find_gap` walks them returning the first
address with room *before* the next one. That is the entire difference
between a hole and a high-water mark, and it is why the self-test grades
the *address* a mapping lands at rather than whether the call succeeded —
a high-water-mark allocator passes every "did mmap work" test ever
written and fails the first line of this one.

*Frames are allocated at the call, not on a fault, and that is a real
limitation with a real reason.* This kernel has no page-fault handler
that could fill a page in later — a ring-3 fault kills the task (M52).
Lazy allocation would be a genuinely better `mmap` and it is a different
milestone: it needs the fault handler to be able to tell "inside a
mapping that has not been backed yet" from "this program dereferenced
null", which is exactly this table plus a decision this project has not
had to make.

*`PROT_WRITE` is honoured and `PROT_EXEC` is not, and both are said out
loud.* A mapping without `PROT_WRITE` is mapped read-only, which is a
difference a program will feel. There is no NX bit set up here, so
`PROT_EXEC` is neither granted nor withheld — refusing it would be a
refusal with nothing behind it and reporting it as enforced would be a
fiction. `PROT_NONE` is refused outright rather than granted-and-ignored,
because a guard page that guards nothing is worse than an error.

*`MAP_SHARED` is refused by name.* Handing back private memory instead
would be the kind of lie this project keeps declining to tell: two
processes would each write to their own copy and neither would find out.
`kernel/ipc/shm.h` is what two processes share memory through here, and
it has lifetime rules a `MAP_SHARED` would have to duplicate badly.

*Anonymous-and-private is also what makes the teardown correct.* The
arena is in `process_destroy_address_space`'s owned list, which is only
sound because every frame in it belongs to this process alone. The
"deliberately not MAP_SHARED" line is therefore not a shortfall being
apologised for — a shared mapping would make that list wrong.

*`vmm_unmap_page_take` is the one new VMM primitive*: unmap and hand back
the frame in one acquisition of the lock. Two calls — "what is mapped
here" then "unmap it" — would be the same thing with a window in the
middle in which another CPU could replace the mapping, after which the
caller would free a frame somebody else is using.

*The frame count is the assertion the program cannot make.* A process can
see its own address space and cannot see the machine's physical memory,
so "the pages actually came back" is checkable only from the kernel — and
it is the claim this milestone is really about, since M19's sbrk could
hand a *virtual* address back to a program's own free list all day
without one frame returning to the allocator.

*malloc splits at 64 KiB, and the reason is not speed.* It is that sbrk
cannot shrink: a 4 MiB buffer allocated and freed out of a bump-allocated
heap leaves 4 MiB this process holds until it exits. The self-test
allocates and frees 1 MiB twenty times and requires the same address back
every time — before this milestone that loop was the bug, not the test.

### M79 — Two threads, one address space ✅

- [x] A second schedulable context inside *one* address space — not
      `SYS_spawn`'s fresh process; the first time two of this
      scheduler's tasks would ever share a page table
- [x] `pthread_create`/`pthread_join`/a mutex in libc, built on that
      primitive
- [x] Per-thread stacks carved out of M78's mmap arena, since the
      current fixed per-process layout has room for exactly one stack
- [x] M63's per-task-switch FXSAVE has to keep holding for two tasks
      that are the same *process* by every other measure — worth
      stating on its own because "a task IS a process" is exactly the
      kind of assumption a from-scratch scheduler bakes in without
      anyone deciding to
- [x] Deliberately not the rest of the pthread surface — condition
      variables, rwlocks, thread-local storage — beyond whatever the
      next real program's link errors ask for, M63's rule again

**Why now, specifically.** Every CPython release since 3.7 requires a
real thread implementation to even build — the GIL itself is a lock and
a condition variable, and the old `--without-threads` escape hatch is
gone. There is no smaller version of "run Python" that skips this.

**How we'll know.** Two threads in one process each increment a shared
counter a million times, one thread's increments protected by the M79
mutex; the total is exactly two million. The classic test, chosen
because a scheduler that ever runs both halves of an unlocked increment
at once fails it visibly instead of "mostly."

#### Progress notes

*"A task IS a process" had been true for seventy-eight milestones, and
every place that had quietly relied on it had to be told which of the two
it actually meant.* Three kinds of thing turned out to be properties of
an **address space** rather than of a task, and all three are now read
through one function, `sched_vm_owner`, rather than through a rule call
sites are asked to follow:

- the sbrk break and its mapped end. Two tasks sharing a page table with
  two independent breaks would each grow into the other's memory — a bug
  with no symptom until it has already happened.
- the shm cursor, which would otherwise map two segments on top of each
  other.
- the mmap arena, which would otherwise hand out the same addresses
  twice.

The working directory and the environment go through it too, for a
different reason: POSIX says a `chdir` in one thread is seen by all of
them, and a child spawned from a thread has to inherit the *process's*
environment rather than the empty one a thread was created with.

*And the address space is torn down by the last task out, not by the
first.* `task_exit_with_code` used to free the page table unconditionally
because the task that was leaving was the only one on it. It now sets an
`exiting` flag under `sched_lock` and then asks whether any other live
task is on the same `pml4_phys` — with the flag set *before* the scan, so
two threads leaving at the same moment cannot each see the other as a
live user and both decline to free. The state cannot be set to
`TERMINATED` that early instead: another CPU would be free to reap the
slot and `kfree` the kernel stack the code is still running on.

*The one race worth recording was found by reasoning rather than by a
failure*, which is unusual for this project. `task_spawn_thread` first
set `is_thread`/`tgid` *after* `task_spawn_common` returned — and a task
is schedulable the instant `sched_lock` drops, so a thread could run for
one instruction believing it was a process, consult its own (zero) heap
break on its first `malloc`, and map a page at virtual address 0. That is
precisely the failure M40's own comment in that function describes for
`heap_brk`, twelve lines above where the new code was; the fix is the
same one, a parameter rather than an assignment afterwards.

*The kernel knows nothing about a pthread, and that is what shared memory
is for.* `SYS_thread_create` makes a task in the caller's address space
and drops it into ring 3 at an address the caller chose on a stack the
caller allocated. Everything a pthread *is* — a start routine with a
return value, joining, a mutex — is built in
`user_space/libc/src/pthread.c` out of memory the two threads now share.
A kernel that owned `pthread_t` would be a kernel with an opinion about a
C library.

*The stack comes from the caller because the layout has room for exactly
one.* `USER_STACK_TOP`/`USER_STACK_PAGES` is a single fixed stack, and
inventing a second fixed location would put a ceiling on how many threads
a process may have into the address map. A stack allocated out of M78's
arena is also a stack a program can free — which is why `pthread_join`
unmaps it and the thread cannot: a thread cannot unmap the stack it is
standing on, and that is the whole reason `pthread_join` exists in every
implementation of it.

*`SYS_exit` now ends a thread group and `SYS_thread_exit` ends one
thread*, which is what POSIX `exit()` and `pthread_exit()` mean and what
`return` from `main` has always meant. `SYS_getpid` answers with the
group's id and `SYS_gettid` with the task's — identical for anything that
is not a thread, which is every process this OS ran before this
milestone, so nothing that called it before means anything different now.

*malloc had to take a lock, and it takes its own.* The free-list walk,
the split and the coalesce were all written when a process had one thread
of control. It now brackets them with a `lock xchg` spinlock of its own
rather than with `<pthread.h>`'s mutex: malloc is linked into every
program here and most will never have a second thread, so it cannot
depend on the thread library. One atomic instruction is the entire cost,
and it is the price every real allocator pays.

*What the self-test checks that a program cannot.* `threadtest` makes the
classic assertion — two million increments through a mutex arriving as
exactly two million — plus the ones that stop it passing for the wrong
reason: memory written by one thread and read by the other (two processes
would fail this), different tids under one pid, and each thread's
floating-point result matching what the same computation produced alone,
which is M63's per-task FXSAVE still holding for two tasks that are the
same process by every other measure. What it *cannot* see is the sentence
this milestone is actually about, so the kernel checks that separately:
while the program runs, three entries in the task table have the same
`pml4_phys`. That has never been true on this machine before.

*The honest limit: threads share memory, and share the descriptors that
were open when they were created — including their file positions, since
a descriptor points at a kernel-side open-file entry (M59) — but a
descriptor opened AFTER a thread starts is not visible to it.* The fd
table is copied at creation the way a spawn copies it. Making it shared
means making `task_t.fds` a pointer to something refcounted, which is a
real change to every path that touches a descriptor, and nothing has
asked for it yet. `<pthread.h>` says so at the top rather than leaving it
to be discovered.

### M80 — Somebody else's language [⊘] superseded by M99; not being resumed

*Superseded rather than pending — see "Before M94" below. M99 is this
same program built the other way round, on the machine with the
machine's own compiler, which is the inverse of what was attempted here
and the reason this one never linked. The bullets below stay as the
record of the attempt; nobody is going back to them.*

- [ ] Pick the program and let it decide the surface, M63's rule at a
      larger scale: CPython, and whatever `./configure` and the link
      step actually demand — not a module list guessed beforehand
- [ ] A fully static build (`--disable-shared`, no loadable extension
      modules, every needed module — `posix`, `time`, `io`, `marshal` at
      minimum — compiled directly into the one binary). M75–M79 buy
      real POSIX primitives and threads, not a dynamic loader, so
      static is the only shape that fits what exists
- [ ] Somewhere real for the standard library to live — frozen into the
      binary or shipped as `.py` files on leanfs, whichever the actual
      port turns out to need cheaper
- [ ] Changes made to CPython's own source: as close to none as M63's
      Whetstone rule demands. A `config.h`/`Modules/Setup.local` naming
      what is built in is configuration, not a patch to the interpreter
- [ ] The memory budget known before starting, not discovered halfway —
      M63's notes are explicit that this is why Whetstone got ported and
      DOOM did not. CPython's own startup footprint is tens of
      megabytes before a user script runs a single line, and M78's mmap
      arena needs sizing against that number up front

**How we'll know.** `python3 -c "print(1+1)"` prints `2` through this
project's own `SYS_spawn`/stdout plumbing, graded the way M63 graded
Whetstone — and then something with actual weight behind it: a small
pure-Python script exercising a `dict`, a class, a loop and a file
open, because an interpreter that starts is a much lower bar than an
interpreter that runs an ordinary program someone else wrote.

**Expect this to be a multi-attempt milestone, like M74.** It is
written as one milestone because the goal is one thing, not because
landing it in one pass is the likely outcome.

#### Attempt notes — the specification, and how far it got

**Status: `python3 -c "print(1+1)"` does not run. What was reached is
that every one of CPython 3.11.9's 113 core translation units — all of
`Objects/`, `Python/` and `Parser/` — compiles against this OS's own
libc, with an `x86_64-elf` cross-compiler, `-ffreestanding`, and no
change to a single line of CPython's source. Nothing has been linked.**

*The method was the milestone's own, and it worked exactly as advertised.*
"Pick the program and let it decide the surface... not a module list
guessed beforehand." So the attempt started with a four-line file
containing `#include "Python.h"`, a near-empty `pyconfig.h`, and the
compiler's own error messages as the specification. What came back, in
the order it came back:

| what CPython asked for | what it got |
|---|---|
| `<wchar.h>` | a real one — see below on what it is honestly wrong about |
| `<inttypes.h>` | `<stdint.h>` plus the `PRI*` macros |
| `NATIVE_TSS_KEY_T` | `pthread_key_*`, thread-specific storage |
| `pthread_cond_t` | condition variables — M79's own deferred item, asked for |
| `<locale.h>`, `<langinfo.h>` | the C locale, and `CODESET` = ASCII |
| `<fcntl.h>`, `<sys/time.h>`, `<setjmp.h>`, `<errno.h>` | four new headers |
| `struct tm`, `localtime_r`, `mktime`, `strftime` | the calendar M63 declined |
| `struct sigaction`, `sigset_t`, `sigemptyset`... | M76's POSIX spelling |
| `getc`, `ferror`, `clearerr`, `fileno`, `ungetc`, `fdopen`, `perror` | the rest of stdio |
| `isnan`, `isinf`, `frexp`, `ldexp`, `modf`, `hypot`, `copysign`... | the rest of math |
| `strtoul`, `strtoll`, `qsort`, `bsearch`, `strpbrk`, `strdup`, `strerror` | the rest of stdlib/string |
| `isatty`, `lseek`, `dup2`, `open`, `fcntl` | the rest of unistd |
| `clock_gettime`, `clock_getres`, `gettimeofday` | the clocks, at their real resolution |

Every one of those is a genuine gap this libc had, and every one is now
filled by something that says what it can and cannot honestly do. Three
are worth singling out because the honest answer is *not* the obvious
one:

- **`wchar.h` converts as Latin-1, and says so at the top.** There is no
  multibyte encoding anywhere in this OS — the font is one glyph per
  byte (M39/M57) — so "one byte is one character" is not an
  approximation here, it is the truth. It is wrong above U+00FF, and
  that is written down rather than discovered.
- **`nl_langinfo(CODESET)` returns `ANSI_X3.4-1968`, not UTF-8.** A
  program told UTF-8 would encode above U+007F and produce bytes nothing
  here can draw. Told ASCII, it either stays in range or reports that it
  cannot represent something — which is the truth.
- **`fcntl` answers `F_GETFD` and `F_GETFL` with 0 and refuses to *set*
  anything.** There is no exec on this machine, so `FD_CLOEXEC` has
  nothing to mean and is genuinely not set; `O_NONBLOCK` is 0 for the
  same kind of reason. Accepting a flag that will not be honoured is the
  failure mode the whole header is written to avoid.

*And two of the demands were milestones cashing cheques they had
written.* M79's "deliberately not the rest of the pthread surface —
condition variables... beyond whatever the next real program's link
errors ask for" was answered by `Include/internal/pycore_condvar.h`
refusing to compile: the GIL is a lock and a condition variable, and
there is no smaller version of CPython that skips it. M63's "deliberately
no struct tm, no strftime, no localtime... a program that needs them will
say so at link time, which is the specification" was answered by
`Python/pytime.c`. Both notes were written years of milestones apart and
both turned out to be exactly right about what would ask.

*The condition variable is a counter and a yield, and that trade is the
interesting part.* There is no futex here — a blocking wait keyed on a
user address is a real kernel feature — so a waiter samples a sequence
counter, drops the mutex, and spins on `SYS_yield` until it moves. That
makes `pthread_cond_signal` wake every waiter rather than one, which is
a legal implementation rather than a corner cut: POSIX permits spurious
wakeups precisely so a correct program re-tests its predicate in a loop.
What it costs is a burned time slice per waiting thread, and on a
scheduler whose quantum is one tick (M69) that is a slice, not a core.

*What is left is not more of the same, and it is worth being precise
about why.* The 113 files that compile are the interpreter's *core* —
the object model, the evaluator, the parser. They are also the part with
almost no operating system in them. Everything still ahead is the part
that is nothing but operating system:

1. **`Modules/posixmodule.c` has not been attempted at all**, and it is
   where the platform surface actually bites: `fork`, `execv`,
   `waitpid`, `pipe2`, `select`, `poll`, `sysconf`, `getuid`, `chmod`,
   `symlink`, `readlink`, `utime`, `statvfs`, `sched_yield`. This OS has
   no `fork` and never will (`SYS_spawn` is a combined fork+exec by
   design, M13), so that file needs real decisions rather than more
   headers — which is a different kind of work from everything above.
2. **Nothing has been linked**, and a compile is a much lower bar than a
   link. Every `-Wimplicit-function-declaration` that is now a
   declaration is a symbol that has to exist.
3. **The build needs a host CPython of the same version** to run
   `Programs/_freeze_module` and `Tools/build/deepfreeze.py` and to
   generate `Python/frozen_modules/*.h`. A cross-build of CPython starts
   with a native build of CPython, which is a prerequisite this attempt
   never reached.
4. **The standard library still has nowhere to live**, and M80's own
   bullet says the choice between frozen-in and `.py` files on leanfs is
   one the port should make rather than one to guess at. It has not been
   made.
5. **The memory budget is still unknown**, which M80's own last bullet
   warned about explicitly: "the memory budget known before starting,
   not discovered halfway". M78's arena is 128 MiB of *address space*,
   but every mapping here is backed by a real frame at the moment of the
   call — there is no page-fault handler that could fill one in later —
   so the ceiling is physical, and nobody has measured it against
   CPython's tens of megabytes of startup footprint.

*Why this is written as a stop rather than as a step.* M74's first
attempt was reverted with a note saying it was "one bisect step from
being understood, and that step is worth taking with a clear head" — and
that turned out to be true and to be worth the wait. This is the same
judgement at a much larger scale: the next thing to do is a native build
of CPython and then `posixmodule.c`, and starting either of those badly
is how a port acquires the local modifications M63's rule exists to
prevent. The libc that came out of this attempt is not provisional -
it is thirteen headers and several hundred lines that this project
needed anyway, every one of them named by a real program rather than
guessed, and every one of them checked by `libctest`.



## Cleanup pass — duplication, dead code, and one measurement that said no

Not a milestone: a sweep for things that had accumulated, done after M79
with the same rule the rest of this file follows - a change is kept
because something measured or verified says to keep it, not because it
sounded like an improvement.

**Eighteen copies of seven functions, in one header now.** Every
protocol in `kernel/net/` reads and writes the same two things - integers
in network byte order and RFC 1071's one's complement sum - and every one
of them had grown its own private copy: four each of `read_be32` and
`write_be32`, two each of `read_be16`, `write_be16`, `sum16` and
`checksum16`, and two more of the checksum fold under two different names
(`fold` in `tcp.c`, `fold_checksum` in `udp.c`), across `arp.c`,
`dhcp.c`, `ip.c`, `udp.c`, `tcp.c` and `icmp.c`, all byte for byte
identical. Nothing had gone wrong, which is the point at which this is
cheap to fix: eighteen copies of a definition are eighteen places a fix
has to land, and the fourth gets written by pasting the third. They are now `static inline` in
`kernel/net/wire.h`: 184 lines deleted from the six protocol files
against 69 added, plus one 81-line header that is mostly the comment
explaining itself. The checksum is split into `net_sum16` (accumulate)
and `net_fold16` (fold and complement), because TCP and UDP need to run
a pseudo-header, a header and a payload through it in three calls
without assembling a buffer - which is exactly why `fold` and
`fold_checksum` existed separately in the first place - while `ip.c` and
`icmp.c` each wanted the one-shot form. `net_checksum16` is now that
one-shot spelled in terms of the other two, so there is one copy of the
arithmetic and no way for the two spellings to disagree.

**`rep insw`, and why it stayed even though it changed nothing.** The
ATA driver moved its sectors one 16-bit word at a time through a
`for` loop of `inw`. Each of those is a VM exit into QEMU's device
emulation and the `rep` string form is one exit for a whole 512-byte
sector - 256 exits down to one - on a boot that pushes about 1.5 MB
through that port seeding 47 programs onto a fresh disk. It was an
obvious win and it is not one: **the seed takes 0.2 s either way, and
boot to `[init] PID 1 spawned` measured 148.7 s before and 148.3 s
after.** Kept anyway, but the comment in `io.h` now says plainly that it
was kept for being the smaller and more conventional way to write the
same transfer and not for speed, so that nobody re-derives the
hopeful version of the reasoning later.

**Where the boot's 148 seconds actually go.** Worth recording, since the
above is the second time an assumption about it has been wrong. Measured
by timestamping markers as they reach the serial log:

| reached | at |
| --- | --- |
| memory, framebuffer, font | 1.5 s |
| scheduler, syscalls, pipes | 9.6 s |
| filesystem up, 47 programs seeded | 10.0 s |
| windowing, clipboard, VFS, ACPI, SMP | 21 s |
| **network self-tests (m42-m50)** | **21 s -> 55 s** |
| m55, m59, m60, m63, m65 | 69, 93, 110, 120, 127 s |
| m67 through m79, init handoff | 139 -> 148.6 s |

Disk I/O is 0.2 s of it. The largest single block is the network
self-tests, and that time is round trips and timeouts rather than work -
which is to say the boot is slow because of what it waits for, not
because of how it computes, and there is no optimisation here worth
making without changing what the tests assert.

**A function whose comment named a test that did not exist.**
`openfile_in_use()` in `kernel/fs/openfile.h` carried the comment "for
the boot self-test that asserts a process's files are given back when it
exits" - and nothing called it. The intent had been written down and
then lost, which is worse than not having written it: the table it
counts is global and holds 64 entries for the whole machine, so a leak
in it is not one process running out of files, it is every `SYS_open` on
the machine failing after the sixty-fourth one, presenting as "the
editor stopped saving" an hour later and pointing nowhere near the cause.
It is now the M59 self-test's seventh claim - the sibling of claim 3,
which asserts the *disk* gets its blocks back, on the other side of the
descriptor - and it was verified the way any new assertion should be:
by leaking exactly one descriptor on purpose and confirming the boot
panics with `the open-file table went from 0x00000000 entries to
0x00000001`.

**Five dead functions removed**, each one superseded rather than merely
uncalled: `vga_puts`, `vga_put_hex32`, `vga_put_hex64` (klog.h already
says every message goes through klog and nothing calls `vga_*`
directly), `serial_puts` (same - klog owns that channel and iterates
itself), and `sched_is_running` together with the `scheduler_running`
flag behind it, whose comment named `pit_sleep_ms` as its caller when
`pit_sleep_ms` had by then grown a comment of its own explaining why it
deliberately halts instead.

**Six more were left alone deliberately**, because "uncalled" and
"cruft" are not the same thing: `pic_set_mask` and `klog_log_hex32` are
each one half of a symmetric pair whose other half is in use, and
`tsc_cycles_per_us`, `dispi_vram_bytes`, `socket_type` and
`tcp_send_space` are documented accessors on interfaces that are
otherwise live. Removing half an API because today's callers happen not
to need it is its own kind of drift.

*Verified*, and worth stating precisely because two runs in the middle
of it were red. The serial harness passes 69/69, including every network
self-test from the handshake through a 16 KiB transfer that survives 17
deliberately dropped segments - which is what actually checks the
consolidated checksum code. One serial run, started seconds after a
full interactive suite finished, reported `[wm] animation missed its
frame budget`; it passed twice on re-run with the machine idle, which is
what that assertion has always been sensitive to. The interactive suite
reported 43/45, and both failures - a boot timeout in
`soak_desktop_stays_usable` and a window count of 3 where
`desktop_survives_losing_the_compositor` wanted 2 - passed when re-run
one guest at a time, at 155 s where the failing run took 171 s. That is
the contention artifact `qemu-input-test.sh`'s own header warns about
and tells you to re-run before believing; it is worth noticing that the
warning was right rather than assuming it was.


## The arc after that: the things underneath the names

M77 was called "POSIX names for what is already here," and that is
exactly what it was — a header layer over machinery this OS already had,
written so that a program could spell things the way it learned to spell
them elsewhere. This arc is the inverse, and the name is the honest
description: **the things underneath the names.** Every milestone below
builds a mechanism this machine has never had, and several of them
collect a deferral written down years of milestones ago.

**The target is not POSIX conformance, and choosing that deliberately is
the first decision of the arc.** POSIX.1-2017 is roughly 1,200 functions
and 160 utilities, and a meaningful fraction of both exists to describe
machines nobody builds any more. Conformance is a certification; this
project has never wanted one. The target instead is the M63 rule at the
largest scale it will bear:

> **Software written for Unix by someone who has never heard of lean_os
> builds here and runs here, without patching its source.**

That is a smaller target than conformance and a much larger one than
M75–M79, and it has the property this project keeps choosing: it is
falsifiable by running something, not by reading a standard. Where the
two targets disagree, this arc follows the program.

**What the previous arc established, and where it stopped.** M80 got
every one of CPython's 113 core translation units compiling and stopped
at `Modules/posixmodule.c`, which is the file where the operating system
actually lives. Its five numbered blockers are not a list of headers
this libc is missing. Four of the five are subsystems this kernel does
not have, and this arc is those subsystems in the order a program hits
them. M80 is not renumbered and not abandoned: it is the milestone that
becomes attemptable again once M84 lands, and the thing that grades M87
and M88 when they do.

**A note on where these estimates come from, and how much to trust
them.** Nine milestones, and at least three of them — M82, M85, M87 —
are of the size that M74 and M80 turned out to be, which is to say
multi-attempt. Writing them as one milestone each is a statement about
what the goal is, not a prediction about landing it in one pass. That
was true of M80's own note and it was right.

**Two things this arc deliberately does not do, stated here rather than
discovered halfway.** There are still no users — M65's argument holds
completely and nothing below weakens it, and where a ported program asks
`getuid()` the honest answer on a single-principal machine is 0, which
is a true statement rather than the fiction M65 refused to write. And
there is still no dynamic linker; every binary this arc produces is
static, for the same reason M80's Python was going to be. See the
closing note for why M89 is probably the milestone that finally collects
that deferral rather than restating it.

### M81 — A filesystem that can hold somebody else's program ✅

- [x] leanfs v2 on-disk format. `LEANFS_MAX_INODES` is **192** — that is
      not a per-directory limit, it is every file on the disk — and a
      source tree, a standard library or an unpacked archive is
      thousands. This is the wall M80's fourth blocker ("the standard
      library still has nowhere to live") actually hits, and it hits it
      before a single syscall is involved
- [x] `LEANFS_MAX_NAME` 27 → 255 and `LEANFS_MAX_PATH` 128 → 4096, the
      numbers `<dirent.h>` already advertises. M77's `NAME_MAX` is 255
      today with a comment admitting leanfs's real limit is 27; one of
      those two numbers is a lie and this milestone decides which
- [x] `d_ino` reports a real inode number. `<dirent.h>` says plainly that
      leanfs has inode numbers and `SYS_listdir` reports names instead —
      that comment is the bug report, and a program that wants identity
      should not have to `stat` every entry to get it
- [x] A directory read as a stream, not whole-shot. `SYS_listdir`'s
      newline-separated buffer was built for a person reading a terminal
      and `<dirent.h>` says so; a directory that can now hold thousands
      of entries is a directory that no longer fits the shape that
      assumed it could not
- [~] A format version in the superblock — **shipped**; the migration
      that runs once — **not**, and the progress notes say why: this
      milestone moves every region on the disk, so there is nowhere to
      migrate *to*. The version field and the branch that would hold a
      migration are in place for the next format change, which will be
      the kind that can
- [x] Deliberately **not** a journal, and deliberately not extents.
      M71's own note argued that write ordering plus a mount check buys
      most of the safety and that a journal is worth it when there are
      multiple writers or a full scan gets slow. Thousands of files
      makes the second one *nearly* true, and "nearly" is not a
      measurement — this is where that deferral gets re-examined, in
      M69's spirit, not where it gets cashed on a guess

**How we'll know.** Unpack a real source tarball — thousands of files,
paths deeper than 128 bytes, names longer than 27 — and `treewalk` it
back, comparing the count and the total bytes against what went in. A
tree that would have fitted in 192 inodes proves nothing at all, and the
test should be written so it cannot accidentally be one.

**Why this is first even though it is not the hardest.** It is the
cheapest of the nine and it is the only one that blocks M80
independently of every other. It also has to happen before M87's links
and before anything writes a `/proc`, and doing the on-disk format once
is better than doing it twice.

#### Progress notes

*The inode cap was the easy half, and the directory record was the whole
milestone.* Raising `LEANFS_MAX_INODES` from 192 to 8192 is a number and
a bigger array. Raising `LEANFS_MAX_NAME` from 27 to 255 is not, because
27 was never a taste decision — it was exactly what made a directory
record 32 bytes, so sixteen fitted a block and no record straddled one.
That invariant is what lets a lookup in a small directory read a single
sector, and it is worth more than it looks.

A fixed record big enough for a 255-byte name is 260 bytes: it no longer
divides a block, and it makes a directory of three thousand
twenty-character filenames 780 KiB that every lookup scans, where the
names themselves are 60 KiB. So records are variable-length, ext2's
shape, with the one rule that keeps the block invariant intact: the last
record in a block has its `rec_len` stretched to reach the end of that
block, so records tile each block exactly and none ever crosses a
boundary. A directory is still a whole number of blocks and scanning one
is still scanning a sector.

*Three things fell out of that which were not planned.* `dir_load` and
`dir_store` — load the whole directory, modify it, write all of it back
— are gone, because at 8192 inodes that was two megabytes of scratch and
four thousand sector writes per created file. Creating a file now writes
the one block its name landed in. Free space is `inode == 0`, which is
safe as a sentinel for exactly the reason block 0 is: inode 0 is the
root, and the root is nobody's child. And the record carries a type, so
`d_type` is read rather than reconstructed from the trailing `/`
`SYS_listdir` appends — which M77 had to do because the old record had
nowhere to put it.

*The stack was the real cost of `PATH_MAX`, and it took two panics to
find out.* M53's comment next to `LEANFS_MAX_PATH 128` said the number
was load-bearing because the syscall layer puts a path on an 8 KiB
kernel stack. It was right. The first boot after the bump panicked in
`pmm_free_frame` with a double-free — which is what a task writing
through the bottom of its stack into the allocator's business looks
like. Fixed by moving the stack, not the path: 8 KiB → 32 KiB.

The second panic was better. `kernel_main` runs every boot self-test on
its *own* stack (`entry.asm`), which was 16 KiB and is not the one that
had just been raised. It overran by about 0x6F0 bytes into `.rodata` and
clobbered the tail of `embedded_programs`, a `static const` array sitting
just below it. That did not present as a stack overflow. It presented as
five garbage program names twenty self-tests later, and then as a page
fault dereferencing the middle of a string literal as a pointer. So that
stack is 64 KiB now and there is a guard word underneath it that
`kernel_main` checks and the serial harness greps for. A guard cannot
catch a leaf frame that jumps clean over it; it catches a chain walking
down through it, which is the thing that actually happens, and it turns
an afternoon of bisecting into one line of log.

*Kernel stacks came out of the heap, and that was a self-test telling the
truth.* With 32 KiB stacks the M67 concurrency test began reporting
nineteen leaked frames. Nothing had leaked: four concurrent 32 KiB stacks
were more than the heap had slack for, so the heap grew, and a heap here
grows by taking frames from the PMM and never giving them back — which is
indistinguishable from a leak to a test that counts free frames on either
side of a spawn. A stack is a page-granular object with a page-granular
lifetime, so it now comes from `pmm_alloc_contiguous` and goes back to
`pmm_free_contiguous`. The frames a task borrows are the frames it
returns, and M67's accounting balances because it is telling the truth
again.

That change then broke M79's own frame assertion, in a way worth
recording because the comment predicted it exactly. M79's test ends by
reaping every terminated task, justified with "their kernel stacks are
heap rather than frames, but their slots have to go back" — true when
written, false as of this milestone. The sweep was handing the test *more*
free frames than it started with, so a strict equality failed with a leak
of zero. The fix is to clear the backlog before the baseline as well as
after, so "every frame back" does not have to be spelled "every frame
back, give or take an earlier test's litter".

*The version field is honest about not being used yet.* M81's own bullet
asked for "a migration that runs once, so an existing image is upgraded
rather than reformatted", and that is not what shipped. The reason is
structural rather than a shortcut: the inode table grew by 2016 sectors,
which moves the bitmap and every data block on the disk, and an in-place
upgrade would have to relocate the entire data region on a disk already
sized to hold it. A format change that only reinterprets bytes can
migrate; one that moves them cannot. So the superblock gained a `version`
distinct from the magic — the magic guards geometry, the version guards
meaning — and the branch that would carry a migration is written, is
currently a reformat, and says in the code that being a reformat is a
decision rather than an oversight. The inode was padded to 128 bytes in
the same change, which leaves 44 bytes for M87's link count and symlink
target to land in without moving anything after them.

*Two bugs the scale found that 400 files would not have.* The self-test
was written at 400 files first, then raised to 1200 to match the
milestone's actual claim, and the raise found both. The first was in the
test: names were formatted with three digits in three places, so file
1000 was called `f000` and overwrote file 0 — a directory told to hold
1200 names held 1000, and the test reported the *filesystem* as broken.
The filesystem was fine. One formatter, wide enough for the count, is the
fix. The second was real: `dir_add` scanned from block zero on every
insert, so filling a directory was quadratic in exactly the workload this
milestone exists for. A one-entry hint remembering where the last insert
landed takes the common case to a single block read; it is never trusted,
so a stale hint costs one extra pass and can never hide a hole.

*And a measurement that another milestone was waiting for.* 1200 files,
a 255-character name, a 252-byte path, a full streaming walk and a
delete/recreate cycle cost 7.7 s of boot, taking it from 130 s to 135 s.
Most of what remains is not the insert — it is `dir_lookup` missing:
creating a file asks whether the name already exists, and that scans
every block of the directory as an uncached PIO read. M87's "deliberately
not a buffer cache or a page cache… no measurement has asked for one" is
the deferral this is the measurement for. It is not cashed here, because
M81 is not the milestone to add a cache to, but the number now exists and
M87 should be made to look at it.

*What "unpack a real source tarball" turned into, and why that is not a
dodge.* The milestone's own "How we'll know" asks for a tarball unpacked
and walked back. There is no `tar` on this machine and no `gzip`, and
building one to test a filesystem would be building M89's work inside
M81. So the self-test creates the tree directly: 1200 files in one
directory, a name at the 255-byte boundary rather than near it, and a
252-byte path built by nesting twelve levels the way a real tree produces
one. The property the tarball was standing in for is what is actually
asserted — a tree that could not have existed under the old caps, walked
back entry by entry with every inode number distinct. The tarball
version of this test becomes possible at M89 and is worth doing then,
against a tree nobody here chose the shape of.

*Verified.* The serial harness passes 70/70 — the 69 this milestone
started with plus `[m81]`, and including the new `[boot] kernel stack
guard intact` line. `tools/leanfs-put.c` was ported to the same format
and checked end to end rather than by inspection: an image preseeded by
the host tool boots without the kernel reformatting it (so magic,
version and geometry all agree) and `/bin` lists 48 entries where the
build ships 47 — the one extra being the file the tool wrote, which is
the kernel reading the tool's variable-length records correctly.

### M82 — A page that arrives when it is asked for ✅

- [x] A page-fault handler that **populates** rather than kills.
      `SYS_mmap`'s own comment is the specification: *"Backed by real
      frames at the moment of the call, because nothing here fills a
      page in on a fault."* Everything in this milestone is that
      sentence stopping being true
- [x] Anonymous zero-fill on first touch, so a mapping costs address
      space until it costs memory. This is M80's fifth blocker — "the
      memory budget is still unknown... M78's arena is 128 MiB of
      *address space*, but every mapping here is backed by a real frame
      at the moment of the call" — answered at the mechanism rather than
      by measuring harder
- [x] Reference-counted physical frames in M5's PMM, which nothing has
      needed until now because no frame has ever had two owners. M83 is
      the milestone that gives them two, and this is where the counting
      goes so that fork does not invent it
- [x] The fault handler distinguishes *this address is not mapped yet*
      from *this address is not yours*, and the second one still ends
      the process the way M4's handler always has. A page fault that
      silently succeeds where it used to catch a bug is the regression
      this milestone is most able to cause, and `badptr` is the test
      that already exists to catch it
- [x] Deliberately **not** swap. There is nowhere to put an evicted page
      that is not this project's own filesystem, no measurement asking
      for one, and M69's rule says performance work waits for a
      measurement. The ceiling stays physical; it just stops being
      reserved in advance

**How we'll know.** A mapping substantially larger than the machine's
RAM, touched sparsely, works — and the physical high-water mark reported
by the PMM equals the number of pages actually touched, not the number
mapped. The second half is the whole test: a large mapping that
*succeeds* proves nothing if it quietly allocated everything.

#### Progress notes

*The handler is twenty lines and the milestone is everything around it.*
Filling a page on a not-present fault is the easy part: find the region,
allocate a frame, zero it, map it, return. What took the work is the set
of faults that must keep being fatal, because a fault handler that is
generous by one case turns every wild pointer in every program into a
silent success. `fill_one_page` refuses four things and each one is a
real bug it would otherwise hide: a fault on a page that is already
present (a protection violation, not a first touch), an address outside
the arena (the heap, stack and image are all mapped eagerly, so a fault
there always was a wild pointer), an address inside the arena that no
mapping covers, and a write to a mapping the caller asked for
read-only. The last two are checked by running a program that commits
them and requiring the kernel to kill it - `lazytest ro` and `lazytest
gap` - because an assertion that a thing still fails is worth more here
than an assertion that a thing works.

*The bug this would have shipped with is not in the fault handler at
all.* Every syscall that writes into a caller's buffer asks
`user_range_ok` whether the range is really the caller's, and that
question is answered by walking the caller's page tables. A page
reserved by `SYS_mmap` and never touched has no page table entry, so
the honest answer to "is this mapped" is *no* - and `read(fd, mmap(...),
n)` would have started returning -1. Demand paging would have quietly
broken the most ordinary thing anyone does with an mmap, and nothing in
the milestone as written would have caught it: the fixture only touched
its own memory.

The fix is to build the range before checking it rather than to let the
kernel's copy loop fault. That is a deliberate choice and not the
obvious one - a page fault taken in ring 0 while the kernel holds a lock
is a much harder thing to reason about than a loop that runs before any
lock is taken - and it keeps the invariant that every kernel access to
user memory touches only present pages. `lazytest` now passes an
untouched mapping to `getcwd`, which is the smallest thing that would
have failed.

*The frame count needed a lower bound, and working out why is the most
useful thing in this milestone.* The obvious assertion is that a 144 MiB
reservation costs far fewer than its 36864 frames. That assertion is
also passed, perfectly, by a test that samples the frame count *before
the program has mapped anything* - the polling loop finds a process that
has barely started, measures almost nothing, and reports success without
ever observing the thing it exists to observe. So `lazytest` touches 256
pages on purpose and the self-test requires the spend to be at least 200
frames as well as at most 2048. The upper bound proves laziness; the
lower bound proves the measurement happened.

*Refcounted frames, for a milestone that does not need them.* M82's
bullet said to put them here so M83 does not invent them, and that turned
out to matter sooner than expected: the guard in `fill_one_page` against
building a page that already exists is unreachable today and is exactly
the case copy-on-write introduces - a page present and read-only inside a
region whose prot says writable. Without the guard, the first write to a
COW page would map a fresh frame over the shared one and strand it: a
leak with no owner, invisible to every free-frame assertion in this
kernel because the count would simply be short. Found by reading the new
code against the next milestone rather than by running it.

*What this changes about failure, said plainly.* Under M78's eager
mapping, running out of memory happened at the call that reserved, and a
program could handle a -1. It now happens at the instruction that
touches, and that instruction cannot be handled - the process dies. That
is the trade demand paging always makes and it is written into
`sched_fault_fill`'s own header rather than left to be discovered. What
it buys is the thing M80's fifth blocker was actually about: a program
can reserve more than the machine has, which is what every real program
that mmaps assumes.

*The arena grew because address space stopped being expensive.* 128 MiB
to 160 MiB, which is now bounded by `USER_SHM_BASE` with a deliberate
32 MiB gap rather than by a policy. That is enough to be larger than this
machine's 128 MiB of RAM, which is the property the test needs; going
further means moving the shm window and the framebuffer's fixed mapping
address, which is a bigger change than this milestone needs.

### M83 — Two processes from one ✅

- [x] `fork()`, with copy-on-write over M82's fault handler and M82's
      frame refcounts. The child gets its own address space, its own
      copy of the fd table (M14 already inherits one, so this half
      exists), the same cwd, the same environment, and 0 where the
      parent gets a pid
- [x] Reverse M13's "no fork," explicitly and in writing. **M13 was
      right when it was written** — a combined spawn is the correct
      primitive for a machine with no fault handler, because the only
      alternative was a full eager copy of an address space. M82 is what
      changes the trade, and the sentence in M80's notes that says this
      OS "has no `fork` and never will" is the one this milestone
      retires. A deferral that named its own condition is being
      collected, which is the pattern, not a reversal of judgement
- [x] `SYS_spawn` stays, unchanged and un-deprecated. It is not a
      workaround that fork replaces — it is `posix_spawn`, it is what
      every launcher on this desktop already calls, and it remains the
      cheap path for the overwhelmingly common case of "start this
      program." Two primitives because there are genuinely two things
- [x] `MAX_TASKS` (128) and the scheduler's slot table meet a workload
      that creates and destroys processes in a loop for the first time.
      Slots are never recycled today — `SYS_taskinfo`'s comment says so
      and `SYS_wait` relies on it — and a shell running a build will
      exhaust 128 in seconds. Recycling a slot without breaking what
      reads it is the real work in this bullet
- [x] Deliberately **not** `vfork`, and not `posix_spawn` as a third
      spelling. One eager primitive and one lazy one is the whole set

**How we'll know.** The classic, and it has to be the classic because it
is the thing that is hard to fake: fork, have both sides write to the
same page, and have each read back only its own value — while the PMM
reports one shared frame before the writes and two after. Then a shell
loop that forks and reaps ten thousand times and leaves the slot table
where it found it, which is the `MAX_TASKS` half.

#### Progress notes

*M13 was right, and that is the point.* M13 chose a combined spawn over
fork because the only fork available to it was an eager copy of an entire
address space at every call. That was the correct call for a kernel with
no page-fault handler, and it stayed correct for seventy milestones. M82
is what changes the trade: with a handler that can populate, a fork
becomes a page-table walk and a refcount, and the copy happens only for
pages somebody actually writes. The deferral named its own condition and
the condition arrived - which is the pattern, not a reversal.

`SYS_spawn` is unchanged and un-deprecated. It is `posix_spawn`, every
launcher on this desktop calls it, and it remains the cheap path for
"start this program". Two primitives because there are genuinely two
things.

*Only writable pages are marked copy-on-write, and getting that backwards
would have been silent.* The obvious implementation marks every shared
page. It is wrong in a way no test would find quickly: a page that was
already read-only - the program's own text, a `PROT_READ` mapping - would
then be indistinguishable from one that was writable before the fork, and
the first write to the program's machine code would be quietly granted a
private writable copy instead of killing the process. So the mark goes on
writable pages only, and `vmm_cow_break` refuses anything unmarked. A
write to text is present, unmarked, and as fatal as it has always been.

*Fork copies exactly what teardown frees, and that is an invariant rather
than a tidiness.* `vmm_fork_address_space` takes an owner reference on
every page it copies and `vmm_destroy_address_space` drops one for every
page it frees. A page in one list and not the other is a frame pinned for
the machine's uptime with nothing pointing at it. The shm window and the
framebuffer are in a process's address space without belonging to it -
`shm.c` and the compositor own those frames - so they are in neither
list, and the child does not inherit them. `PROCESS_OWNED` in proc.c is
now one list read by both.

*The guard M82 added for a case that could not happen yet was the case
this milestone made happen.* M82's `fill_one_page` refuses to build a page
that already exists, written for "a page present and read-only inside a
region whose prot says writable" - which is precisely a copy-on-write
arena page. Without it, the prefault path would have mapped a fresh frame
over the shared one and stranded it. Found by reading M82's new code
against M83 rather than by running either.

*A threaded process cannot fork here, and that is an SMP limit stated
rather than discovered.* Making a page copy-on-write clears its writable
bit in the parent's tables, and the `invlpg` that follows flushes only
this CPU. A second thread of the same process on another core still holds
the old writable entry and would keep writing to a page the child has
been promised is private - intermittently, on multi-core only, in a way
no single-core self-test would ever reproduce. The real fix is a TLB
shootdown IPI, which is its own piece of work and is needed by more than
fork. So `SYS_fork` returns -1 from a process with more than one live
thread. POSIX makes fork-from-a-thread nearly unusable anyway (only
async-signal-safe calls are legal in the child), so this refuses
something no correct program was going to do.

*Two failures that had nothing to do with fork, both found by the same
build.* The kernel image reached 2049 sectors and the Makefile's size
guard refused to build - which is the guard doing its job, and the fix
was to move the filesystem from 1 MiB in to 4 MiB in. Doing that exposed
the second: `FS_TOTAL_SECTORS` still said 65585, the number from when the
inode table was 32 sectors. M81 made it 2048, so the filesystem had
actually been ending 17 sectors *past* the start of the EFI System
Partition since that milestone. Nothing failed, because the ESP is
rewritten by every `make` and those 17 blocks are at the far end of a
32 MiB data region nothing had filled - the kind of bug that waits for
the day somebody's disk is full. Both are now checked at build time
rather than documented, and `ESP_START_LBA` is derived from the
filesystem's real size.

*And a third, which cost an entire test run.* Moving the ESP did not
rebuild `mbr.bin`, because that target depended on `mbr.asm` and not on
the Makefile the two hardcoded numbers come from. The result was a
correct image with a partition entry pointing where the ESP used to be,
and a firmware reporting "No bootable option or device was found" - a
failure with no visible connection to the one-line edit that caused it.
The target depends on the Makefile now.

*What the boot costs, because a self-test that costs more than the
milestone it checks is one nobody keeps.* M81, M82 and M83's self-tests
are 8.1 s, 12.2 s and 14.2 s, taking the boot from 130 s to 162 s. The
first draft of M82 and M83 took it to 280 s - almost all of it in park
loops sized generously rather than measured - and those are now as small
as the sampler tolerates. One optimisation was attempted and failed to
help: `munmap` was walking every *address* in a range rather than every
page table, which after M82 means thirty-six thousand four-level walks to
free two frames. Fixing it changed the boot by 0.1 s. The fix is kept on
its own merits and the comment says plainly that it did not buy what it
was written to buy.

### M84 — A program that replaces itself ✅

- [x] `execve`, which tears down the calling task's address space and
      loads a new image into the same task — same pid, same cwd, same
      parent, same fd table minus what is marked to close
- [x] `FD_CLOEXEC` becomes a thing that means something. `<fcntl.h>`
      currently reads *"There is no exec on this machine (SYS_spawn
      loads a fresh image and copies the fd table), so FD_CLOEXEC has
      nothing to mean and is genuinely not set"* — an honest comment
      that this milestone makes false, and the header has to change with
      it or it becomes the first dishonest sentence in that file
- [x] The wait family, properly. `SYS_wait`'s comment says it *"polls +
      cooperatively yields rather than a real blocking wait queue — M14
      is where 'more complete wait semantics' is scoped to land."* M14
      did not land it. `waitpid` with `WNOHANG`, a real wait queue, and
      `WIFEXITED`/`WEXITSTATUS`/`WIFSIGNALED`/`WTERMSIG` that decode a
      status rather than a bare exit code — which also fixes
      `SYS_task_alive`'s three-way squashing of "exited nonzero" and
      "died on a signal" into one answer
- [x] `PATH` search in the exec family (`execvp`, `execlp`), which M75
      explicitly deferred: *"Deliberately not a full PATH-search exec...
      M72 already owns those."* M72 owns the shell's copy of it; this is
      the libc's
- [x] Deliberately **not** `#!` handling in the kernel. M72 already
      resolves a shebang in the shell and that is where it belongs on a
      machine whose kernel has no opinion about interpreters

**How we'll know.** `sh` runs a three-stage pipeline in which every
stage is a real fork + exec + `waitpid`, and reports each stage's exit
status distinctly — including one that died on a signal, which is the
case the current `SYS_wait` structurally cannot report. And an fd marked
`FD_CLOEXEC` is gone on the far side of the exec while its neighbour
survives.

#### Progress notes

*The wait bullet was half already done, and finding that out was the
useful part.* `SYS_wait`'s ABI comment said it "polls + cooperatively
yields rather than a real blocking wait queue - M14 is where 'more
complete wait semantics' is scoped to land". M68 landed the blocking part
fifty-four milestones ago: a parent waiting on one child parks on that
child and is woken by it. The comment had simply never been re-read. What
was genuinely missing was not the blocking but the *answer*: an exit code
cannot distinguish a program that called `exit(139)` from one killed by
SIGSEGV, because 128 + signal is exactly the convention that makes a
signal death readable to a person and therefore ambiguous to a program.
So `task_t` gained an `exit_signal` that remembers which happened, and
`waitpid` reports a status that decodes into either.

*Exec is the shorter half of fork, and the ordering is all of it.*
Everything that reads the caller's address space - the path, argv, envp -
happens first, because that address space is about to stop existing.
Everything that can fail happens second, while the caller still has its
memory and can be handed a -1. Then the swap, after which nothing may
fail: there is no longer a program to return an error to. The frame is
rewritten rather than returned through, which is the same mechanism
`enter_user_mode` uses for a fresh process, minus the fresh process.

*One refactor, and it was the right size.* `process_build_address_space`
came out of `spawn_common` unchanged - an image, a stack and an argument
region, with no `task_t` anywhere in it. That is precisely what exec
wants and all it wants: it already has a task. Before the split the only
way to get an address space was to spawn a whole process, which is the
thing exec exists not to do.

*`O_CLOEXEC` was defined as `FD_CLOEXEC_BIT`, and that was a real bug
caught by reading rather than by running.* `FD_CLOEXEC_BIT` is 1 because
POSIX says the `F_SETFD` argument is 1; `OPEN_READ` is also 1 because
this ABI has said so since M59. They are two different namespaces
describing the same property, and conflating them makes `open(path,
O_RDONLY)` mark a descriptor close-on-exec and `open(path,
O_WRONLY|O_CLOEXEC)` mean `O_RDWR`. `O_CLOEXEC` is now a bit of its own
in the `OPEN_*` space. Nothing would have caught this quickly: both
mistakes produce a working `open` that is wrong about one thing.

*A spawn honours `FD_CLOEXEC` and a fork does not, and that is not an
inconsistency.* The flag means "close this when a different program
starts". A fork starts no program - the child is still running this one -
so it copies the flag and acts on nothing. A spawn is a fork and an exec
in one call, so it acts on it. Writing the rule down at both sites was
what made it obvious that it was one rule and not two.

*What exec keeps, and the one that is easy to get backwards.* Same pid,
parent, process group, working directory, descriptors except the marked
ones. Signal handlers go back to the default - their addresses are in
memory that no longer exists - but a signal the process had chosen to
*ignore* stays ignored, because that is a decision about the process
rather than about the image. Capabilities are intersected with the new
program's manifest and never widened, which keeps M65's "can only ever
shrink" true through a call that replaces the program entirely.

*Deliberately not `#!` in the kernel*, as the milestone said. `SYS_spawn`
resolves a shebang because it is the call every launcher on this desktop
goes through; exec is the call a program makes about itself, and a
program that wants to run a script can run its interpreter. A kernel with
an opinion about interpreters is a kernel doing the shell's job.

### M85 — A terminal that is a device [~] five of six; the pty is the one left

- [x] A real terminal device with a line discipline: canonical mode,
      echo, erase, kill, and the raw mode an editor needs.
      `<unistd.h>`'s `isatty` says today that *"There is no terminal
      device here to ask, so this is the honest approximation and not a
      stub"* — this is the milestone that gives it something to ask.
      **`/dev/tty` itself was not here when M85 landed** and arrived with
      M87: a device file needs a vnode layer with a mount table, and the
      arc put this milestone two ahead of the one its first line depends
      on. M87 built the seam and `devfs` wired `/dev/tty` to this same
      console terminal. Nothing about the discipline changed when the
      path arrived, which is exactly what `kernel/dev/tty.h` predicted -
      recorded here rather than silently ticked, because "M85 was
      finished by M87" is the useful fact and a tick is not
- [x] `<termios.h>` and the four `ioctl`s that matter
      (`TCGETS`/`TCSETS`, `TIOCGWINSZ`, `TIOCSPGRP`), which is the first
      `ioctl` on this machine and should be the narrow one it looks like
      rather than a general escape hatch
- [x] Sessions and controlling terminals: `setsid`, `getsid`,
      `tcgetpgrp`/`tcsetpgrp`. Process groups already exist —
      `SYS_getpgid` has been there since syscall 10 — and have never had
      the thing that makes them useful
- [x] Job control signals, which are simply absent from
      `system_api/include/signal.h` today: `SIGSTOP`, `SIGCONT`,
      `SIGTSTP`, `SIGTTIN`, `SIGTTOU`. `SIGSTOP` joins `SIGKILL` as the
      second uncatchable one, and the scheduler grows a STOPPED state
      distinct from the blocked-on-something states it has
- [~] `^C` raises `SIGINT` on the **foreground process group** and not
      on the shell; `^Z` raises `SIGTSTP`; a background process reading
      the terminal gets `SIGTTIN` rather than stealing the user's
      keystrokes
- [ ] **Not started.** `gui_terminal` becomes the master side of a pty rather than a
      program that owns a pipe, so that everything above is true of the
      terminal a person actually types into and not only of the serial
      console

**How we'll know.** In `qemu-input-test.sh`, with real keys, because
M40's entire lesson is that this is the layer where the serial harness
and the truth diverge: `^C` kills a running pipeline and leaves the
shell's prompt alive; `^Z` suspends it and `fg` brings back the same
process rather than a new one; and a backgrounded program that reads
stdin stops instead of consuming the next thing typed at the prompt.

#### Progress notes

**Two of the six bullets did not land, and the ordering of the arc is
why.** M85's first bullet asks for `/dev/tty`. A device file needs a
vnode layer with a mount table, and that is M87 - so the arc as written
put this milestone two ahead of the thing its first line depends on.
Nothing about the discipline changes when the path arrives; what exists
is a terminal reachable through syscalls rather than through a name, and
`fd 0/1/2` are it, which is exactly the approximation `isatty` has been
making since M77. The last bullet - `gui_terminal` becoming the master
side of a pty - is not started, and with it the "How we'll know" that
asked for `^C` to be graded through real keys in the input harness. That
is the honest state: the mechanism is built and tested, and the hardware
it is attached to is still a placeholder.

*The stopped state had to be a state.* A task suspended by `^Z` is
waiting for a decision, not for an event, and every one of the fourteen
`sched_wake_all` calls in this kernel would otherwise have had to
remember not to wake it. As a state, `pick_next`'s existing "is it
TASK_READY" test excludes it for free and always will. `sched_wake_task`
deliberately still only moves `TASK_BLOCKED`; resuming is a decision and
`sched_resume_stopped` is the one function that makes it. The self-test
checks exactly that: it calls `sched_wake_task` on the stopped process
and requires it to stay stopped.

*Two signals that cannot be argued with, for two different reasons.*
`SIGKILL` was already uncatchable by definition. `SIGSTOP` joins it
because a program able to catch or block it could make itself
unsuspendable, which is precisely the thing a terminal must be able to
do. `SIGTSTP` is the catchable one - it is what `^Z` sends, and an editor
with unsaved work is entitled to hear about it first. And `SIGCONT` acts
before any disposition is consulted, because a process that has ignored
it still has to be resumable: otherwise a program could make itself
permanently unstoppable in the other direction, suspended with no way
back.

*A stopped process must still be killable, which is easy to miss.*
Nothing schedules a `TASK_STOPPED` task, so a `SIGKILL` sent to one would
be a kill that never happens - the one thing `SIGKILL` is not allowed to
be. `sched_raise_signal` resumes it first.

*The default-action table was one row and is now four.* M76's comment
said "this kernel has one signal whose default is to be ignored, and a
table of one row is a table nobody reads", which was exactly right at the
time. Job control adds stop and continue, and the table now lives in
`system_api/include/signal.h` where the kernel and a program read the
same answer.

*A test that would have passed for the wrong reason.* The check that a
background job is refused its terminal used the session id `jobtest` left
behind, which was zero - and `tty_may_read` allows any process to read a
terminal with no session, so the assertion would have passed against a
function that never refuses anything. It now fabricates a session and
checks both directions: the background job refused, the foreground job
served.

*And one where the discipline was right and the test was wrong.* The
first version fed `"abX"`, two erases, `"c"`, Enter, and asserted
`"abc\n"`. Two erases take the `X` and the `b`; the correct answer is
`"ac\n"`, and the terminal said so. One erase.

*Verified, and precisely what.* The serial harness reaches `[m85] ... -
self-test passed (40 ms)` with no panic, alongside `[m84] a program that
replaces itself`. What that covers is the line discipline and the
terminal's ownership rules; what it does not cover is stop, continue and
`^C`, whose self-test is the one that had to be removed. The 40 ms is
worth noting on its own: everything this milestone tests is a function
call over a buffer, which is what makes it cheap and also what makes the
untested half untested - job control is the part that needs a process,
and a process is what the removed test could not drive safely.

### M86 — A shell that is a shell ✅

*Was scheduled second of the five before M94 — see "Before M94" below. Two
corrections to the bullets, from reading `sh.c` rather than this entry:
`&&`, `||` and `$?` shipped in M72 and the third bullet should not claim
them, and `jobs`/`fg`/`bg` are dropped from this milestone — they need
M85's pty, which this arc deliberately holds until M98. What that costs
is stated in the notes at the end of this entry rather than left as a
quiet omission.*

- [x] `sh.c`'s own header is the specification. It says what is missing:
      *"job control, `&`, subshells, functions, and `|`"* — the last one
      meaning more than a single pipe. **Four of the five: `&`,
      subshells, functions and pipelines of any length. Job control is
      the one deliberately left, and the re-scope note above says why**
- [x] Two variable namespaces, and `export` that moves a name between
      them. M75 wrote: *"`export` is accepted and does nothing but
      assign, because this shell has one namespace — the reason POSIX
      has two is subshells and functions, and it has neither."* This is
      the milestone where it has both, and that sentence stops being a
      justification and becomes a to-do
- [x] `if`/`while`/`for`/`case`, `&&`/`||`, command substitution,
      parameter expansion with the `${x:-y}` family, and `$?`/`$#`/`$@`.
      **Also `until`, `!`, `${#x}`, the `${x#pat}`/`${x%pat}` family,
      heredocs including `<<-`, `2>&1`, and `test`/`[` as a builtin —
      each one added because a real script cannot be run without it,
      not for completeness**
- [~] `jobs`, `fg`, `bg`, and the builtins job control implies, over
      M85's `tcsetpgrp` — **dropped from this milestone on purpose; the
      pty stays M98's, and `&` plus `wait` shipped without them**
- [x] Deliberately **not** a `bash`, and deliberately not an arithmetic
      expansion engine or `[[`. The measure is a `configure` script that
      someone else wrote running to completion, not a feature list —
      **and that measure could not be taken here, for a reason that is
      about M89 rather than about this shell. See the notes**

**How we'll know.** A shell script nobody here wrote runs correctly —
the honest candidate is a real `configure`, because it is the densest
per-line user of exactly the five things the header admits are missing.
A script written here to exercise the new features proves that they were
implemented, which is a much weaker claim than that they were
implemented *right*.

#### Progress notes

**It had to be a rewrite, and the reason fits in one line.** M72's shell
read a line, scanned it for `;`, `&&` and `||`, and expanded variables
while it tokenised. That is a good design for what it was and it cannot
grow any of this milestone's features, because every one of them is a
*nested* structure and a line is not: `if` spans lines, `|` spans
commands, `$(...)` contains a whole program, and a `case` pattern must
reach the matcher **unexpanded** or it globs against the directory
before it is ever compared. So this is a lexer, a recursive-descent
parser and an evaluator over a tree — 1400 lines where there were 760 —
and expansion moved to execution time, where POSIX puts it.

**The thing that mattered most was not a feature.** It is that every
system call the new shell makes is POSIX — open, read, write, close,
dup2, pipe, fork, execve, waitpid, chdir, getcwd, opendir, readdir — and
there is not one `sys_` wrapper left in the file. That is not tidiness.
It means `user_space/shell/sh.c` **compiles for the host**, and the host
has a reference implementation of the thing being tested sitting in
`/bin/sh`. `tools/sh-test.sh` builds the same source the machine runs and
puts every fixture in `tests/sh/` through both, and requires the output
and the exit status to be **byte-identical**.

Nothing in `tests/sh` says what the right answer is. That is the whole
value: for a program whose entire job is to agree with every other shell
about what a script means, a test that checks strings this project chose
itself is close to worthless. Eight fixtures, 130 lines of script,
agreeing with `/bin/sh` exactly — and the ability to write that test at
all is a dividend of M75–M85 rather than of this milestone. M72's shell
could not have been tested this way because it could not have been
compiled anywhere else.

**Four bugs the host fixtures caught in seconds**, each of which would
have cost a three-minute boot to find and some of which would not have
been found at all:

- `x='a b'` was not an assignment. The parser required the whole word to
  be unquoted, when what must be unquoted is the *name*. Every
  assignment of a quoted value became a command.
- `<<'EOF'` ran to the end of the file. The delimiter kept its quotes, so
  it never matched the line that ended it, and the rest of the script
  became the heredoc's body — a failure that reads like the parser
  losing its place rather than like a string compare.
- `shift` was a use-after-free: it passed `pos_params + n` to a function
  whose first act is to free `pos_params`.
- a pipeline reported the *first* stage's status rather than the last.
  This one was found deliberately, by breaking it on purpose to check
  that the fixtures could fail at all — the Q1–Q10 ritual — and it is
  the reason that ritual exists: the diff was one line and no boot-time
  marker would have looked at it.

**And two that only the machine could show, one of them this project's
own bug taught twice.**

The first: a script's first redirect worked and every one after it went
to the console. That is *verbatim* the symptom M72 recorded and fixed,
and its note explains it exactly — the parking slot must be claimed
**before** the target is opened, because `open` returns the lowest free
descriptor and after one redirect cycle that is the parking slot itself.
This rewrite opened first and re-broke it. The instrumentation that
found it printed `RD target=20 saved=20`, which is that sentence in
numbers.

The second is new and is the more interesting half. The fixed parking
number M72 used was safe for M72 and is not safe in general: **a shell on
this machine does not start with two descriptors.** It is spawned by
`init` and inherits a table with roughly forty entries in it — the first
`open` in a script returned fd 42 — so a constant parking slot is
eventually one of *those*, and `dup2` does not ask before it releases
what is there. The slot is now probed with `fcntl(F_GETFD)`, which is the
one call available here that can tell an open descriptor from a free one.

**A third thing the machine showed, which was not a bug in this shell at
all.** The instrumentation printed nothing the first time, because it
wrote to fd 2 — and a task on this machine starts with fd 0 and fd 1 and
nothing else. Every error `/bin/sh` has ever written, including
`command not found`, has gone nowhere since M13. The shell now makes fd 2
a copy of fd 1 when it starts and finds none, which is what a login shell
inherits everywhere else; `2>file` still redirects it afterwards, because
it is now a real descriptor to redirect.

**`_exit` was added to the libc**, by M63's rule: the program asked for
it by name. A forked child that has decided not to exec must leave
without running what the parent registered on the way out. Today `exit`
is a syscall and no more, so the two are the same call — and the header
says so rather than hiding it, because the distinction becomes real the
day this libc grows `atexit` or a buffered stdio that flushes, and a
shell that had spelled it `exit` would then flush its parent's buffers
once per forked command.

**What this milestone could not grade, and why that is M89's problem
rather than a shortfall here.** The bullet above says the measure is *"a
`configure` script that someone else wrote running to completion"*, and
that could not be run. Not because of the shell: a configure script
shells out to `sed`, `grep`, `rm`, `mkdir`, `expr` and `install` in its
first hundred lines, and `/bin` on this machine holds six programs. The
ordering note in "Before M94" put M89 after M86 on the grounds that
toybox is static and needs no loader; what it did not notice is that
M86's own grading instrument is downstream of M89. **The measure is not
abandoned, it is relocated:** running a real `configure` is now part of
M89's test, where the utilities it needs will exist, and M94's own "How
we'll know" already requires the same thing one step later. What stands
in for it here is stronger than a feature list and weaker than a real
script: byte-for-byte agreement with a reference shell over every
construct the milestone claims.

*Verified.* `tools/sh-test.sh` 8/8 byte-identical to `/bin/sh`, including
exit status, and each fixture checked against a deliberately broken build
before being believed. `qemu-serial-test.sh` **81/81** with the new
`[m86]` marker, which runs a fixture using nothing the host could have
exercised: a pipeline from a forked builtin into an exec'd `/bin/cat`, a
subshell whose assignment does not escape it, a here-document fed by a
second process, and an exported variable reaching a child's environment
through `env` while an unexported one does not. `qemu-input-test.sh` 9/9
on the quick subset, `make test-fast` 123/123.

#### What dropping job control costs, and why it is affordable here

`jobs`/`fg`/`bg` need a controlling terminal that can hand the foreground
process group around, which is M85's pty and `tcsetpgrp` — the one bullet
M85 has left, and the one this arc deliberately holds until M98 because
that is the first milestone with a build long enough to want `^C`.
Building the pty here to satisfy a bullet would be doing M98's work early
so that M86 could look complete.

What it costs is exactly one thing: a `configure` run cannot be
suspended and resumed from this shell. It can still be interrupted, and
it can still be run from `gui_terminal`, so the grading test is
unaffected — which is the check that made this affordable rather than
merely convenient. If a configure script turns out to need a foreground
process group for something other than interactivity, the pty comes
forward to here and this note is the record of the bet that it would not.

### M87 — Files with a type, a place, and more than one name [~] five of seven; the `*at()` family is now M89's

*The two unfinished bullets were absorbed into M93 and did not land
there. They move to M89 — see "Before M94" below — on the grounds that a
ported userland asks for `openat` by name, which is a better reason to
build it than a format rewrite happening to be open at the time.*

- [x] `kernel/fs/vfs.h` calls itself *"the seam a second filesystem type
      would plug into if this project ever needed one"* and says the
      pass-through is honest *"right now."* This milestone is the day it
      needs one: a vnode layer and a real mount table, replacing
      path-string dispatch
- [x] `devfs` at `/dev`: `null`, `zero`, `full`, `random`/`urandom`,
      `tty`, `console`, `fd/*`. This is not a nicety — it is the single
      most common thing a ported program touches that this machine
      cannot answer, and every one of them is a few lines behind a vnode
      layer that exists
- [x] `procfs` at `/proc`: `self`, `self/exe`, `N/status`, `N/cmdline`,
      `uptime`, `meminfo`. `SYS_taskinfo`'s whole-shot snapshot stays
      for `task_manager`, which is a lean_os program and should keep
      using the lean_os interface; `/proc` is for programs that have
      never heard of it
- [~] **Symbolic links shipped; hard links did not.** Symbolic links and hard links in leanfs v2, and `O_NOFOLLOW`,
      `readlink`, `symlink`, `link`. M75 wrote down exactly where this
      lands: *"With no symbolic links on this machine, `/a/b/..` and
      `/a` name the same directory by construction... The day this
      filesystem grows links is the day that stops being true, and the
      comment above `path_normalize` is where it stops."* That comment
      is this bullet's specification, written a year early
- [x] Loop detection with a hop limit, because the first symlink is also
      the first way to hang the kernel's path walker in a way no input
      before it could
- [~] `O_EXCL` and `ftruncate` **shipped**; the `*at()` family and `fsync` did not. Originally: the `*at()` family (`openat`, `fstatat`, `unlinkat`, `renameat`),
      a real `O_EXCL` — `<fcntl.h>` currently defines it as 0 with a
      comment saying a program relying on it *"gets no protection"* —
      plus `ftruncate` (declined in `<unistd.h>` pending a caller;
      `leanfs_handle_truncate` has been sitting there the whole time)
      and `fsync`
- [x] Deliberately **not** a buffer cache or a page cache. M82 makes one
      possible and no measurement has asked for one; M69's rule

**How we'll know.** A program that has never heard of this OS opens
`/dev/null` and writes to it, reads 64 bytes from `/dev/urandom` and
gets 64 different-looking bytes, and finds itself at `/proc/self/exe`. A
symlink loop returns `ELOOP` rather than hanging the machine. And `cp
-r` over a tree containing a symlink and a hard link reproduces both as
what they were, rather than as two copies.

#### Progress notes

**Three of the seven bullets landed. The header says `[~]` for that
reason and the rest of this note says which.** What is here is the mount
seam, `/dev`, `/proc`, `O_EXCL` and `ftruncate`. What is not is symbolic
and hard links, the `*at()` family, and `fsync` - and the largest of
those, links, is the one M75 predicted would matter.

*The seam M53 declined to build was right to decline, and is right to
build now.* `vfs.h` has said since M53 that a vtable would be "pure
speculative generality for a single-filesystem kernel", and that was
exactly true for thirty-four milestones. It stopped being true the moment
`/dev/null` had to exist: that is not a file with no bytes in it, it is a
rule - reads end immediately, writes are accepted and discarded - and
leanfs cannot express it without learning to lie about what a file is. A
dispatch table with one implementation is speculation; with three it is
the only way to avoid special-casing two filesystems inside a third.

Deliberately still keyed by path, and deliberately not a reference-counted
vnode cache. The mount table is three entries searched by prefix, which
is faster than anything cleverer and easier to be sure of.

*Prefix matching only at a component boundary, and there is a test for
it.* `/devices` starts with `/dev` and belongs to the root filesystem. A
matcher that missed that would silently shadow every path sharing a
prefix with a mount, which shows up as one program mysteriously failing
and nothing else. The self-test creates `/devices`, reads it back, and
would catch it.

*A mount point has to appear in its parent's listing*, or `ls /` says
this machine has no `/dev`. The cookies for those synthetic entries are
numbered from `0x40000000` upward - far past any byte offset a real
directory could produce, since leanfs's are offsets into a file capped at
8 MiB - so "have I finished the real entries" is a comparison rather
than a flag the caller has to carry.

*A handle has to say which filesystem it belongs to*, because the fd
table stores one and comes back much later asking to read it. The mount
index goes in the top byte, which leaves leanfs's handles numerically
unchanged - that is the point, not a coincidence: an inode index has been
the value of a handle since M59, it is in `openfile_t` and in every fd
slot, and a scheme that renumbered them would have changed something
already written into a saved session.

*`/proc` files are generated at open, not per read.* A program that reads
`/proc/uptime` in two calls must not see two different uptimes and a byte
offset that means nothing between them. The self-test reads it twice a
second apart at the *file* level and requires the answers to differ,
which checks the other half - that it is generated at all rather than
cached once.

*`/proc/self/exe` is a real answer that will stop being one.* Every
program on this machine lives in `/bin` and a task's name is its
filename, so composing them is true today. The day something runs from
somewhere else it is a guess, and the comment where it is composed says
so.

*`/dev/random` and `/dev/urandom` are the same device and neither is
cryptographic.* There is no entropy pool, so a blocking variant would be
a lie of a different shape. It is provided anyway because the
overwhelming majority of reads from it anywhere are a hash seed or a
temporary filename, and because the alternative is that the path does not
exist and a program fails at startup rather than at the one operation
that needed real entropy. Said in `devfs.c` rather than discovered.

*`O_EXCL` is real, and the atomicity comes from a lock that was already
there.* `<fcntl.h>` defined it as 0 for twenty-eight milestones with a
note that "a program that relies on O_EXCL to avoid a race gets no
protection". The existence check and the creation now happen inside one
critical section of `fs_lock`, so of two processes that both ask, exactly
one gets the file. That is what makes a lock file a lock.

*`ftruncate` does more than the function it exposes.* `<unistd.h>`
declined to declare it for ten milestones - "a declaration with no
implementation would be worse than its absence: a program that probes for
it at configure time would find it and then fail to link" - while
`leanfs_handle_truncate` sat there truncating only to zero. Growing a
file now only changes its size, because an unallocated block already
reads as zeros: reserved, not allocated, which is what the call promises.

*Symbolic links needed no format change at all, and that is M81 paying
back a debt it did not know it was owed.* A link's target lives in its
data blocks exactly the way a regular file's contents do, with `size` as
the target's length - so the only new thing on disk is a type value, and
an old disk has no inode carrying it. The magic did not have to move.
M81 padded the inode to 128 bytes and reserved 44 of them specifically
for "M87's link count and symlink target"; the symlink half turned out
not to need a single one of them.

*The walk substitutes and restarts, which is what makes an absolute
target work.* When a component resolves to a link, the target replaces
that component and the walk begins again on the rewritten path - because
`/a/b` where `b` points at `/c` has to end up at `/c` and not at `/a/c`.
Restarting is also why the hop limit has to exist: a link that points at
itself would otherwise rewrite forever, and a resolver without a limit
hangs the machine on the smallest possible loop rather than failing. The
self-test builds that loop out of two links pointing at each other and
requires it to resolve to nothing.

*`readlink` and `lstat` do not follow, and everything else does.* That
distinction is the entire reason a program can tell a link from what it
points at, and the reason a tree walker does not descend through one into
a directory above itself. It cost two resolvers rather than one -
`resolve` and `resolve_nofollow`, both over the same walk with one flag.

*Removing a link removes the link.* That fell out of `dir_lookup` naming
the entry in a directory rather than resolving it, which is what `unlink`
has always done; the only change needed was to stop refusing type 3.
Getting it the other way round would make `rm` on a link delete somebody
else's file, which is why the self-test checks the file survives.

*Two claims this made false, both corrected where they stood.*
`<sys/stat.h>`'s `S_ISLNK` was `0` with "no symbolic links here, and
saying so beats a bit that is never set". And libc's `lstat` was an alias
for `stat`, defended - correctly, at the time - on the grounds that "there
are no symbolic links on this filesystem, so there is nothing for lstat
to decline to follow" and that an alias beat a stub returning an error.
Both arguments were right for ten milestones and stopped being right in
this one.

*What symbolic links did NOT fix, and it is the half M75 actually
predicted.* M75's note says resolving `..` textually is safe "with no
symbolic links on this machine" and that "the day this filesystem grows
links is the day that stops being true". That day is here and `..` is
still resolved textually in `copy_path_from_user`, before leanfs sees the
path - so `/a/b/..` where `b` is a link to `/c/d` gives `/a` where a
physical resolution would give `/c`. Doing it properly is a bigger change than "stop
stripping them", and working out why is worth recording.

The walk itself could do it - it has the chain of inodes it came through,
so `..` is a pop. The problem is above it. `vfs_resolve_mount` matches a
path against the mount table BEFORE leanfs sees it, so a path that still
contains `..` breaks the match: `/dev/../tmp/x` matches the `/dev` mount
and hands devfs a relative path it cannot answer, while `/tmp/../dev/null`
never reaches devfs at all. Textual normalisation is what currently makes
mount matching work, and physical resolution is what makes symlinks work,
and the two cannot both be done in the layer that currently does either.

The real fix is for the VFS to own path resolution - walking components
itself, consulting the mount table at each one, and handing whichever
filesystem owns the result a path it can resolve. That is the vnode layer
this milestone explicitly declined to build, and it is now the thing that
would justify building it. Recorded here as a known divergence with a
named cause rather than as a small thing left undone. Shells resolve `..`
logically too, so this is the behaviour most people see - but the
kernel's answer should be the physical one.

*And the clock bug came back, which means the first fix was wrong about
why.* M85 made `rtc_read` extrapolate from the last good sample instead
of reporting no time, and the M63 Whetstone failure recurred anyway. The
extrapolation was built on the PIT tick count - and a host too busy to
let this guest read the CMOS twice in a row is also too busy to deliver
its timer interrupts on time, and QEMU coalesces the ones it misses. A
clock built on lost ticks runs slow by exactly the amount the load is
bad, which is exactly when it is being asked. It extrapolates from the
TSC now, which counts cycles the CPU actually executed and which nothing
coalesces.

### M88 — Everything else a ported program calls [~] poll and the identity/limits calls; the rest is open

*Reopened third of the five before M94 — see "Before M94" below. The
second attempt is UTF-8 and the `getrlimit`/`getrusage`/`times`/
`statvfs`/`utime` group only. `O_NONBLOCK` and `AF_UNIX` stay absorbed
into M100, where the arc put them and for the reason it gave.*

- [~] `poll()` **shipped**; `select()` did not. Built over `SYS_waitfds` (syscall 68),
      which is genuinely the same idea under a lean_os name — so this is
      a header and a shim, not a kernel feature, and should be the
      cheapest bullet in the arc
- [ ] `O_NONBLOCK` that is a real bit. `<fcntl.h>` defines it as 0 today
      with *"every descriptor here is what it is"* next to it, and
      `fcntl` refuses to set flags specifically because it will not
      accept one it cannot honour. Pipes, sockets and M85's tty each
      grow a non-blocking path, and `fcntl`'s `F_SETFL` starts saying
      yes
- [ ] `AF_UNIX` sockets and `socketpair`, which is what a ported program
      reaches for where this project has always used a named pipe, plus
      the BSD spelling (`<sys/socket.h>`, `<netdb.h>`,
      `getaddrinfo`/`gethostbyname`) over the stack M27, M64 and M66 already
      built. The stack is real; only the names are missing
- [~] `sysconf` and the identity calls **shipped**; `getrlimit`/`setrlimit`, `getrusage`, `times`,
      `statvfs`, `utime`/`utimensat`, `getuid`/`geteuid`/`getpwuid`.
      The identity calls return 0 and a single `root` entry, and that is
      **not** the fiction M65 refused to write: a machine with exactly
      one principal that reports one principal is telling the truth. The
      lie M65 declined was a *permission model* that pretended to
      enforce something, and nothing here enforces anything
- [ ] UTF-8 in the C library: `mbrtowc`, `wcrtomb`, a real
      `<wchar.h>` instead of the Latin-1 one that says at the top that
      it is wrong above U+00FF, and `nl_langinfo(CODESET)` finally
      allowed to say `UTF-8` truthfully
- [ ] Deliberately **not** the glyphs. The font is one byte per glyph
      (M39/M57) and a font covering more than Latin-1 is a font project,
      not a libc one. The split is exact and worth stating: after this
      milestone the *encoding* is correct end to end and text round-trips
      through the system unmangled; text above U+00FF still draws as a
      replacement box. That is a rendering limitation a program can be
      told about, which is categorically better than an encoding
      limitation that corrupts its data

**How we'll know.** `nettest` and `httpd` rewritten against the BSD
names and the plain `<sys/socket.h>` spelling, still passing the same
serial assertions they pass now — the point being that they are now
programs a Unix programmer could have written without reading anything
in `system_api/`. And a UTF-8 string written to a file, read back,
`wcrtomb`'d and compared byte for byte, which is the round trip that
catches an encoding that is merely plausible.

#### Progress notes

*`poll` is the unusual case in this project: a header and a shim rather
than something built.* `SYS_waitfds` (M68) already was poll, spelled
differently - it blocks until one of a set of descriptors would not
block, and a task inside it is `TASK_BLOCKED` rather than spinning. What
was missing was the name and the shape of the answer.

Two honest limits, both in the header rather than discovered. `POLLOUT`
is reported ready for any open descriptor, because there is no
write-readiness anywhere in this kernel - a pipe write blocks when the
pipe is full and nothing can be asked in advance whether it would. Saying
"ready" is what a caller then acts on anyway, and it beats never
reporting `POLLOUT`, which would make a program waiting for it wait
forever. And `POLLPRI`/`POLLRDHUP` are not defined at all: there is no
out-of-band data here, and a constant a program could test but never see
set is the failure mode `<fcntl.h>` spent a header comment avoiding.

*The implementation is two passes, and the reason is in `SYS_waitfds`'s
own note.* It returns ONE ready index rather than a bitmask, deliberately
- "a mask would be an API that promises a fairness this scheduler does
not implement". `poll` has to report every ready descriptor. So the first
pass blocks, which is the part that must not spin, and the second asks
each descriptor with a zero timeout, which the same call documents as a
poll that returns immediately. That is n+1 syscalls to build a mask out
of a call that returns an index, and it is nothing next to the block it
just came out of.

*One kernel change fell out of it, and it broke a test that was right to
break.* `SYS_waitfds` refused a count of zero. POSIX says an empty poll
set with a timeout is a sleep, and this libc had no sleep primitive at
all - programs busy-yield. A wait with nothing that could satisfy it
early IS a sleep, so zero is now allowed and `poll(NULL, 0, ms)` is a
real one; a negative timeout with no descriptors is `pause()`, which is
also correct.

`racetest` asserted the old refusal in as many words - "a count of zero
is an argument error, not an indefinite sleep on nothing" - and failed.
That is the third time in this arc a test has failed because a milestone
deliberately changed what it encoded (`libctest`'s `fcntl` in M84,
`lstat`'s alias in M87, this). The fix is the same shape every time:
assert the NEW contract rather than delete the check. `racetest` now
verifies that a zero count sleeps *and actually waits* - the half that
would still be wrong if it returned immediately - and separately that an
over-long count is still refused, because the refusal moved rather than
went away.

The mistake underneath it is worth naming: widening a syscall's accepted
input without grepping for anything asserting the old refusal. The kernel
comment explaining why zero is now legal does not find the test that
disagrees; `grep -rn waitfds user_space/` would have, in two seconds,
before a nine-minute cycle said so instead.

*`getuid` and friends return 0, and that is the truth rather than a
stub.* M65 argued at length that there are no users here and refused to
invent one. A machine with exactly one principal that reports one
principal invents nothing; what M65 declined was a *permission model*
that pretended to enforce something, and nothing here enforces anything -
`access()` still says so in its own comment and `chmod` is still a
truthful failure. Real and effective ids are equal because there is no
setuid for them to differ about, and a program comparing them is asking
"am I running with borrowed authority", whose honest answer here is no.

*`sysconf` answers three things and refuses the rest.* Page size, open
file limit and tick rate are facts this machine can state.
`_SC_NPROCESSORS_ONLN` is -1 because it is not answerable from user
space without inferring a number from an error, and `_SC_PHYS_PAGES` is
-1 because the kernel knows it but no syscall reports it and reading
`/proc` from inside libc would make `sysconf` depend on a filesystem
being mounted. -1 means "no limit is defined", which is a far better
answer to a configure script than a plausible number it would build
against.

*And a diagnostic that should have existed three milestones ago.* The M63
Whetstone failure - "Insufficient duration - Increase the LOOP count" -
survived two clock fixes on two different theories, because the benchmark
prints the same line whether the clock read zero twice or read the same
number twice. Those are different bugs. `libctest` now asserts the clock
directly and the failure names itself: a non-positive reading means it is
not answering, two equal readings across a 1.5 s sleep mean it is
answering and not advancing. Both fixes are kept - each is right on its
own terms - but neither was ever *verified*, because a quiet run passed
before them too, and "the symptom did not reproduce" was treated as "the
cause is gone" for a symptom already known to be load-dependent. The
check is the first user of the `poll(NULL, 0, ms)` sleep above.

### M89 — Somebody else's userland

*Scheduled fourth of the five before M94 — see "Before M94" below, which
also hands this milestone M87's unlanded `*at()` family. This is where
that surface gets asked for by a program instead of by a checklist.*

- [ ] Port **toybox** (or busybox) — M63's rule at the largest scale it
      goes: its build's own errors are the specification, and no patch
      to its source. One static binary, multi-call, a hundred and fifty
      or so utilities
- [ ] `/bin` stops being six programs. Today it is `ls`, `cat`, `cp`,
      `echo`, `env` and `sh`; there is no `rm`, no `mv`, no `mkdir`, no
      `ps`, no `grep`, no `sed`, no `find`, no `sort`, no `wc`, no
      `test`, no `tar`, no `xargs` — and every one of those is a program
      a script someone else wrote assumes without thinking about it
- [ ] The lean_os programs that overlap stay, and this is a real
      decision rather than sentiment: `ls` here knows about leanfs's own
      shape and `caps` has no toybox equivalent. Where toybox and this
      tree both provide a name, the one in `/bin` is the ported one and
      the lean_os one keeps its own name, because a script does not care
      and a person might
- [ ] This milestone is also the arc's own test. Toybox is the densest
      single consumer of the POSIX surface that exists in one build:
      whatever M81–M88 got wrong, its build breaks on, and it breaks
      with a compiler error naming the thing rather than with a desktop
      that feels slightly off

**How we'll know.** `find . -type f | xargs grep -l something | sort |
uniq -c | sort -rn` — five stages, every one of them a program nobody
here wrote, connected by M86's pipes, forked and exec'd by M83 and M84,
over a tree that could not have existed before M81, interruptible with
`^C` by M85. One command line that is false if any milestone in the arc
is incomplete.

### Where this leaves M80, and the deferral M89 is likely to collect

**M80 becomes attemptable again after M84**, not after M89.
`posixmodule.c`'s list — `fork`, `execv`, `waitpid`, `pipe2`, `select`,
`poll`, `sysconf`, `getuid`, `chmod`, `symlink`, `readlink`, `utime`,
`statvfs`, `sched_yield` — is M83, M84, M87 and M88 almost exactly, with
`chmod` the one entry that stays a truthful failure. Its two remaining
blockers that this arc does not address are both build-side rather than
OS-side: a host CPython to run `_freeze_module`, and the decision about
where the standard library lives — which M81 turns from a hard
constraint into an ordinary choice, since a filesystem holding thousands
of files can simply hold the `.py` files.

**And the dynamic-linking deferral names its own collection date.** The
"Deliberately not next" note below says shared objects become worth
building *"once something concrete asks for it (a C-extension wheel; a
second and third static binary duplicating the same libc)."* After M89
there is a toybox and a Python and a `/bin` full of programs, every one
of them carrying its own copy of this libc into memory. That is the
second and third static binary, arriving exactly as predicted. It is
still not in this arc — the arc is nine milestones already and a loader
is its own — but the condition will have been met rather than argued
about, which is how this project has always preferred to decide.

## And the arc after that: a machine big enough to build on

The M81–M89 arc's target is one sentence — *software written for Unix by
someone who has never heard of lean_os builds here and runs here,
without patching its source* — and that sentence is about a **surface**:
the syscalls and library functions a program names. This arc keeps the
sentence and changes the axis, because the three programs now being
asked for — **Python, GCC and Chrome** — are, with one exception, not
blocked by a surface. They are blocked by numbers, and every one of the
numbers is already written down somewhere in this tree:

| the number | where it is written | what wants more |
|---|---|---|
| 1 GiB of physical memory tracked | `kernel/mm/pmm.h`: *"Extending past 1 GiB is future work for whoever needs more than that much physical memory tracked"* | this arc is whoever |
| 128 MiB of actual RAM | `tools/run-qemu.sh` passes no `-m` at all, so the machine has been QEMU's default for ninety milestones and no file in this tree says so out loud | one `cc1plus` translation unit |
| 256 MiB heap, 160 MiB mmap arena, both fixed | `kernel/proc/proc.h` | a GCC garbage-collected heap; V8 |
| 8 MiB per file, 32 MiB of data region | `kernel/fs/leanfs.h` | GCC's source tree is ~25× the second number unpacked, and its bootstrap tree is hundreds of times |
| `MAX_TASKS 128`, `MAX_FDS 128` | `kernel/sched/sched.h` | `make -j` and a linker |
| a sector at a time through a CPU register, no cache | `kernel/drivers/ata.c`; M87 declined a buffer cache because *"no measurement has asked for one"* | this arc is the measurement |
| one program, one fixed load address, statically linked | `user_space/lib/user.ld`, and M80's *"nothing on this machine has ever loaded code it did not statically link"* | `libstdc++.so`, `dlopen`, and every browser ever shipped |

The one exception — the thing that genuinely *is* a missing surface and
not a small number — is **C++**. There is not one line of it in this
tree, there is no unwinder, and GCC has been written in C++ since 4.8
while Chrome is C++20. That is M97 and it is the hardest milestone here.

### What this arc assumes from the last one, and what it absorbs

M81–M89 is not finished, and this arc does not pretend the leftovers are
optional. Sorted by whether this arc needs them:

- **M86 is a hard prerequisite, not a leftover.** `./configure` is a
  shell script — it is the densest per-line user of exactly the five
  things `sh.c`'s header admits are missing, which is why M86's own
  "How we'll know" already names it. Nothing from M94 onward is
  attemptable through a shell without `if`, `&&`, command substitution
  and a real `|`. If one milestone from the previous arc gets finished
  before this one starts, it is that one.
- **M87's `*at()` family, `fsync` and hard links** are absorbed into
  M93, because M93 rewrites the on-disk format anyway and doing that
  twice would be the mistake M81 already recorded.
- **M88's `O_NONBLOCK` and `AF_UNIX`** are absorbed into M100, where a
  browser's process model asks for both at once and for the first real
  reason.
- **M88's UTF-8** is not absorbed and should be finished on its own:
  every program in this arc has non-ASCII bytes in its source tree and
  in its test suite, and an encoding that is *merely plausible* corrupts
  data quietly, which is the failure mode the milestone's own last
  bullet spent a paragraph distinguishing from a rendering limit.
- **M85's pty** is needed by M98 and not before: a build that runs for
  an hour is a build somebody wants to `^C`, and `gui_terminal` owning a
  pipe means `^C` reaches the shell instead of the compiler.

### Three decisions taken here rather than discovered halfway

**Chrome is a program, not a milestone, and this arc does not contain
it.** The "Deliberately not next" note has called a browser a decade of
work twice and nothing below reverses that. What changes is that the
distance stops being an argument and becomes a measurement: M100 builds
the dependency stack a browser actually links against and writes down,
in numbers, what broke. Chromium's own published requirements are on the
order of 100 GB of disk and 16 GB of RAM to build, ~35 million lines of
C++, a Linux syscall surface most of which this kernel has never heard
of, and a GPU path this project has already refused on good grounds.
A milestone that says "port Chrome" would be the first unfalsifiable
item in this file. A milestone that says "build every library Chrome
needs and report the gap" is the same information, honestly priced.

**The libc stays this project's own, through M99 at least, and the
condition for changing that is named now.** The M63 rule — let the
program's own link errors be the specification — has worked twice, and
M80's notes are explicit that it produced a *better* libc than a
standard would have, because every header had to answer a real question
honestly rather than plausibly. It also cost several hundred lines for
one interpreter. GCC will cost more and Chrome will cost far more. The
rule does not break on volume; it breaks on **symbol versioning** — the
day a program's build needs `GLIBC_2.17`-style versioned symbols, or
`__libc_start_main`'s exact glibc contract, the surface stops being
learnable from an error message and starts being a specific
implementation's ABI. That day, and not before, is when adopting a
third-party libc becomes an honest choice rather than a shortcut. It is
not expected before M100.

**There is still no NX bit, and M91 is where that stops being free.**
`SYS_mmap`'s comment says it plainly: *"there is no NX bit here, so
`PROT_EXEC` is neither granted nor withheld and `PROT_NONE` is refused
rather than pretended."* Every page in every process is executable
today. A JIT would love that and everything else about it is a bug — and
the moment `mprotect` exists, a kernel that ignores `PROT_EXEC` is
answering a question with a lie rather than with a refusal, which is the
one thing this project's headers have consistently declined to do.

**A note on sizing, in M69's spirit.** Every number this arc raises is
raised to something specific, and each one should be *measured* against
a real build before the next milestone locks it in. The vendors'
published requirements are the starting estimate, not the answer; the
answer is what this machine reports.

---

### M90 — More than a gigabyte ✅

- [x] `PMM_TRACKED_MEMORY` stops being a constant. The frame bitmap is
      sized from the e820 map at boot and allocated out of the memory it
      describes, instead of a fixed 1 GiB array — the recursion that
      makes this awkward is real and is the milestone
- [x] `vmm_init`'s 1 GiB identity map becomes a direct map of all
      physical memory, sized the same way, ~~using 1 GiB pages where the
      CPU reports them~~ — 2 MiB pages, and the progress notes say why
      the 1 GiB ones were declined. `pmm.h`'s constraint — *"a frame this
      allocator hands out has to be addressable before any virtual-memory
      mapping exists for it"* — is exactly right and is what makes the
      ordering here delicate rather than mechanical
- [x] `tools/run-qemu.sh` and both harnesses grow an explicit `-m`, and
      the number appears in the boot log. A machine whose memory size is
      "whatever QEMU defaults to" is a machine nobody has decided the
      size of
- [x] Deliberately **not** swap, still, and now for a better reason than
      M82's: with 8 GiB the question is whether a GCC bootstrap fits,
      and that is answerable by running one. M98 is where the answer
      arrives; if it is no, swap gets a milestone rather than a guess

**How we'll know.** Boot with `-m 8192` and have a program touch 6 GiB
through M78's mmap, with the PMM's high-water mark tracking it page for
page — and then boot the same kernel with `-m 128` and pass the entire
existing serial harness. The second half is the actual test: a kernel
that only works when the machine is big has traded one hardcoded size
for another.

#### Progress notes

**What shipped, and where the grading criterion was changed.** The
machine boots with 4 GiB rather than 8, and the kernel-side proof is a
frame allocated at physical `0x100000000` — written, read back through
the identity map, and freed with the free count returning exactly —
rather than a user program touching 6 GiB. The reason is the harness
rather than the kernel: `tools/qemu-input-test.sh` runs three guests
concurrently, and a test that *touches* 6 GiB touches 6 GiB of the host's
memory three times over. QEMU commits only what a guest writes, so a
4 GiB guest that touches a page costs a page — which is why the memory
size could be raised at all — and a test built to defeat that property is
a test that cannot run in CI.

What replaced it is not weaker in the part that matters. The claim was
never "a large number appears"; it was *tracked and addressable are
different properties*, which is what the old `pmm.h` comment was about.
`0x100000000` is a physical address that does not fit in 32 bits, and
writing to it through the identity map is exactly the thing a 1 GiB map
could not do. The self-test picks the strongest floor the machine
supports — above 4 GiB, else above 1 GiB, else half of what there is —
so it keeps saying something true on a small machine instead of being
skipped.

*The number that made this a milestone rather than an edit is 4096.*
`-m` was never passed to QEMU, by any of the three scripts, in
eighty-nine milestones. The machine has been 128 MiB the whole time and
no file in this repository said so. 4 GiB was chosen for one specific
reason and not for headroom: QEMU splits it across the PCI hole, so the
guest gets a RAM region at `0x100000000` and the allocator has to handle
a frame whose address needs more than 32 bits. A round 2 GiB would have
been entirely below the hole and would have tested nothing 1 GiB did not.

*The bootstrap knot, and how it is untied.* A bitmap over 8 GiB is
256 KiB and the refcount array M82 added is 2 MiB, and neither belongs in
`.bss`: `.bss` is sized at link time, for the largest machine anyone
might boot on, and is loaded (or zeroed) before there is an allocator to
ask. So both are placed at run time in physical memory found in the very
map they are about to describe. The ordering that makes that safe is to
do every read before any write — pass one measures how much RAM there is,
how big the metadata must be and where it fits; nothing is written until
a home is chosen. The home has to avoid three ranges, and all three are
*inside* memory the map calls usable, which is why they are listed rather
than inferred: low memory, the kernel image, and **the e820 buffer
itself**. That last one is the interesting one — the firmware allocates
it as `EfiLoaderData`, which this loader classifies as usable, so the
metadata's natural placement would have landed on top of the map while
still reading it.

*A latent bug found on the way, which had nothing to do with memory
size.* The loader called `AllocatePages` for the *sectors it reads off
disk* — and `.bss` is `NOBITS`, so it is not in `kernel.bin` and not in
that count. `__kernel_end` is at `0x4af650` and the file ends at
`0x31c000`: 2.6 MiB of the loaded image was outside any allocation the
firmware knew about, and `entry.asm` zeroes all of it before
`kernel_main` runs. Firmware was free to put a pool allocation there —
and the e820 handoff buffer is a pool allocation. The failure mode was
the memory map being erased by the kernel that was about to read it. It
never fired because the pool happened to land elsewhere. The Makefile now
computes the page count from `__kernel_end` in the ELF and the loader
reserves the whole loaded image; the boot log's own memory map shows the
result, with a usable range that now begins exactly at `0x4B0000`.

*The identity map's rule took two attempts, and the first one was wrong
in a way only the log showed.* "Map every range that is not MMIO" is the
right shape — ACPI tables live in firmware-reserved RAM and `acpi.c`
reads them where they lie, so mapping only the *usable* ranges would
have broken ACPI, while mapping MMIO would make `drivers/fb.c` and
`lapic.c` panic on their own device pages, which `vmm_map_page_in`
refuses to place inside an existing huge page. Three cases, which is why
`e820.h` now has three kinds of answer where it had two.

But it reported **16384 MiB identity-mapped on a 4 GiB machine**. OVMF
describes a 12 GiB `EfiReservedMemoryType` range at 1012 GiB — the
address-space window the PCIe hierarchy lives in, reserved so nothing
allocates over it, and emphatically not memory. "Reserved" answers *may
anything allocate here*; it does not answer *is there RAM here*. So the
map is now additionally bounded by the top of real memory, where "real"
means a range something could actually occupy: usable, or ACPI. The
second run reports 4096 MiB, which is the RAM exactly. Worth recording
that the only thing that caught this was printing the number — the boot
worked fine either way, and a 12 GiB region of device address space
mapped as cached memory is precisely the kind of thing that works on the
emulator and does something else on hardware.

*1 GiB pages were declined, deliberately.* The CPU supports them and they
would take the page tables for 8 GiB from 40 KiB to 4 KiB. Every walk in
`vmm.c` tests for `PTE_HUGE` at exactly one level — the PD — and a 1 GiB
page puts one at the PDPT level instead. Supporting both means auditing
nine functions (map, unmap, unmap-take, `user_range_ok`, destroy, fork,
`cow_break`, `unmap_range_free`, and the mapper itself) for a second
huge-page case, to save 36 KiB on a machine with gigabytes. That is not
what this milestone is for, and it is exactly the kind of audit that gets
one function wrong.

*Three things elsewhere were resting on the 1 GiB constant, and one of
them would have corrupted memory silently.*

  - `heap.c` started the kernel heap at `PMM_TRACKED_MEMORY` — "the first
    address above the identity map", a correct derivation that becomes
    wrong the moment the map is sized from the machine. A heap that moves
    when the RAM does lands *inside* the identity map on a bigger
    machine. It now starts at a fixed 256 GiB, which is above any
    physical memory and below `USER_REGION_BASE`, so it stays in the
    PML4[0] subtree every address space shares. The `vmm` self-test's own
    scratch address had the same bug for the same reason and moved with
    it.
  - `acpi.c` refused any table address `>= 1 GiB`, hardcoding the size of
    a map it does not own. It now asks `vmm_identity_covers`, which walks
    the tables rather than comparing against a limit — necessary, because
    the map is no longer one range starting at zero: a machine with a PCI
    hole has RAM below it and RAM above 4 GiB with nothing in between,
    and a limit would call the hole mapped. This makes M29's graceful
    degradation *better* rather than merely preserved: a table in high
    reserved memory is now usually in range instead of skipped.
  - **`ac97.c` writes a frame address to the device as a 32-bit value.**
    So does `rtl8139.c`. Until this milestone every frame in the machine
    was below 4 GiB and the truncation could not happen; afterwards it
    can, and it is silent — the cast compiles, the driver reports
    success, and the card DMAs into somebody else's page. `pmm.h` grew
    `PMM_DMA_LIMIT` and `pmm_alloc_frame_dma`, and `pmm_alloc_contiguous`
    is now always below 4 GiB because both its callers (DMA rings, kernel
    stacks) are fine with that and one rule that is always safe beats two
    rules where the unsafe one is the default. This was found by reading
    the code against the new invariant, not by a test — the same way M82
    found its own COW guard.

*Verified.* `tools/qemu-serial-test.sh` reaches `[init] PID 1 spawned`
with no panic and every existing marker, plus the new one:

```
[pmm] 0xFF4CB / 0x140000 frames free, tracking to 0x140000000 (5120 MiB);
      bitmap+refcounts at 0x4B0000
[vmm] kernel-owned page tables installed (4096 MiB identity-mapped in 2 MiB pages)
[heap] kernel heap starts at 0x4000000000
[m90] more than a gigabyte: 5120 MiB tracked in 0x140000 frames, a frame at
      0x100000000 written and read back through the identity map, freed with the
      count returning exactly, and an address past the end of memory correctly
      reported as not mapped.
```

The tracked span is 5120 MiB and the mapped memory is 4096 MiB, and the
difference is not a discrepancy: the bitmap covers the frames *up to* the
highest usable address, which includes the 1 GiB PCI hole between 3 GiB
and 4 GiB, and every frame in it is marked reserved because no e820 entry
freed it. The identity map covers only what is there.

The last assertion in that line is the one that makes the rest mean
anything. `vmm_identity_covers` returning 1 unconditionally would pass
every other check in the self-test, and `acpi.c` would then dereference
whatever a firmware table pointed at — so the test also requires a
**no**, for an address a gigabyte past the end of memory.

**And the half of the criterion that was the actual point: the same
kernel, `QEMU_MEM=128`.** 76 of 76 markers, no panic, and the self-test
reports 126 MiB tracked, 128 MiB identity-mapped, and its probe frame at
`0x3F7A000` — half of what there is, because that is the strongest floor
a small machine supports. Two runs, one binary, a thirty-two-fold
difference in memory, and nothing in between them was a constant. That
was written as the real test and it stayed the real test:

```
-m 4096   PASS: 76/76   5120 MiB tracked   4096 MiB mapped   probe 0x100000000
-m 128    PASS: 76/76    126 MiB tracked    128 MiB mapped   probe 0x3F7A000
```

`tools/qemu-input-test.sh` passes unchanged — it grades pixels, and
nothing in this milestone was supposed to reach them.

### M91 — An address space that is a set of mappings [~] six of seven; MAP_SHARED, file-backed and mremap are the three left

*Reopened fifth and last of the five before M94 — see "Before M94"
below. The second attempt is `MAP_SHARED` and file-backed mappings,
because M95 cannot exist without them. `mremap` is not in it: nothing
has measured a need for one, which is M69's rule and the same reason the
header line above should stop reading as though all three were one
backlog. The NX bit in the third bullet shipped.*

- [~] Per-process VMAs replace `proc.h`'s fixed layout. The heap, the
      stack, the image and the mmap arena stop being six constants with
      "generous gaps" between them and become entries in a list that the
      fault handler, `user_range_ok` and `SYS_munmap` all read.
      **Half of this: the map was rebuilt and the numbers are no longer
      the constraint, but the heap, image and stack are still regions the
      kernel names rather than list entries** — see the notes for why
      that turned out to be the right stopping point rather than a
      shortfall
- [~] `mmap` with a hint and `MAP_FIXED` — **shipped**; `MAP_SHARED` and
      file-backed mappings — **not**. `SYS_mmap` refuses all three today,
      in its own words, *"rather than quietly given private anonymous
      memory"* — that refusal was right and this is the milestone that
      earns the yes for the first one. A dynamic loader maps a file at an
      address it chose; there is no version of M95 that does not need
      this first
- [~] `mprotect` and `madvise(MADV_DONTNEED)` — **shipped**; `mremap` —
      **not**. And a real NX bit, so `PROT_EXEC` is a fact rather than a
      shrug and `PROT_NONE` is a mapping rather than a refusal —
      **shipped, and it reached further than expected**: see the notes on
      what had to change in the linker script before the bit meant
      anything
- [x] A stack that grows on fault, and ~~a heap that is just another
      mapping~~. GCC recurses deeply on generated code and the current
      stack is a fixed number of pages placed below a constant
- [x] The private region is [512 GiB, 1 TiB) and stays that way — M52's
      argument for why that boundary is what makes a kernel pointer an
      error rather than a read is untouched by any of this, and should
      be

**How we'll know.** A program reserves 4 GiB at an address it names,
`mprotect`s one page read-only and dies on the write, then makes a
second page executable and *calls into code it wrote there* — three
different answers from three page-table bits that all previously
answered the same way. And `badptr` and `lazytest` still die exactly
where they died before, because a fault handler that gets more generous
by one case is the regression M82 already named as the most likely one.

#### Progress notes

*The map, and why moving it was the milestone rather than a preamble to
it.* What `proc.h` described until M91 fitted an entire process into the
first gigabyte of a 512 GiB private region: a 2 MiB image window, the
stack immediately above it, a 256 MiB heap, a 160 MiB mmap arena, and the
shm and framebuffer windows at 512 MiB and 1 GiB pinning the arena's top.
`cc1plus` is a hundred megabytes of text — the image window alone was
fifty times too small — and because the stack sat directly above the
image, growing one meant moving the other. The new map uses the region
this OS was already entitled to and was using a fifth of a percent of:

```
512 GiB  +   0        image           64 GiB
         +  64 GiB    sbrk heap       64 GiB
         + 128 GiB    mmap arena     256 GiB
         + 384 GiB    shm window      64 GiB
         + 448 GiB    framebuffer
         + 496 GiB    stack top, growing down; argv/envp just above
1 TiB                 end of the private region
```

M82 is what makes this an edit to a header rather than a memory budget:
address space is free until it is touched and the page tables that
describe it are built on demand, so a 256 GiB arena costs exactly what a
160 MiB one did until something reserves inside it. M82's own note
predicted the cost of not doing this — *"going further means moving the
shm window and the framebuffer's fixed mapping address, which is a bigger
change than this milestone needs"* — and it was right about the size of
the change: four constants moved and three other files had to be looked
at.

*What did not become a VMA, and why that is the honest stopping point.*
The bullet asked for the heap, image and stack to become list entries
alongside the mmap regions. They did not. The mmap arena has a region
list because a program creates and destroys mappings there at arbitrary
addresses; the image is placed once by the ELF loader, the heap is a
single growing range with one syscall that moves its end, and the stack
is one range with a fixed top. Turning three singletons into list entries
would add a lookup to every path that touches them and would not answer a
question anybody is asking. What the milestone was actually *for* — that
the layout stops being the constraint, and that a program can arrange its
own address space — is delivered by the map above plus `MAP_FIXED`. The
bullet is marked `[~]` rather than ticked because that is not what it
said, and rewriting the bullet to match what was built is the drift this
file exists to prevent.

*NX reached three files further than it looked like it would.* The bit
itself is small: `PTE_NX` at bit 63, `EFER.NXE` per CPU, and one function
(`leaf_flags`) that every place writing a leaf entry now goes through.
Three things around it were not small.

  - **Every binary this project has ever produced had a single RWE
    segment.** `readelf -l` on any of them said so, because
    `user_space/lib/user.ld` said nothing about program headers and `ld`
    merged everything into one. Page permissions are per page, so a
    loader honouring `p_flags` on that image maps the whole program
    writable *and* executable — no protection at all. The script now
    emits two `PT_LOAD`s, R+X for text and rodata and R+W for data and
    bss, with an `ALIGN` between them that is not cosmetic: two segments
    sharing a page have to be given the union of both sets of rights.
  - **`elf.c` mapped every segment `VMM_FLAG_WRITABLE`**, ignoring
    `p_flags` entirely. It honours them now, so a program's text is
    read-only for the first time in this project's history.
  - **`vmm_cow_break` dropped the bit.** The copy path rebuilt a leaf
    entry as `new_phys | (entry & (PTE_USER | PTE_PRESENT)) |
    PTE_WRITABLE`, which is correct for every bit it names and silently
    loses the one it does not. A fork followed by a write would have
    handed back an *executable* copy of a page that was not executable
    before — a W^X hole that appears only after a fork, which is
    precisely the class of bug nobody finds by running a program. Found
    by grepping for every write to a page-table leaf after adding a bit
    to them, which is the same method M82 used to find its own
    unreachable-today COW guard.

*The stack needed a heuristic, and the heuristic is the whole
correctness argument.* A stack that grows on any fault inside its window
is one case more generous than it should be in exactly the way M82's
notes warn about: a wild pointer 30 MiB below the stack would quietly
become a valid page instead of killing the process. So the fault has to
be *near the stack pointer* — 64 KiB, which covers every prologue GCC
emits without `-fstack-clash-protection` and is small enough that a
pointer computed from garbage lands outside it. That check is why
`sched_fault_fill` grew a third argument: rsp is a fact about the fault,
not about the address space, and it had to be threaded from `isr.c`.
`vmtest stackfar` is the test, and it is the one a growable stack written
in a hurry would not have.

The prefault path deliberately has no such heuristic and the reason is
that there is nothing to apply it to: a syscall buffer's address comes
from an argument, not from a faulting instruction. `read(fd, buf,
sizeof buf)` into a 1 MiB local that the program has not touched is
ordinary, and refusing it would be an `EFAULT` for a buffer the program
has every right to pass. The cost — a wild syscall pointer inside the
process's own stack window gets built rather than refused — is the trade
every Unix makes for the same reason, and it is written into the code
rather than left to be discovered.

*Two tests failed because they asserted contracts this milestone
deliberately changed, which is now the fourth time in two arcs.*
`mmaptest` required `PROT_NONE` to be **refused** ("a guard page that is
not one" — M78 declining to pretend), and `libctest` required a non-NULL
`addr` to be **refused** ("addr and fd are refused rather than ignored").
Both were honest encodings of what was true when they were written and
both are false now. The fix is the same shape every time and it is worth
naming again because the temptation is always to delete the check: assert
the *new* contract. `mmaptest` now requires a `PROT_NONE` mapping to
succeed; `libctest` now passes a hint that **cannot** be honoured — its
own load address — and requires the kernel to quietly ignore it and map
somewhere legal, which is the interesting half of what a hint means and a
stronger check than the refusal it replaced.

*A pre-existing flake, correctly diagnosed at last.* Four boot failures
in the M90 input-harness run were all the same thing: `[m63]` panicking
with *"floating point, the libc subset, or the ported program is
wrong"* after whetstone printed `Insufficient duration- Increase the LOOP
count`. None of those three things was wrong. Whetstone runs the entire
benchmark and only then compares `time(0)` either side of it; a
difference of zero prints that message and skips the figure. M88's notes
say the same thing about the same message — *"the benchmark prints the
same line whether the clock read zero twice or read the same number
twice. Those are different bugs"* — and answered it for `clock()` in
`libctest`. `time()` is a different clock (the RTC, not the PIT) and it
is the one whetstone uses. So the self-test now reads that same clock
itself, either side of the run: if the kernel's reading advanced and
whetstone's did not, `time()` is wrong and that is a failure; if neither
advanced, nothing measured anything and the run still completed, which is
what proves the FP and the libc. A boot that dies because four QEMU
guests were competing for the same cores is a harness grading the host.

*One regression this milestone introduced and then removed, found by
reading rather than by measuring.* Widening `sched_prefault_range` to
cover the stack window made every syscall that passes a stack buffer -
which is most of them - do a locked four-level page walk per page, where
before M91 a stack address left the function immediately on the bounds
test and did no walking at all. It was correct and it made the free case
cost something. The fix is one line at the top: ask about the whole range
once, and only fall into the per-page loop if something is actually
missing. That is strictly better than the code that was there for the
arena too, which had been doing a locked walk per page since M82.

Worth being precise about what this is and is not: it is not a
measurement, and no measurement asked for it. It is a case that used to
be free and stopped being, noticed while adding the second window, and
put back. M69's rule is about not doing performance work on a guess; it
is not about leaving a cost in that was introduced two hours earlier.

*Verified.* `tools/qemu-serial-test.sh` reaches `PASS: 77/77` with no
panic, including the new marker, which reports what it actually proved:

```
[m91] an address space that is a set of mappings: an address hint honoured and
      MAP_FIXED landing exactly where it was told and replacing what was there,
      mprotect taking write away and giving it back with the bytes intact and
      refusing a range no mapping covers, bytes written to a page and then
      executed from it, MADV_DONTNEED returning the frames and keeping the
      mapping, a 4 GiB reservation touched at both ends on a machine whose whole
      user region used to be under a gigabyte, a stack grown sixteen times past
      what it was given, NX enforced, and all four faults that must stay fatal -
      executing a non-executable page, writing to one mprotect made read-only,
      touching a guard page, and touching far below the stack pointer - still
      killing only the program that made them - self-test passed (350 ms).
```

The four fatal modes are the half that matters and they are four different
mechanisms rather than four spellings of one: `nx` is an instruction
fetch nothing before M91 could refuse; `wx` is a write to a page that
*was* writable and stopped being, which goes through
`vmm_protect_range_in` rewriting a live entry rather than through the
fault handler consulting a region; `guard` is a `PROT_NONE` mapping; and
`stackfar` is the one that separates a growable stack from a 64 MiB
window of silently valid addresses. The frame count is equal either side
of all five runs, which is what says that a mapping placed, replaced,
reprotected, dropped and torn down gives every frame back through four
different unmap paths.

### M92 — A disk worth reading, and a cache in front of it [~] the driver and the cache; interrupts and AHCI deliberately not, writeback deliberately not

- [~] A DMA block driver ~~with interrupts~~ — virtio-blk first because
      it is the smallest real one and QEMU always has it, ~~with AHCI as
      the one that matters on hardware~~. `ata.c` stays as the fallback
      that boots anything. **Polled rather than interrupt-driven, and
      AHCI not attempted** — both declined with reasons in the notes
      rather than deferred vaguely
- [x] A block cache ~~with readahead and~~ writeback, which M87 declined
      because *"no measurement has asked for one"* and M69's rule says
      to wait for the measurement. Unpacking a source tarball is the
      measurement, and it should be taken *before* the cache exists so
      that the milestone can report a ratio rather than an adjective
- [x] ~~Writeback~~ **Write-through** that keeps M71's ordering
      guarantees. A cache that reorders writes turns "write ordering plus
      a mount check" into neither, and M71's whole argument for not
      having a journal rests on the first half being true — **which is
      the argument that decided this bullet against writeback rather
      than for it, and the notes say why that is the same conclusion
      seen from the other side**
- [x] The measurement, written into the milestone: ~~seconds to unpack
      and to `treewalk` the same tree, PIO versus DMA,~~ cold versus warm

**How we'll know.** The same tree, the same two operations, four
numbers. And a power-cut test — kill QEMU mid-write, remount, and have
the unclean-mount check say the same thing it says today rather than
something new and worrying.

#### Progress notes

**The four numbers, which are the milestone.** One megabyte, read three
ways, timed on the same host with the same image:

```
ata-pio,    cold        95405 us
virtio-blk, cold         5603 us      17x
either,     warm          2500 us     38x  (and 2.2x against DMA)
```

M87 declined a buffer cache because *"no measurement has asked for
one"*, and M69's rule is that performance work waits for a measurement.
That is the measurement. The interesting part is not the headline
seventeen-fold — it is the second column: **once the driver is DMA, the
cache is only worth another 2.2x**, because what a cache saves is the
round trip and what DMA already removed was the per-word port I/O. If
this milestone had built the cache and left the driver alone it would
have reported a large number and attributed it to the wrong thing.

Timed with the TSC and not the PIT, and M69's header explains why in
advance: the first version of this self-test reported *"0 ms cold, 0 ms
warm"*. That is not a ratio, it is a 10 ms clock saying the question was
below its resolution. Reading a megabyte over DMA turns out to be one of
the things the PIT cannot see.

*Where the seventeen-fold actually comes from, and it is not the DMA.*
ATA PIO moves two bytes per `insw`; that is 2048 port instructions per
4 KiB and every one of them is a VM exit into QEMU's device emulation.
virtio moves the whole request with one notification. So the win is the
*number of exits*, and the same argument is why `blk_read` decides once
for a whole range rather than per cache line: the obvious per-line cache
would have turned leanfs's single 2048-sector inode-table read into 256
device requests of eight sectors each, which is 256 round trips to avoid
one. The cache is consulted for the whole range; if any line is missing
the entire range is read in one request straight into the caller's
buffer, and the resident lines are then populated from what came back.
That last step is only safe because the cache is write-through — what is
on the disk and what is in a resident line are the same bytes by
construction, so a copy can never be stale.

**Write-through, and this is the milestone's real decision.** The bullet
asked for writeback. Writeback is the thing that would quietly make M71
false. M71 bought "files worth trusting" with *write ordering plus a
mount check*, and its own note says a journal becomes worth it when there
are multiple writers or when a full scan gets slow. The entire guarantee
is that a data block reaches the disk before the metadata pointing at it;
a cache free to defer or reorder writes turns "write ordering plus a
mount check" into "a mount check". So writes go to the device in the
caller's order and are *also* placed in the cache, which keeps M71 exactly
as true as it was and still collects nearly all of the win — because the
traffic a filesystem generates is overwhelmingly reads. An unpack writes
each block once and reads its inode table, bitmap and directory blocks
over and over.

This is not caution. It is the same conclusion as "a journal is worth it
when..." arrived at from the other direction: **writeback is not deferred,
it is declined until there is a journal to make it safe**, and M93's
bullet is where that gets re-measured with a filesystem big enough for
the question to matter.

*Two bullets declined outright, with reasons rather than a shrug.*
Interrupts: `ata.h` has said since it was written that "nothing here
needs to overlap disk I/O with other work yet", leanfs holds one lock
across a request, and an interrupt would buy a context switch this kernel
has nothing to switch to. What made PIO slow was never the polling. AHCI:
it is what a real machine needs and it is a second full driver; `ata.c`
remains the fallback that boots anything, and M92 is about the transfer
model rather than the controller. Both are named here rather than left as
an unticked box someone would later assume was an oversight.

*Two bugs found by reading the code against its own assumptions.* The
cache's first version allocated 2048 frames and required them to come
back consecutive, "because a fresh allocator hands out consecutive
frames" - which is false at the point `blk_init` runs, late in boot after
every self-test that allocates and frees, including M90's own probe that
frees a frame at 4 GiB and pulls the search hint back with it. A line is
exactly one page, so contiguity was never needed; it is an array of
pointers now. And the virtqueue pointers were not `volatile`, which is
the difference between a poll loop and an infinite loop the moment a
compiler decides `used->idx` cannot change.

*A claim this note made and then had to withdraw, recorded rather than
edited away.* The first `QEMU_DISK=ide` run reported `[wm] animation
missed its frame budget: 1 of 4 frames`, and the obvious story wrote
itself: PIO is 17x slower, so of course the compositor misses a frame.
That story was wrong. The same configuration passes 78/78 on a re-run,
and the same message had already appeared once on a *virtio* boot
earlier in this arc. It is a marginal 16 ms assertion on this host,
intermittent on both disk backends, and it has nothing to do with the
disk. Written down because the attractive explanation was available, fit
the facts to hand, and was false - which is exactly when a measurement
is worth repeating before it becomes a sentence in this file.

*Verified.* `PASS: 78/78` on virtio-blk and `PASS: 78/78` on
`QEMU_DISK=ide`, one image, two drivers, the same self-test reporting
95405 us and 5603 us for the same megabyte. The PIO number reproduces
within 2% across runs (95405, 97280), which is what makes the ratio a
measurement rather than a sample.

### M93 — A filesystem that can hold a source tree ✅ (second attempt)

*The first attempt shipped the format, hard links and `fsync` and left
the image builder and the journal measurement open. Both landed in the
second, which is the first of the five prerequisites before M94 — see
"Before M94" in the arc below. The format was not touched.*

- [~] leanfs v3: a file ceiling in the gigabytes (~~a triple-indirect
      block is the cheap answer~~ — **a bigger block turned out to be
      cheaper still, and the notes say why**; extents are the right one
      and M81 already declined them once with a reason worth re-reading),
      inodes into the ~~hundreds of thousands~~ **hundred and thirty-one
      thousand, with the reason the next order of magnitude is a
      different kind of work written down rather than left to be
      discovered**, and a data region measured in gigabytes rather than
      the current 32 MiB
- [ ] The migration M81 wrote the version field for and could not use,
      *now* used — that bullet says the branch is *"in place for the
      next format change, which will be the kind that can."* This is
      that change or it is an admission that the field was speculation.
      **It is the second: this is a geometry change, exactly like M81,
      and the notes say plainly why it could not be anything else**
- [~] Hard links **and** `fsync` — **shipped**; the `*at()` family —
      **not**. M87's three unfinished bullets, absorbed here because this
      milestone is already rewriting the format they touch
- [x] A host-side image builder. Today every program reaches the disk by
      being `incbin`'d into the kernel and seeded on first boot
      (`kernel/proc/embed_programs.asm` says so at the top: *"there's no
      way to get them onto the disk filesystem... other than the kernel
      seeding them there itself"*). A source tree is 800 MB and cannot
      live inside a kernel image; a tool that writes a leanfs image from
      a host directory is how a tarball gets here at all. **Shipped as a
      second mode of `tools/leanfs-put.c` rather than as a second tool,
      for the reason Q3 exists**
- [x] The journal deferral, re-measured rather than re-argued. M81 said
      thousands of files makes "a full scan gets slow" *nearly* true and
      that "nearly" is not a measurement. Hundreds of thousands of files
      is where the measurement gets taken, with M92's numbers in hand.
      **Taken — see the second attempt's notes. The number is small
      enough that the deferral holds, and it is now a number**

#### Progress notes

**A bigger block, not a third level of indirection, and that was the
whole design.** The bullet assumed the file ceiling would be raised by
adding triple indirection. It was raised by making a block 4096 bytes
instead of 512, which reaches four gigabytes with the *same* two levels -
and does three other things at once that a third level would not have:
eight times less bitmap to scan for a filesystem of the same size, no
deeper walk on a deep access, and a block that is finally the same size
as M92's cache line and as a page. A 512-byte filesystem block meant
every block operation touched one eighth of a cache line, which was
invisible until there was a cache to touch.

leanfs's block and the disk's sector had been the same word since M12.
They are eight of one to the other now, and the conversion happens in
four functions rather than at each of twenty-six former LBA
computations.

**The numbers, and one that is deliberately not as big as the bullet
asked for.** File ceiling 8 MiB -> 4 GiB (capped by the `size` field
rather than by the block tree, which reaches 4295 MiB). Data region
32 MiB -> 2 GiB. Inodes 8192 -> **131072**, not "hundreds of thousands",
and the reason is written into `leanfs.h` rather than left as a
shortfall: the table is held in memory in full, so 131072 is 16 MiB of
it and a million would be 128 MiB. At that point the answer is not a
bigger array - it is to stop holding the table and read inodes through
M92's cache like every other block. That has a trigger rather than a
date: the day a tree with more than 131072 files has to exist here at
once. GCC's source without its test suite is under that; with it, it is
not.

The image is 2.03 GiB and **3 MiB on disk**, because it is sparse and a
format writes only the 16 MiB inode table and the 64 KiB bitmap. That is
the property that makes the data region a constant rather than a budget.

**The migration the version field exists for, still not taken, and this
time it is a decision rather than an admission.** M81 wrote that the
version field is *"in place for the next format change, which will be the
kind that can [migrate]."* This is not that kind. A block changing size
moves every region on the disk and every pointer in every inode, from a
35 MiB filesystem into a 2 GiB one - there is nowhere to stand. The magic
is bumped (LFS4 -> LFS5) and the disk reformats, exactly as M81 did. What
can be said honestly is that the field is now on its second opportunity
and has not had one it could take; whether that makes it speculation is a
fair question, and the answer is "not yet, but one more of these and it
is."

**The bug this milestone's own test found, which is the best argument for
the test.** Moving 4 KiB buffers off the kernel stack meant turning
`uint32_t table[N]` into a pointer to a file-scope array.
`k_memset(table, 0, sizeof(table))` on a pointer zeroes **eight bytes**.
So every freshly allocated indirect table went to disk with 4088 bytes of
whatever was in that block before, and the first read that trusted a
stale entry returned somebody else's data.

The self-test found it at exactly 12 MiB into a 16 MiB file - the third
mid-level table, which is where the first stale entry that happened to
look allocated lived - and it found it because the pattern written into
each block *depends on that block's offset*. A constant fill would have
passed. That is the difference between a test that checks a file reads
back and one that checks it reads back **from the right place**, and it
is worth the four extra lines every time.

The fix is to make the buffers function-local statics rather than
pointers, so `sizeof` means what it says. The general lesson is narrower
and worth stating: converting an array to a pointer silently changes the
meaning of every `sizeof` on it, and the compiler is entirely happy about
it.

**What M92 bought, visible in a test that predates it.** M81's
file-storm self-test creates 1200 files and reports its own timing. It
was 3.2 s; it is **1680 ms** now. Nothing in that test changed - the DMA
driver and the block cache halved it, which is a second independent
measurement of M92 and the only one taken by code that was not written to
measure it.

**And a second bug the same run exposed, in the logging rather than the
filesystem.** An interactive test failed with *"the machine never came
back up after `reboot`"* while the log said the machine had come back up
perfectly well:

```
[init] P[elf] loaded, entry = 0x8000000000
...
ID 1 spawned - handing off to the desktop shell.
```

`klog` took its lock **per character**, so two writers could interleave
inside a word - and the harness greps for `[init] PID 1 spawned` as a
string. The boot got further than the log could say it had. M92's
straddling marker was the same category (an answer that depended on an
input nobody thought was an input) and this is the same category again,
which is worth noticing: two of the three bugs in this arc's last two
milestones were in the machinery that reports results rather than in the
machinery that produces them.

`klog_puts` and `klog_log` now hold the lock across the whole string, so
every single-call message - which is every marker in this project - is
atomic. A message assembled from several calls can still interleave at
those seams and deliberately is not addressed: fixing that needs a
per-CPU line buffer, and what it prevents is cosmetic where this was a
false test result.

**And a third, in the desktop, which M92 caused and M93 found.** The
interactive test `desktop_survives_losing_the_compositor` began failing
reproducibly: after the compositor is killed and replaced, two app
windows where there should be one. Bisecting says M91 passes and M92
fails, and the mechanism is the nicest thing in this arc.

`init` respawns a dead compositor (M55) and the clients it was serving
stay alive and reconnect. A replacement compositor called
`session_restore()` exactly as the first one does - so it *relaunched*
every program the session file listed, while the surviving copies of
those same programs were already coming back. Two windows for one app.

That has been true since M74. It never fired because the session file
was usually still empty at the moment the compositor died: writing it
took long enough on a 95-millisecond-per-megabyte disk that the kill
arrived first. M92 made the disk seventeen times faster and the write now
lands. The test had been passing because a file was empty, which is a
pass for the wrong reason - and the bisect is worth stating plainly,
because "M92 broke the compositor" is what the evidence looks like and
is not what happened.

The fix keeps the half that is right: a replacement still *parses* the
session file, so a reconnecting window lands back in the geometry it had
rather than wherever there is room; it simply does not spawn anything.
`init` says which kind of compositor this is through the same environment
variable M74 chose over an argument, for the reason M74's own note gives.

*Verified.* `qemu-serial-test.sh` `PASS: 79/79` at `-m 4096` and
`PASS: 79/79` at `QEMU_MEM=128`, where the block cache scales itself down
to 7568 KiB and the 16 MiB inode table still fits. The new marker
reports what it proved: a 16 MiB file written and read back block for
block where 8 MiB was the structural ceiling, 9000 files created past an
inode cap of 8192 and every one read back, a second name for a file with
the link count to prove it is the same file rather than a copy, one name
removed leaving the other readable, a hard link to a directory refused,
and `fsync` refusing a pipe it cannot make durable. The self-test costs
21 s and the boot reaches the desktop in 190 s.

`qemu-input-test.sh` **45/45 in one batch with no re-runs**, which this
arc had not managed before - every previous run needed at least one test
run again on its own. Three real bugs stood between the first M93 build
and that number and none of them was in this milestone's own code: a
straddling log marker (M92's note), a klog lock held per character, and a
replacement compositor relaunching a session its clients were already
bringing back. The input harness's single-guest boot allowance went from
300 s to 420 s, because the boot itself is 20 s longer and that was
measured rather than suspected.

**How we'll know.** Unpack GCC's own release tarball on the machine,
`treewalk` it, and compare file count and total bytes against the host's
own count of the same tarball — then check a SHA of a file deeper than
any path this filesystem could previously express. A tree small enough
to have fitted in v2 proves nothing, and the test should be written so
it cannot accidentally be one.

#### Second attempt: the road onto the disk, and the measurement it made takeable

**What shipped.** `tools/leanfs-put.c` grew a `-r` mode that writes a host
directory tree into a leanfs image, and that is the whole of the "image
builder" bullet. A second program was the obvious shape and is the wrong
one: everything a tree needs — the allocator, the block map, the
directory records, the format — was already in this file and already
correct, and Q3 exists because a second copy of exactly that code drifted
from the kernel's for twelve milestones without anybody noticing. A
`leanfs-mkimage.c` would have been the third copy.

Alongside it: `tools/leanfs-fsck.py --compare-tree`, which reads the
image back and compares it to the directory it came from;
`selftest_image_manifest` in `kernel.c`, which walks the tree from inside
the machine and hashes every byte of it; and `tools/image-tree-test.sh`,
which runs both against one image because neither alone is the claim.

**The two halves, and why one of them is not enough.** A host-side check
proves the image parses on the host — which is the weaker of the two
claims available, and the one a tool grading its own output would make.
The claim that matters is that the machine the image was built *for* can
read it, and only the machine can make it. So the builder leaves
`/.image-manifest` saying what it put there and the kernel walks the tree
and compares: directories, file names, symlinks, total bytes, depth, and
a hash over every name and everything it holds. The hash is the part that
grades the *data*: counts and sizes come out of the inode table, and an
image whose directory records were right and whose data blocks were wrong
would satisfy every one of the other five.

That self-test is conditional, which almost nothing else in this kernel
is. A normal build has no manifest, because a normal build has no
source tree on its disk. The precedent is Q1's fw_cfg switch — a
self-test whose trigger comes from outside the image rather than from an
`#ifdef` — and the discipline is the same one: the branch that did *not*
run says so in the log, so "this passed" and "this did not apply" cannot
be confused by reading the serial output.

**Six things were wrong, and the interesting thing is where they were.**
Only one was in the new code.

1. **`tools/leanfs-fsck.py` could not read a large directory.** It walked
   a directory's sixteen *direct* blocks and stopped. The first directory
   this project ever made that needed an indirect block was one this
   builder wrote, and the checker reported 17280 of its 20000 entries as
   unreachable inodes — under a `note:` reading *"expected after an
   interrupted create"* — and then printed `consistent`. A checker that
   under-reads and then explains away what it missed is worse than one
   that cannot read at all, because the second kind gets fixed. It walks
   the full block map now.
2. **The tool had never set `nlink`.** M93 added the field and Q3 moved
   the structs into a shared header, and neither made the tool write a 1
   into it. Every file `make preseed` has ever written has a link count of
   zero. Nothing had broken yet, and the way it would have is worth
   stating: `leanfs_link` on such a file makes the count 1 with two names
   on it, and the second `unlink` then frees a file a name still reaches.
3. **`dir_add` stamped `time(NULL)`.** Found by the property rather than
   by a symptom: two images built from one tree a second apart differ at
   byte 4198409, which is the root inode's `mtime`. Every timestamp comes
   from the host's `stat` now and the walk is sorted, so the same tree
   builds the same image byte for byte — which is a stronger thing to be
   able to check than "it parses".
4. **Empty files were an error** ("local-file is empty, unreadable"),
   which is a plausible check for a tool that writes programs and a wrong
   one for anything that has met a Python package.
5. **Symlinks would have been followed** and hard links copied, either of
   which produces an image that reads the same and is a different tree.
6. **A directory count off by one**, and this is the one in the new code —
   the builder counted the directories it created, the walker counted
   itself as well. It was caught by the machine, on the first boot that
   ran the check, with every other field including the content hash in
   agreement: `dirs 6/7`. That is the check doing precisely what it was
   written for, on its first outing, against its own author.

**And a seventh, in the instrument rather than in the machine.** The new
host tests were run against a deliberately broken hash to check that they
could fail — the ritual Q1–Q10 adopted — and they passed. `make test-fast`
does not rebuild when a *kernel header* changes: the test binary's
prerequisites were the test sources and `tests/*.h`, and these tests exist
to compile kernel units, most of what they assert lives in a kernel
header. Every header-only edit since Q2 has been graded by whatever binary
happened to be lying in `build/tests`. The Makefile now depends on every
header under `kernel/`, `system_api/` and `tests/`; the same broken hash
now fails without a manual clean. Two of the last three arcs' worst bugs
have been in the machinery that reports results rather than in the
machinery that produces them, and this is the third.

**The journal measurement, finally taken.** M71 deferred a journal and
named two conditions: multiple writers, and a full scan getting slow. M81
said thousands of files makes the second *nearly* true and that "nearly is
not a measurement". M93's fifth bullet said the measurement gets taken at
hundreds of thousands of files — and it could not be taken, because
nothing could put hundreds of thousands of files on this disk. That is the
real reason the two bullets shipped together: the builder is what makes
the measurement takeable at all.

A tree of **100,000 files** written into an image by the builder, booted,
and `leanfs_check` — the full scan an unclean mount runs before the
filesystem is trusted — driven directly:

| | |
|---|---|
| full scan at 100,000 files | **30 ms** |
| the same tree walked and every byte of it hashed, from inside | 49.4 s |
| the 4007-file synthetic tree, same walk | 5.4 s |

**The deferral holds, and now it is a number rather than a judgement.**
30 ms is not a wait anybody has after a power cut, and the reason it is so
small is structural rather than lucky: the inode table is held in memory
in full (M93's own choice, and the one that caps this filesystem at
131072 inodes), so the scan is a pass over RAM plus one read per
double-indirect block. The condition for revisiting this has moved with
it, and should be written down as such: **a journal becomes worth
building when the inode table stops being resident** — the work M93's own
header already schedules for the day a tree needs more than 131072 files
at once — because that is the day this 30 ms becomes a disk-bound scan of
a table that no longer fits.

The other number worth keeping is the one that is *not* small: 49.4 s to
read a hundred thousand files back. That is 2.2 MB of data, so it is not
bandwidth — it is per-file cost, a path resolution and an open per name,
and it is the first workload this machine has run where that dominates.
Nothing is being optimized on it today, in M69's spirit; it is written
down because M98 is a build, a build is millions of `open` calls, and this
is the measurement that will be pointed at when that gets slow.

**One deviation from "How we'll know", stated rather than quietly taken.**
That paragraph says to unpack GCC's tarball *on the machine*. Nothing here
can unpack a tarball — `tar` is M89's — so the host unpacks and the
builder writes the tree, which is the arrangement the bullet actually
asked for ("a tool that writes a leanfs image from a host directory is how
a tarball gets here at all"). What was kept is the part that makes the
test worth anything: the counting and the hashing happen *on the machine*,
against numbers the host wrote down before the image ever booted, over a
tree that could not have existed under any earlier version of this
filesystem.

*Verified.* `tools/image-tree-test.sh` PASS end to end: 4007 file names in
7 directories, one symlink, one hard link, a 255-byte name and a non-ASCII
one, the whole battery's `qemu-serial-test.sh` markers still green, and
the machine's own hash equal to the host's. The same run at 100,000 files
also PASS. `make test-fast` 123/123 with two new host tests over the shared
hash — known answers, and the chunk-size independence the two sides
actually rest on, since the builder hashes 4096 bytes at a time and the
kernel 32768. Both were checked against a broken hash before being
believed, which is how the Makefile bug above was found.

### Before M94: the five things this arc named and did not schedule

*Written after M93 landed, from re-reading this arc's own prerequisites
against the tree rather than against the plan.* The section that opens
this arc names four pieces of unfinished M81–M89 work and says what
each one blocks. It then schedules M94 first anyway. That is exactly the
drift this file exists to catch, so it gets caught here rather than at
the bottom of somebody's `config.log`.

M94's grading test is `./configure --host=x86_64-lean_os && make` on a
project nobody here wrote. Two things stand between this tree and that
command and neither of them is a target triple:

- **Nothing can get a source tree onto the disk.**
  `kernel/proc/embed_programs.asm` says at the top that the kernel
  seeding files itself is the only road there is. A tarball is not
  `incbin`-able, and M93's fourth bullet — the one that says so — is
  still open.
- **`/bin/sh` cannot run a configure script.** No `if`, no `for`, no
  command substitution, no multi-stage `|`. The arc's own prerequisite
  note states this in one sentence — *"Nothing from M94 onward is
  attemptable through a shell without `if`, `&&`, command substitution
  and a real `|`"* — and then M86 stayed unscheduled anyway.

The next five milestones are those prerequisites, ordered by which one
blocks the next. **The first two are done** — see M93's second-attempt
notes, which also record that the journal deferral finally has a number
behind it (30 ms at a hundred thousand files) instead of an argument, and
M86's, which record that the shell is now graded against a shell nobody
here wrote. They keep their own numbers rather than becoming
M101–M105, because none of this is new scope and M68 and M74 already set
the precedent for a milestone reopened under the number it was given.

| order | milestone | what closes | why here and not later |
|---|---|---|---|
| 1 | M93 (2nd) ✅ | the host-side image builder, and the journal measurement | nothing else in the arc can begin until a tarball can reach the disk |
| 2 | M86 ✅ | the shell | M94's grading test *is* a shell script |
| 3 | M88 (2nd) | UTF-8, and the calls a build probes for | every source tree in this arc has non-ASCII bytes in it |
| 4 | M89 | toybox | a configure run shells out to `sed`, `grep`, `install`; `/bin` has six programs |
| 5 | M91 (2nd) | `MAP_SHARED` and file-backed `mmap` | M91's own words: *"there is no version of M95 that does not need this first"* |

**What this ordering assumes, written down so it can be wrong.** That
M89 belongs before M91: toybox is static and needs no loader, and
putting it first means M94's configure test has real utilities beneath
it rather than six programs. If toybox's own build turns out to want
`MAP_SHARED`, the two swap — and this paragraph is what says that was
allowed rather than a plan quietly rewritten after the fact.

**And one place it was already wrong, recorded here because this is the
paragraph that asked to be checked.** M86 was scheduled before M89 on the
grounds that the shell is what M94's configure test runs *in*. It is —
and the same configure test is also what M86's own bullet named as its
measure, which makes M86's grading instrument downstream of M89's
utilities rather than upstream of them. Nothing about the order needed to
change: the shell still has to exist before anything can run a script,
and M86 shipped with a stronger instrument than the one it planned (see
its notes). What moved is where the configure run gets graded — M89, and
then M94 again. The lesson is narrower than the ordering: **a milestone
whose test names a program should check that the program will exist.**

**What it does not change.** M92's writeback cache and AHCI, M91's
`mremap`, and `O_NONBLOCK`/`AF_UNIX` all stay exactly where they were
left. The first three are deferred for want of a measurement, which is
M69's rule and not a backlog; the last two were absorbed into M100 on
purpose and collecting them here would be this arc doing the thing the
"Deliberately not next" section warns arcs about.

**And three entries whose status is corrected rather than reopened:**

- **M80 is superseded, not pending.** M99 is the same program done the
  other way round — built on the machine instead of cross-compiled and
  frozen — and leaving M80 at `[~] attempted` implies somebody is going
  back to a build that never linked. Nobody is.
- **M87's `*at()` family was absorbed into M93 and did not land there.**
  It moves to M89, where toybox asks for it by name, rather than staying
  where a format rewrite made it look convenient.
- **M85's pty stays where this arc put it** — M98, for `^C` on an
  hour-long build — and M86 gives up `jobs`/`fg`/`bg` to keep it there.
  See M86's entry for that re-scope and what it costs.

---

### M94 — A target this compiler knows by name

- [ ] `x86_64-lean_os` as a real triple in binutils and GCC: a config
      fragment, an OS name, and the default library and startup-file
      rules that go with it. M63 and M80 both built with `x86_64-elf`
      plus `-ffreestanding` plus a hand-written link line, and a
      `./configure` script cannot be told about a hand-written link line
- [ ] A sysroot: `usr/include` from `user_space/libc/include`, `usr/lib`
      with `libc.a`, `crt1.o`, `crti.o`, `crtn.o` and a `libgcc` built
      for the target. `crt0` exists in `user_space/lib` and has never
      had to be a file a linker finds by name
- [ ] GCC specs that make `x86_64-lean_os-gcc hello.c -o hello` produce
      a program this OS runs, with no flag invented by hand — which is
      the entire point, because every flag invented by hand is a flag
      someone else's build system will not pass
- [ ] `install` targets, so the sysroot is generated by the build rather
      than assembled by a person. The drift M25's tooling was found in
      is the precedent: a thing maintained by hand is a thing that is
      wrong by the time anybody looks
- [ ] Deliberately **not** a GCC fork. A target port is upstream-shaped
      configuration; a patch to the compiler's own passes is the thing
      M63's rule exists to forbid

**How we'll know.** `./configure --host=x86_64-lean_os && make` on a
small autotools project nobody here wrote — `bzip2`, `less` or `jq` —
producing a binary that runs, with zero flags supplied by hand and zero
edits to its source. The configure script's own log is the evidence,
because it records every test it ran and which ones this OS failed.

### M95 — Code that is loaded, not linked

- [ ] ELF `ET_DYN` and `PT_INTERP` in `kernel/proc/elf.c`, which today
      maps `PT_LOAD` at fixed addresses and checks them against
      `USER_IMAGE_BASE`/`LIMIT`. A position-independent executable has
      no fixed address by construction
- [ ] `/lib/ld-lean.so`: relocation processing (`R_X86_64_RELATIVE`,
      `GLOB_DAT`, `JUMP_SLOT`, `COPY`, `TPOFF64`), symbol lookup across
      a search scope, `DT_NEEDED`, lazy PLT binding, and `LD_LIBRARY_PATH`
- [ ] `dlopen`/`dlsym`/`dlclose`/`dlerror`
- [ ] `libc.so`, `libgcc_s.so` and later `libstdc++.so` as real shared
      objects, with the static archives kept — the kernel and anything
      that runs before the loader will always be static
- [ ] This collects the deferral M89's closing note scheduled: *"a
      second and third static binary duplicating the same libc."* By the
      time this milestone starts, `/bin` is full of them

**How we'll know.** Two different programs running at once share exactly
one copy of `libc.so`'s text, and the proof is the PMM's frame count —
not the fact that both programs ran, which a static build also achieves.
And `dlopen` of a shared object compiled *after* the program that loads
it, resolving a symbol by name.

### M96 — A thread with its own variables, and a wait that costs nothing

- [ ] Thread-local storage properly: `%fs` base per task, `arch_prctl`
      (`ARCH_SET_FS`/`ARCH_GET_FS`), the TLS segment from `PT_TLS`, and
      the initial-exec, local-exec and general-dynamic models with
      `__tls_get_addr`. `__thread` and C++'s `thread_local` are not
      optional for libstdc++ and they do not exist here in any form
- [ ] `errno` becomes genuinely per-thread, over that
- [ ] A futex: `FUTEX_WAIT`, `FUTEX_WAKE`, and a wait queue keyed on a
      user address. `pthread.h` asks for it by name — *"There are no
      futexes here... a waiter also burns CPU while it waits, which a
      futex would not"* — and every mutex, condition variable and
      barrier in this libc is a spin-then-yield loop until this exists
- [ ] Real `pthread_mutex`, `pthread_cond`, `pthread_rwlock`,
      `pthread_barrier` and `sem_*` over the futex, replacing the spin
      loops rather than sitting beside them
- [ ] `clone`-shaped thread creation with the flags a ported runtime
      passes, over M79's `SYS_thread_create`

**How we'll know.** Sixteen threads on a contended mutex, and the
measurement that matters is the *CPU time a blocked waiter consumes*,
which must be indistinguishable from zero — the disappearance of the
spin loop is the test, and a throughput number would pass with the spin
loop still in place. Plus a `__thread` counter per thread that stays
independent across a `dlopen`ed object, which is the case the
general-dynamic model exists for and the one a local-exec-only
implementation silently gets wrong.

### M97 — C++

- [ ] libstdc++ (GCC's own) built for the target: `operator new`/`delete`,
      static initialization and `__cxa_atexit`, `std::string`,
      the containers, iostreams, `<atomic>`, and `std::thread` over M96
- [ ] **Exceptions**, which is the milestone inside the milestone:
      `.eh_frame`, `_Unwind_RaiseException` and the rest of the unwinder
      ABI, the personality routine, and unwinding *through* a shared
      object boundary. This is the single largest new mechanism in the
      arc and there is no version of GCC or Chrome that runs without it
- [ ] RTTI and `dynamic_cast`, which need type identity to be stable
      across shared objects — the same problem exceptions have, with the
      same answer, and both get it wrong in the same way if `COPY`
      relocations and symbol interposition are not right in M95
- [ ] `libsupc++`/`libstdc++` as both an archive and a shared object
- [ ] Deliberately **not** `libc++`. One C++ runtime, and it is the one
      GCC ships with, because M98's whole test is GCC compiling itself

**How we'll know.** A `throw` in one shared object caught by type in
another, with destructors running for every frame in between — verified
by counting the destructor calls, because a catch that fires while
skipping a destructor is the bug this gets wrong and it looks like
success. And a C++ program nobody here wrote, with templates,
containers and iostreams, run unmodified.

### M98 — A compiler that runs here

- [ ] binutils built *for* lean_os and running *on* it: `as`, `ld`, `ar`,
      `nm`, `objdump`, `strip`
- [ ] GCC built for and running on lean_os — `cc1`, `cc1plus`, the
      driver — plus GNU make (or toybox's, if M89 got there first)
- [ ] Whatever the build actually asks the kernel for that is still
      missing, in M63's method: process counts past `MAX_TASKS 128`
      under `make -j`, fd counts past `MAX_FDS 128` in a linker, `/tmp`,
      `vfork`, `posix_spawn`, `wait4`, big `O_APPEND` writes. The list
      is a prediction; the build's own failures are the specification
- [ ] The measurements the arc has been deferring to this point, all
      taken here: peak RSS of the largest translation unit, disk used by
      the build tree, wall-clock for a bootstrap. Whether M90's memory
      ceiling and M93's disk are big enough stops being a plan and
      becomes a number

**How we'll know.** The classic test, and it is classic precisely
because nothing weaker is convincing: **a three-stage bootstrap where
stage 2 and stage 3 are byte-identical.** Stage 1 proves the compiler
runs. Stage 2 proves it compiles itself. Stage 3 being bit-for-bit equal
to stage 2 proves that the compiler it produced is the same compiler —
which catches a miscompilation, an uninitialized read, and a libc
function that is subtly wrong, none of which a `hello.c` that prints
would catch. If a full bootstrap does not fit this machine, the honest
outcome is the *number* that says by how much, and a stage-1 compiler
that builds a real program.

### M99 — Python, built here

- [ ] `./configure && make` for CPython **on the machine**, with the
      machine's own compiler — the inverse of M80, which cross-compiled
      and froze and never linked
- [ ] The standard library as `.py` files on the filesystem, which M81's
      closing note already identified as the thing M93 turns *"from a
      hard constraint into an ordinary choice"*
- [ ] Extension modules built as shared objects and imported at runtime
      over M95 — which is also the second concrete thing the
      dynamic-linking deferral named: *"a C-extension wheel"*
- [ ] `python3 -m test` over a subset of CPython's own regression suite,
      with the pass/fail counts recorded rather than summarized. A test
      suite someone else wrote, reporting its own failures, is the most
      honest grading instrument this project will ever get
- [ ] Deliberately **not** `pip` reaching the network. That needs TLS
      and a certificate store, and neither exists; a package installed
      from a local wheel is the same mechanism without the fiction

**How we'll know.** M80's own bar first — `python3 -c "print(1+1)"` —
then the bar it said mattered more: a real script with a dict, a class,
a loop and a file open. Then the regression suite's own numbers, which
is a bar M80 never got to name.

### M100 — What a browser actually needs, measured rather than argued

- [ ] Build the stack a browser links against, in dependency order, each
      one unmodified: zlib, libpng, libjpeg, freetype, harfbuzz, expat,
      sqlite, ICU, and a TLS library. Every one is a well-behaved
      autotools or CMake project and every one is a real test of M94
      through M97
- [ ] A TLS library working end to end over M66's TCP, which finally
      gives this machine an `https://` — and gives `fetch` something to
      do that is not a plaintext port 80 demo
- [ ] M88's `O_NONBLOCK` and `AF_UNIX`/`socketpair`, absorbed here
      because a multi-process browser is the first program that needs
      both at once and for a reason rather than for completeness
- [ ] A real but small browser engine — NetSurf has its own layout
      engine and a framebuffer front end, which makes it the honest
      candidate: it is a browser, it is not a toy, and it is four orders
      of magnitude smaller than Chromium
- [ ] **The measurement, written down as the milestone's deliverable.**
      What Chromium's build actually asks for on this machine, in
      numbers: disk, RAM, syscalls it uses that this kernel does not
      have, and what its GPU and sandbox layers assume. Not an estimate
      — a list produced by trying and reading the errors, which is this
      project's method applied to the question "how far away is it"

**How we'll know.** A page fetched over TLS, laid out, and drawn on this
compositor by an engine nobody here wrote. And a written gap analysis
for Chromium with a number next to every line — which is a deliverable
this file can grade, unlike "port Chrome", which it cannot.

### What this arc does not answer, and where that gets decided

Three things stay open on purpose, and each one has a milestone that
will hand it a number rather than an opinion:

- **Swap.** M82 deferred it for lack of a measurement and M90 defers it
  again for a better reason. M98 measures a GCC bootstrap's peak
  footprint; if that number is larger than what M90 makes available,
  swap gets a milestone and the measurement is its specification.
  **That milestone is M102,** written so that a bootstrap which fits is
  a complete outcome rather than a milestone with nothing in it.
- **A journal.** M71 and M81 both deferred it. M93 takes the scan-time
  measurement at hundreds of thousands of files, which is the second of
  M71's two conditions, and M92's writeback cache puts real pressure on
  the first. **M93 took it and refused again at 30 ms per hundred
  thousand files. The first condition is M105's**, once M98's parallel
  build is the writer and M104's cache is the thing an unclean mount has
  to recover.
- **A third-party libc.** Named above: the trigger is symbol versioning,
  not volume, and it is not expected before M100. If M100's gap analysis
  says a browser needs a glibc ABI rather than a glibc-shaped surface,
  that is the milestone that turns the decision over — and it will have
  said so with a link error, which is how this project has decided every
  other thing.

**On the size of all this.** The M81–M89 arc's own note said three of
its nine were multi-attempt milestones and that writing them as one each
was a statement about the goal rather than a prediction about landing
them. That is more true here, not less: M91, M95, M97 and M98 are each
larger than anything in that arc, and M97 and M98 are each plausibly
larger than the whole of M75–M79. Eleven headings is the shape of the
work, not its cost.

## And the arc after that: the other half of the goal sentence

*Written after M93 and M86 landed, against the tree rather than against
a wish list — the M94–M100 arc is planned and unbuilt, and nothing below
starts before it finishes.* The goal in `CLAUDE.md` is one sentence with
two halves:

> A Unix-interface-compatible OS that is **stable enough and complete
> enough to build and run somebody else's software against**, on a
> **machine that manages its own CPU, memory and disk honestly**.

M94–M100 is entirely the first half. Every milestone in it is about a
surface a program names or a size a program needs, and its closing test
is somebody else's build system succeeding. None of it touches the
second half, and the second half is where this project has been quietly
carrying two different kinds of debt.

The first kind is **deferrals waiting on a number that M98 and M100
produce by construction.** Every one of them was deferred correctly, by
M69's rule, and every one of them named the measurement that would end
the deferral. That arc takes those measurements as a side effect of
doing something else:

| what M94–M100 produces | the deferral it finally prices | where the deferral is written |
|---|---|---|
| a GCC bootstrap's peak RSS and build-tree size (M98) | **swap** | M82 deferred it for want of a measurement, M90 again "for a better reason", and M94–M100's closing note already says *"if that number is larger than what M90 makes available, swap gets a milestone and the measurement is its specification"* |
| a bootstrap's wall-clock, attributable (M98) | **`syscall`/`sysret`** | the deferred list: *"M98 is the first thing likely to ask... If `int 0x80`'s cost shows up in that attribution, this stops being a deferral and becomes a number"* |
| `make -j` against `MAX_TASKS 128` (M98) | **one scheduler lock** | `sched.c:126` is a single `sched_lock`; `sched.c:499` broadcasts one PIT tick to every core by IPI. Nothing has ever run more than one CPU-bound process on purpose |
| thousands of small writes from a parallel build (M98, M99) | **a journal** | M71's two conditions were *"multiple writers"* and *"a full scan gets slow"*. M93 measured the second at 30 ms per hundred thousand files and refused it again. A parallel make is the first multiple writer this machine has ever had |
| a page fetched over TLS by an engine nobody here wrote (M100) | **window scaling, SACK, Nagle** | same deferred list, same rule: they come back when a measurement asks |

The second kind is larger and older, and it is not a deferral because
nobody ever wrote it down as one. **Every number in this file is a number
about QEMU.** `tests/budgets.tsv`'s header is the only place that says so
out loud — *"measured on QEMU under macOS on Apple Silicon... a claim
about this machine"* — and the M57+ path note said the rest: *"none of
this has run on metal. That is now the single largest claim in the
project."* Fifty-three milestones later it still is, and the audit is
short enough to fit in a table:

| what the machine assumes | where it is written | what a real machine does instead |
|---|---|---|
| interrupts arrive through the legacy 8259 PIC | `kernel/arch/x86_64/pic.c`; there is no `ioapic.c` in this tree | routes them through an I/O APIC, with the MADT's interrupt source overrides deciding which line is which |
| PCI devices raise a pin | `kernel/drivers/pci.h:8` — *"there's no capability list, MSI, or..."* | every PCIe device built since about 2010 raises an MSI or MSI-X, and several of them cannot raise anything else |
| the block device is virtio or ATA PIO | `virtio_blk.c`, `ata.c` | AHCI or NVMe. M92 declined AHCI in exactly these words: *"it is what a real machine needs and it is a second full driver"* |
| the block driver polls | `virtio_blk.c:203-207` — *"This driver polls, and nothing in this kernel is registered on the PCI interrupt this device would raise"* | one completion interrupt per request, which is the only way a parallel build's disk queue is ever more than one deep |
| input arrives on port 0x60 | `keyboard.c:12`, and the mouse beside it | a laptop built this decade has no PS/2 controller at all. The keys come off an xHCI controller through USB HID |
| the network is an RTL8139 | `kernel/drivers/rtl8139.c`, 218 lines | an Intel or Realtek PCIe part with MSI-X and a descriptor ring, and a link that drops |
| a physical machine has never booted this | M28's fourth box, open since M28 | nothing. It needs hands, and it has needed them for eighty-two milestones |

M101–M110 is those two lists, in dependency order.

### Three decisions taken here rather than discovered halfway

**Three of these ten may end in a refusal, and a refusal is a result.**
M102's swap, M105's journal and M101's `syscall`/`sysret` are each
scheduled as *a measurement with a decision attached*, not as work.
If the number says the thing is not needed, the milestone's deliverable
is the number and the entry closes as refused. M65 is the precedent —
it is one of the more useful entries in this file and it built nothing —
and M93's second attempt is the more recent one: it re-measured the
journal at a hundred thousand files and declined it again, which is why
M105 below is allowed to exist without prejudging its own outcome.
Writing three milestones that might not build anything is the honest way
to schedule a measurement; writing them as "add swap" and discovering
halfway that nothing wanted it is how a plan drifts.

**A driver for a standard is not a driver per vendor, and the GPU
refusal is untouched.** AHCI, NVMe, xHCI and MSI-X are published
specifications with one implementation each: the same code drives every
part that implements them, which is the whole reason they exist. A GPU
is the opposite — a driver per vendor per generation, with a
reverse-engineered command stream — and that is the distinction the
"Deliberately not next" refusal has always rested on, stated here
because M107 is the first milestone that could be mistaken for eroding
it. Software rasterization into this compositor's framebuffer remains
the answer, and it remains a slow answer rather than a missing one.

**Metal is the point of this arc and it is scheduled last anyway.**
M110 is the only milestone in this file whose blocker is a physical
object, and pretending otherwise by putting it first would block nine
milestones behind a thing this environment has not had since M28. So it
goes last, its two real prerequisites (M107's devices, M108's NIC) go
where they belong, and **its position is a scheduling artifact rather
than a dependency**: the day there is a machine and a USB stick, M110
jumps the queue. That sentence is here so that running it early is
recorded as the plan working rather than as the plan being abandoned.

### What this ordering assumes, written down so it can be wrong

M101 is first because it is the instrument the four milestones after it
are graded by, and M69's rule — *"performance work on an unmeasured path
doesn't get done"* — applies to this arc's own contents before it
applies to anything else. M103 is third rather than seventh because
interrupts are a prerequisite of both halves of the arc at once: M104's
writeback cache needs completion interrupts to have a queue depth worth
measuring, and every device in M107 and M108 raises an MSI-X or nothing.

The assumption most likely to be wrong is that **M104 and M105 are two
milestones.** A writeback cache and a journal are the same ordering
problem seen from two sides, and if M104's crash test turns out to need
journal-shaped machinery to be correct at all, they merge — under M104's
number, per M68 and M74's precedent for a milestone reopened rather than
renumbered. The second most likely is that **M106 is bigger than M101
makes it look**: per-CPU run queues touch `sched.c`'s 2,157 lines, which
Q13 has already priced as *"larger than the whole of Q1–Q10's host
tier"* and never started.

---

### M101 — Where the time actually goes ✅

- [x] A sampling profiler in the kernel (`kernel/profile/sampler.c`): the
      PIT interrupt records the interrupted RIP into a hash table keyed
      by (rip, pid). Sampling is off until something asks for it, so a
      stopped profiler costs one load and one branch per tick
- [x] Per-syscall accounting (`kernel/profile/syscount.c`): calls always,
      cycles only when asked. There are 97 live entries in
      `syscall_table` and, before this, no idea which of them anything
      actually calls
- [x] `SYS_profile` (98), gated on `CAP_PROCESS_LIST`, plus
      `/proc/profile` and `/proc/syscalls` for a person with a terminal
- [x] `/bin/profile`: `start`, `stop`, `reset`, `report [n]`,
      `syscalls [n]`, `timing on|off`, and `run <secs> <cmd>` which
      brackets one workload rather than sampling whatever the machine
      happened to be doing
- [x] Symbol resolution, in user space, against `/etc/kernel.syms` —
      generated by `tools/gen-kernel-syms.sh` from the linked ELF and put
      on the disk by `make syms`
- [~] **The attribution over an M98 bootstrap: not taken, because M98
      does not exist.** This milestone was built ahead of the arc it
      belongs to. What is measured instead is stated below rather than
      implied, and the box stays half-open until there is a compiler here
      to point this at
- [x] **`syscall`/`sysret`: measured, and still deferred.** The number is
      `syscall_null_cycles`, now a row in `tests/budgets.tsv`

**The two decisions that were taken against the plan.**

**No frame-pointer chain, so a flat profile rather than a call graph.**
Walking one needs `-fno-omit-frame-pointer` on every kernel translation
unit, which changes the code generation of every path this project has a
budget for — all eleven rows of `tests/budgets.tsv`. Buying caller
attribution by perturbing every existing measurement, before any
measurement has asked for caller attribution, is the trade M69 exists to
refuse. The flat profile answers this arc's
question; the day something needs to know *who called*, that is a
milestone with a number behind it.

**Symbols in a file, not in the kernel.** A symbol table compiled into
the kernel changes the addresses it describes — Linux solves that with
two link passes and a fixed-point iteration, and nothing here needs to.
The kernel stores raw addresses and `/bin/profile` resolves them, which
also puts the string handling where a bug is a wrong line of output
rather than a fault in an interrupt handler.

**The measurement.** `int 0x80` costs **1020–1300 cycles** round trip on
this machine, measured with `SYS_getpid` — a call that does almost
nothing, so what is left is the trap, the dispatch and the return. It
varied by 25% across three boots of the same image, which is what a
cycle count on a virtualised TSC looks like.

**The decision that number drives, and it is not the one that was
expected.** The deferred list says `syscall`/`sysret` comes back when a
measurement asks. This measurement does not ask, and it cannot: a
thousand cycles is only meaningful against a workload's total, and the
workload that was supposed to supply the total is M98's bootstrap.
`sysret` would save perhaps 700 of those cycles. Whether that matters
depends entirely on how many syscalls a compiler makes per second, which
is exactly the number this project still does not have. **So the
deferral stands, and for the first time it stands on an arithmetic
statement rather than a principle:** at 1,200 cycles a call, the trap
path costs 1% of a 3 GHz machine at 25,000 syscalls a second. If M98's
build makes fewer than that, `sysret` is not worth a milestone. That
sentence is falsifiable, which the old entry was not.

**What the profiler found on its first run, which is the most useful
thing here.** Pointed at its own self-test, the first report said half
of "kernel time" was `pit_sleep_ms+0x50`. That is the `hlt`. A CPU
halted waiting for the clock is in ring 0, at a real address, in a
function — so a sampler that classifies by CS alone reports a sleeping
machine as a busy one, which is precisely the failure
`kernel/profile/sampler.c`'s own comment warned about one paragraph
before it happened.

The fix took two attempts and the second one is the interesting part.
The obvious signal is the per-CPU `idle_depth` M68 already keeps — and
it is wrong for this. `pit_sleep_ms` brackets a `hlt` loop that the
timer can schedule *away* from, so the next task to run on that CPU
finds the counter still raised and looks idle while it is working. Using
it classified an entire kernel busy loop as idle time, and this
milestone's own self-test caught it within one boot. The counter that
answers "is this task waiting" has to live on the task, so `task_t`
grew `idle_wait_depth` beside the per-CPU one. **Two counters that look
redundant and answer different questions: "is this CPU halted" and "is
this task waiting".**

After the fix the same workload reports 50% kernel, 50% idle, and
`proftest`'s ring-3 busy loop reports 50 user, 0 kernel, 50 idle. The
missing 50% in both cases was always a halted CPU.

**Two bugs found on the way, neither of them M101's.**

**The VFS had no close path at all, and procfs leaked every handle.**
`proc_open` set `used = 1` and nothing ever cleared it, so the
seventeenth open of any `/proc` file on a given boot failed — and every
one after it. It shipped in M87 and survived every milestone since,
because nothing had ever opened one of these files in a loop and
`/bin/profile` is the first thing that does. The fix
is a `close` hook in `vfs_ops_t`, `vfs_handle_close`, and
`openfile_unref` calling it at refcount zero — with the openfile lock
*dropped* first, because `vfs_handle_close` takes `fs_lock` and every
other path in this kernel takes `fs_lock` first. leanfs supplies no hook
and needs none: its handle is an inode index, which is exactly why the
gap existed. Graded by a self-test that opens `/proc/self/status`
twenty-four times, deliberately more than the sixteen-entry table, so
that a "fix" which merely enlarged the table would fail it.

**`make -j8` could silently link a stale program.** `crt0.o` and
`setjmp.o` are built by pattern rules and named only as prerequisites,
which makes them intermediate files that make deletes after the first
link — and with `-j8` the next link races the deletion. The failure is
intermittent and its shape is worse than its frequency: the `.elf`
silently keeps its previous contents and the image boots the program you
edited five minutes ago. It cost a three-minute QEMU run and a wrong
conclusion about a test failure during this milestone. One `.SECONDARY:`
line. **The compounding mistake was mine and is worth recording
separately: `make -j8 2>&1 | grep error:` reports the exit status of
`grep`, so the build looked clean while it had failed.**

**A third bug, found by the fix for the second one.** Putting
`user_space/lib/symtab.c` into the host tier meant putting it into the
coverage report, and `llvm-cov` prints each file with the longest common
prefix of everything on its command line removed. One source from a
second top-level directory therefore *renamed every other file in the
report* — `net/ip.c` became `kernel/net/ip.c`. Every row in
`tests/coverage-floor.tsv` stopped matching, all ten files were reported
as "new", and the ratchet printed **"nothing fell"**. It had stopped
enforcing anything and said so in words that read like success. That is
Q11's own failure mode one level up: an instrument that cannot fail.
`tools/coverage-ratchet.py` now exits 1 when a floor row names a file the
report does not contain — checked by drifting a name on purpose and
confirming the exit code, because a check added in response to a silent
failure should not be taken on trust.

**The one red result, and why it is not this milestone's.** `--full`
fails one of fifty interactive tests with a boot timeout, and it is a
different test each run: `start_button_opens_launcher` once,
`overlap_click_reaches_the_front_window` the next. Both pass in
isolation in 14 s. `/tmp/leanos-input-failures` holds seven such logs
from before this milestone existed, stalling in the same region of the
boot. And the decisive run: **`--jobs 2` passes 50/50**, in 678 s rather
than 843 s. Four guests at once oversubscribes this machine, which makes
the suite both flakier and slower — a finding for the test arc rather
than for M101, and recorded here rather than re-run until green. The
commit tier, which is what gates a commit, passes.

**What it cost.** 1,545 new lines across eleven new files, plus about
820 lines of edits to nineteen existing ones:
`sampler.c`/`syscount.c` and their headers,
`system_api/include/profile.h`, `user_space/lib/symtab.c`,
`/bin/profile`, `/bin/proftest`, `tools/gen-kernel-syms.sh`, and edits to
the timer IRQ, the syscall dispatch, procfs, the VFS, openfile and the
scheduler. **48 KiB of kernel BSS** for the histogram (2048 buckets at 24
bytes) and **1.5 KiB** for the syscall table — both read out of the
linked ELF rather than estimated. No budget moved: boot to desktop 186 s
against 192 s at `b7bb520`, and every input-to-photon row is inside its
ceiling.

**How it is graded.** Six boot markers, eleven host tests, one budget
and one coverage floor.

- `tests/test_symtab.c` — the first user-space code in the host tier, and
  it earns the place: the resolver's failure mode is a *plausible wrong
  answer*, a report attributing every sample to the function before the
  right one. Booting the machine cannot catch that, because there is
  nothing in there to compare against. Eleven tests, mostly boundaries.
  Four mutations were run against them by hand — the `<=` in the binary
  search, the end-of-text guard, the below-first-symbol guard and the
  capacity ceiling — and all four were caught.
- `kernel.c`'s `selftest_profile` — samples land, every kernel one is
  inside `.text`, the classes add up, stopping stops it, a hundred
  `getpid` calls move that counter by a hundred and nobody else's, timing
  is off by default and real when on, `/proc` survives twenty-four
  open/close cycles, and `/bin/profile` produces a report into the boot
  log.
- `/bin/proftest` — the half that only ring 3 can prove. It exists
  because `user_range_ok` short-circuits for a caller on the kernel's own
  PML4, with a comment above it saying a garbage-argument matrix run from
  `kernel_main` "would prove nothing". The first version of this
  milestone's pointer checks ignored that comment and panicked a machine
  that was behaving correctly.

**And one instrument that graded itself.** Q5's negative syscall suite
failed the first boot after `SYS_profile` was added — not on behaviour,
but because its table did not classify syscall 98. A census that fails
when a syscall is added is the design working. Its entry now also
records what it does *not* prove: `syscalltest` holds no capabilities, so
every `SYS_profile` call in that sweep is refused at the gate before an
argument is examined, which is a vacuous pass. That is why `proftest`
exists.

**One finding handed forward rather than acted on.** `sched_current()`
is `current_task[smp_current_cpu()]`, and `smp_current_cpu()` reads the
Local APIC's ID register over MMIO and then scans the CPU table. Nearly
every syscall calls it, several of them more than once, and nothing has
ever measured what that costs — it is why M101's syscall accounting uses
a `lock xadd` on a shared table rather than the obvious per-CPU one,
because finding out which CPU you are on is more expensive here than the
atomic it would save. The fix is a per-CPU data pointer in a segment
base, which is scheduler work and belongs to **M106**, not to the
milestone that noticed. Two of these reads were merged into one in
`sched_idle_enter` on the way past, because they were adjacent.

**What is still open.** The attribution this milestone was written to
produce needs a workload worth attributing, and this machine does not
have one yet. 100 Hz is the sampling rate because the PIT is the
machine's timebase; M103's per-CPU LAPIC timer is the honest place for a
faster one. There is no symbol file for user programs, so a ring-3
sample is reported as a pid and an address — resolving those needs the
same generator pointed at each `.elf`, which is small work waiting for a
reason.

### M102 — Memory that runs out honestly

- [ ] `pmm_alloc_frame` stops halting the machine. `pmm.c:297` is
      `panic("pmm_alloc_frame: out of physical memory")` and it is
      reachable by any process that allocates in a loop — the test
      arc's own audit called it *"on a desktop that is a bug; on
      anything called production it is the whole question"*
- [ ] A failure path that goes all the way up: the frame allocator, the
      heap, the address space, `mmap`, `fork`'s copy, and the syscall
      that started it, each returning rather than panicking, and each
      with a host test that reaches it. This is where the rest of the
      production `panic()` count from that audit — about forty, once
      `kernel.c`'s self-test assertions are excluded — gets audited
      rather than counted
- [ ] An OOM policy that names a victim and says why: the process that
      asked, not a heuristic invented here, and a log line that records
      the decision. The compositor and PID 1 are not candidates, and
      the machine stays up
- [ ] **Swap, or the number that refuses it.** M98's peak RSS against
      what M90 makes available. If a bootstrap fits, this milestone
      ships the failure paths and records the margin; if it does not,
      swap to a leanfs file is specified by exactly that measurement —
      a clock hand over the page tables' accessed bits, an eviction path,
      and a `PTE` that faults a page back in
- [ ] This collects **Q9**, which has sat unstarted since it was written

**How we'll know.** A process that allocates until it cannot, run in a
loop, with the desktop still responding to a click at the end of it and
the boot markers still green on the reboot after. And the swap decision
stated as a number with the two sides of it named — what a bootstrap
peaks at, and what this machine has.

### M103 — Interrupts a real machine delivers

- [ ] An I/O APIC: parse the MADT's I/O APIC and Interrupt Source
      Override entries — `acpi.c` already walks that table for the
      LAPIC base and the APIC ids, so this is one more entry type — and
      route the legacy lines through it instead of `pic.c`, with the PIC
      masked rather than deleted. The overrides are the part that is not
      optional: the timer is routinely not on the line the machine
      thinks it is
- [ ] MSI and MSI-X: the PCI capability list `pci.h:8` says out loud
      does not exist, vector allocation, and per-device masking. A
      modern NIC or NVMe controller has no other way to interrupt
- [ ] A per-CPU LAPIC timer as the tick source, replacing the single PIT
      tick that `sched.c:499` broadcasts by IPI. One core's timer
      interrupt driving every other core's scheduling decision is a
      thing that works and is not what a real machine does
- [ ] `virtio_blk` registered on its own interrupt, and the polling loop
      at `virtio_blk.c:277-284` retired — the driver's own comment says
      the `NO_INTERRUPT` flag is *"the flag a polling driver owes the
      device"*, which is the comment that gets deleted here
- [ ] Interrupt statistics per vector per CPU, in `/proc`, because an
      interrupt that stops arriving is otherwise indistinguishable from
      a device that has nothing to say

**How we'll know.** The whole self-test battery green with the PIC
masked and every line arriving through the I/O APIC, and again on
`QEMU_DISK=ide`, which routes a different set of lines. A `/proc`
counter proving the disk's completions are arriving as interrupts on
more than one CPU rather than being polled by whoever asked. And the
input-to-photon budgets re-measured, because moving the tick source is
exactly the kind of change M69's numbers exist to catch.

### M104 — Writeback, and a disk that keeps up with a build

- [ ] The writeback cache M92 deferred, over M103's completion
      interrupts: dirty tracking, a flush deadline, and a real queue
      depth rather than one request at a time. M92's own note deferred
      this *for want of a measurement*, and M98's build is the
      measurement — a bootstrap writes hundreds of thousands of small
      files and waits for every one of them today
- [ ] Ordering that survives the cache: the write-ordering guarantees
      M71 bought at the filesystem layer have to be re-established
      against a cache that reorders by design, or M71's atomic replace
      quietly stops being atomic
- [ ] `fsync`/`fdatasync` that mean something, and the `O_SYNC` path a
      build's temporary files should *not* be taking
- [ ] Readahead, sized by measurement rather than by taste: a compiler
      reading a header tree is the most sequential workload this machine
      has ever had
- [ ] `tools/crash-test.sh` extended to cut the power with a dirty cache
      rather than with a synchronous write in flight — which is a
      strictly harder test than the sixteen cuts M71 and Q17 survived,
      and the reason this milestone is not just a performance one

**How we'll know.** The build wall-clock from M101's attribution, before
and after, with the disk's share of it named in both. And the extended
crash test: sixteen more cuts, at the point of maximum dirty data, each
one followed by an independent reader that finds a consistent
filesystem. A cache that makes the build faster and the crash test
flaky has failed this milestone, not passed it with a caveat.

### M105 — The journal, or the measurement that refuses it a third time

- [ ] Re-measure M71's two conditions with M104's cache in place and a
      parallel build running. The first condition — *"multiple
      writers"* — is met for the first time by `make -j`; the second was
      measured by M93 at 30 ms per hundred thousand files and refused.
      Both get taken again, together, because M104 changes what an
      unclean mount has to reconstruct
- [ ] **If the numbers say no, this milestone is that paragraph and
      nothing else,** and the deferred entry gets its third condition
      written down rather than its second restated. That is a complete
      outcome and it is the likelier one
- [ ] If they say yes: a metadata journal in leanfs — a circular log, a
      transaction around every multi-block metadata update, replay at
      mount, and a checksum that decides whether a transaction was
      complete. Metadata only, and deliberately: data journalling is a
      second decision needing a second measurement
- [ ] Either way, the mount-time number that decides it goes into
      `tests/budgets.tsv` as a row, so the third re-measurement is a
      regression rather than an investigation

**How we'll know.** A number, and a decision attached to it. If a
journal is built: `tools/crash-test.sh` with cuts placed inside a
transaction rather than around one, and a mount that replays rather than
scans — proven by the replay being *faster* than the scan it replaces on
a filesystem with a build tree in it, because a journal that costs more
than the scan is a journal that failed.

### M106 — Cores a build can use

- [ ] Per-CPU run queues, replacing the single `sched_lock` at
      `sched.c:126` that every scheduling decision on every core
      currently serializes behind
- [ ] Work stealing or a balancer — whichever M101's profile says the
      contention actually wants — and the measurement that chose it
      recorded here rather than the reasoning that predicted it
- [ ] `MAX_TASKS 128` and `MAX_FDS 128` (`sched.h:213`, `sched.h:120`)
      raised to whatever `make -j` and a linker actually ask for. The
      M94–M100 arc's own table lists both as numbers that want more; the
      new numbers are set by the failure, not by rounding up
- [ ] CPU affinity, at least enough that the compositor is not migrated
      off a warm cache mid-frame, and `sched_setaffinity`-shaped access
      to it for anything that asks
- [ ] Per-CPU idle accounting that is real, so the task manager stops
      reporting a number derived from one core's view of the world

**How we'll know.** `make -j N` for N from 1 to the core count, with the
speedup curve recorded. A curve that flattens is a finding, not a
failure — but it has to be *attributed*, by M101's profiler, to a named
lock or a named serialization rather than shrugged at. Plus the input
budgets under a full parallel build, which is the first time this
machine has had a genuinely CPU-saturated background load and the
honest test of whether the desktop survives one.

### M107 — The devices a real machine has

- [ ] **AHCI**, the driver M92 named and declined: port command lists,
      a command FIS, NCQ deep enough to matter, and hotplug ignored on
      purpose. Under `blk.c`'s existing abstraction, beside virtio and
      ATA rather than instead of them
- [ ] **NVMe**, which is the disk a machine built in the last five years
      actually has: admin and I/O queue pairs, MSI-X completions from
      M103, and a namespace enumerated rather than assumed
- [ ] **xHCI plus USB HID**: enumeration, control and interrupt
      transfers, a boot-protocol keyboard and a boot-protocol mouse.
      `keyboard.c:12`'s port 0x60 has no counterpart on a modern laptop,
      and an OS that cannot be typed at on the machine in front of you
      is a demo whatever else is true of it
- [ ] Every one of them graded **under QEMU first** — `-device ahci`,
      `-device nvme`, `-device qemu-xhci` — so the drivers are debugged
      here and only their assumptions are debugged on metal. This is
      what makes M110 a boot rather than a bring-up
- [ ] `QEMU_DISK=ahci` and `QEMU_DISK=nvme` join `ide` and `virtio` as
      configurations the harnesses are expected to pass, on the same
      terms `CLAUDE.md` already sets: supported paths, not fallbacks,
      with a byte-identical image

**How we'll know.** The full self-test battery green four times over,
once per storage backend, with the disk budgets recorded separately for
each — four numbers, not one, because the point of this milestone is
that they differ. And the input suite passing with the PS/2 devices
absent from the QEMU command line entirely, which is the only way to
prove the USB path is carrying the keys rather than sitting beside a
PS/2 driver that still works.

### M108 — A real NIC, and the TCP deferrals it prices

- [ ] A driver for a NIC that exists in physical machines — Intel
      `e1000e`/`igb` or Realtek `r8169` — with descriptor rings, MSI-X
      from M103, and multiple queues if the part has them. `rtl8139.c`
      is 218 lines and QEMU is the only place that chip is common
- [ ] Link state that changes: carrier up and down, a cable pulled, DHCP
      renewal against a real router rather than QEMU's user-mode stub,
      and an address that expires
- [ ] **Window scaling, SACK and Nagle if and only if M100's TLS fetch
      asks for them.** The deferral's condition is a measurement, and a
      browser pulling a real page over a real link at real latency is
      the first one this stack has ever had. A 64 KiB window on a link
      with 30 ms of round trip is a number, and the number decides
- [ ] Whatever the real link breaks that QEMU never did: MTU discovery,
      a middlebox, retransmission against a loss rate that is not zero.
      The list is a prediction; the capture is the specification

**How we'll know.** A page fetched over TLS from a host nobody here
controls, over a physical link, laid out and drawn by M100's engine.
Then the throughput number on that link, with and without whichever of
the three deferrals the measurement selected — and if the measurement
selects none of them, that sentence, with the number that says so.

### M109 — lean_os built on lean_os

- [ ] The kernel compiled by the compiler M98 put on this machine:
      `kernel/`, `system_api/`, `user_space/`, `tools/`, the linker
      scripts, the NASM sources, the whole tree, with `make` running
      here rather than on a Mac
- [ ] The EFI application too, which is the awkward one —
      `kernel/boot/uefi/boot.c` is built with `clang`+`lld` into a PE32+
      object today, and a self-hosted tree needs binutils' own PE
      support or an equivalent. This is the box most likely to be the
      one that fails
- [ ] The image assembled here: `tools/leanfs-put`, the host-side image
      builder M93 added, and `mtools`' job done by something on this
      machine — written for the target rather than shelled out to
      somebody's package
- [ ] Written to a second disk and booted from it, which is a thing
      QEMU can be handed two of, and the self-test battery run on the
      result
- [ ] **The generational test**, which is the one that makes this
      falsifiable rather than sentimental: the self-built machine builds
      a third image, and images two and three are **byte-identical**.
      Same argument as M98's three-stage bootstrap and for the same
      reason — a machine that can build a working copy of itself but not
      a *stable* one has a miscompilation or a nondeterminism in it, and
      nothing weaker than bit-for-bit equality finds either

**How we'll know.** `sha256` of image two equals `sha256` of image
three, and image three boots to a desktop and passes every marker and
every budget. If the two differ, the diff is the milestone's real
output — this project has never had a reproducibility instrument, and
the first thing one finds is usually a timestamp somebody embedded on
purpose. Failing this test with a named cause is a good outcome; passing
it is the end of the sentence M72 started when it said some of what
needed a cross-compiler needs a script instead.

### M110 — The boot that has never happened

- [ ] M28's fourth box, open since M28 and the oldest unfinished line in
      this file: write `build/os-image.bin` to a USB drive with the
      `tools/write-usb.sh` that has been waiting for it, and boot a
      physical x86-64 UEFI machine from it
- [ ] The self-test battery on that machine, and a **second column in
      `tests/budgets.tsv`**: every one of its eleven rows measured again
      on metal. The header already says the numbers are a claim about
      one machine; this is the milestone that stops that being the only
      claim available
- [ ] Everything the firmware does differently, recorded as it is found:
      the memory map's reserved regions — M29's audit already named the
      class, *"never an issue on QEMU, routine on real hardware"*, and
      fixed the one instance of it anybody could find from here — the
      framebuffer mode the firmware chose, ACPI tables written by
      somebody who was not QEMU, an `_S5` inside a method that
      `power.c`'s narrow DSDT reader cannot follow, and whichever of
      M103's interrupt source overrides turns out to matter
- [ ] `docs/real-hardware.md` corrected from the boot rather than from
      the plan — it is a runbook written by someone who has never run it,
      and the difference between those two documents is the deliverable
- [ ] A photograph in the repository, which is not evidence and is not
      pretending to be. The serial log is the evidence

**How we'll know.** The machine in front of you boots to its own
desktop, the self-tests run to completion over the serial cable, and
eleven budgets have a metal column. Anything short of that is written
down as what it was — this is the one milestone in this file where a
partial result is genuinely worth more than a delay, because a boot that
gets to the framebuffer and hangs on the disk is a bug report, and
eighty-two milestones of *not knowing* is not.

### What this arc does not answer

- **Multi-user is still not next, and nothing here changes that.**
  M65's condition — two people sharing a machine — is not met by a
  machine that compiles its own kernel, and inventing a second principal
  because the machine got more capable would be exactly the lie M65
  refused. Recorded here because an arc that puts a compiler and a
  browser on a box is where the temptation shows up.
- **A GPU is still not next**, for the reason stated above rather than
  a restated one: these are drivers for standards, and that is a
  different category from a driver per vendor per generation.
- **A third-party libc is still not next.** M100's gap analysis is where
  that decision gets handed over, and this arc does not front-run it.
- **Nothing here schedules a second architecture.** `x86_64-lean_os` is
  a triple with one machine behind it, and it stays that way until
  something asks — which is the same discipline as everything else in
  this section, applied to the one place where a self-hosting compiler
  makes the work look cheap.

**On the size of all this.** Six of these ten are measurements with a
decision attached, and three of those may build nothing — which makes
this arc cheaper than M94–M100 and much harder to predict. The two that
are certainly large are M106 and M107: per-CPU scheduling touches the
file Q13 has been avoiding for two arcs, and M107 is three device
drivers where this project has previously done one per milestone. M110
is the smallest of the ten and the only one that cannot be started at
this desk.

## The arc alongside all of it: tests that can fail

*Written after M93, from an audit of the two harnesses rather than from
a wish list.* Every milestone in this file has ended with a self-test,
and that discipline is why M40's lesson (a bug hidden from every
protocol-level check by the absence of a real click) was learned once
rather than repeatedly. This arc does not question that discipline. It
questions what the harnesses can currently **fail on**, which is a
smaller set than the number 79 suggests.

These are numbered Q1-Q10 rather than M101-M110 on purpose. They are
orthogonal to the M94-M100 arc and mostly cheaper than any single
milestone in it, and several of them are things that arc will need
before it starts: a GCC bootstrap is the first workload here whose
failures will be *intermittent*, and an intermittent failure against a
harness with no CI, no reruns and no history is not a bug report, it is
a rumour.

### What the audit found, with the line that shows it

| the gap | where it is | why it matters |
|---|---|---|
| the serial harness always sleeps its full budget | `tools/qemu-serial-test.sh:196` — `sleep "$SECONDS_TO_RUN"`, unconditional | boot is measured at ~140s against a 400s budget, so every run donates ~260s. The header at line 86 says *"the run stops as soon as the last marker appears"*. It does not. That sentence is the only untrue one in the file |
| markers are presence-only | `REQUIRED_MARKERS` is 79 `grep -qF` calls | no ordering, no counts, no *absence* assertions beyond two. A self-test that prints its pass line and then corrupts the machine passes |
| performance is printed, never asserted | `kernel/kernel.c:1734` (m92 disk), `kernel/kernel.c:7352` (m69 input-to-photon) | both emit real microsecond figures; the harness greps the prefix. Disk could get 100x slower and the run is green. M69's whole point was that measuring is how this project decides — and the measurement is not wired to a verdict |
| zero host-side unit tests | ~69,000 lines of C; the only `HOSTCC` targets are `gen-font` and `leanfs-put`, both tools | every check in this project costs a QEMU boot. There is no tier below "boot the operating system" |
| no `make test` | `Makefile` targets: `all run leanfs-put preseed font font-check clean` | the two harnesses are remembered, not invoked |
| no CI | no `.github/`, nothing scheduled | 83 commits, every one graded by a human who remembered to run two scripts |
| no visual baseline | the input suite reads single pixels: `s.px(*probe) == TOAST_ERROR_C` | a font metric, a padding value or a colour that changes anywhere other than a probe point is invisible. "Visual" here means 45 spot checks, not a frame |
| two copies of the on-disk format | `tools/leanfs-put.c:80-107` redeclares the superblock, inode and dirent structs; it does not include `kernel/fs/leanfs.h` | its own comment says *"The structs below still have to match kernel/fs/leanfs.c byte for byte"*. Nothing checks that they do |
| the parsers are never fed garbage | `arp.c:88`, `ip.c:194-205` do bound their input — verified, and written carefully | the checks are good and completely untested. Nothing has ever handed this stack a malformed frame |
| 219 `panic()` calls, some reachable by exhaustion | `pmm.c:297` — `panic("pmm_alloc_frame: out of physical memory")` | a user process that allocates until it cannot halts the machine. On a desktop that is a bug; on anything called production it is the whole question. **Corrected later:** 178 of those 219 are assertions inside `kernel.c`'s own self-tests, which is where a panic belongs. The production count is about forty — see the correction at the head of the Q11–Q19 arc |
| four files nothing names | `procfs.c` (388 lines), `virtio_blk.c`, `pci.c`, `lapic.c` have zero mentions in `kernel.c`; `devfs.c`, `udp.c`, `ethernet.c` have one | M92 shipped a virtio driver whose self-test exercises it only through leanfs |
| 96 syscalls, no negative suite | `syscall_table` in `syscall.c:4254`; `copy_from_user` has 9 call sites | the happy path of each is covered by whichever milestone added it. Nothing systematically hands all 96 a null, a kernel address, or a length of `2^63` |

Two things this audit did **not** find, and both are worth recording
because the opposite was expected. The network parse layer is
defensively written throughout — every length check that should be there
is there. And `-Werror` with `-Wall -Wextra` has been on since the
beginning, for kernel and user code both, which is a quieter form of the
same discipline as the self-tests.

### One decision taken here rather than discovered halfway

**A unit test is not a lesser self-test, and the boot self-tests do not
go away.** The obvious objection to Q2-Q5 is that this project's method
is to prove things on the real machine, and a host-compiled
`kernel/fs/leanfs.c` running against a RAM disk is not the real machine.
That is true and it is not the trade being made. The boot self-tests
prove *integration* — that leanfs works when it is mounted on a real
device, under a real scheduler, on a real disk driver — and they will
keep doing that unchanged. What they cannot do is get near an error
path, because reaching "the disk is full" from inside a booted OS means
filling the disk. Every error branch in this kernel is currently
unreachable from any test, and the number of them is in the hundreds.
Host tests exist to reach those, and for one other reason: a check that
takes 200 milliseconds is a check that runs while you type.

---

### Q1 — A test command, and a clock on it ✅

The cheapest milestone here and the one everything else is measured
against. Nothing new is proved; the existing proofs get faster and get a
name.

- [ ] `make test` in three tiers, with the time each takes printed at
      the end: `make test-fast` (host units, seconds), `make test`
      (serial harness + the `--quick` input subset, minutes),
      `make test-full` (everything, tens of minutes)
- [ ] The serial harness watches the log for its last marker and stops,
      instead of sleeping the budget. `SECONDS_TO_RUN` becomes a real
      timeout rather than a schedule. Delete the sentence at line 86
      that already claims this, or make it true — this milestone makes
      it true
- [ ] Both harnesses print their own wall-clock at the end, and append
      it to `build/test-history.tsv` alongside the commit. The boot
      already prints its time; nothing has ever collected one
- [ ] A `--rerun-failures` flag on the input suite, and a recorded
      count of how often a rerun changes the verdict. That number is the
      flake rate, and this project does not know it

**How we'll know.** `make test` from a clean tree on a machine that has
never seen this repo. And a serial run that finishes in the ~140s it
takes rather than the 400s it is given, with the difference visible in
`test-history.tsv`.

### Q2 — The first unit test this project has ever had ✅

Establishes the seam and the harness. Deliberately starts with the three
easiest units, because the point of this milestone is the mechanism, not
the coverage.

- [ ] `tests/` built by `HOSTCC`, no framework, no dependency — a
      `CHECK(cond)` macro and a runner that reports counts, in the same
      spirit as everything else here being written rather than fetched
- [ ] `kernel/lib/libk.c` compiled unmodified against it: every string
      and memory function against overlapping ranges, zero lengths,
      and the boundary cases that `-Werror` cannot see
- [ ] `kernel/mm/heap.c` against a fake `pmm`/`vmm`: allocation,
      free-list coalescing, growth, alignment, and the fragmentation
      pattern (alloc-alloc-free-first-alloc-larger) that a first-fit
      list gets wrong if it gets anything wrong
- [ ] `kernel/mm/pmm.c`'s bitmap logic against a synthesised e820 map,
      including the M90 case that motivated it — a usable range above
      the 4 GiB mark — which currently requires QEMU to reproduce
- [ ] The fakes live in `tests/fakes/` and are the deliverable as much
      as the tests are: a `panic()` that longjmps to the runner and
      records the message, so a test can assert *that* the kernel
      panicked and with what, which no test in this tree can do today

**How we'll know.** `make test-fast` runs in under two seconds and fails
if a single character of `libk.c` is corrupted. And the panic-catching
fake proves itself by asserting `pmm_free_frame`'s double-free panic —
the first time that branch has ever been executed by a test.

### Q3 — leanfs against a RAM disk, and one format instead of two ✅

`leanfs.c` is 2,336 lines behind a five-function block interface
(`kernel/drivers/blk.h`). That interface is the seam; this milestone
uses it.

- [ ] `kernel/fs/leanfs.c` compiled for the host against a fake `blk`
      layer backed by a `malloc`'d image, with `blk_read`/`blk_write`
      counted so a test can assert *how many* device operations an
      operation costs — the first time this project can regress an
      algorithm rather than an outcome
- [ ] The error paths, none of which any test reaches today: a full
      data region, a full inode table, a full directory, a file at the
      4 GiB ceiling, an exhausted indirect and double-indirect chain,
      an unlink that must return every block it took
- [ ] The corruption paths: a bad magic, a superblock claiming
      impossible geometry, an inode pointing outside the data region, a
      directory entry pointing at a free inode, a cycle in the
      directory tree. Every one should be a refusal, and M71's mount
      check is the existing half of this
- [ ] **One format, not two.** `tools/leanfs-put.c` includes
      `kernel/fs/leanfs.h` and deletes its hand-copied structs, or — if
      the freestanding header genuinely cannot be included by a host
      tool — a `_Static_assert` per field offset in both files, plus a
      differential test that builds an image with the host tool and
      mounts it with the kernel's own code
- [ ] A property test: N random sequences of create/write/unlink/rename
      against both the filesystem and a `malloc`'d model of what the
      filesystem should contain, compared after every step, with the
      failing seed printed

**How we'll know.** The differential test catches a deliberately
introduced one-byte change to `leanfs_inode_t`'s padding in
`leanfs-put.c` — the exact drift its comment worries about. And a
property run of 10,000 operations with no divergence, with the seed
recorded so the run is repeatable.

### Q4 — The stack, fed garbage on purpose ✅ (fuzzers land; the TCP state table does not)

The parsers are already careful. This milestone's job is to find out
whether "careful" is true, and to keep it true.

- [ ] `arp.c`, `ip.c`, `icmp.c`, `udp.c` and `tcp.c` compiled for the
      host against a fake `net`/`rtl8139` that captures what would have
      been transmitted, so a test can assert the *reply* to a frame and
      not merely the absence of a crash
- [ ] A libFuzzer target per parser — `clang` is already a build
      dependency for the EFI app, so this costs no new tool — run with
      ASan and UBSan, seeded with real captures from a `nettest` run
- [ ] The corpus and any crashing input checked in under
      `tests/corpus/`, which is how a fuzz finding becomes a permanent
      regression test rather than an afternoon
- [ ] `tcp.c`'s eleven-state machine driven directly: every legal
      transition, and every *illegal* one — a RST in each state, a SYN
      in ESTABLISHED, an ACK for data never sent, sequence numbers
      wrapping through zero, an out-of-order arrival, a retransmission
      the peer already acknowledged. This is the single most complex
      piece of logic in the tree and it is covered today by one
      handshake and 16 KiB of payload
- [ ] The option parser at `tcp.c:699` specifically, which is the one
      place a length field from the wire drives a loop

**How we'll know.** Twenty-four hours of fuzzing across five targets
with no crash and no sanitizer report — and if there *is* one, it is a
finding this project is better off owning, which is the honest version
of "how we'll know". Plus a state-transition table where every cell is
either a test or an explicit "cannot happen, because".

### Q5 — Every syscall told a lie ✅

96 entries in `syscall_table`, each covered on its happy path by the
milestone that added it, and none covered on the path a hostile or
merely buggy program takes.

- [ ] A user-space program that walks the syscall table and calls every
      entry with: a null pointer, a kernel address, an unmapped user
      address, an address that straddles a mapping boundary, a length
      of `2^63`, a negative fd, an fd of `MAX_FDS`, and every flag bit
      set. Every one must return an error. None may panic, and none may
      succeed
- [ ] The results as a table in the boot log, so the harness fails on
      a count rather than on a marker — *"96 of 96 syscalls refused
      every malformed argument"* — which makes a newly added syscall
      that forgets validation a failure by default rather than by
      someone remembering
- [ ] `copy_from_user`/`copy_to_user` audited against that table: 9 and
      31 call sites today against 96 syscalls, so the gap is either
      syscalls that take no pointers or syscalls that dereference one
      directly. The deliverable is knowing which
- [ ] The capability boundary tested the same way: `captest` exists and
      proves the model works; this proves it cannot be walked around,
      one capability and one syscall at a time
- [ ] A cross-process test: a pointer valid in *another* process's
      address space, passed by this one

**How we'll know.** The table prints 96 of 96 and the machine is still
running afterwards. The interesting outcome is the first run, which is
unlikely to print 96.

### Q6 — Numbers that fail, not numbers that print ✅

M69 established that this project measures before it decides. The
measurements exist. Nothing grades them.

- [ ] A budget file — `tests/budgets.tsv` — with a name, a threshold
      and the commit the threshold was measured at, for: boot to
      desktop handoff, input-to-photon idle and loaded, 1 MiB disk read
      cold and warm, cache hit ratio, `int 0x80` round trip, context
      switch, `spawn`+`exit`, and compositor frame time
- [ ] The self-tests emit a machine-readable line each
      (`[perf] name value unit`), and the harness fails when one exceeds
      its budget — with the budget, the measurement and the commit that
      set the budget all printed, so a failure says *"disk got 3x
      slower since b7bb520"* rather than *"something is wrong"*
- [ ] Budgets are ceilings with stated headroom and a recorded
      measurement, in the same discipline `SECONDS_TO_RUN`'s comment
      history already applies to itself — and a budget that has been
      2x slack for ten milestones is a bug in the budget
- [ ] The history appended to `build/test-history.tsv` and plotted by a
      script, because a trend is the thing a threshold cannot see: ten
      commits each 5% slower pass every check
- [ ] `QEMU_MEM=128` and `QEMU_DISK=ide` get their own budget rows.
      The README calls them configurations the harnesses are expected to
      pass in; today neither harness runs them

**How we'll know.** A deliberate 50 ms `pit_sleep_ms` inserted into the
compositor's frame path fails `make test` by name. And the ide/virtio
rows differ by roughly the 95ms-vs-5.6ms the README already quotes,
which is the first time that sentence has been under test.

### Q7 — Pixels, all of them (partly landed: the invariant, not the baselines)

*Started ahead of its place in the arc, because a person reported the bug
it catches.* The full golden-frame harness below is still unwritten. What
exists is the half that turned out to matter most, and it arrived the way
most things here do - from a real complaint.

**The bug.** "When I launch an app, the desktop screen jumps/flickers a
bit." It does, and it had nothing to do with the compositor. A process's
stdout is `FD_STDOUT`, which is klog, which is `console.c`, which paints
glyphs directly into the framebuffer the compositor is composing into -
and when the console cursor reaches the bottom row, `console_putc`
**scrolls the entire framebuffer up by one text row**. The compositor
repaints on its next frame and puts everything back, so the whole event
is one frame long: the desktop jumps sixteen pixels, a line of kernel log
appears across the taskbar, and it is gone.

Launching was merely the commonest trigger. The compositor calls
`session_save()` whenever the window layout changes and settles, and that
writes `[wm] session: N window(s) saved` to its own stdout - so opening,
closing, moving or minimising any window did it.

**The fix.** The screen has one owner, and after the handoff it is not the
kernel. `SYS_fb_map` now calls `klog_release_console()`, and from that
moment klog writes to serial and to M70's ring buffer only. Nothing is
lost - `Console` reads that ring, which is what M70 built it for - and a
panic still paints, because `panic_render` draws through `fb_fill_rect`
and the raw font rather than through `console.c`. One-way on purpose: if
the compositor dies, the console does not come back and start scribbling
over the frozen desktop it left behind.

**Why no test caught it, and what does now.** Every one of the 45
interactive tests checks that something *did* change - a window opened, a
button lit, a menu appeared. None could see the opposite failure: that
something changed which had no business changing. A transient that puts
everything back is invisible to a spot probe by construction.

So `assert_stable_outside()` inverts the question. Take a baseline, do the
thing, capture as fast as the monitor allows, and require that no frame
differs from the baseline outside the rectangles the action is *allowed*
to touch. Two tests use it - launching and closing - and both were
verified the only way a regression test is worth anything: they were run
against a deliberately unfixed build first, and both failed there, at
frame 5 of 25, with about 81,000 changed pixels spanning the full width
of the screen.

Three things that were learned writing it, because each is a trap:

- **The first second test passed on the buggy build.** It drove a terminal
  and printed a lot, on the theory that any stdout would do. The theory
  was right and the trigger was wrong - `gui_terminal.c` dup2s its child's
  stdout onto a pipe it reads itself, so `ls` never goes near the console.
  A test that passes for the wrong reason is worse than no test; it was
  replaced with one that closes a window, which does trigger it.
- **Frames must be captured back-to-back with no settle.** The fault
  lasted one frame in twenty-five. Any polling loop with a sleep in it
  steps straight over the thing it is looking for.
- **Remove variables rather than masking them.** The first version failed
  on a single pixel - the mouse cursor leaving screen centre. The fix is
  to park the cursor before taking the baseline, not to add an allowed
  region for it: the allowed list is the part of the test a reader has to
  trust, and every entry in it is a place the test has stopped looking.

**Still to do here** is everything the original milestone listed: golden
frames for a set of canonical states, the diff image as the failure
artifact, `make accept-visuals`, both resolutions, and the font page. The
determinism work those need is real and is named below.

### Q7 (original) — Pixels, all of them

The input suite grades real pixels, which was M40's whole insight, and
it grades 45 of them. This milestone grades the frame.

- [ ] A golden-frame harness: `screendump` to PNG, compared against a
      checked-in baseline with a per-pixel tolerance, for a set of
      canonical states — bare desktop, launcher open, three overlapping
      windows, each application's initial window, the shutdown
      confirmation, a toast, a context menu, the task manager
- [ ] The diff is the artifact on failure: baseline, actual, and a
      highlighted difference image written to `build/visual-diff/`, so
      a failure is looked at rather than reasoned about
- [ ] `make accept-visuals` to re-baseline deliberately, and baselines
      reviewed in the diff like any other change. A visual test nobody
      can update is a visual test everybody disables
- [ ] Every baseline captured at both resolutions M58 supports, because
      a layout that only works at one is exactly the regression this
      catches and exactly the one 45 fixed probe coordinates cannot
- [ ] Font rendering specifically: a page of every glyph at every
      weight the UI uses, baselined. `font-check` proves the *table* is
      consistent; nothing proves the rendering of it is
- [ ] Determinism first, and it is the hard part: the clock in the
      panel, the cursor position, and anything animating have to be
      pinned or masked before a frame comparison means anything

**How we'll know.** A one-pixel change to the titlebar's corner radius
fails, with a diff image that shows the corner. And the suite passes
twice in a row on the same commit, which is the property that decides
whether any of this is usable.

### Q8 — What the tests never touch ✅ (measured; the ratchet is not built)

Coverage, measured rather than argued — the same move M92 made for the
disk and M69 made for latency.

- [ ] `gcov`/`llvm-cov` on everything Q2-Q4 build for the host, with
      the percentage per file printed by `make test-fast` and a floor
      that ratchets upward: a file's coverage may not go down
- [ ] For the QEMU-only code, the cheaper instrument that fits: a
      marker-to-source map that names which file each of the 79 markers
      exercises, published as a table, so *"nothing tests `procfs.c`"*
      is a row rather than an audit
- [ ] First coverage for the four files nothing names: `procfs.c` (388
      lines, and the thing `task_manager` reads), `virtio_blk.c`
      (M92 shipped it and reaches it only through leanfs), `pci.c`, and
      `lapic.c`
- [ ] `devfs.c`, `udp.c` and `ethernet.c` past one mention each
- [ ] The uncomfortable number written down: overall coverage on the
      first run, in the file, next to the date. A number that starts low
      and is honest is worth more than a target nobody set

**How we'll know.** A coverage percentage in `make test-fast`'s output
and a ratchet that fails a commit which lowers it. And the four unnamed
files named.

### Q9 — A machine that runs out of things and stays up

This is the milestone that decides whether "production grade" is a fair
description, and it is the one most likely to change code rather than
add tests.

- [ ] Every resource exhausted deliberately, one at a time, with the
      machine still serving the desktop afterwards: physical memory, the
      kernel heap, `MAX_TASKS`, `MAX_FDS`, pipe buffers, shm segments,
      inodes, data blocks, TCP sockets, the block cache
- [ ] `pmm_alloc_frame`'s `panic("out of physical memory")` becomes a
      failed allocation that propagates. It is the audit's clearest
      example of the class: a panic that is correct for an impossible
      state and wrong for an ordinary one. The deliverable is the
      *triage* into "impossible" and "reachable", and the conversion of
      the second group. **The count in this bullet was wrong when it was
      written** — it said 219, which counted the 178 assertions inside
      `kernel.c`'s own self-tests. The production figure is about forty,
      and the job is correspondingly smaller
- [ ] A leak audit with numbers rather than adjectives: frames, heap
      bytes, task slots, fds and cache blocks recorded at boot and again
      after 10,000 spawn/exit rounds, 10,000 open/close rounds and
      10,000 window open/close rounds. M50 and M54 already do exactly
      this for two resources; this generalises the move they made
- [ ] A soak that runs for 24 hours under continuous input, with those
      counters sampled throughout, and the graph as the artifact
- [ ] SMP stress: `racetest` extended to hammer the shared structures
      the locks guard — the scheduler run queue, the heap free list, the
      pmm bitmap, the net lock, the block cache — with the lock order
      documented in `kernel/lib/spinlock.h` and asserted at runtime under
      a debug build, because `heap.c` already documents its order in a
      comment and a comment cannot fail

**How we'll know.** A program that allocates in a loop until it cannot
gets an allocation failure, exits, and the desktop is still there — the
single clearest before/after in this arc. And 24 hours of soak whose
frame count, free-frame count and task-slot count end where they
started.

### Q10 — It runs without me ✅ (written, never run, then withdrawn — see *Testing is local*)

Last, because everything above needs somewhere to run, and putting this
first would have automated a test suite that could not fail.

- [ ] GitHub Actions with the tiers: `test-fast` on every push (host
      only, no QEMU, under a minute), `make test` on every push to
      `main`, `test-full` nightly, and the Q9 soak weekly
- [ ] The cross-toolchain built once and cached, which is the only
      genuinely fiddly part — `tools/build-ovmf.sh` already solves the
      harder half of the same problem
- [ ] Failure artifacts uploaded and kept: serial log, framebuffer PNGs,
      visual diffs, coverage report, the perf history row. A CI failure
      you cannot see the framebuffer of is a CI failure you rerun
- [ ] Flake tracking: every run's verdict per test recorded, and a test
      whose verdict changes without a code change between them is
      quarantined by name and reported, rather than rerun until green
- [ ] A marker-coverage gate: a new `klog` self-test line that no entry
      in `REQUIRED_MARKERS` covers fails the build. The 79 markers have
      been maintained by hand and by memory for 93 milestones, which has
      worked and will not keep working
- [ ] The README's test section rewritten to say `make test`, and the
      badge counting tests rather than milestones

**How we'll know.** A pull request that breaks the compositor is red
before anyone reads it, with the framebuffer attached. And the flake
rate is a number in a file rather than a thing people have opinions
about.


### What landing Q1-Q10 actually found

*Written after the work, from the runs rather than from the plan.* The
arc above was an audit's list of gaps. This is what appeared once the
instruments existed, which is a different list and a more useful one.

**Five real defects, none of which any existing test could see.** Four
were found by the new instruments; the fifth was reported by a person, and
is now covered by an instrument that did not exist when it was reported.

- **The desktop jumped sixteen pixels every time an app opened.** The
  kernel console was still painting into the framebuffer after the
  compositor took it over, and scrolled the whole screen whenever its
  cursor reached the bottom row. Full account in the Q7 section above,
  including why forty-five interactive tests passed throughout.

- **`tools/leanfs-put.c` had been writing the wrong filesystem since
  M93.** It carried a hand-copied duplicate of the on-disk structs under
  a comment reading *"The structs below still have to match
  kernel/fs/leanfs.c byte for byte"*, and they had not matched for a
  milestone: the tool wrote magic `LFS4`, version 4, 512-byte blocks,
  8192 inodes and a 32 MiB data region at a kernel that had moved to
  `LFS5`, 4096-byte blocks, 131072 inodes and 2 GiB. Every `make preseed`
  since M93 produced an image the next boot silently reformatted — which
  is exactly the failure the old comment predicted. The structs now live
  in `kernel/fs/leanfs_format.h` and both programs include it; there is
  nothing left to keep in sync. Verified end to end: the tool writes an
  image, the kernel mounts it without reformatting, and seeds only the
  programs the tool had not already written.
- **`SYS_shm_create` could panic the kernel from any program.** Its frame
  loop deliberately uses `pmm_try_alloc_frame` and its own comment says
  why — *"a large enough request from a user process could exhaust
  physical memory mid-loop; that has to fail this call, not panic the
  whole kernel"*. The guard was defeated by the line above it: the
  bookkeeping array is a `kmalloc`, and `kmalloc` grows through
  `pmm_alloc_frame`, which panics. `SYS_shm_create(0x7FFFFFFFFFFFFFFF)`
  asked for a 32-petabyte array and walked every free frame in the
  machine before dying in the manner the comment forbids. Found by
  `syscalltest` on its first run. Bounded at `SHM_MAX_SEGMENT_BYTES`.
- **`panic()` took a lock it is documented not to take.** `panic_render`
  is carefully written to allocate nothing and lock nothing — and the
  `klog_puts` two lines above it took `klog_lock` unconditionally. The
  reachable case is the halt broadcast: a panicking CPU NMIs the others,
  an NMI is not blocked by the `cli` that guards that lock, and a core
  interrupted inside it re-enters `klog_puts` and spins forever on a lock
  its own stack holds. It never reaches the `hlt` it was told to reach.
  `klog_enter_panic()` now stops klog locking for the rest of the
  machine's life.
- **`leanfs_init` leaked 16 MiB of contiguous below-4 GiB memory per
  mount.** That is the scarcest allocation in the kernel and the only one
  needing a contiguous run bigger than a page. Latent, because a booted
  machine mounts once — and the corruption tests mount five times, which
  is how it surfaced.

**Two things the audit predicted and got wrong, recorded because being
wrong in public is the point of writing predictions down.**

- The network parsers were expected to yield findings and did not. 14.3
  million fuzzed inputs across six parsers, under ASan and UBSan, found
  nothing; every length check that should be there is there. The gap was
  never the code, it was that nothing had ever exercised it.
- The arc claimed Q1 and Q6 were the two to land if only two could. That
  was wrong: **Q5 was worth more than either**, because it is the one
  that found a way for any program on the machine to halt the kernel.
  Q1 made everything else cheaper and Q6 has yet to catch anything.

**The measurements, now that they exist.**

| | before | after |
|---|---|---|
| `run-qemu.sh` to a usable desktop | ~190 s | **8 s** |
| `qemu-serial-test.sh` wall clock | 400 s (a fixed `sleep`) | **~195 s**, stops when the boot does |
| the fastest available check | a QEMU boot | **0.5 s**, 66 host tests |
| boot markers | 79 | 80 |
| performance numbers asserted | 0 | 5 |
| host-side tests | 0 | 66 (+2 slow) |

**The coverage number, written down because Q8 said it would be.**
Measured over the units the host tier builds, which is not the kernel:
`libk.c` and `heap.c` at 100%, `ethernet.c` 90%, `arp.c` 84%, `icmp.c`
74%, `ip.c` 62%, `leanfs.c` 56%, `udp.c` 28%, and **`tcp.c` at 4%**. That
last one is the honest result of this whole arc: the most complex logic
in the tree is covered by one handshake in a boot self-test and, here, by
nothing except the paths that reject a malformed segment. Q4's
state-transition table is what closes it and it is not written.

### What is left, and where it stopped

Said plainly rather than folded into the checklists above.

- **Q7 (visual regression) is half started**, and the half that landed was
  not the half this list expected. See the Q7 section above: a
  user-reported flicker on app launch turned out to be the kernel console
  scrolling the framebuffer under the compositor, and the instrument that
  catches it — `assert_stable_outside`, which asserts on the pixels
  *nobody is looking at* — is now in the suite with two tests using it.
  Golden frames, the diff artifact and `make accept-visuals` are still
  unwritten. The determinism note was right and held up in practice: both
  tests needed the cursor parked before the baseline, and both had to
  capture with no settle at all.
- **Q9 (exhaustion and the panic triage) is one third done.** The
  `shm_create` fix and the `leanfs_init` leak are two entries from a
  triage of about forty production `panic()` calls that has not been
  performed — not the 219 this arc kept saying, which counted 178
  assertions inside `kernel.c`'s own self-tests. The headline
  item is untouched: `pmm_alloc_frame` still panics on OOM, and
  `tests/test_heap.c` asserts that it does — deliberately, as the
  *before* half of a pair. The 24-hour soak has not been run.
- **Q4's TCP state-transition table is not written.** See the coverage
  number above; this is the largest single gap.
- **Q8's coverage ratchet is not built.** `make coverage` reports; it
  does not yet fail a commit that lowers the number.
- **Q10's CI never ran, and has since been withdrawn.** The workflow was
  written and its assumptions were stated, but a workflow that has not
  executed is a hypothesis, and this one stayed one for its whole life.
  Two assumptions were load-bearing and were never tested: that Ubuntu's
  `x86_64-linux-gnu` cross-compiler produces objects this linker script
  accepts, and that the performance budgets in `tests/budgets.tsv` — all
  measured on QEMU under macOS on Apple Silicon — are survivable on a
  hosted runner. Neither will be answered now. See *Testing is local*.

### What this arc deliberately does not do

- **No test framework is fetched.** `CHECK()` and a runner, for the same
  reason there is no third-party code anywhere else in this tree. The
  fuzzer is the one exception and it is a compiler flag, not a
  dependency.
- **No self-test is deleted.** Q2-Q5 add a tier below the boot tests;
  they do not replace one. The M40 lesson applies with full force to
  host tests too: a unit test of the compositor protocol would have
  passed throughout the bug that milestone exists because of.
- **No coverage target is set in advance.** Q8 measures first and the
  number goes in the file. A target chosen before the measurement is how
  a project ends up testing getters.
- **Static analysis is not here.** `-Wall -Wextra -Werror` has been on
  since the first commit and is doing the work a linter would; adding
  `clang-tidy` before the suite above exists would be sorting the
  warnings on an untested codebase.

**On the order.** Q1 and Q2 are days and everything after them is
cheaper for having been done. Q9 is the only one that will change kernel
code substantially, and Q10 is deliberately last. If only two land, they
should be Q1 and Q6 — a test command, and measurements wired to a
verdict — because between them they turn the harnesses this project
already has into harnesses that can fail.

## The arc after that: tests worth trusting

*Written after Q1-Q10 landed, from what doing them taught rather than
from what planning them predicted.* The last arc's question was "can
these harnesses fail at all", and the answer turned out to be "less often
than the number 79 suggested". This arc's question is narrower and
harder: **how much of what breaks would these tests actually catch?**

It is not a rhetorical question, and the reason it is not is the most
useful thing that happened in the last arc. Q7 needed a second test.
The one written first passed on a build with the bug still in it - it
drove a terminal and printed a great deal, on a correct theory
(a process writing to stdout paints on the desktop) with the wrong
trigger (`gui_terminal.c` sends its child's output to a pipe it reads
itself, so it never goes near the console). It looked like a test. It
asserted something true. It could not fail.

That was caught only because Q1-Q10 had adopted a ritual of running every
new regression test against a deliberately broken build before believing
it. Nothing enforces that ritual, nothing measures how many *existing*
tests would survive the same scrutiny, and there are now 47 interactive
tests, 66 host tests and 80 boot markers that have never been asked.

### One correction to the record, first

The last arc's audit said **"219 `panic()` calls"** and its findings
section repeated the number. That is true of the string and misleading as
a fact: **178 of them are inside `kernel.c`'s own self-tests**, which are
assertions in test code and exactly where a panic belongs. The kernel's
production paths contain about **forty**, and Q9's triage is that much
smaller and that much more tractable than it was made to sound.

Recorded here rather than quietly fixed in place, because a number that
gets quoted forward is worth correcting loudly once.

### What the audit finds now

| the gap | the number | why it matters |
|---|---|---|
| the suite's own detection power is unmeasured | one test written *in the last arc* passed against a build with the bug in it | every other test in this tree has the same standing: assumed to work, never asked |
| the scheduler has no unit test | `kernel/sched` is 2,157 lines | it is the most concurrency-sensitive code here, its bugs present as "one boot in ten hangs" (spinlock.h's own M56 note), and the host tier has never compiled a line of it |
| the compositor has no unit test | `compositor.c` is 5,189 lines | the last user-visible bug was in the paint path, and every check on this file goes through a booted machine and a screendump |
| user space has one test program | 24,434 lines across `bin`, `lib` and `libc`; `libctest.c` is 775 of them | the libc is 3,777 lines that a third-party program links against, checked by a program written by the same hand on the same day |
| `pmm.c` is faked, not tested | 445 lines, and `tests/fakes/fake_pmm.c` stands in for it in every single host test | the frame allocator underneath every host test is a fake, and the real one is covered only by a boot marker |
| a flaky disk halts the machine | 8 panics across `ata.c`, `virtio_blk.c` and `rtl8139.c` - timeouts and device-error paths | on QEMU these never fire. On the USB-boot machine M28 is still waiting for, they are the likely first failure, and each one is a halt |
| crash consistency is asserted, never tested | M71 bought "files worth trusting" with write ordering plus a mount check | nothing has ever cut power to this machine mid-write and remounted it. The one guarantee the filesystem makes is the one nothing tests |
| there is no CI | Q10 wrote a workflow, nothing ever ran it, and it has since been deleted | every tier is graded by a person who remembered to run a script — the arrangement all 93 milestones were built under, now stated rather than half-denied by a file in `.github/` |
| the newest kernel file has no test | `kernel/dev/fwcfg.c`, 148 lines, added by Q1 | it decides whether *the tests run at all*, and it is tested by nothing. If it silently returned 0 on a machine with fw_cfg, every boot self-test would stop running and every harness would still pass |
| latency is one number | `input_to_photon_idle_us` measured 40581, then 13364 on the next run | a 3x swing between consecutive runs is not a measurement, it is a sample. The budget that guards it is 200000 |
| the flake rate is now a number, and the number is not zero | `session_restores_windows_across_a_reboot` failed once in a 47-test run at 4-way parallelism, then passed 3/3 alone | the harness header has predicted this failure mode for milestones - *"a boot timeout, which is this harness giving up rather than a verdict"* - and nothing has ever counted one. A suite whose flake rate is unknown teaches people to re-run it |
| the expensive tier is cheaper than its own documentation says | the full interactive suite is **458 s** (47 tests, 4 at a time); `tools/qemu-input-test.sh`'s header still says "about twenty minutes" | Q1's 8-second boot did that, as a side effect nobody planned. The number in that header is now wrong in the direction that makes people avoid running it |

Two things worth saying in the other direction, because the arc should
not read as though nothing works. The host tier is genuinely fast - 66
tests in half a second, which is the property that makes it get run. And
the fix that came out of it is real: `run-qemu.sh` went from 190 seconds
to 8, and the full interactive suite got several times cheaper as a side
effect nobody planned.

### Two decisions taken here rather than discovered halfway

**Mutation testing comes first, and everything else in this arc is
measured against it.** The obvious ordering would put the biggest
untested subsystem first - the scheduler, or the compositor. That is the
wrong order for one reason: writing three hundred new tests for the
scheduler without knowing whether tests *of this kind, written by this
hand* detect faults would be building on an unmeasured foundation. Q12
takes a day and tells you what the existing 66 are worth. Every milestone
after it can then report a survival rate rather than a count, and "we
added forty tests" stops being the metric.

**"Nothing else changed" is promoted from a trick to a policy.** Q7's
`assert_stable_outside` was written for one bug and is the only check in
this project that asserts on pixels nobody is looking at. Every other
test here answers "did the thing happen", which is a strictly weaker
question and the reason a full-screen flicker survived 45 tests. The
generalisation is cheap and it is Q15's whole first half.

---

### Q11 — The leftovers, with conditions rather than intentions ✅ (the CI item withdrawn, not done)

The unglamorous first entry, because an arc that opens new work while the
last one is half-finished is how a test suite starts rotting. Each of
these was named in Q1-Q10's own closing section as undone.

- [~] **Run the CI.** *Withdrawn.* The two load-bearing assumptions —
      that Ubuntu's `x86_64-linux-gnu` cross-compiler produces objects
      `kernel/linker.ld` accepts, and that budgets measured on
      QEMU-on-Apple-Silicon survive a hosted runner — are now permanently
      unanswered, because the workflow they belonged to is gone. See
      *Testing is local*
- [ ] **The TCP state-transition table.** `tcp.c` is at 4% line coverage,
      which is the largest single hole in the host tier and was called
      that at the time. Every legal transition and every illegal one - a
      RST in each state, a SYN in ESTABLISHED, an ACK for data never
      sent, sequence numbers wrapping through zero, a retransmission the
      peer already acknowledged
- [ ] **The coverage ratchet.** `make coverage` reports and does not yet
      fail a commit that lowers the number
- [ ] **Q9's panic triage**, now correctly sized at ~40 rather than 219:
      each one classified as *impossible* (a kernel bug if reached, and
      correct to panic), *device* (Q16), or *exhaustion* (the class
      `SYS_shm_create` was in). The classification is the deliverable;
      the conversions follow it
- [ ] **A test for `fwcfg.c`**, which decides whether any of the boot
      self-tests run and is tested by nothing. The failure that matters
      is the silent one: a `boot_selftests_enabled()` that returns 0 when
      it should return 1 turns the entire serial harness into a
      passing no-op

**How we'll know.** `tcp.c` past 60%, and a commit that lowers coverage
failing to merge. (The green-CI-badge half of this is withdrawn — see
*Testing is local*.)

### Q12 — Does this suite detect anything? Mutation testing ✅

The milestone this arc is arranged around, and the direct answer to a
test that passed on a build with the bug in it.

- [ ] A mutation harness over the units the host tier builds: apply one
      small semantic change to a kernel source file - flip a comparison,
      shift a constant by one, drop a statement, negate a condition,
      replace a return with a constant - rebuild `make test-fast`, and
      record whether the suite noticed
- [ ] **The survival rate, per file, written down.** A mutant that
      survives is a fault this suite cannot see. The number is expected
      to be bad in the places the coverage table already says are bad
      (`tcp.c` at 4% cannot detect much) and the interesting result is
      wherever a *high-coverage* file has surviving mutants, because that
      is coverage without assertions - the failure mode a percentage
      cannot show
- [ ] Equivalent mutants excluded by hand and listed with reasons, not
      silently dropped. A mutation that genuinely cannot change behaviour
      is not a hole and counting it as one makes the number a lie in the
      other direction
- [ ] The ritual made mechanical: `make mutate FILE=kernel/mm/heap.c`
      for a developer who has just written a test and wants to know
      whether it can fail
- [ ] **Deliberately not** a mutation score target. Q8 refused to set a
      coverage target before measuring and the same argument holds
      harder here: a target chosen in advance is how a suite ends up with
      tests written to kill mutants rather than to describe behaviour

**How we'll know.** A survival rate per file in `make test-fast`'s
output, and at least one surviving mutant in a file at 100% line
coverage - which is the specific result that would prove the coverage
number was never the thing worth measuring.

### Q13 — The scheduler, off the machine

2,157 lines, the most concurrency-sensitive code in the tree, and zero
host tests. Its bugs have historically presented as "about one boot in
ten hangs", which is the worst possible failure signature and the one a
deterministic test rules out completely.

- [ ] `kernel/sched/sched.c` compiled for the host against a fake timer
      and a fake CPU: a tick is a function call, so a test drives the
      scheduler one quantum at a time rather than waiting for a PIT
- [ ] Round-robin fairness as a property, not an anecdote: N tasks over M
      quanta each get within one quantum of M/N, at every N from 1 to
      `MAX_TASKS`
- [ ] The lifecycle at its edges: every task slot allocated and freed,
      the table full, a task exiting while another waits on it, a parent
      exiting before its child, reaping, and the slot-reuse M54 added
- [ ] Wait queues (M68): a task blocked in `SYS_waitfds` is
      `TASK_BLOCKED` and not runnable; a wake with no waiter; two waiters
      on one fd; a wake that arrives before the sleep
- [ ] Signal delivery order and masking, which is a state machine with
      no test today
- [ ] pgid/sid/foreground-job rules (M73, M85), which are pure logic over
      a task table and are currently exercised only by a shell
- [ ] **The lock-order assertion made real.** `heap.c` documents its lock
      order in a comment and `spinlock.h` documents the interrupt rule in
      three paragraphs. A comment cannot fail. The host tier's fake
      spinlock already catches recursive acquisition; extend it to record
      the order locks are taken in and fail on an inversion

**How we'll know.** A fairness property that holds for every task count,
and the M56 deadlock - a lock taken from a path an interrupt can
re-enter - reproduced as a failing test on a build with the fix reverted.

### Q14 — The compositor, off the machine

5,189 lines. The largest single file in user space, the one every visible
bug in this project has lived in, and the one every check reaches only
through a booted machine and a screendump.

Most of it is not graphics. Hit-testing, z-order, clipping, damage
regions, snapping, cascade placement and the taskbar's layout arithmetic
are pure functions of a window list, and every one of them can be tested
in microseconds.

- [ ] The window model compiled for the host against a fake framebuffer:
      an array of pixels a test can read back, which is exactly what the
      real one is
- [ ] Hit-testing against overlap, the case M51 added and the input suite
      covers with one test: a click at a point covered by three windows
      reaches the top one, at every z-order permutation
- [ ] The z-order invariants as properties: a panel is always above every
      app, the desktop always below, `z_raise` is idempotent, and no
      operation ever leaves a window in the list twice or in none
- [ ] Clipping: no draw call may write outside `clip_x0..clip_x1`, ever,
      for any window geometry including ones that start off-screen. The
      fake framebuffer is guard-banded, so this is checked rather than
      hoped
- [ ] Placement: the cascade, the bottom clamp M45 added after a seventh
      icon found it, and session restore at a *different resolution* than
      the one that saved it - which is the case most likely to put a
      window where nobody can reach it
- [ ] The animation clock: given a fixed time base, every animation
      reaches its endpoint exactly, retires, and never overruns
      `FRAME_BUDGET_MS` in its own arithmetic

**How we'll know.** The M45 bottom-clamp bug and the M51 occlusion bug
both reproduced as failing host tests against reverted fixes, in
milliseconds rather than by launching seven applications.

### Q15 — Nothing else changed, everywhere ✅ (five invariants; one with a documented blind spot)

Q7 wrote `assert_stable_outside` for one bug. This makes it the default
question the interactive suite asks.

- [ ] Every existing interactive test gains a stability assertion: after
      the thing it checks, nothing outside the regions that action may
      touch has changed. The allowed-region lists are the deliverable and
      they are the part a reader has to trust - each entry is a place the
      suite has stopped looking, and each one needs a reason next to it
- [ ] The golden-frame harness Q7 deferred: canonical states captured,
      compared with tolerance, and **the diff image as the failure
      artifact**. A visual failure you cannot look at is a visual failure
      you re-run
- [ ] `make accept-visuals`, and baselines reviewed in the diff like any
      other change. A visual test nobody can update is a visual test
      everybody disables
- [ ] The determinism work, which is the actual milestone and was
      correctly identified as such before any of it was attempted: the
      panel clock, the cursor, and every animation pinned or masked.
      Q7's two tests already needed the cursor parked before the baseline
      and no settle between frames, and those are the two smallest
      instances of this problem
- [ ] Both resolutions M58 supports, because a layout that only works at
      one is exactly what a fixed probe coordinate cannot see

**How we'll know.** A one-pixel change to the titlebar's corner radius
fails with a diff image showing the corner, and the suite passes twice in
a row on the same commit - which is the property that decides whether any
of this is usable.

### Q16 — Devices that fail, and a machine that keeps running

Eight panics across `ata.c`, `virtio_blk.c` and `rtl8139.c` are device
timeouts and error reports. Under QEMU they never fire. On the real
machine M28 is still waiting for, they are the likeliest first failure -
and every one of them is a halt.

- [ ] A fault-injecting block device: a read that never completes, a
      write that reports an error, a device that resets itself mid-request,
      a sector that returns different bytes each time it is read
- [ ] The same for the NIC: a transmit that never drains, a receive ring
      that overruns, a device that stops answering
- [ ] Each panic in that class converted to an error that propagates, or
      kept with a written argument for why a halt is genuinely the right
      answer. "The disk is unreliable" is not obviously fatal; "the
      framebuffer is not 32bpp at boot" obviously is
- [ ] The filesystem's behaviour when the disk below it starts failing
      mid-operation, which is where the write-ordering guarantee M71
      bought either holds or does not
- [ ] QEMU's own fault injection (`blkdebug`) used where it fits, so at
      least one of these is tested against the real driver on the real
      machine rather than only against a fake

**How we'll know.** A disk that fails every write from the tenth onward
leaves a machine that reports an error, keeps its desktop, and mounts to
a consistent filesystem on the next boot.

### Q17 — Power cut, and a filesystem that survives it ✅

M71's title is "Files worth trusting" and its mechanism is write ordering
plus an unclean-mount check. Neither has ever been tested by an unclean
mount, because nothing has ever cut power to this machine mid-write.

- [ ] Kill the guest - `SIGKILL` to QEMU, not a shutdown - at a
      randomised point during a write, then boot the same image and check
      the filesystem. Repeat across the whole distribution of interrupt
      points
- [ ] The invariant, stated so it can be checked: a file is either
      entirely its old contents or entirely its new ones, never a mixture,
      and no block is both free and referenced
- [ ] The unclean-mount path exercised for real, rather than by writing
      `DIRTY` into a superblock by hand
- [ ] `leanfs_check` given something genuinely broken to find, and a
      recorded answer to what it does about it
- [ ] **The measurement M71 and M93 both deferred**, taken here because
      this is the milestone that finally has the instrument for it: how
      long a full scan takes at hundreds of thousands of files, which is
      the second of the two conditions M71 set for a journal being worth
      building

**How we'll know.** A hundred power cuts at a hundred different
instants, and a hundred consistent filesystems - or a reproducible
counterexample, which would be worth more.

### Q18 — Latency as a distribution ✅

`input_to_photon_idle_us` measured 40581 on one run and 13364 on the
next. That is a 3x swing between consecutive boots of the same image, and
it means the number is a sample rather than a measurement. The budget
guarding it is 200000, which is wide enough to be true and too wide to
mean anything.

- [ ] Every latency measured N times and reported as a distribution -
      median, p95, worst - rather than once
- [ ] Budgets restated against the percentile that matters. A desktop is
      judged by its worst frames, so p95 and worst are the numbers with
      opinions in them; the median mostly says the machine is idle
- [ ] The frame budget checked the same way: `FRAME_BUDGET_MS` is
      asserted today by its silence, which is the right design and gives
      no idea how close a passing run came
- [ ] The measurement done under a stated load rather than an incidental
      one, so two runs are comparable
- [ ] `QEMU_MEM=128` and `QEMU_DISK=ide` given their own rows, which
      Q6 listed and did not deliver. The README quotes 95 ms versus
      5.6 ms for the two disk paths and neither harness runs the first

**How we'll know.** A latency row whose p95 moves by less than 20%
between consecutive runs on an idle host - and if it does not, that is
the finding, and the budget becomes a percentile of a distribution
instead of a ceiling over a sample.

### Q19 — Boot once, test many

Infrastructure, and the reason it is a milestone rather than a chore: the
interactive suite boots a fresh guest per test, and it is the only tier
expensive enough that people will avoid running it.

- [ ] A QEMU snapshot taken once at the desktop, restored per test. Boot
      is ~8 seconds of every test's ~25 and the restore should be well
      under one. The measured baseline to beat is **458 s** for 47 tests
      at 4-way parallelism
- [ ] Snapshot validity tied to the image: a stale snapshot silently
      testing yesterday's kernel is the one failure this must not have,
      and it fails closed by hashing the image into the snapshot's name
- [ ] The tests that genuinely need a cold boot - session restore,
      settings persistence, the first-boot format path - marked as such
      and left booting, because a snapshot restores the state they exist
      to check
- [ ] The measured before and after, in `build/test-history.tsv`

**How we'll know.** The full interactive suite in a quarter of 458 s,
with the same verdicts, and a stale snapshot proven to fail rather than
to pass quietly. The second prize is the flake in the table above: less
time per test at the same parallelism is less contention, which is what
produced it.

### Q20 — The tests as a product ✅ (flake tracking; the rest deferred)

Everything above adds tests. This one is about the suite as a thing
people have to live with, and every entry now has evidence behind it
rather than a worry.

- [ ] **The flake rate, measured.** Every run records a per-test verdict;
      a test whose verdict changes with no code change between the two is
      quarantined by name and reported, rather than re-run until green.
      The first entry is already known:
      `session_restores_windows_across_a_reboot`, which failed once at
      4-way parallelism and passed 3/3 alone
- [ ] **The buggy-build ritual, made mechanical.** Q7 caught a test that
      could not fail only because someone thought to revert the fix and
      re-run. `make prove-test TEST=...` turns that from a habit into a
      command, and Q12's harness is most of the machinery already
- [ ] A written contract for adding a test: what it must assert, that it
      must be shown to fail, and - the one Q7 learned the hard way - that
      an allowed-region list is a list of places the suite has *stopped
      looking* and needs a reason per entry
- [ ] Per-test runtime recorded, and a budget on the total. The
      interactive tier is 458 s today and the number that matters is
      whether it is still 458 next quarter
- [ ] `tools/qemu-input-test.sh`'s header corrected: it says "about
      twenty minutes" and the answer is under eight. A stale number in a
      header is how people decide not to run something

**How we'll know.** A flake rate in `build/test-history.tsv` with a
non-zero entry and a name against it, and a test that cannot fail caught
by a command rather than by somebody's suspicion.


### What landing Q11-Q20 actually found

*Written from the runs.* The arc's bet was that Q12 should come first
because it measures what everything else is worth. That was right, and it
was right in a way that was uncomfortable to read.

**The first mutation census said 34.7%, and it was wrong.** Two bugs in
the harness cancelled into a plausible number: the relational operator
was matching the `>` in `b->size` and producing `b->=size`, and the
build-failure detection was counting those compile errors as *kills*. So
the tool reported a number that looked like a measurement and was an
artifact. Fixed, and the honest first figure was **34.7% across eight
files** with the survivors where nobody would have guessed.

**Coverage and detection are different things, and now there is proof.**

| file | line coverage | mutation score, before | after |
|---|---|---|---|
| `net/ethernet.c` | 100% | **0.0%** | 88.5% |
| `net/udp.c` | 96% | 3.7% | 80.5% |
| `net/icmp.c` | 74% | 7.5% | 59.4% |
| `net/arp.c` | 84% | 29.8% | 76.6% |
| `mm/heap.c` | 100% | 66.7% | 89.4% |

`ethernet.c` is the one to remember: **100% line coverage and a zero
mutation score.** Every line ran and not one injected fault was detected,
because every test that "covered" it asserted only that a malformed frame
produced no reply. Q12's own text predicted this exact result -
"a survivor in a well-covered file is the interesting kind" - and finding
it was still a surprise.

The cause was the same everywhere and is worth stating once: **a test
that asserts an absence constrains almost nothing.** Dropping a bad
packet is one bit of behaviour. The several hundred bits that matter are
in the reply, and nothing was reading them. The fix was mechanical once
named - assert the content - and `net/udp.c` went from 3.7% to 80.5% on
one insight: every UDP test had been sending a checksum of zero, which
RFC 768 defines as "not computed", so the entire validation path had
never been entered.

**Three more real defects, all found by tests written this arc.**

- **A blind RST could tear down any TCP connection.** `tcp.c` acted on a
  RST before its acceptance test, so a segment at *any* sequence number
  was believed. RFC 5961 exists because that reduces killing somebody
  else's connection from guessing a four-tuple and a sequence number to
  guessing a four-tuple.
- **Sixteen packets could stop this machine speaking TCP.** A control
  block created by an arriving SYN belongs to `tcp.c` until `tcp_accept`
  hands it over. If it died first - a RST, a failed handshake - nobody
  called `tcp_release`, so `tcb_dispose` never reclaimed the slot.
  `TCP_MAX_TCBS` is 16, and the leak was permanent until reboot. A denial
  of service costing an attacker thirty-two packets.
- **`tcp_abort` did not free.** `tcp.h` calls it "RST and free"; it did
  the first half. A caller following the documented contract leaked a
  slot per aborted connection.

All three were found by `tests/test_tcp_states.c` on its first run, which
also took `tcp.c` from **3.74% to 62.62%** line coverage - the largest
single hole in the suite, named as such since Q8, and closed.

**The filesystem survives losing power.** Fourteen cuts with `SIGKILL`
spread across the heaviest metadata window there is - format, then
seeding fifty programs into `/bin` - each followed by a real reboot and
an independent structural check. Fourteen consistent filesystems, zero
failures. M71 has been called "Files worth trusting" since it landed;
it is now a claim with evidence rather than an argument.
`tools/leanfs-fsck.py` is deliberately a *second* implementation of the
on-disk format, which is the one place Q3's "never two copies" rule is
worth breaking: a checker built from the kernel's own code cannot find a
corruption the kernel does not believe is possible.

**Latency was measured wrong, and the shape says something different
from the number.** Within a single boot the idle path is tight - best
40757 us, median 42303, worst 43475, a 7% spread. Under load the worst
frame is twice the median (81547 against 40742). So the 3x swing between
consecutive runs that motivated Q18 is *between* boots rather than within
one: it is the boot conditions that vary, not the path. That is a
different problem and it is now visible.

**A limit of the pixel instrument, found by trying to prove a test.**
Q15's `moving_the_cursor_changes_only_the_cursor` was written to catch a
cursor trail, and was then run against a build with one deliberately
injected - the compositor repainting only the cursor's new footprint and
never restoring the old. **It passed.** Twice: the first version allowed
the whole corridor the cursor traversed, which is Q7's own warning about
allowed-region lists happening inside a test written to honour it; the
second brought the cursor home so the corridor had to be pixel-identical,
and passed anyway. The second failure is not a mistake in the test.
`compositor.c` does a full redraw every 100 ms as a fallback and a
`screendump` round trip takes longer than that, so the trail is really
there and is really repaired before the instrument can look.

The conclusion is worth carrying: **a transient repaired within 100 ms is
below the resolution of a screendump-based test.** The launch flicker was
catchable because it survived into a frame. Catching a cursor trail needs
a different instrument - the guest reporting its own damage rectangles,
or a build with the fallback redraw disabled - and that is Q14's
territory rather than Q15's. The test is kept, with its limits written
into it, because the invariant it *does* assert (a persistent corruption
the fallback does not repair) is real and nothing else here looks for it.

**Two flaws in tooling written earlier this arc, found by using it.**
The mutation harness could leave a mutant *binary* on disk beside a
restored source, at which point `make test-fast` reported failures with
no cause - it cost twenty minutes of looking for a bug that was not
there. And `make test-fast TEST_SAN=0` left a non-sanitized binary that a
later plain run considered up to date, quietly reporting "no ASan
findings" about a build ASan was never in. Both fixed; the second is the
more dangerous, because it makes a weaker check look like a stronger one.

### What is left from Q11-Q20, and where it stopped

- **Q13 (the scheduler) and Q14 (the compositor) are not started.** Both
  were priced in this arc's own closing note as "each larger than the
  whole of Q1-Q10's host tier", and that estimate stands: `sched.c` has
  25 includes and needs fakes for the CPU, the interrupt frame, the GDT,
  SMP, the PIT, pipes, shm, processes and the address space before a
  single line of it compiles on a host. They remain the two largest
  untested subsystems - 2,157 and 5,189 lines.
- **Q16 (device fault injection) is not started.** `fake_blk` already has
  the hook (`fake_blk_fail_writes_after`) and nothing uses it yet; the
  driver-level half needs QEMU's `blkdebug`.
- **Q19 (snapshots) is not started.** The interactive suite is 458 s and
  that is now bearable, which is exactly why this slipped.
- **Q15 is five invariants, not forty-seven.** The generalisation to
  every interactive test is real work: each one needs an allowed-region
  list, and each entry in such a list is a place the suite has stopped
  looking.
- **The CI still never ran, and now never will.** Same two unverified
  assumptions as before, retired unanswered along with the workflow. See
  *Testing is local*.
- **`fs/leanfs.c` is at 55.68% and its mutation score is unmeasured** -
  1,440 mutants at ~1.3 s each is half an hour, and it was not spent.

### What this arc deliberately does not do

- **No new test framework, still.** `CHECK` and a runner. The mutation
  harness in Q12 is a shell script and a compiler, and libFuzzer stays
  the only third-party thing anywhere near this code.
- **No property-based testing library.** Q3's leanfs model test is
  hand-written and that is the right size for it. A library becomes worth
  it when shrinking a failing case by hand is the bottleneck, which it is
  not yet.
- **No performance target.** Q18 measures distributions and restates
  budgets against them. It does not try to make anything faster - M69's
  discipline is that a measurement comes before the work, and this arc is
  the measurement.
- **No coverage number for the kernel as a whole.** The host tier covers
  9 of 59 kernel files and the honest number is the per-file table, not
  an average over a denominator chosen to flatter it. Q13 and Q14 move
  the denominator by adding the two biggest subsystems; nothing here
  reports a single figure.

**On the size of this.** Q13 and Q14 are each larger than the whole of
Q1-Q10's host tier, and Q17 needs a harness that does not exist. Q12 is
the smallest and is the one to do first for exactly that reason: it is a
day's work that tells you what the other nine are worth.

**If only two land, they should be Q12 and Q15** - mutation testing, and
"nothing else changed" applied everywhere. The last arc named Q1 and Q6
and got that wrong: Q5 was worth more than either, and Q6 has yet to
catch anything. This prediction is offered with that record attached.
The reasoning is that those two attack the arc's question from both
ends - Q12 measures whether these tests detect faults at all, and Q15
covers the one class of fault that has actually reached a user.


## Deliberately not next, and why

*Re-read after the M81–M89 arc was written, because "next" moved.* Every entry below is still deliberately not next — none of them is in
that arc either. Three have changed status in a smaller way and say so
in place: the journal, the loader, and uids. Nothing here was
reversed by writing the arc, which is worth recording, because an arc
that quietly collected its own deferrals would be the drift
this section exists to prevent.

*Re-read a third time after the M101–M110 arc.* **Two entries are
collected outright — the USB boot and the performance quartet — and one
is re-measured for the third time.** None was promoted because the arc
wanted it; each is collected under the condition it wrote for itself,
which is the whole test this section applies. What is worth recording is
the pattern: the deferrals that survive are the ones whose condition is
about *the world* (two people sharing a machine, a vendor's GPU), and
the ones that fall are the ones whose condition was about a
*measurement this project had not taken yet*. The second kind is not
really a refusal — it is a scheduled question — and calling it one for
five arcs was slightly too flattering to the discipline.

*Re-read again after the M90–M100 arc, and this time two entries were
reversed — which is exactly the event the paragraph above says to watch
for, so it is recorded here rather than left to be noticed.* **Dynamic
linking is now M95 and self-hosting is now M98.** Neither was promoted
because the arc wanted them; both were promoted because the condition
each one named for itself had been met and written down in advance. The
loader's condition was *"a second and third static binary duplicating
the same libc"*, which M89 produces by construction. Self-hosting's was
never stated as a condition at all — it was called "the romantic end
state, and genuinely out of reach" — and what moved is that a target
triple, a C++ runtime and a filesystem that can hold a source tree turn
it from a category of work into a list of four milestones with a
falsifiable test at the end. Both are still fair to call reversals, and
calling them that is cheaper than pretending they were always coming.
Everything else below is unchanged or changed only in place.

- **A GPU driver, or real mode-setting.** Already argued in the
  stretch-goal list: it is a driver per vendor, and it is not a thing
  this project will do. M58's Display pane showing only the firmware's
  mode on real hardware is the honest outcome. **Unchanged by M100,**
  which is worth saying because a browser is where the pressure for one
  comes from: software rasterization into this compositor's framebuffer
  is the answer here, and it is a slow answer rather than a missing
  one. **Unchanged by M107 as well, and that one is worth stating
  because it looks closer:** AHCI, NVMe and xHCI are published
  specifications with one implementation each, which is a category a GPU
  has never been in. See that arc's second decision.
- **A browser.** Now a named goal rather than a hypothetical one — see
  the arc above — and the estimate has not moved: HTML, CSS, a layout
  engine, a JS runtime, TLS, GPU compositing, codecs, a sandbox, and a
  Linux-scale syscall surface underneath most real-world binaries of
  that size. `fetch` and M75–M80 are the right-sized steps in that
  direction; there is no version of this list where a browser is the
  *next* one. **Unchanged, and M100 is not a crack in it:** that
  milestone builds the libraries a browser links against and *measures*
  the remaining gap to Chromium, which is the opposite of porting it.
  A small engine with its own layout code running there is a real
  browser and is still four orders of magnitude short of the one being
  asked about.
- **Dynamic linking (shared objects, `dlopen`).** M80's Python is
  planned fully static on purpose — M75–M79 buy POSIX primitives and
  threads, not a loader. Worth building once something concrete asks
  for it (a C-extension wheel; a second and third static binary
  duplicating the same libc), same measure-first discipline as M69's
  deferred performance work. **Still not next, but the condition is now
  scheduled rather than hypothetical:** M89 produces exactly that second
  and third static binary, so this is a deferral with a date rather than
  an open question — see the arc's closing note. It is also the real fork in the road
  toward a browser: nothing at that scale ships as one static binary.
  **Collected. This is M95.** The condition was met exactly as
  scheduled, which is the only reason it moved.
- **Multi-user, logins, uids.** M65 argued this exactly right: there are
  no users here, and inventing one would be a larger lie than the one it
  fixed. It becomes real if and when two people share a machine, and not
  before. **Unchanged by the arc, and M88's `getuid()` is not a crack in
  it:** a machine with one principal that reports one principal is
  telling the truth. What M65 refused was a permission model that
  pretended to enforce something, and M88 adds no enforcement — which
  is also why `chmod` stays a truthful failure rather than a no-op that
  returns 0. **Unchanged by M101–M110 too**, recorded because a machine
  that compiles its own kernel is exactly where the temptation to invent
  a second principal turns up.
- **A journalling filesystem.** M71 buys most of the safety with write
  ordering and a mount check. A journal is worth it when there are
  multiple writers or when a full scan gets slow, and neither is true of
  an 8 MiB-file filesystem on a 36 MiB image. **M81 is where that second
  clause gets re-measured**, because thousands of files is the first
  thing that could make a full scan slow — re-measured, not assumed:
  M81's own bullet says "nearly" is not a measurement. **Still not next,
  and the re-measurement moved rather than happened:** M93 takes it at
  hundreds of thousands of files instead of thousands, with M92's cache
  numbers in hand, because both of M71's two conditions are finally
  under pressure at once. **Re-measured there and refused again — 30 ms
  per hundred thousand files — and now scheduled for a third
  re-measurement as M105,** because M104's writeback cache and M98's
  parallel build put M71's *first* condition under pressure for the
  first time. M105 is written so that refusing it a third time is a
  complete outcome; a deferral that can only ever be collected is not a
  deferral.
- **Self-hosting (a compiler on the machine).** The romantic end state,
  and genuinely out of reach — but M72 moves the line: after it, some of
  what needed a cross-compiler needs a script instead. **Reversed. This
  is M98,** and the honest account of why is that "out of reach" was a
  judgement about a category of work rather than a condition anything
  could satisfy. M93, M94, M96 and M97 make it a list with a test at the
  end — a three-stage bootstrap whose stages 2 and 3 are byte-identical
  — and a thing with a falsifiable test is a milestone whatever it felt
  like beforehand.
- **`syscall`/`sysret`, window scaling, SACK, Nagle.** Performance work
  on paths whose performance nobody has measured. M69 establishes that
  measuring first is how this project decides; these come back when a
  measurement asks for them. **Unchanged, and M98 is the first thing
  likely to ask:** a compiler bootstrap is millions of `open`/`read`/
  `write` calls and is the first workload on this machine whose
  wall-clock is worth attributing. If `int 0x80`'s cost shows up in that
  attribution, this stops being a deferral and becomes a number.
  **Collected — as measurements rather than as work: `syscall`/`sysret`
  is M101's last box, and the other three are M108's third.** Both are
  written as *if and only if the number asks*, which is the form this
  entry has always implied and never actually been given.
- **The USB boot** (M28's one open box). **Collected. This is M110** —
  the oldest unfinished line in this file, and the only milestone
  anywhere in it whose blocker is a physical object rather than a
  decision, which is why that arc says its position in the order is a
  scheduling artifact and it may jump the queue. It still needs hands. M70's painting panic and M71's mount check both make the
  day it happens go better, which is a nice side effect and not a reason
  to reorder anything.

## Testing is local

`.github/` is gone: the workflow Q10 wrote, and every reference in this
tree that spoke as though a hosted runner existed. Nothing was ever run
on one.

**What changed.** Deleted `.github/workflows/tests.yml`. Reworded the
comments that had been written against it — `CLAUDE.md`'s test section,
the `Makefile` header's account of why `AS`/`CC`/`LD` are overridable
from the environment, `tools/run-qemu.sh`'s note on the `none` audiodev,
`tests/runner.c`'s note on `slow_` tests, `tests/budgets.tsv`'s header,
and `tests/corpus/README.md` — so each states its actual reason rather
than deferring to a runner. The Q10 and Q11 entries above keep their
history and are marked withdrawn.

**What it costs.** The two assumptions Q10 named are now permanently
untested: that a distribution's `x86_64-linux-gnu` cross-compiler
produces objects `kernel/linker.ld` accepts, and that budgets measured
on QEMU-on-Apple-Silicon hold anywhere else. The budgets stay a claim
about *this* machine, and `tests/budgets.tsv` now says so.

**What it taught.** The reasons in those comments were mostly good ones
that had been attributed to the wrong cause. `test-fast` is worth
keeping cross-toolchain-free because that is what makes it get run on
every edit, not because a runner needed it; `-audiodev none` is right
because it makes the run headless, not because a runner had no sound
card. A comment that justifies a constraint by naming a thing outside
the tree is a comment that goes stale the day that thing leaves — and
this one had never even arrived.

The `git remote` still points at GitHub. That is where the repository is
stored, not something the build or the tests touch, and removing it is
the user's call rather than this entry's.
