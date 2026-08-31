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

### M80 — Somebody else's language [~] attempted; every core file compiles, nothing links

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

### M84 — A program that replaces itself

- [ ] `execve`, which tears down the calling task's address space and
      loads a new image into the same task — same pid, same cwd, same
      parent, same fd table minus what is marked to close
- [ ] `FD_CLOEXEC` becomes a thing that means something. `<fcntl.h>`
      currently reads *"There is no exec on this machine (SYS_spawn
      loads a fresh image and copies the fd table), so FD_CLOEXEC has
      nothing to mean and is genuinely not set"* — an honest comment
      that this milestone makes false, and the header has to change with
      it or it becomes the first dishonest sentence in that file
- [ ] The wait family, properly. `SYS_wait`'s comment says it *"polls +
      cooperatively yields rather than a real blocking wait queue — M14
      is where 'more complete wait semantics' is scoped to land."* M14
      did not land it. `waitpid` with `WNOHANG`, a real wait queue, and
      `WIFEXITED`/`WEXITSTATUS`/`WIFSIGNALED`/`WTERMSIG` that decode a
      status rather than a bare exit code — which also fixes
      `SYS_task_alive`'s three-way squashing of "exited nonzero" and
      "died on a signal" into one answer
- [ ] `PATH` search in the exec family (`execvp`, `execlp`), which M75
      explicitly deferred: *"Deliberately not a full PATH-search exec...
      M72 already owns those."* M72 owns the shell's copy of it; this is
      the libc's
- [ ] Deliberately **not** `#!` handling in the kernel. M72 already
      resolves a shebang in the shell and that is where it belongs on a
      machine whose kernel has no opinion about interpreters

**How we'll know.** `sh` runs a three-stage pipeline in which every
stage is a real fork + exec + `waitpid`, and reports each stage's exit
status distinctly — including one that died on a signal, which is the
case the current `SYS_wait` structurally cannot report. And an fd marked
`FD_CLOEXEC` is gone on the far side of the exec while its neighbour
survives.

### M85 — A terminal that is a device

- [ ] `/dev/tty`, a real terminal device with a line discipline:
      canonical mode, echo, erase, kill, and the raw mode an editor
      needs. `<unistd.h>`'s `isatty` says today that *"There is no
      terminal device here to ask, so this is the honest approximation
      and not a stub"* — this is the milestone that gives it something
      to ask
- [ ] `<termios.h>` and the four `ioctl`s that matter
      (`TCGETS`/`TCSETS`, `TIOCGWINSZ`, `TIOCSPGRP`), which is the first
      `ioctl` on this machine and should be the narrow one it looks like
      rather than a general escape hatch
- [ ] Sessions and controlling terminals: `setsid`, `getsid`,
      `tcgetpgrp`/`tcsetpgrp`. Process groups already exist —
      `SYS_getpgid` has been there since syscall 10 — and have never had
      the thing that makes them useful
- [ ] Job control signals, which are simply absent from
      `system_api/include/signal.h` today: `SIGSTOP`, `SIGCONT`,
      `SIGTSTP`, `SIGTTIN`, `SIGTTOU`. `SIGSTOP` joins `SIGKILL` as the
      second uncatchable one, and the scheduler grows a STOPPED state
      distinct from the blocked-on-something states it has
- [ ] `^C` raises `SIGINT` on the **foreground process group** and not
      on the shell; `^Z` raises `SIGTSTP`; a background process reading
      the terminal gets `SIGTTIN` rather than stealing the user's
      keystrokes
- [ ] `gui_terminal` becomes the master side of a pty rather than a
      program that owns a pipe, so that everything above is true of the
      terminal a person actually types into and not only of the serial
      console

**How we'll know.** In `qemu-input-test.sh`, with real keys, because
M40's entire lesson is that this is the layer where the serial harness
and the truth diverge: `^C` kills a running pipeline and leaves the
shell's prompt alive; `^Z` suspends it and `fg` brings back the same
process rather than a new one; and a backgrounded program that reads
stdin stops instead of consuming the next thing typed at the prompt.

### M86 — A shell that is a shell

- [ ] `sh.c`'s own header is the specification. It says what is missing:
      *"job control, `&`, subshells, functions, and `|`"* — the last one
      meaning more than a single pipe. All five, now that M83 and M85
      make four of them expressible
- [ ] Two variable namespaces, and `export` that moves a name between
      them. M75 wrote: *"`export` is accepted and does nothing but
      assign, because this shell has one namespace — the reason POSIX
      has two is subshells and functions, and it has neither."* This is
      the milestone where it has both, and that sentence stops being a
      justification and becomes a to-do
- [ ] `if`/`while`/`for`/`case`, `&&`/`||`, command substitution,
      parameter expansion with the `${x:-y}` family, and `$?`/`$#`/`$@`
- [ ] `jobs`, `fg`, `bg`, and the builtins job control implies, over
      M85's `tcsetpgrp`
- [ ] Deliberately **not** a `bash`, and deliberately not an arithmetic
      expansion engine or `[[`. The measure is a `configure` script that
      someone else wrote running to completion, not a feature list

**How we'll know.** A shell script nobody here wrote runs correctly —
the honest candidate is a real `configure`, because it is the densest
per-line user of exactly the five things the header admits are missing.
A script written here to exercise the new features proves that they were
implemented, which is a much weaker claim than that they were
implemented *right*.

### M87 — Files with a type, a place, and more than one name

- [ ] `kernel/fs/vfs.h` calls itself *"the seam a second filesystem type
      would plug into if this project ever needed one"* and says the
      pass-through is honest *"right now."* This milestone is the day it
      needs one: a vnode layer and a real mount table, replacing
      path-string dispatch
- [ ] `devfs` at `/dev`: `null`, `zero`, `full`, `random`/`urandom`,
      `tty`, `console`, `fd/*`. This is not a nicety — it is the single
      most common thing a ported program touches that this machine
      cannot answer, and every one of them is a few lines behind a vnode
      layer that exists
- [ ] `procfs` at `/proc`: `self`, `self/exe`, `N/status`, `N/cmdline`,
      `uptime`, `meminfo`. `SYS_taskinfo`'s whole-shot snapshot stays
      for `task_manager`, which is a lean_os program and should keep
      using the lean_os interface; `/proc` is for programs that have
      never heard of it
- [ ] Symbolic links and hard links in leanfs v2, and `O_NOFOLLOW`,
      `readlink`, `symlink`, `link`. M75 wrote down exactly where this
      lands: *"With no symbolic links on this machine, `/a/b/..` and
      `/a` name the same directory by construction... The day this
      filesystem grows links is the day that stops being true, and the
      comment above `path_normalize` is where it stops."* That comment
      is this bullet's specification, written a year early
- [ ] Loop detection with a hop limit, because the first symlink is also
      the first way to hang the kernel's path walker in a way no input
      before it could
- [ ] The `*at()` family (`openat`, `fstatat`, `unlinkat`, `renameat`),
      a real `O_EXCL` — `<fcntl.h>` currently defines it as 0 with a
      comment saying a program relying on it *"gets no protection"* —
      plus `ftruncate` (declined in `<unistd.h>` pending a caller;
      `leanfs_handle_truncate` has been sitting there the whole time)
      and `fsync`
- [ ] Deliberately **not** a buffer cache or a page cache. M82 makes one
      possible and no measurement has asked for one; M69's rule

**How we'll know.** A program that has never heard of this OS opens
`/dev/null` and writes to it, reads 64 bytes from `/dev/urandom` and
gets 64 different-looking bytes, and finds itself at `/proc/self/exe`. A
symlink loop returns `ELOOP` rather than hanging the machine. And `cp
-r` over a tree containing a symlink and a hard link reproduces both as
what they were, rather than as two copies.

### M88 — Everything else a ported program calls

- [ ] `poll()` and `select()`, built over `SYS_waitfds` (syscall 68),
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
- [ ] `sysconf`, `getrlimit`/`setrlimit`, `getrusage`, `times`,
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

### M89 — Somebody else's userland

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

## Deliberately not next, and why

*Re-read after the M81–M89 arc was written, because "next" moved.* Every entry below is still deliberately not next — none of them is in
that arc either. Three have changed status in a smaller way and say so
in place: the journal, the loader, and uids. Nothing here was
reversed by writing the arc, which is worth recording, because an arc
that quietly collected its own deferrals would be the drift
this section exists to prevent.

- **A GPU driver, or real mode-setting.** Already argued in the
  stretch-goal list: it is a driver per vendor, and it is not a thing
  this project will do. M58's Display pane showing only the firmware's
  mode on real hardware is the honest outcome.
- **A browser.** Now a named goal rather than a hypothetical one — see
  the arc above — and the estimate has not moved: HTML, CSS, a layout
  engine, a JS runtime, TLS, GPU compositing, codecs, a sandbox, and a
  Linux-scale syscall surface underneath most real-world binaries of
  that size. `fetch` and M75–M80 are the right-sized steps in that
  direction; there is no version of this list where a browser is the
  *next* one.
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
- **Multi-user, logins, uids.** M65 argued this exactly right: there are
  no users here, and inventing one would be a larger lie than the one it
  fixed. It becomes real if and when two people share a machine, and not
  before. **Unchanged by the arc, and M88's `getuid()` is not a crack in
  it:** a machine with one principal that reports one principal is
  telling the truth. What M65 refused was a permission model that
  pretended to enforce something, and M88 adds no enforcement — which
  is also why `chmod` stays a truthful failure rather than a no-op that
  returns 0.
- **A journalling filesystem.** M71 buys most of the safety with write
  ordering and a mount check. A journal is worth it when there are
  multiple writers or when a full scan gets slow, and neither is true of
  an 8 MiB-file filesystem on a 36 MiB image. **M81 is where that second
  clause gets re-measured**, because thousands of files is the first
  thing that could make a full scan slow — re-measured, not assumed:
  M81's own bullet says "nearly" is not a measurement.
- **Self-hosting (a compiler on the machine).** The romantic end state,
  and genuinely out of reach — but M72 moves the line: after it, some of
  what needed a cross-compiler needs a script instead.
- **`syscall`/`sysret`, window scaling, SACK, Nagle.** Performance work
  on paths whose performance nobody has measured. M69 establishes that
  measuring first is how this project decides; these come back when a
  measurement asks for them.
- **The USB boot** (M28's one open box). Unchanged and still open — it
  needs hands. M70's painting panic and M71's mount check both make the
  day it happens go better, which is a nice side effect and not a reason
  to reorder anything.
