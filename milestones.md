# lean_os — Milestones

This file is the living progress record for the project. Update the status
and notes on a milestone as soon as it's reached — this is the source of
truth for "where are we" across sessions.

Status legend: `[ ]` not started · `[~]` in progress · `[x]` done

## Ground rules / assumptions

- **Architecture:** x86_64, legacy BIOS boot (not UEFI).
- **Bootloader:** written entirely from scratch (MBR stage 1 → stage 2 →
  long mode), no GRUB/Multiboot/Limine or any third-party boot code.
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

## Stretch goals (unordered, orthogonal to the desktop path)

- [x] SMP (multi-core) support
- [ ] Networking stack + NIC driver
- [ ] UEFI boot path as an alternative to BIOS
- [ ] Port to real hardware (USB boot test)
- [ ] Package/build tooling for third-party user programs (still built from
      scratch, just easier to author)

---

## Progress log

Append a dated entry each time a milestone (or notable sub-step) completes.
Keep it terse — this is a changelog, not a diary.

- 2026-08-19 — Plan drafted, milestones.md created. Nothing built yet.
- 2026-08-19 — M0 complete: repo scaffolding (`kernel/`, `system_api/`,
  `user_space/`, `tools/`, `docs/`, `build/`) in place with per-dir
  README stubs; toolchain installed via Homebrew (`nasm` 3.02,
  `x86_64-elf-gcc` 16.2.0, `x86_64-elf-binutils` 2.47) alongside existing
  QEMU 10.1.3, decisions documented in `docs/toolchain.md`; top-level
  `Makefile` (`all`/`run`/`clean`) and `tools/run-qemu.sh` stub in place
  and verified to run cleanly with the expected "nothing to build/boot
  yet" messages. Also fixed `.gitignore`, which was accidentally
  excluding `milestones.md` itself. Next: M1 (boot sector).
- 2026-08-19 — M1 complete: `kernel/boot/stage1.asm` (512-byte MBR, prints
  a status message, reads 4 sectors of stage2 via `INT 13h`, jumps to it)
  and `kernel/boot/stage2.asm` (16-bit stub that prints its own message
  and halts — full protected/long mode transition is M2's job) both
  assembled with `nasm -f bin`. Top-level `Makefile` now builds
  `build/os-image.bin` (stage1 + stage2 concatenated) and `make run`
  boots it in QEMU. Verified headlessly via `qemu-system-x86_64
  -display none -monitor stdio` + `screendump`: SeaBIOS → stage1 message →
  stage2 message all appear on screen, confirming the disk-read handoff
  works. Note: stage1/stage2 each have their own copy of `print_string` —
  intentional duplication until M2 replaces stage2 with real linked code.
  Next: M2 (protected mode → long mode).
- 2026-08-19 — M2 complete: `kernel/boot/stage2.asm` rewritten to do the
  full 16-bit → 32-bit protected → 64-bit long mode transition. Execution
  order deviates from the checklist's listed order on purpose — E820 is
  collected FIRST, in real mode, before A20/GDT/protected mode, because
  `INT 15h` (like all BIOS calls) only works in real mode. Fast A20 gate
  via port 0x92 (documented as good-enough-for-now; real-hardware
  robustness via keyboard-controller/BIOS fallback methods is a stretch
  goal). Identity-maps the first 1 GiB with 2 MiB pages (PML4[0] ->
  PDPT[0] -> PD[0..511]), static page tables at fixed addresses 0x1000/
  0x2000/0x3000. Long mode entry (`long_mode_entry`) is a temporary
  stand-in for the real M3 kernel entry point — receives a pointer to the
  E820 map in RDI (System V AMD64 ABI first-arg register), matching the
  convention M3's real kernel loader will use. stage1's
  `STAGE2_SECTOR_COUNT` bumped 4 → 8 (2 KiB → 4 KiB) to fit the larger
  stage2. Hit and fixed a real bug during bring-up: `kernel_stub_main`
  read the E820 count into `AL` before printing a status message, but
  `print_string_vga`'s `lodsb` clobbers `AL` as a side effect of walking
  the string — count showed as 0x00 until fixed by stashing it in `BL`
  across the print call. Verified via temporary real-mode hex-print
  debug instrumentation (added, used to confirm the real-mode E820 loop
  itself found 7 entries correctly, then removed once the bug was
  isolated to the 64-bit print path) and a final headless QEMU
  screendump showing "E820 entries = 0x07" — proving A20, protected
  mode, paging, and long mode all work and the memory-map handoff
  survives intact. No exceptions/triple-faults in QEMU's `-d int` log.
  Next: M3 (minimal freestanding kernel).
- 2026-08-19 — M3 complete: real C kernel now runs, loaded and jumped to
  by the bootloader instead of the M2 inline stub. New files:
  `kernel/arch/x86_64/entry.asm` (`_start`, switches onto a kernel-owned
  16 KiB stack in `.bss` rather than the bootloader's scratch stack, calls
  `kernel_main`), `kernel/linker.ld` (flat image linked at 0x100000),
  `kernel/kernel.c` (`kernel_main`, parses and prints the E820 map handed
  off in RDI), `kernel/panic.c`/`panic.h` (halt + message; wired to a real
  check — `kernel_main` panics if the E820 map is empty, not a fake demo
  trigger), `kernel/drivers/vga.{c,h}` (clear/putc/puts/scroll + hex
  printing). Kernel objects are linked directly with `x86_64-elf-ld`
  (no libc/crt0 in the picture at all) then `objcopy -O binary`'d to a
  flat image — ground rules updated to spell out why `<stdint.h>` is fine
  under "no external dependencies."
  Loader rework: switched BOTH stage1 and stage2 from CHS disk reads to
  LBA extended reads (`INT 13h, AH=42h` + Disk Address Packet) — CHS caps
  at 63 sectors/track and stage2 now needs to load a kernel binary of
  unknown/growing size. `kernel.bin`'s size is computed at build time
  (`Makefile` stats the objcopy'd binary, rounds up to a sector boundary,
  pads the file to match, records the sector count in
  `build/kernel.sectors`) and fed into stage2's assembly via `nasm -D
  KERNEL_SECTOR_COUNT=...` — no more hand-maintained size constants.
  Kernel load is a two-step dance: real mode can't address 1 MiB directly,
  so stage2 first reads the kernel into a low real-mode-addressable
  scratch buffer (0x10000, chunked across `INT 13h` calls with segment
  bumping every 64 KiB), then after paging is live in 64-bit mode, does a
  flat `rep movsb` copy from there up to the kernel's real load address
  (0x100000) before jumping in. Scratch buffer is capped at 64 KiB
  (128 sectors) for now, enforced by a `nasm %error` build-time guard —
  documented as a known ceiling to revisit once the kernel legitimately
  grows past it (current size: 1.5 KiB, so no rush).
  Verified via headless QEMU screendump: VGA output shows real formatted
  C output (`vga_put_hex32`/`vga_put_hex64`), a fully sane E820 map for
  QEMU's default RAM layout, and "No scheduler yet - halting." No
  exceptions in QEMU's `-d int` log. One cosmetic linker warning ("LOAD
  segment with RWX permissions") — harmless at this stage since we
  objcopy to a flat binary and don't rely on ELF segment permissions;
  worth revisiting once page-level W^X protections matter (M4/M5).
  Next: M4 (CPU fundamentals in the kernel — GDT/TSS, IDT, PIC).
- 2026-08-19 — M4 complete: kernel now owns its own GDT/TSS, a full IDT,
  and the PIC, replacing stage2.asm's temporary bring-up GDT. New files
  under `kernel/arch/x86_64/`:
  `gdt.c`/`gdt.h`/`gdt_asm.asm` (null + flat 64-bit kernel code/data
  descriptors + a 16-byte TSS descriptor; `gdt_flush` does `lgdt` + reloads
  every segment register, using the standard push-far-pointer-then-`o64
  retf` trick since CS can't be loaded with a plain `mov` — no direct far
  jump either, since long mode can't encode a full 64-bit target for one;
  `tss_flush` loads TR). The TSS's IST1 points at a dedicated 4 KiB
  double-fault stack, so a fault caused by a corrupt/overflowed kernel
  stack still gets a valid stack to push the exception frame onto instead
  of silently triple-faulting.
  `idt.c`/`idt.h`/`idt_asm.asm` (256-entry IDT; vector 8 wired to IST1,
  everything else IST0/current-stack; `idt_flush` does `lidt`).
  `isr_asm.asm`/`isr.c`/`isr.h` (per-vector trampolines for all 32 CPU
  exceptions — pushing a dummy error code for the ones the CPU doesn't
  supply one for — and the 16 remapped IRQ lines, both funneling into a
  shared save-regs/call-C/restore-regs/`iretq` stub; `isr_handler`
  dispatches CPU exceptions, `irq_handler` dispatches IRQs and EOIs the
  PIC). Register-dump struct layout in `isr.h` matches the asm push order
  exactly (documented inline) — this is the one place a mismatch would be
  silent and nasty, so it's called out explicitly rather than left
  implicit.
  `pic.c`/`pic.h` (remaps IRQ0-15 from their power-on vectors 0x08-0x0F —
  which collide with CPU exceptions like #DF and #GP — to 0x20-0x2F, then
  masks every line since no IRQ-driven driver exists yet; `pic_set_mask`/
  `pic_clear_mask` are there for M6's drivers to unmask their own line as
  they come online). `io.h` adds shared `outb`/`inb`/`io_wait`.
  `kernel_main` now calls `gdt_init()` → `idt_init()` → `pic_remap()`,
  then deliberately executes `int3` as a self-test — proving the full
  gate → stub → C handler → `iretq` round trip actually resumes execution
  rather than just trusting it compiled — before continuing into the
  existing E820 dump.
  Makefile generalized to glob all `kernel/**/*.asm` (was hardcoded to
  just `entry.asm`) with a pattern rule matching the one already used for
  `.c` files. Hit and fixed a build bug immediately: `isr.asm` and
  `isr.c` both compiled to `arch/x86_64/isr.o`, so the linker saw
  duplicate symbols — renamed the asm file to `isr_asm.asm` to match the
  `gdt_asm.asm`/`idt_asm.asm` naming already in use.
  Verified via headless QEMU screendump (paced through the monitor via a
  FIFO so the screendump command lands after boot actually completes,
  not before): output shows "GDT/TSS, IDT, and PIC remap initialized.",
  the breakpoint handler firing with `vector=0x3 error_code=0x0`, a
  `cs=0x8`/`ss=0x10` register dump matching the new GDT selectors exactly,
  "Resumed after breakpoint self-test.", then the unmodified E820 dump
  and halt message. No CPU resets/triple-faults beyond QEMU's own
  pre-BIOS boilerplate in the `-d int,cpu_reset` log.
  Next: M5 (memory management — physical frame allocator, virtual memory
  manager, kernel heap).
- 2026-08-21 — M5 complete: real memory management, replacing the E820
  map's status as "printed but otherwise ignored." New `kernel/mm/`:
  `e820.h` (the `e820_entry_t` layout, pulled out of `kernel.c` so `pmm.c`
  can share it instead of a second copy - the kind of duplication M1's
  progress note flagged as temporary).
  `pmm.{h,c}`: bitmap physical frame allocator, one bit per 4 KiB frame,
  capped at `PMM_TRACKED_MEMORY` = 1 GiB (matching the identity map below -
  a frame can't be handed out before it's mapped). `pmm_init` starts every
  frame reserved, clears bits for E820 `type=1` (usable) ranges, then
  unconditionally re-reserves low memory (`< 0x100000`: real-mode IVT/BDA,
  the E820 map itself, stage2's bootstrap page tables, VGA text memory)
  and the kernel's own image (`0x100000` to linker.ld's `__kernel_end`) so
  a stray or overly-generous E820 entry can't hand out memory that's
  already spoken for. `pmm_alloc_frame`/`pmm_free_frame` do a linear
  bitmap scan with a monotonically-advancing search hint (pulled back down
  on free) so a long-running kernel doesn't re-scan an ever-growing
  used-prefix on every allocation. Both `panic()` on exhaustion/double-free
  respectively - consistent with the project's existing "catch it via a
  diagnosable panic, don't corrupt state silently" style.
  `vmm.{h,c}`: kernel-owned page tables, replacing stage2.asm's temporary
  bootstrap ones (fixed addresses 0x1000/0x2000/0x3000) the same way M4's
  `gdt_init` replaced stage2's temporary GDT. `vmm_init` builds a fresh
  PML4 out of `pmm_alloc_frame()` frames, rebuilds the identical 1 GiB
  2 MiB-page identity map so nothing currently running moves, then loads
  the new CR3. `vmm_map_page`/`vmm_unmap_page` do a real 4-level page walk
  with 4 KiB pages for anything outside that identity range, allocating
  intermediate PDPT/PD/PT frames from the PMM on demand; both panic if a
  target address falls inside the huge-page identity region instead of
  misinterpreting a 2 MiB PDE's physical address as a page-table pointer.
  Simplifying assumption, called out in a comment: every PMM frame lives
  inside the 1 GiB that's always identity-mapped (bootstrap or kernel-
  owned), so `phys_to_table` can just cast a physical address straight to
  a pointer - stops being true if physical tracking or kernel mappings
  ever grow past 1 GiB.
  `heap.{h,c}`: first-fit free-list `kmalloc`/`kfree` over a virtual range
  starting at `PMM_TRACKED_MEMORY` (1 GiB) - deliberately *above* the
  identity map, so every heap page is a genuine non-identity
  `vmm_map_page` call rather than a free ride on already-present
  structure. Grows page-by-page on demand; splits free blocks that have
  enough left over to be worth their own header, and coalesces forward
  only (the free list stays address-ordered by construction - splits
  insert adjacent, growth only appends at the high end - so the next
  pointer is always the next block by address, no backward-merge
  bookkeeping needed).
  `kernel_main` now runs `pmm_init` → `vmm_init` → `heap_init` right after
  the E820 dump, then two explicit self-tests in the same spirit as M4's
  `int3` self-test ("prove it, don't just trust it compiled"): a direct
  `vmm_map_page`/write/read/`vmm_unmap_page` round trip on a throwaway
  address, and a `kmalloc`/write/read/`kfree` round trip through the heap
  (a different code path - heap-driven growth vs. a direct call).
  Verified via headless QEMU screendump: `[pmm] 7ECF / 40000 frames free`
  (~127 MiB usable out of the 1 GiB tracked, matching QEMU's default RAM
  minus reserved/kernel regions - sane), `[vmm] kernel-owned page tables
  installed`, `[heap] kernel heap starts at 0x40000000`, both self-tests
  printing "passed", ending at the existing halt message. `-d
  int,cpu_reset` log during the same run shows exactly one interrupt for
  the whole boot (`v=03`, the pre-existing int3 test) - no page faults,
  GPFs, or double/triple faults from the new paging code or the CR3 swap.
  Next: M6 (timer & core drivers — PIT/APIC timer, serial (COM1), PS/2
  keyboard).
- 2026-08-21 — M6 complete: real IRQ-driven drivers, plus a refactor that
  makes every future milestone faster to verify. `isr.c`/`isr.h` gained
  `irq_register_handler(irq, fn)` - a 16-entry table `irq_handler` checks
  before falling back to its old generic "unhandled IRQ" print; EOI stays
  centralized in `irq_handler` itself so no driver has to remember to send
  one. New `kernel/drivers/`: `serial.{h,c}` (16550 UART on COM1, polling
  mode, 38400 8N1) and `klog.{h,c}` - a thin fan-out facade
  (`klog_puts`/`klog_putc`/`klog_put_hex32/64`) that writes every message
  to both VGA and serial. Every existing `vga_*` call site that was
  producing a log message (`kernel.c`, `panic.c`, `isr.c`, `pmm.c`,
  `vmm.c`, `heap.c`) was switched to `klog_*` - genuinely "parallel to
  VGA" logging rather than just serial existing in isolation. This also
  unlocked a much faster verification loop: QEMU's `-serial file:...`
  captures all of it as plain text, so `tools/qemu-serial-test.sh` (new)
  boots headless and dumps the log in about 2 seconds, versus the
  screendump+monitor-FIFO dance M1-M5 relied on. Used for every
  verification from here on.
  `pit.{h,c}`: channel 0, mode 3, programmed for `PIT_HZ` = 100 (10 ms/
  tick) via the standard command-byte + lo/hi-divisor sequence,
  registered on IRQ0. `pit_sleep_ms` busy-waits via `hlt` (so the CPU
  actually idles between ticks) until the tick counter advances enough -
  which doubles as an implicit hang-test: if IRQ0 were wired wrong, this
  loop would never return and boot would visibly stop dead instead of
  printing anything after it.
  `keyboard.{h,c}`: PS/2 on IRQ1, scancode-set-1 make/break decoding for
  the main US QWERTY block (letters/digits/punctuation/space/tab/enter/
  backspace) via unshifted/shifted lookup tables, shift state tracked
  across make/break, results pushed into a small ring buffer
  (`keyboard_read()` pops one character or returns -1). Extended
  (0xE0-prefixed) keys and F-keys/keypad aren't decoded - not needed until
  something (M13's shell) wants arrow keys.
  `kernel_main` now does an explicit `sti` (defensive - IF turns out to
  have been set since real mode already, nothing had touched it, but
  relying on that implicitly felt wrong to leave undocumented), then
  `pit_init()` + a sleep(50ms)-and-compare self-test, then
  `keyboard_init()` + a bounded (3s) non-blocking wait for a test
  keypress - printing whatever it got, or a clean "no keypress, driver
  installed" message on timeout so a normal unattended boot doesn't hang
  waiting for interactive input.
  Verified via `tools/qemu-serial-test.sh`: PIT self-test shows ticks
  advancing (e.g. `1 -> 6` across a 50 ms sleep, consistent with 100 Hz);
  keyboard verified twice via the QEMU monitor's `sendkey` command injected
  mid-boot - `sendkey k` -> `received keypress: 'k'`, `sendkey shift-a` ->
  `received keypress: 'A'` (confirms both the scancode table and shift
  tracking). `-d int,cpu_reset` over a 5s run shows the expected `v=03`
  (int3) plus ~300 `v=20` (IRQ0/timer) events and nothing else - no
  spurious vectors, no faults, no resets; IRQ0 stops cleanly once
  `entry.asm`'s post-`kernel_main` `.hang` loop hits its own `cli`.
  Next: M7 (multitasking basics — TCB, context switch, round-robin
  scheduler driven by the PIT tick).
- 2026-08-21 — M7 complete: preemptive round-robin scheduling for kernel
  threads, plus two real bugs found and fixed by actually running it
  rather than trusting the design on paper. New `kernel/sched/`:
  `context_switch.asm` (`context_switch(old_rsp_out, new_rsp)`) - the
  classic "swtch" technique: save the 6 callee-saved registers (the ones
  the System V ABI says a caller can assume survive any call) plus RFLAGS
  onto the current stack, stash RSP, load the new task's RSP, restore its
  6 registers + RFLAGS, `ret`. That `ret` doesn't return to this
  function's own caller - it pops whatever return address sits on the
  *new* stack, which is either back inside `schedule()` (a task that's
  been preempted before) or `task_entry_trampoline` (a task's first-ever
  run, via a stack `sched.c`'s `task_spawn` fabricates to look exactly
  like an already-suspended `context_switch` call). Everything else about
  a task's state - general-purpose registers, and if it was preempted
  mid-interrupt, the CPU's own iretq frame - rides on that same stack,
  saved by M4's existing `isr_common_stub`; this file only owns what a
  plain function call wouldn't already preserve.
  `sched.{h,c}`: `task_t` (saved rsp, kernel stack pointer, state,
  id, entry+arg), a fixed 8-task table, `sched_init` (turns the calling
  context into task 0), `task_spawn` (kmalloc's an 8 KiB stack, fabricates
  its initial frame, marks it READY), `schedule` (round-robin scan for
  the next READY task, context-switches if it differs from current), and
  `task_exit` (marks TERMINATED, calls `schedule` one last time - never
  returns, since a TERMINATED task is never picked again). A
  `scheduler_tick` function registered via `pit_set_tick_hook` (new in
  `pit.h`/`pit.c`) counts `SCHED_QUANTUM_TICKS` (5, i.e. 50 ms) before
  actually calling `schedule`, giving each task a real time slice instead
  of rescheduling on every single 10 ms tick.
  **Bug 1 (caught via testing):** a freshly spawned task, resumed for the
  first time via `context_switch`'s plain `ret` rather than a real
  `iretq`, silently inherited `IF=0` - every IDT gate here is a 64-bit
  *interrupt* gate (M4), which clears IF on entry, and only `iretq`
  restores it. The task would run its first line then hang forever with
  interrupts permanently disabled. Fixed by having `context_switch`
  `pushfq`/`popfq` RFLAGS explicitly alongside the callee-saved registers,
  and giving each fabricated new-task stack a real RFLAGS value (`0x202`,
  IF set) to pop.
  **Bug 2 (caught via testing, after fixing bug 1):** with IF fixed, a
  new symptom appeared - the very first preemption still hung. Root
  cause: `irq_handler` (`isr.c`) sent the PIC's EOI *after* dispatching to
  the registered handler. When the timer's handler calls `schedule` and
  that switches away entirely, the call never "returns" in the normal
  sense - it only unwinds back through `pit_irq`/`irq_handler` much later,
  whenever that exact task is next resumed. Until EOI is sent, the 8259
  withholds every further interrupt on the line, so the timer simply
  never fires again. Fixed by moving `pic_send_eoi` to *before*
  dispatching in `irq_handler`, so it goes out regardless of whether or
  when the handler call ever returns.
  Verified via `tools/qemu-serial-test.sh` with two demo tasks that each
  print 5 lines separated by a real (PIT-tick-based, not instruction-
  count-based, so it's independent of host CPU speed) CPU-bound busy-spin
  spanning multiple scheduler quanta: log shows genuine interleaving
  (`A0,A1,B0,B1,A2,A3,B2,B3,A4,B4`), both tasks exit cleanly, and
  `kernel_main` (task 0) resumes and prints its own "back on the main
  task" message afterward. `-d int,cpu_reset` over the same run shows
  only the expected `v=03` (int3) and ~450 `v=20` (IRQ0) events - no
  faults, no resets.
  Next: M8 (system call interface — ABI, `system_api/` headers, dispatch
  table, `write`/`exit`/`getpid`).
- 2026-08-21 — M8 complete: a real syscall boundary, still exercised from
  ring 0 only (M9 puts a ring-3 caller on the other end of it).
  `system_api/include/syscall.h`: the shared ABI contract - `int 0x80`
  chosen over `syscall`/`sysret` (no MSR setup needed, reuses the IDT/ISR
  machinery M4 already built), calling convention `rax` = number in /
  return value out, `rdi`/`rsi`/`rdx`/`rcx`/`r8`/`r9` = args 1-6 (plain
  SysV order - `int 0x80` doesn't clobber `rcx`/`r11` the way the raw
  `syscall` instruction does, so there's no need for Linux's r10-instead-
  of-rcx shuffle). Numbers for `SYS_write`/`SYS_exit`/`SYS_getpid`.
  Kernel side: `isr_asm.asm` gained a dedicated `isr128` stub +
  `syscall_common_stub` (parallel to, not reusing, `isr_common_stub`/
  `irq_common_stub` - a syscall's dispatch semantics are different enough
  from "unhandled exception -> panic" or "unhandled IRQ -> ignore" to
  deserve its own path) that calls a new `syscall_handler`
  (`kernel/arch/x86_64/syscall.c`); `idt.c`'s `idt_set_gate` grew a
  `type_attr` parameter so vector `0x80` can be installed at DPL3 (`0xEE`)
  while every other gate stays DPL0 - ring 0 can invoke any gate
  regardless, so this doesn't change M8's kernel-only self-test, but it's
  the correct, forward-looking setting for M9's ring-3 callers.
  `sys_write` only honors fd=1 (stdout, routed through `klog_puts` - no
  real fd table until M12's VFS); `sys_exit` calls the M7 scheduler's
  `task_exit()` (task id doubles as pid until M9 gives "process" a
  meaning distinct from a bare kernel thread); `sys_getpid` returns
  `sched_current()->id`. Hit one naming collision immediately: the
  kernel-side entry header and the shared ABI header were both going to
  be called `syscall.h`, which would have made a quoted `#include
  "syscall.h"` from `syscall.c` always resolve to the local one (same-
  directory search wins) and silently never reach the real ABI header -
  renamed the kernel-side one to `syscall_entry.h` before it became a
  live bug.
  Verified via `tools/qemu-serial-test.sh`: `SYS_getpid` from
  `kernel_main` (task 0) returns `0`; `SYS_write` round-trips a real
  message through `int 0x80` -> dispatch -> `klog_puts` and returns the
  correct byte count; a dedicated task spawned specifically for this test
  calls `SYS_getpid` (gets its own id, `3`) then exits via `SYS_exit`
  rather than just returning, proving that path terminates the task
  correctly instead of just relying on the trampoline's implicit
  `task_exit()` call. `-d int,cpu_reset` shows exactly 4 `v=80` events
  (one per `do_syscall` call made) alongside the expected `v=03`/`v=20`
  traffic - no faults.
  Next: M9 (user mode & process loading — ring 3 transition, ELF64
  loader, per-process user stack).
- 2026-08-21 — M9 complete: real ring-3 user processes, each in their own
  address space, running actual ELF64 executables end to end.
  **GDT** (`gdt.c`/`gdt.h`): appended user code/data descriptors (DPL3,
  selectors `0x28`/`0x30` - kept after the existing null/kernel/TSS
  entries so no existing selector constant had to change) and
  `tss_set_rsp0()`, since the CPU needs a valid ring-0 stack (TSS.RSP0) to
  switch to on every ring3->ring0 transition - and with more than one
  user task, that stack has to change per task, so the scheduler now sets
  it on every context switch.
  **VMM** (`vmm.{h,c}`): refactored to support more than one address
  space. `table_walk`/`vmm_map_page` now take an explicit PML4 rather
  than always assuming the kernel's own; `vmm_create_address_space()`
  builds a fresh PML4 that shares PML4[0] (the identity map + heap) with
  the kernel's, so ring-0 interrupt/syscall handlers keep working no
  matter whose CR3 is loaded, while a `VMM_FLAG_USER` (the hardware U/S
  bit) marks pages accessible from ring 3 - propagated through every
  level of a freshly built table chain, never into the always-supervisor
  kernel hierarchy. This works without any "temporarily switch CR3 to
  build tables" trick because every pmm-allocated frame - including a
  brand-new process's own page-table storage - lives inside the 1 GiB
  that's *always* identity-mapped in every address space, so raw
  physical writes are safe regardless of which CR3 happens to be loaded
  at the time.
  **Scheduler** (`sched.{h,c}`): `task_t` gained `kernel_stack_top` and
  `pml4_phys`; `schedule()` now calls `tss_set_rsp0` and (skipping a
  needless TLB-flushing reload when the address space hasn't actually
  changed - the common case for plain kernel tasks) `vmm_switch_address_
  space` on every switch. `task_spawn_in()` is the same primitive
  `task_spawn` already was, parameterized by address space.
  **ELF loader** (`kernel/proc/elf.c`, new): parses a trusted, in-memory
  ELF64 header + program headers, maps each `PT_LOAD` segment page by
  page into the target address space (`vmm_map_page_in`), copying the
  correct file-backed slice into each frame *before* mapping it (a
  segment's file/BSS boundary can fall mid-page, so this is computed per
  page, not per segment) and zeroing everything else - no libc `memcpy`/
  `memset` available yet, so two tiny local helpers. No validation beyond
  magic/type/machine - there's no untrusted-input story until M12 gives
  the kernel something other than its own embedded blob to load.
  **Ring 3 entry** (`kernel/proc/enter_user_mode.asm` + `proc.c`, new):
  `enter_user_mode` builds an `iretq` frame by hand (the only instruction
  that can drop privilege and load a new CS/SS/RSP in one step - the same
  "CS can't be loaded with `mov`" rule stage2.asm and `gdt_flush` already
  ran into, one privilege level further). `proc.c`'s `process_spawn`
  glues it together: new address space -> `elf_load` -> map a 16 KiB user
  stack -> `task_spawn_in` with a launcher whose only job is to call
  `enter_user_mode` - reusing M7's *entire* task/context-switch machinery
  unmodified, since from the scheduler's point of view a "user task" is
  just a kernel task whose first move happens to be dropping to ring 3.
  Termination reuses M8's `task_exit` too: `SYS_exit` called from ring 3
  arrives via the ordinary ring3->ring0 syscall path (TSS.RSP0-switched
  stack) and calls the exact same `task_exit()` a kernel-only task's
  `SYS_exit` call already did.
  **Bring-up payload** (`kernel/proc/ring3_test/`, new): since there's no
  filesystem yet (M12) and `user_space/`'s own runtime doesn't exist yet
  either (M10), M9's self-test needed *some* real ELF to load - a tiny,
  deliberately standalone program (own `.asm`, own linker script, no
  relation to the future `user_space/lib`) that calls `SYS_write` then
  `SYS_exit` via `int 0x80`. Built and linked entirely separately from
  the kernel (a real ELF64 executable - unlike the kernel's own
  objcopy'd-flat boot image, since `elf_load` needs actual program
  headers to parse), then `embed.asm` `incbin`s the resulting file into
  the kernel image as a data blob. The Makefile grew a real build-order
  dependency for this (`embed.o` needs `ring3_test.elf` to exist first,
  the same shape of problem as stage2 needing the kernel's sector count)
  and excludes `ring3_test.asm` from the normal kernel-object glob, since
  it's linked into its own separate binary, not into `kernel.elf`.
  **A genuine heisenbug, investigated and resolved (not just papered
  over):** partway through verification, task C's `SYS_getpid` (from M8's
  self-test, still kernel-only) briefly returned `0` (main's id) instead
  of its own `3`, but only on one run, and adding diagnostic prints to
  chase it made it disappear rather than pin it down. After reasoning
  through the full preemption path in detail (in particular, whether the
  `pushfq`/`popfq` IF=0 window mid-unwind, from M7's earlier bug fix,
  could leak into a resumed task's visible state) and confirming it
  self-heals via the subsequent `iretq` restoring the *real* saved
  RFLAGS, the conclusion was that the anomaly was environmental - a
  stray, still-running QEMU process left over from an earlier `sendkey`
  test contending for CPU - not a logic bug: six consecutive clean runs
  afterward (with the debug instrumentation fully removed again) never
  reproduced it. Recorded here rather than silently dropped, since "ran
  clean afterward" and "root-caused" are different confidence levels and
  this is the former.
  Verified via `tools/qemu-serial-test.sh`: `[elf] loaded, entry =
  0x8000000000` followed by the test program's own `Hello from ring 3!`
  - printed by the kernel's `SYS_write`, but *invoked from ring 3* - and
  a clean `[proc] ring3_test task ran and terminated.`. `-d int,cpu_reset`
  confirms it for real: the last two `v=80` (syscall) frames show
  `cpl=3`, `CS=002b`/`SS=0033` (`0x28|3`/`0x30|3` - the user selectors
  with RPL 3), `RIP` inside `0x8000000000`'s range, and `RSP=
  0x8000200000` (the configured user stack top) - genuine ring-3
  execution, not a same-privilege illusion. 6 total `v=80` events across
  the whole boot (2 from `kernel_main`'s own M8 self-test, 2 from task
  C, 2 from `ring3_test`), matching exactly; no faults, no resets.
  Next: M10 (user-space runtime — `user_space/lib` crt0, syscall
  wrappers, hand-written string/mem helpers).
- 2026-08-21 — M10 and M11 complete together: a real, reusable user-space
  runtime, proven by a real program built against it - implemented as one
  step since a library with no consumer can't be verified as more than
  "it compiles" (the same reason M9 needed its own throwaway test payload
  to prove itself).
  **`user_space/lib`** (M10): `crt0.asm` (`_start` - calls `main()`, then
  `sys_exit()` with its return value; RSP arrives already 16-byte aligned
  since `enter_user_mode` sets it to a page-aligned stack top, so no
  alignment dance needed before the first `call`). `syscall_wrappers.{h,c}`
  (`sys_write`/`sys_exit`/`sys_getpid`, thin wrappers around the same
  `int 0x80` calling convention `kernel.c`'s M8 self-test used inline -
  header deliberately named `syscall_wrappers.h`, not `syscall.h`, to
  avoid the exact same local-file-shadows-shared-header collision that
  `kernel/arch/x86_64/syscall_entry.h` was renamed to dodge). `str.{h,c}`
  (`memcpy`/`memset`/`strlen`/`strcmp`, hand-written - no libc anywhere in
  this project; header named `str.h` rather than `string.h` as a
  precaution against the same class of collision, even though this
  toolchain has no real libc installed to actually collide with).
  `user.ld`: every user program's linker script, `ENTRY(_start)` at
  512 GiB - the same PML4[1]-private region M9's bring-up payload used,
  now the one canonical location instead of a one-off.
  Hit a build error immediately: `-fno-pic` at 512 GiB overflows the
  default "small" code model's 32-bit-relocation assumption
  (`relocation truncated to fit: R_X86_64_32S`) the moment code takes the
  address of anything in `.rodata`/`.data`. Fixed with `-mcmodel=large`
  in the Makefile's new `USER_CFLAGS` - not full PIC (unnecessary
  overhead for statically-linked, position-*dependent* binaries loaded at
  a fixed address), just a code model that can reference 64-bit addresses.
  **`user_space/bin/hello.c`** (M11): the real deliverable - built purely
  against `user_space/lib` + `system_api`, calls `sys_write` (a real
  message) and `sys_getpid` (formats the result by hand into a fixed
  buffer - no `itoa`/`printf` yet, not worth writing for one call site),
  then returns normally through `crt0`'s `sys_exit`.
  M9's `ring3_test` scaffold (its own hand-rolled asm, its own embed
  step) is retired now that a real user-space program exists to prove the
  same mechanism more honestly - deleted rather than left alongside as
  dead weight, with the Makefile/`kernel.c` wiring pointed at
  `build/hello.elf` (`kernel/proc/embed_hello.asm`) instead.
  Verified via `tools/qemu-serial-test.sh`: `[elf] loaded, entry =
  0x8000000000`, then `hello`'s own two `SYS_write` messages - "Hello,
  world from user_space!" and "hello: my pid is 0x0000000000000004" (pid
  4 is correct: main=0, demo tasks A/B=1/2, M8's syscall-exit test task=3,
  `hello`=4) - followed by a clean "hello task ran and terminated."
  `-d int,cpu_reset` shows 8 total `v=80` events for the whole boot (2
  from `kernel_main`'s own M8 self-test, 2 from task C, 4 from `hello`:
  write, getpid, write, exit via `crt0`) - matching exactly - and the last
  three all show `cpl=3` at the identical `int 0x80` instruction address
  inside `do_syscall` (all three wrappers share one call site, so of
  course they match), confirming real ring-3 execution through the actual
  library, not a same-privilege illusion. No faults, no resets.
  Next: M12 (storage & filesystem — ATA PIO driver, a minimal filesystem
  + VFS, loading user binaries from disk instead of an embedded blob).
- 2026-08-21 — M12 complete: real persistent storage, replacing the
  embedded-blob loading M9-M11 used with genuine disk I/O.
  `ata.{h,c}`: PIO-mode primary-master driver, polling (no IRQ14 - PIO is
  inherently CPU-driven anyway), 28-bit LBA. `ata_read_sectors`/
  `ata_write_sectors` bound their BSY/DRQ polls (`ATA_POLL_LIMIT`) and
  panic on a timeout or the ERR status bit rather than hanging forever -
  same "diagnosable panic over silent corruption" discipline as every
  other driver here. Assumes the boot disk itself is the primary master,
  true for QEMU's default `pc` machine with a bare `-drive`.
  `io.h` gained `inw`/`outw` (16-bit port I/O) - ATA's data port moves a
  sector 16 bits at a time.
  `kernel/fs/leanfs.{h,c}`: a small bespoke filesystem (not FAT, not
  ext2 - milestones.md's own "minimal custom format" allowance), flat
  (no subdirectories), fixed-size inode table (32 entries, 16 direct
  blocks each = 8 KiB max file size, no indirect blocks - a deliberate
  cap, not an oversight), a 1-sector free-block bitmap (4096 blocks =
  2 MiB of data region), starting at a fixed LBA (2048, 1 MiB in) with a
  build-time guard (`Makefile`) that fails the build if the boot image
  ever grows into it. `leanfs_init()` formats a fresh filesystem
  automatically if the superblock magic doesn't match - nothing to
  migrate, this format has only ever had one version. The inode table and
  bitmap are mirrored fully in memory after init (both are tiny - a few
  KiB total) and written back on every mutation, rather than re-reading
  from disk per lookup.
  `kernel/fs/vfs.{h,c}`: a thin, honest pass-through to leanfs - the seam
  a second filesystem would plug into if this project ever had one, not
  a driver-table/vtable architecture that would be speculative generality
  for exactly one implementation.
  `kernel/lib/libk.{h,c}` (new): `k_memcpy`/`k_memset`/`k_strlen`/
  `k_strcmp`/`k_strlcpy` - split out once leanfs.c became the *second*
  kernel-side file needing the same handful of primitives `elf.c` had
  already hand-rolled locally for itself; `elf.c` was refactored onto
  the shared versions in the same pass rather than leaving two copies.
  `kernel_main` now calls `vfs_init()`, seeds a "hello" file from the
  still-embedded `hello_elf_start` blob only if it doesn't already exist
  (first boot only), then - unconditionally - reads "hello" back via
  `vfs_read` and passes *that* buffer to `process_spawn`, not the
  embedded pointer. The embedded blob's only remaining job is that
  one-time seed, exactly matching this milestone's own framing ("instead
  of an embedded/in-memory blob").
  Verified via `tools/qemu-serial-test.sh` across two separate boots of
  the same disk image: first boot shows `[fs] no valid leanfs superblock
  found - formatting fresh` and `[fs] seeding disk with the embedded
  hello binary`, then loads and runs it from disk with the same output
  M11 already verified. Second boot shows neither message - straight to
  `[fs] leanfs ready` and loading "hello" from disk - proving the format
  and the seeded file both genuinely persisted in the QEMU disk image
  file itself, not just in kernel memory. `-d int,cpu_reset` shows no
  faults or resets across either run.
  Next: M13 (init & shell — fork/exec-equivalent syscalls, `wait`,
  `user_space/init` as PID 1, a minimal shell, coreutils).
- 2026-08-21 — M13 complete: the OS is interactive now. This is the
  milestone where `kernel_main` stops running a fixed self-test sequence
  and halting, and instead hands off to a real init/shell that keeps the
  machine alive indefinitely - a real behavioral shift, not just more
  code.
  **Syscall ABI grew five entries**: `SYS_spawn(path, arg)` combines
  fork+exec into one call - a fork/exec *equivalent* (the milestone's own
  wording), not literal `fork()`: no address-space duplication, just
  `vfs_read` the named file and hand it to M9's `process_spawn`. Real
  `fork()` (especially with copy-on-write) is meaningfully more machinery
  for no benefit a shell that just runs one foreground command at a time
  actually needs. `SYS_wait(pid)` polls `sched_task_by_id(pid)->state`
  and cooperatively yields (`schedule()`) each iteration rather than a
  real blocking wait queue - correct, not efficient; "more complete wait
  semantics" is explicitly M14's job per its own checklist wording, so
  this is scoped exactly where the project already expected it to land.
  `SYS_read(fd, buf, len)` (fd=0/keyboard only) blocks the same
  cooperative way until at least one byte is ready. `SYS_readfile`/
  `SYS_listfiles` are whole-file-by-name and flat-directory-listing
  reads - no open/close/fd-table/lseek yet, since nothing needs one
  (`cat`/`ls` are the only two callers, and both want "the whole thing at
  once").
  **`task_t` gained `exit_code`** (`task_exit_with_code`, threaded through
  from `SYS_exit`'s argument - previously discarded) and
  `sched_task_by_id()`; `MAX_TASKS` went 8 -> 64 since a shell spawns a
  fresh task per command instead of a fixed handful of demo tasks.
  **A minimal argv mechanism** (not real argc/argv): `process_spawn`
  gained an optional single string `arg`, written into a dedicated page
  just past the user stack (`USER_ARG_ADDR`) in the process's private
  address space; `enter_user_mode` (`kernel/proc/enter_user_mode.asm`)
  loads that page's address into RDI right before the `iretq`, and
  `crt0.asm` passes RDI straight through as `main`'s first argument via
  the normal SysV convention - no crt0 changes needed at all, since RDI
  already flows through untouched. `SYS_spawn`'s second argument is
  exactly this string, so the shell splits a typed line at its first
  space and passes the remainder as-is.
  **`user_space/bin/{echo,cat,ls}.c`, `user_space/shell/shell.c`,
  `user_space/init/init.c`** (all new): `shell.c` owns line editing
  (echo, backspace) itself via repeated `SYS_read(fd=0, buf, 1)` calls -
  that's a real tty's job in a real OS, kept out of the kernel's `SYS_read`
  on purpose. `init.c` loops spawning `"shell"` and waiting on it,
  respawning if it ever exits - the same "the login shell should always
  come back" behavior a real init's getty loop has, at the minimum that's
  actually meaningful without signals or a service table.
  Six programs now need building/embedding instead of one - the Makefile
  grew a `USER_PROGRAMS` list and a generic `$(BUILD)/%.elf` pattern rule
  instead of hand-copying M11's hello-specific rules five more times;
  `kernel/proc/embed_programs.asm` incbin's all six (written out longhand,
  not via a parameterized macro - nasm doesn't substitute macro
  parameters inside a double-quoted string, so `incbin "build/%1.elf"`
  doesn't work). `kernel_main` seeds every program onto disk on first
  boot (same one-time-seed pattern M12 established), then spawns `init`
  and becomes the idle task (`hlt` in a loop) instead of returning -
  letting `kernel_main` return would fall into `entry.asm`'s post-return
  `cli`, permanently disabling interrupts and freezing the scheduler for
  every other task, which is exactly wrong once the machine's whole point
  is staying alive for interactive use.
  Verified interactively via `tools/qemu-serial-test.sh` plus QEMU
  monitor `sendkey` injection (typing real commands, not just single
  keys, this time - one `sendkey` per character with an explicit `spc`
  for spaces): `ls` correctly lists all six seeded files; `echo hi there`
  prints "hi there" (proving the arg-splitting and the argv mechanism
  both work, not just that `echo` runs); `hello` runs correctly with a
  fresh, correctly-incrementing pid; `exit` terminates the shell and
  *init visibly respawns a new one* - the clearest possible proof
  `SYS_wait` and the respawn loop are both real. `-d int,cpu_reset` over
  a full run shows no faults and (compared to every prior milestone) a
  much higher `v=20` (timer) count with no `cli` ever seen - direct
  confirmation the machine now idles forever instead of halting, exactly
  as designed.
  Next: M14 (IPC & process management — pipes, signals, process groups,
  more complete `wait` semantics).
- 2026-08-21 — M14 complete: the kernel-side IPC/signal/process-management
  work (`kernel/ipc/pipe.{h,c}`, `system_api/include/signal.h`, and
  `SYS_kill`/`SYS_pipe`/`SYS_getpgid`/`SYS_wait(-1)` in
  `kernel/arch/x86_64/syscall.c`) already existed from the previous
  session, along with matching `task_t` fields (`fds[]`, `pgid`,
  `pending_signal`, `parent_id`, `reaped` - `sched.h`/`sched.c`) and
  user-space wrappers (`user_space/lib/syscall_wrappers.{h,c}`). What was
  left broken was `kernel_main`'s self-test wiring in `kernel.c`: a build
  error (a call to a nonexistent `k_strcmp_prefix_pingpingping`, clearly
  a stand-in never finished) and four self-test task bodies
  (`pipe_producer_task`/`pipe_consumer_task`/`spinner_task`/`quick_task`)
  defined but never actually spawned from `kernel_main` - `-Werror`
  correctly refused to build with them unused.
  Fixed the bogus comparison (`k_strcmp(buf, "pingpingping")` against the
  now-null-terminated received buffer, pulling `k_strcmp` in via a new
  `#include "lib/libk.h"`) and wired up five self-tests in `kernel_main`,
  same "prove it, don't just trust it compiled" discipline as every
  earlier milestone: (1) a direct `pipe_create`/`pipe_write`/`pipe_read`
  producer-consumer round trip where the consumer genuinely blocks
  before the producer's written anything; (2) the same thing through the
  syscall boundary (`SYS_pipe` installs fds into task 0's own table,
  `SYS_write`/`SYS_read` move real data through them); (3) `SYS_kill`
  against a `spinner_task` that never yields or syscalls, proving
  delivery really does happen via `scheduler_tick`'s per-tick check
  (`sched.c`) rather than only at a syscall boundary, and that the exit
  code comes back as `128 + SIGTERM`; (4) `SYS_wait(-1)` draining any
  unreaped children left over from earlier self-tests, then spawning
  exactly two fresh ones and confirming it reaps precisely those two
  (either order) before correctly returning `-1` once none remain; (5)
  `SYS_getpgid` confirming a spawned child inherits its parent's process
  group.
  Verified via `tools/qemu-serial-test.sh`: all five self-test messages
  print "passed" in sequence, consumer output shows the expected
  `"pingpingping"` (3x `pipe_write(p, "ping", 4)` from the producer).
  `-d int,cpu_reset` over a 7s run shows 24 `v=80` (syscall) events and
  692 `v=20` (timer) events, no faults - the two `CPU Reset` lines in the
  log are both at the very top (QEMU's own pre-BIOS boilerplate, same as
  every prior milestone's verification), none during actual kernel
  execution. Also re-ran the full M13 interactive shell flow (`ls`,
  `echo hi there`, `hello`, `exit` via QEMU monitor `sendkey` injection)
  as a regression check since this touched `kernel_main` - all four
  still work exactly as before, including init respawning the shell
  after `exit`.
  This closes out every milestone through M14 - the numbered list in
  this file. What's left is the explicitly-unordered "Stretch goals"
  section below (SMP, networking, UEFI, real-hardware boot, third-party
  package tooling) - each one a substantially larger, independent body
  of work rather than a next sequential step, so which (if any) to
  pursue is an open decision rather than an assumed "next."
- 2026-08-22 — M15 complete: leanfs gained singly-indirect block support
  and a much bigger data region, closing the concrete blocker flagged
  when this milestone was scoped - existing user ELFs (~7 KiB) were
  already nearly at the old 8 KiB direct-only cap, before any GUI/toolkit
  code existed to make it worse.
  `leanfs_inode_t` gained one `indirect` field: a block number pointing
  at a data block used as a 128-entry `uint32_t` pointer table (one 512 B
  block / 4 bytes per pointer). `LEANFS_MAX_FILE_SIZE` is now
  `(16 direct + 128 indirect) * 512 B` = 72 KiB, deliberately stopping at
  one level of indirection - doubly-indirect would be more complexity
  than anything this project ships actually needs. `leanfs_read`/
  `leanfs_write` both grew a `block_for_index`-style branch (direct
  lookup below block 16, else lazily-read/written indirect table above
  it); `leanfs_write`'s allocation-failure rollback path now has to unwind
  both direct and indirect-table entries plus the indirect block itself,
  not just a flat `direct[]` array - the trickiest part of this change,
  since a partial allocation mid-indirect-table isn't yet reflected in
  `inode->size` and the indirect table isn't on disk yet, so rollback
  works off local counters rather than re-reading anything back.
  Data region grew from `LEANFS_DATA_BLOCKS` = 4096 (2 MiB, 1-sector
  bitmap) to 65536 (32 MiB, 16-sector bitmap) - sized for several GUI app
  binaries plus a toolkit and font assets, not just a handful of ~7 KiB
  coreutils. `Makefile`'s `FS_TOTAL_SECTORS`/`IMAGE_SECTORS` grew to match
  (65560 sectors for the filesystem region; 69632-sector, ~34 MiB disk
  image overall, with headroom past the filesystem's own end).
  Added a new self-test in `kernel_main` (`kernel/kernel.c`), right after
  the existing M12 disk-seeding step: every file that existed before this
  milestone (the seeded coreutils) fits entirely in direct blocks, so
  none of them would have exercised the new indirect path at all -
  "compiles" isn't "works", so a dedicated 20000-byte buffer (deliberately
  past the 8 KiB direct-only boundary, well inside the new 72 KiB cap) is
  written to a file named `fstest` and read back byte-for-byte.
  Verified via `tools/qemu-serial-test.sh`: `[fs] leanfs ready:
  data_lba=0x00000818 data_blocks=0x00010000` (0x818 = 2072 = 2048 +
  1 superblock + 7 inode-table + 16 bitmap sectors, confirming the new
  layout math; 0x10000 = 65536 data blocks), followed by `[fs] leanfs
  indirect-block self-test passed (20000-byte round trip)`. Re-ran the
  same image a second time without rebuilding (same persistence check
  M12 relied on) - no "formatting fresh" message the second time,
  confirming the new on-disk layout persists correctly across boots.
  Also re-ran M13's interactive shell regression check (QEMU monitor
  `sendkey` injection) - `ls` correctly lists all seven files now
  (the six coreutils plus `fstest`), confirming the self-test's file
  coexists cleanly with normal filesystem use rather than corrupting it.
  `-d int,cpu_reset` over both runs shows no faults.
  Next: M16 (linear framebuffer & graphics primitives - VBE mode set in
  stage2, framebuffer info threaded through the boot handoff struct,
  `kernel/drivers/fb.c` pixel/rect/blit primitives).
- 2026-08-22 — M16 complete: lean_os has real graphics now - a VESA VBE
  linear framebuffer set up entirely from scratch in 16-bit real mode,
  mapped and driven by a new kernel driver.
  `kernel/boot/stage2.asm` gained `setup_vbe_mode` (called from `start:`
  right after `collect_e820_map`, still in real mode - the only place a
  BIOS `INT 10h` VBE call can happen): `AX=4F00h` fetches a VbeInfoBlock
  (after writing the "VBE2" signature into the buffer first, since some
  BIOSes only populate the VBE2-only linear-mode-list pointer if asked
  that way), a new `scan_modes` helper walks that mode list looking for
  one that's supported, graphics-capable, and linear-framebuffer-capable
  (`ModeAttributes` bits 0 and 7) at a target resolution/depth - tried at
  1024x768x32 first, falling back to 800x600x32 for less-capable
  BIOSes/VMs. `AX=4F02h` sets the matched mode with bit 14 (linear
  framebuffer model), and the matched ModeInfoBlock's `PhysBasePtr`/
  `BytesPerScanLine`/`XResolution`/`YResolution`/`BitsPerPixel` get
  packed into a new compact `fb_boot_info_t` at a fixed low-memory
  address (`FB_INFO_ADDR` = 0x9B00, alongside the existing E820 scratch
  structures) - any VBE failure halts with a diagnosable message via the
  same pattern `load_kernel`'s `.disk_error` already used, rather than
  silently continuing into a kernel that assumes graphics exist.
  Boot handoff grew a second argument: `long_mode_entry` now loads RSI
  (the System V ABI's second integer-arg register) with `FB_INFO_ADDR`
  alongside RDI's existing E820 pointer - `kernel_main` picked it up as
  `fb_boot_info_t *fb_info` with zero entry.asm changes needed (a plain
  `jmp`, not a `call`, so RDI/RSI just ride through from stage2 untouched).
  New `kernel/drivers/fb.{h,c}`: `fb_init` maps the framebuffer's physical
  region into the kernel's address space page-by-page via the existing
  `vmm_map_page` (identity-style, virt == phys - PCI MMIO framebuffer
  addresses live well above the kernel's 1 GiB huge-page identity range
  on every target tested, so this never collides with it; `vmm_map_page`
  itself would panic outright if that ever stopped being true, rather
  than silently corrupting a huge mapping). `fb_put_pixel`/`fb_get_pixel`/
  `fb_fill_rect`/`fb_clear`/`fb_blit` treat the framebuffer as packed
  XRGB8888 (the near-universal layout for 32bpp VBE/Bochs direct-color
  modes, and the only depth `setup_vbe_mode` ever requests) and panic on
  out-of-bounds coordinates - the same "diagnosable panic over silent
  corruption" discipline as every other driver in this kernel.
  `kernel_main` calls `fb_init` right after the heap self-tests (needs
  `vmm_init` already live), then a new self-test: clear to a background
  color, fill a smaller rectangle with a different one, and read three
  pixels back (inside the rectangle, outside it, and the origin) to
  confirm the write landed exactly where expected.
  Verified via `tools/qemu-serial-test.sh`: `[fb] framebuffer at
  0x00000000FD000000 00000400x00000300 pitch=0x00001000 (0x300 pages
  mapped)` (1024x768, pitch exactly 4096 = 1024*4 with no padding, 768
  pages = 1024*768*4/4096 exactly) and `[fb] framebuffer clear/fill/
  readback self-test passed`. `-d int,cpu_reset` over a 5s run: 490 `v=20`
  (timer) + 20 `v=80` (syscall) + 1 `v=03` (int3 self-test), no faults.
  Went further than an in-kernel readback self-test can prove, though:
  reading back your own writes only proves the mapping and pixel math are
  right, not that anything actually reaches the emulated display - so
  also took a real QEMU `screendump` mid-boot (monitor pipe, same
  technique M1-M5 used before serial logging took over) and sampled its
  PPM pixel data programmatically. Background pixel (0,0) = (26,26,46) =
  0x1A1A2E, inside the rectangle (60,35) = (233,69,96) = 0xE94560, and
  outside it (200,200) back to 0x1A1A2E - an exact match against what the
  self-test wrote, confirming QEMU's emulated VBE display genuinely
  renders this framebuffer, not just that the memory round-trips.
  Deliberately left VGA text mode (`kernel/drivers/vga.c`) and its klog
  fan-out both in place rather than retiring them in this milestone as
  originally scoped - they're still the only logging surface until M17
  gives the framebuffer console something to replace them with; retiring
  them now would leave a boot with no visible log output at all in
  between milestones.
  Next: M17 (framebuffer text console + font rendering - embed a bitmap
  font, software glyph renderer, scrolling console over the framebuffer,
  then route klog through it and retire VGA text mode for real).
- 2026-08-22 — M17 complete: lean_os's boot log now renders as real
  graphical text over the M16 framebuffer, and a genuine concurrency bug
  got caught and fixed along the way.
  Font data (`kernel/drivers/font8x16.{h,c}`): 128 ASCII glyphs, 8x16
  1bpp each. Rather than transcribe a classic bitmap font from memory
  (risk of silent transcription errors) or add a font-rasterization
  dependency to the actual OS build, generated it once as an authoring
  step - rasterized each glyph from a system monospace font (Courier New
  Bold) at 4x supersampling then downsampled/thresholded to 8x16 - and
  committed the result as a plain static array, the same category of
  thing as this project's other embedded binary blobs. Caught during
  visual verification (not assumed correct because it compiled): five
  glyphs with descenders - g/j/p/q/y - didn't survive the downsample
  intact ('g' was visually indistinguishable from '9'). Two rounds of
  fixing: first attempt hand-authored replacements but placed them at
  cap-height instead of x-height, which made them read as accidental
  capitals next to correctly-sized neighbors ("keypress" rendering as
  "keYPress"); second attempt fixed the vertical alignment to match the
  x-height/baseline rows already established by the auto-rasterized
  lowercase letters (verified against 'i'/'a'/'n'/'v' byte patterns) and
  gave 'g' a distinctive swept-left foot so it can't be confused with
  '9'. Verified each round with an actual rendered pixel comparison
  before touching the kernel, not by eye on the byte table.
  `kernel/drivers/console.{h,c}`: fixed-cell scrolling text console
  (cols/rows computed from `fb_width()`/`fb_height()` / font cell size -
  128x48 at the current 1024x768 mode), handles `\n`/`\r`/`\b`/`\t`,
  scrolls via a new `fb_scroll_up` (fb.c) that memcpy's whole scanlines
  rather than per-pixel calls - the console is the first caller with any
  real per-frame volume, so this was worth doing properly from the start.
  `klog.c` gained `klog_use_console()`: VGA text mode stays the *only*
  visual output until graphics come up (console_init needs vmm live, so
  it can't happen at klog_init's very first call) - kernel_main calls it
  right after the M16 fb self-test, so everything from GDT/IDT setup
  through fb bring-up itself is VGA-only, and everything after is
  framebuffer-only. Satisfies the original milestone wording ("retire
  VGA as the primary display") without a boot-log blackout in between.
  **Real bug, caught on the very first boot with the console wired up:**
  a kernel panic, `fb_put_pixel: coordinates out of bounds`, right after
  init spawned the shell - the first point in boot with more than one
  runnable task that could call `klog_putc` concurrently. `console.c`'s
  cursor state (`cur_col`/`cur_row`) had zero locking - it never needed
  any before, because `vga.c`'s equivalent globals have had the exact
  same unprotected-shared-state shape since M3, but `vga_putc` just
  silently wraps/scrolls on a bad value instead of validating coordinates
  and panicking. The framebuffer console's stricter "diagnosable panic
  over silent corruption" bounds checking (the same discipline every
  other driver in this kernel already follows) is what turned a
  pre-existing, previously-invisible race into a hard crash. Fixed by
  giving `klog_putc` a proper critical section (`cli`/`pushfq`/`popfq`
  around the single-character draw - the standard single-core technique;
  no SMP yet per the stretch goals, so no second CPU to still race with)
  and changing `klog_puts` to call `klog_putc` per character instead of
  handing whole strings to `vga_puts`/`console_puts` unlocked. Confirmed
  by testing, not just reasoning: same boot sequence, same two
  concurrent tasks, panic gone.
  Verified via `tools/qemu-serial-test.sh`: fb self-test, console
  switchover message, and every M12-M14 self-test all still print
  "passed" in sequence (regression coverage - this touched shared logging
  infrastructure every prior milestone depends on). `-d int,cpu_reset`
  over a 5s run: 488 `v=20` + 20 `v=80` + 1 `v=03`, no faults. Went beyond
  the serial log again for the part serial can't show: three rounds of
  QEMU `screendump` + pixel-level PNG inspection confirmed first the
  crash's absence, then the g/j/p/q/y legibility bug, then its fix -
  cropped/zoomed regions of real boot-log text ("waiting up to 3s for a
  test keypress", "logging switched over from VGA text mode") read
  correctly with no ambiguous glyphs.
  Next: M18 (PS/2 mouse driver & cursor - IRQ12 packet decode, same
  shape as M6's keyboard driver, cursor sprite draw/erase over the
  framebuffer).
- 2026-08-22 — M18 complete: a real IRQ12-driven PS/2 mouse and a visible
  cursor, with a genuine sync bug caught and root-caused during bring-up
  rather than papered over.
  `kernel/drivers/mouse.{h,c}`: same overall shape as M6's keyboard
  driver (IRQ handler decodes into a small ring buffer, `mouse_read()`
  drains it non-blocking) but with a real init handshake first - enable
  the auxiliary device (`0xA8`), set defaults + enable reporting (`0xF6`/
  `0xF4` via the controller's "write to aux" command `0xD4`, each ACKed
  with `0xFA`), decode the standard 3-byte packet (status byte with
  button bits + sign/overflow flags, then X/Y deltas sign-extended from
  9-bit two's complement) - Y negated once, since the wire protocol is
  up-positive and screen coordinates are down-positive.
  `kernel/drivers/cursor.{h,c}`: an 8x8 hand-authored arrow sprite drawn
  directly over the framebuffer, save-then-draw on move and restore on
  erase - explicitly scoped as a stopgap (documented in the header): once
  a compositor (M20) can put other content under the cursor without going
  through this code, "restore what I saved" stops being correct.
  **Real bug, caught via raw-packet logging, not assumed away:** the
  first interactive test showed a cursor position wildly off from the
  commanded delta, and a spurious `buttons` bit that no command had set.
  Added temporary per-byte and per-ACK debug logging (removed once fixed)
  and found the actual fault: the very first IRQ12 event after unmasking
  delivered a phantom `0xFA` that didn't belong to any real movement
  packet, permanently shifting 3-byte packet sync by one from that point
  on - a plain output-buffer flush placed right before unmasking didn't
  help, which ruled out "leftover buffered byte" as the mechanism.
  Root cause: `mouse_init` was setting the controller's IRQ12-enable
  configuration bit *before* running the `0xF6`/`0xF4` handshake, so
  IRQ12 generation was live (at the 8042 level, independent of the PIC
  mask) during a polling-only exchange - something about that combination
  left a stale/latched interrupt condition that fired the instant the PIC
  mask itself was cleared, even though every byte up to that point had
  already been drained via polling. Fixed by reordering: the `0xF6`/`0xF4`
  handshake (and a defensive buffer flush) now happens entirely before
  the controller's IRQ12-enable bit is ever set, so that condition never
  gets a chance to latch. Verified precisely, not just "no more crash":
  logged raw packet bytes for an isolated `mouse_move 50 0` now show
  `status=0x08 dx=0x32(50) dy=0x00` exactly, and the resulting cursor
  position matched hand-computed expected coordinates exactly across
  three independent single-axis/button test cases (pure X, pure Y with
  sign inversion, and a button-only packet) before the debug logging was
  removed.
  Verified via `tools/qemu-serial-test.sh` (regression: every M12-M17
  self-test still passes, mouse driver reaches its own clean timeout path
  with no monitor input) and interactively via QEMU monitor `mouse_move`/
  `mouse_button` injection: exact-match coordinate checks as above, plus
  a `screendump` crop showing the actual arrow sprite correctly rendered
  at the commanded position. `-d int,cpu_reset` over the interactive run
  shows only the expected `v=20`/`v=80`/`v=2c` (IRQ12)/`v=21` (IRQ1)/`v=03`
  traffic, no faults.
  Next: M19 (user-space heap allocator & shared memory - `SYS_sbrk`-style
  syscall, `user_space/lib` malloc/free, a shared-memory syscall for
  compositor/app pixel buffers).
- 2026-08-22 — M19 complete: user-space processes can allocate memory
  dynamically for the first time in this project's history, and two
  independent processes can genuinely share physical memory - both
  proved by real ring-3 code, not just kernel-side plumbing.
  **Address space layout** (`kernel/proc/proc.h`, new constants - moved
  out of proc.c since syscall.c now needs to bounds-check against them
  too): `USER_HEAP_START`/`USER_HEAP_LIMIT` (512 GiB + 4 MiB, growing up
  to a 256 MiB ceiling) and `USER_SHM_BASE` (512 GiB + 512 MiB) - both
  comfortably clear of the existing stack/arg region, generous gaps
  rather than packed tightly so nothing has to reason about exact
  boundaries.
  **`SYS_sbrk`** (`kernel/arch/x86_64/syscall.c`): growth-only (negative
  increment rejected outright - nothing a free-list allocator does ever
  needs to give pages back), maps whole new pages on demand via
  `vmm_map_page_in`, same "grow by exactly what's needed" shape as the
  kernel's own `heap.c`. `task_t` (`sched.h`) gained `heap_brk`/
  `heap_mapped_end` (M19) plus `shm_next_vaddr` - all zeroed for a plain
  kernel thread (meaningless there, only a ring-3 process's syscalls ever
  read them) and set to their real starting addresses in `process_spawn`
  right after `task_spawn_in` returns.
  **`kernel/ipc/shm.{h,c}`** (new, alongside `pipe.h/c`): a flat global
  registry of shared segments - `shm_create(size)` allocates physical
  frames and registers them under an id, independent of any process's
  address space; `shm_map_into(id, pml4_phys, vaddr, flags)` maps an
  existing segment's frames into a given address space. `SYS_shm_create`/
  `SYS_shm_map` wrap these; a process's own `shm_next_vaddr` cursor picks
  where each of its own `SYS_shm_map` calls lands (same "only ever grows
  forward" pattern as the heap).
  **`user_space/lib/malloc.{h,c}`** (new): first-fit free list, same
  split/coalesce rules as `kernel/mm/heap.c`, grown via `sys_sbrk` instead
  of `pmm_alloc_frame`/`vmm_map_page` directly - the same design one
  privilege level up, not a different shape invented for user space.
  `free()` on an already-free block is a silent no-op rather than a
  panic (documented): user space has no abort/assert mechanism yet, so
  crashing the whole kernel over a user program's double-free would be
  the wrong failure mode.
  **`user_space/bin/memtest.c`** (new, seeded/embedded like every other
  coreutil): the actual proof. With no arg (the role `kernel_main` spawns
  it in): exercises malloc/free (allocate three blocks, verify no
  overlap, free the middle one, allocate something that should reuse it,
  verify the untouched blocks survived), then `shm_create`s a segment,
  writes a distinctive byte pattern into it, and spawns *a second copy of
  itself* via `sys_spawn("memtest", <id>)` - passing the shm id through
  lean_os's one-string argv mechanism - and waits for that child's exit
  code. With an arg (only ever reached by that spawn): maps the same id
  and verifies the pattern is really there. Two *different* processes,
  two *different* address spaces, same physical pages - this is what
  actually distinguishes shm from "one process talking to itself," and
  it's what the test is structured to prove.
  Verified via `tools/qemu-serial-test.sh` (needed a longer window than
  earlier milestones - one more embedded program and self-test genuinely
  pushed boot past 8s, not a hang): `[memtest] malloc/free self-test
  passed`, then `[memtest] cross-process shm self-test passed (writer +
  reader agree)`. `kernel_main` itself waits on the creator child via
  `do_syscall(SYS_wait, ...)` and panics on a nonzero exit code, so a
  regression here would stop boot outright, not just log a failure.
  `-d int,cpu_reset` over a 15s run: 1475 `v=20` + 37 `v=80` + 1 `v=21` +
  1 `v=03`, no faults. Re-ran the M13 interactive shell regression (`ls`
  via QEMU monitor `sendkey`) - now lists eight files including
  `memtest`, confirming the new program coexists cleanly with the
  existing filesystem/shell flow.
  Next: M20 (windowing compositor - a user-space process taking exclusive
  ownership of the framebuffer, an IPC protocol for apps to create
  windows and receive input events, z-ordered blitting).
- 2026-08-22 — M20 complete: a real user-space compositor
  (`user_space/bin/compositor.c`) now owns the framebuffer exclusively,
  and a real client (`wm_demo.c`) can get a window on it and draw into it
  - genuine ring-3 IPC, not kernel-side plumbing standing in for it.
  `system_api/include/wm.h` (new): `wm_fb_info_t` (`SYS_fb_info`'s
  output), and the window-creation protocol - a client writes a
  `wm_create_request_t` to the well-known named pipe `wm_req`
  (`SYS_pipe_open`, not an inherited fd, since the compositor and its
  clients aren't parent/child), the compositor replies with a
  `wm_create_response_t` (a shm id) on `wm_resp`. Three new syscalls in
  `kernel/arch/x86_64/syscall.c`: `SYS_fb_info`/`SYS_fb_map` (geometry,
  then the real mapped pixels - by convention only the compositor calls
  `SYS_fb_map`, unenforced, matching this project's no-permission-model
  trust level so far) and `SYS_mouse_read` (non-blocking, wraps
  `kernel/drivers/mouse.h`'s existing ring buffer).
  `compositor.c`: maps the real framebuffer, blocks on one client
  connection (`accept_one_window` - deliberately narrow, M21's own job to
  generalize into something that isn't one-shot), allocates a shm pixel
  buffer per window (`SYS_shm_create`/`SYS_shm_map`, M19), then redraws
  the desktop (background, window border/titlebar, window content
  blitted from the client's shm buffer, cursor) - continuously at first,
  then only on real mouse movement once a real perf problem showed up
  during bring-up (below).
  **Two real bugs, both caught during verification, not assumed away:**
  (1) a from-scratch software compositor doing per-pixel `fill_rect`/
  blit calls at the Makefile's previous `-O0` turned out genuinely too
  slow - a single full-screen redraw took close to a second. Fixed with
  `-O1` (Makefile's `CFLAGS`/`USER_CFLAGS`), which in turn required
  `-mgeneral-regs-only`: at `-O1` GCC started auto-vectorizing some code
  (struct copies) into SSE instructions, and this kernel never sets up
  FPU/SSE state (no `CR0`/`CR4` `OSFXSR` etc.), so the first one executed
  faulted as an invalid opcode - caught as a real boot panic, not
  anticipated in advance. (2) with `-O1` fixing the *speed*, the
  self-test's pixel-verification still failed inconsistently run to run -
  traced to redrawing unconditionally in a tight loop, which meant the
  overwhelming majority of sampled instants caught the frame mid-repaint
  (background cleared, window not yet composited back in) rather than a
  finished frame. Not a compositing bug at all - fixed by redrawing only
  when real input arrived, which also happens to be the more sensible
  behavior for a static desktop.
  Verified via `tools/qemu-serial-test.sh`: spawns the real compositor
  and `wm_demo` (a client that requests a 200x120 window and draws a
  deterministic pattern - solid content color, smaller accent square),
  waits for settlement, then reads pixels straight out of the physical
  framebuffer via the kernel's own `fb_get_pixel` (always live in every
  address space via `PML4[0]`, the exact same physical frames the
  compositor's `SYS_fb_map` points at - genuinely observing what the
  compositor drew, not a kernel-side copy) - 5/5 checks (content color,
  accent color, titlebar, border, desktop background) matched. Caught a
  third, subtler issue this way too: both the compositor's and the
  self-test's own status messages go through the same graphical console
  (M17) the compositor is compositing onto, so printing anything between
  drawing and verifying risked a console scroll shifting the frame under
  test out from under itself - fixed by finishing all output before the
  frame that needs to hold still, and capturing every pixel read into a
  local variable before printing anything about the results.
  Next: M21 (app UI toolkit + input routing - `user_space/lib` drawing
  API, focus-follows-click keyboard/mouse routing, 1-2 demo GUI apps).
- 2026-08-22 — M21 complete: a real drawing API, real multi-window input
  routing with focus-follows-click, and two real GUI apps proving the
  whole pipeline - closing the gap M20 itself flagged ("z-order... 
  unproven beyond one window").
  **Three new syscalls** (`system_api/include/syscall.h`,
  `kernel/arch/x86_64/syscall.c`): `SYS_kbd_read` (the `SYS_mouse_read`-
  shaped, never-blocks counterpart to `SYS_read(fd=0)`'s line-blocking
  contract - wraps the same `keyboard.h` ring buffer, so a process
  juggling several input sources in one loop, i.e. the compositor, isn't
  stuck picking one to block on), `SYS_pipe_poll` (bytes currently
  buffered on a pipe read end, without consuming - `pipe_t`'s `count`
  field was already right there, so this needed no new `pipe.c` entry
  point) and `SYS_uptime_ms` (a unit conversion over the PIT's existing
  tick counter - backs time-driven redraws that have nothing to do with
  input arriving). `MAX_FDS` (`sched.h`) bumped 8 -> 32 and
  `MAX_NAMED_PIPES` (`pipe.c`) bumped 8 -> 24 - a compositor now juggling
  several windows' worth of event pipes plus the request/response pair
  needed real headroom, the same "bump the fixed cap when a real need
  arrives" precedent as `MAX_TASKS`'s M13 8->64.
  **Input-routing protocol** (`wm.h`): `wm_event_t`
  (KEY/MOUSE_MOVE/MOUSE_BUTTON/FOCUS/UNFOCUS), one named pipe per window
  (`wm_event_pipe_name` - `wm_evt0`..`wm_evt9`, both compositor and
  client build the name from the same function so the scheme can't drift
  apart) - kernel/ipc/pipe.h's `pipe_read` consumes what it reads, so a
  shared channel across clients would mean two windows stealing each
  other's events, hence one pipe per window rather than one global one.
  `wm_create_response_t` gained `width`/`height` fields (the
  compositor's actual allocation, not an echo of the request) - not
  needed yet at M21 itself but added because M22's panel windows would
  need it and duplicating the response struct later would've been worse.
  **`user_space/lib/gfx.{h,c}`** (new): `gfx_put_pixel`/`fill_rect`/
  `draw_rect` (outline)/`draw_line` (integer Bresenham - no FPU/SSE state
  anywhere in this kernel, see M20's own note on that) /`draw_char`/
  `draw_text`, operating on a caller-supplied `gfx_ctx_t` (pixels + width
  + height) - has no idea a compositor or framebuffer exist at all, same
  separation `fb.c`/`console.c` keep kernel-side. Needs the bitmap font
  (M17's `kernel/drivers/font8x16.c`) but a user program can't link
  against the kernel image, so `user_space/lib/font8x16.{h,c}` is a
  verbatim duplicate of the data table - the same kernel/user split
  reasoning M20's compositor.c already used for its hand-authored cursor
  sprite.
  **`user_space/lib/wmclient.{h,c}`** (new): the connect/event handshake
  factored out once a *second* real client needed the exact same
  request/response/shm-map/event-pipe-open sequence M20's `wm_demo.c`
  had inlined for itself - `wm_connect`, `wm_wait_event` (blocking),
  `wm_poll_event` (non-blocking, for a client with its own reason to keep
  running with no input, i.e. the clock demo).
  **`compositor.c` rewritten** for M21: `accept_pending_window` is now
  non-blocking (`SYS_pipe_poll` before ever calling the blocking
  `SYS_read`) so the same loop that accepts new clients also drains
  input and redraws - M20's version could get away with one blocking
  accept because it only ever served one client, once. Tracks
  `focused_window`; a left-click inside a window's content-or-titlebar
  rect focuses it (a brand-new connection also takes focus immediately);
  every mouse/keyboard event gets translated to window-relative
  coordinates and routed to whichever window is currently focused.
  Redraws periodically (100ms) now, not just on input - M20's "only
  redraw on input" fix stops being sufficient once a client's content can
  change with nothing external driving it (the clock demo).
  **Two new demo apps**: `gui_clock.c` (a 200x90 window, static "CLOCK"
  caption drawn once via `gfx_draw_text`, a live "uptime: Ns" line
  updated from `sys_uptime_ms()` every 250ms - proves periodic,
  input-independent redraw reaches a real client) and `gui_paint.c` (a
  220x140 window with a static caption/border/separator drawn via
  `gfx_fill_rect`/`gfx_draw_rect`/`gfx_draw_line`, then a real event loop:
  mouse-move-with-left-button-held draws a small square at the routed
  window-relative position, the `'c'` key clears the canvas - the one
  demo that can only really be proven with live input).
  Verified two ways, matching M18's own precedent for input-shaped
  behavior: (1) automated, via `tools/qemu-serial-test.sh` - kernel_main
  spawns the compositor plus *both* demo apps at once (serialized with
  `pit_sleep_ms` between each spawn so which window lands at index 0 vs 1
  is deterministic, not a race), then pixel-checks 12 points straight out
  of the physical framebuffer: gui_clock's titlebar/border/background/
  caption-glyph-on-and-off-pixel (deliberately in the region gui_paint,
  drawn second and so on top, doesn't visually overlap), gui_paint's
  titlebar (focused - the later connection)/own border/background/
  caption glyph/separator line, and the untouched desktop background -
  12/12 matched. (2) interactive, via a QEMU monitor pipe scripted with
  `mouse_move`/`mouse_button`/`sendkey` (the same technique M18's mouse
  driver verification used) plus `screendump` + programmatic PPM pixel
  sampling: clicking gui_clock's titlebar swapped focus away from
  gui_paint (both titlebars' colors flipped exactly as expected);
  dragging with the left button held over gui_paint produced 95
  stroke-colored pixels along the drag path where there had been zero
  before; pressing `'c'` while gui_paint was focused brought that back to
  zero - keyboard routing and mouse-drag routing both confirmed with real
  hardware-shaped input, not just the automated structural checks.
  Also re-ran the M13 interactive shell regression (`ls`/`exit` via
  `sendkey`) since this touched shared `kernel_main` state - unaffected,
  `ls` now lists ten files including both new demos, `exit` still
  triggers a respawn.
  Next: M22 (desktop shell - taskbar/dock listing running apps with
  click to focus/minimize, an app launcher reading the filesystem).
- 2026-08-22 — M22 complete: `user_space/bin/desktop_shell.c`, a real
  taskbar-and-launcher client, making "desktop environment running custom
  apps" genuinely true rather than aspirational (this milestone's own
  framing) - the last item on the numbered milestone list.
  **Panel windows** (`wm.h`'s `wm_create_request_t.panel` flag,
  `compositor.c`): a panel ignores its own requested width (the
  compositor always gives it the full display width) and docks to the
  bottom of the screen with no border/titlebar chrome, and - the one
  piece of z-ordering this compositor enforces at all - is always drawn
  in a second pass *after* every ordinary window, so it can never be
  occluded regardless of connection order. `wm_create_response_t`'s M21
  width/height fields (added ahead of needing them, see that entry) are
  what let a panel client size its own `gfx_ctx_t` correctly despite not
  knowing the display resolution itself.
  **Query/action protocol** (`wm.h`): `WM_QUERY_PIPE`/`WM_QUERY_RESP_PIPE`
  (a client pings, the compositor snapshots every window it knows about -
  id, geometry, focused, minimized, is_panel - into a
  `wm_query_response_t`) and `WM_ACTION_PIPE` (`WM_ACTION_FOCUS`/
  `WM_ACTION_TOGGLE_MINIMIZE` by window id) - the two things a desktop
  shell needs that no ordinary client does: seeing every *other* window,
  and being able to change one. `window_t` (`compositor.c`) gained a
  `minimized` flag; a minimized window is skipped by both `redraw()` and
  click hit-testing (nothing on screen to click), and
  `WM_ACTION_TOGGLE_MINIMIZE` clears focus if it minimizes the window
  that currently holds it - `WM_ACTION_FOCUS` un-minimizes on its way to
  focusing, so a taskbar's "click an unfocused/minimized entry" and
  "click the focused entry" cases naturally map to two different verbs
  from the same one click.
  **A real race, caught during interactive bring-up, not assumed away:**
  `wm_query_response_t` is far bigger than any struct M20/M21 ever put on
  a pipe (up to `WM_MAX_ROUTABLE_WINDOWS` entries), and desktop_shell
  calls `wm_query_windows` repeatedly (once per redraw tick) rather than
  once like `wm_connect`. `kernel/ipc/pipe.h`'s `pipe_read` only
  guarantees "at least one byte, then whatever else is immediately
  ready" - it never promised a whole struct arrives in one `sys_read`,
  since the writer's own `pipe_write` can be preempted mid-copy
  (`SCHED_QUANTUM_TICKS`) and a reader that's been sitting blocked wakes
  the instant the first byte lands. M20/M21's small structs (tens of
  bytes) never hit this in practice; the taskbar's bigger, far more
  frequent reads did - query replies started arriving short, and once
  one read landed short, every later read desynced against the leftover
  unread bytes of a previous reply, permanently freezing the taskbar's
  view of the world (confirmed by watching it: launching a second app
  via the launcher rendered correctly on the *desktop*, but the taskbar
  never picked up the new running-window entry no matter how long you
  waited). Fixed with a `read_exact` helper (loops `sys_read` until the
  full byte count is in) added to both `wmclient.c` and `compositor.c`
  and used everywhere a fixed-size struct comes off one of these pipes,
  not just the query response - the same latent risk existed for every
  earlier protocol here too, just too small in practice to have shown
  up yet.
  **`desktop_shell.c`**: connects a chrome-less panel
  (`wm_connect_panel`), reads the real on-disk file list once
  (`SYS_listfiles`, M13) into launcher slots (one per file, 64px wide,
  clicking spawns it via `sys_spawn`), and polls `wm_query_windows` every
  300ms into running-window slots (one per non-panel, non-self window,
  right-aligned, colored by focus state) - clicking an unfocused/
  minimized one sends `WM_ACTION_FOCUS`, clicking the already-focused one
  sends `WM_ACTION_TOGGLE_MINIMIZE`. Reuses M21's whole event-routing
  path unmodified: the panel is just another window from the compositor's
  point of view, so "click the taskbar" arrives as the exact same
  `WM_EVENT_MOUSE_BUTTON` any other window would get.
  Verified the same two ways as M21: (1) automated, via
  `tools/qemu-serial-test.sh` - kernel_main spawns the compositor,
  desktop_shell, and gui_clock together, then pixel-checks 8 points:
  launcher slot 0's background and its "hello" caption's `'h'` glyph
  on/off pixels (computed straight from `font8x16.c`'s bitmap table, same
  method M21's checks used), the running-window slot's background
  (focused color, since gui_clock is the only other window) and its
  `'#1'` label's `'#'` glyph on/off pixels, an empty stretch of panel
  background between the two slot groups, and the untouched desktop
  background above the panel - 8/8 matched. (2) interactive, via QEMU
  monitor `mouse_move`/`mouse_button` injection: clicking a launcher slot
  spawned a real second window at the expected cascade position (found by
  scanning the screendump for its background color's bounding box);
  clicking a running window's taskbar slot while it was already focused
  minimized it (window content and titlebar both reverted to plain
  desktop background, taskbar slot's own color flipped to "not focused");
  clicking the same slot again focused-and-restored it (content
  reappeared, taskbar slot flipped back) - confirming click-to-focus and
  click-to-minimize both work with real routed input, not just the
  automated structural checks.
  Also re-ran the M13 interactive shell regression - `ls` now lists all
  twelve files including `desktop_shell` itself, `exit` still respawns.
  This closes out every milestone through M22 - the numbered list in
  this file is now complete. What's left is the explicitly-unordered
  "Stretch goals" section below (SMP, networking, UEFI, real-hardware
  boot, third-party package tooling) - each a substantially larger,
  independent body of work, so which (if any) to pursue next is an open
  decision rather than an assumed "next."
- 2026-08-22 — Stretch goal complete: SMP (multi-core) support. Every core
  QEMU starts (`-smp N`, N up to `MAX_CPUS`=8) now genuinely runs kernel
  and user tasks in parallel, not just time-sliced on one - proven, not
  assumed, by a self-test whose probe tasks record which physical CPU
  (read live off each core's own Local APIC ID) actually executed them.
  **New `kernel/acpi/`**: `acpi.c` finds the RSDP (EBDA + the legacy
  0xE0000-0xFFFFF BIOS range), walks the RSDT or XSDT (whichever the
  RSDP's revision indicates), and parses the MADT for the Local APIC's
  physical MMIO base plus every *enabled* Processor Local APIC entry
  (type 0) - just enough ACPI to answer "how many CPUs, and what are
  their APIC IDs", not a general table walker. Missing ACPI/MADT is
  treated as a normal fallback to single-core, not a panic - a
  legitimate platform state this kernel has no reason to refuse to boot
  on.
  **New `kernel/arch/x86_64/lapic.{h,c}`**: Local APIC driver, distinct
  from the still-unmodified 8259 (`pic.c`, which keeps delivering every
  legacy device IRQ to the BSP exactly as it always has - no I/O APIC,
  no rerouting). `lapic_init` maps the MMIO region virt==phys and enables
  the software bit in the Spurious Interrupt Vector Register;
  `lapic_send_ipi`/`lapic_send_ipi_all_excl_self` are what starts an AP
  at all (INIT-SIPI-SIPI) and what lets already-running cores signal each
  other afterward.
  **New `kernel/arch/x86_64/smp.{h,c}`**: orchestrates bring-up. APs are
  started strictly one at a time - a single shared low-memory scratch
  struct (`AP_PARAMS_ADDR`, physical 0x7000) hands each one its target
  CR3, its own kmalloc'd stack, the kernel's real GDT pointer, and its
  assigned cpu index, and the BSP waits (bounded, with a diagnosable
  timeout - the same pattern M6/M18's keyboard/mouse self-tests already
  used for "real hardware might just not respond") for that AP to set an
  `ap_ready` flag before reusing the struct for the next one.
  `smp_current_cpu()` is a live Local APIC ID MMIO read + a linear scan
  over `MAX_CPUS` (=8) - no per-CPU storage mechanism exists in this
  kernel (no GS-base/swapgs setup), and at this scale a live read is
  simple and fast enough not to need one; "correct, not maximally
  efficient" is a repeat theme across this codebase (SYS_wait's
  cooperative polling, PIT_HZ's 10 ms granularity) and applies here too.
  **New `kernel/arch/x86_64/ap_trampoline.asm`**: a standalone 16-bit
  flat binary (built separately with `nasm -f bin`, like stage1/stage2 -
  excluded from the normal kernel `-f elf64` glob in the Makefile),
  copied to physical 0x8000 before every SIPI. Walks real -> protected ->
  long mode exactly like `stage2.asm` already does for the BSP, minus any
  BIOS calls (an AP has none of its own to make, and the BSP already
  enabled A20 for the whole machine) - reads its own transient 32/64-bit
  GDT descriptor bytes copied verbatim from stage2's already-proven
  `gdt_code32`/`gdt_data32`/`gdt_code64`, reaches 64-bit mode, then
  immediately retires that GDT for the kernel's real one (read out of
  `AP_PARAMS_ADDR`) and jumps to a normal linked kernel symbol -
  `kernel/arch/x86_64/ap_entry.asm`'s `ap_entry_asm_stub`, which switches
  onto the AP's real stack and calls into C (`ap_main`, `smp.c`).
  **GDT/TSS** (`gdt.c`/`gdt.h`): the single shared TSS became `MAX_CPUS`
  TSS descriptors, one per possible core - RSP0 (the ring3->ring0 entry
  stack) is inherently per-CPU the instant two cores can each be running
  a different user-mode task at once; a single shared TSS would have one
  core's RSP0 stomped by the other's on every context switch. User
  selector constants (`GDT_USER_CODE_SEL`/`GDT_USER_DATA_SEL`) are now
  computed from `MAX_CPUS` rather than hardcoded, so they can't silently
  drift out of sync with the TSS block's real size.
  **Scheduler** (`sched.c`): `current_task`/`ticks_in_slice`/
  `loaded_pml4_phys` all went from single globals to `[MAX_CPUS]` arrays -
  every CPU pulls from the exact same shared `tasks` table (no per-CPU
  run queue, no task affinity: any online CPU can pick up any READY
  task), so "what am I running" and "how far into my own time slice am I"
  had to stop being single answers. A new `sched_lock` spinlock protects
  the table and the pick-next/state-transition half of `schedule()` -
  *not* the `context_switch()` call itself, which can't run under a lock
  a different CPU might need in order to make progress. Ownership
  protocol (the same technique xv6 uses for exactly this problem): the
  outgoing task acquires the lock and holds it across `context_switch`;
  whichever task/CPU resumes it next releases it - either right after its
  own earlier `context_switch` call inside `schedule()`, or at the top of
  `task_entry_trampoline` for a task that's never run before. Both are
  genuine resume points symmetric with the acquire, so exactly one
  release always pairs with exactly one acquire regardless of which CPU
  does which half.
  **Locking swept through every already-shared kernel structure now that
  a second CPU can genuinely be inside it at the same instant**: `pmm.c`
  (the frame bitmap), `heap.c` (the free list - held across its own
  `pmm`/`vmm` calls, so lock order is always heap -> {pmm, vmm}, never
  reversed, so no cycle), `vmm.c` (page-table mutation), and `klog.c`
  (console/serial output - its old comment about `cli` alone being
  sufficient explicitly said "no SMP yet, no second CPU to still race
  with"; that stopped being true, so a real `klog_lock` was added
  alongside it). New `kernel/lib/spinlock.h`: a plain test-and-set lock
  over `__atomic_exchange_n`/`__atomic_store_n`, which lower to `lock
  xchg`/a plain store on x86_64 - no libatomic, nothing this freestanding
  kernel doesn't already have.
  **Two real bugs found by testing, not caught by inspection** (matching
  this project's long-running "prove it, don't just trust it" discipline
  at every earlier milestone):
  1. Every `sched_lock`-taking function (`schedule()`, `task_spawn_common`,
     `sched_init_ap`) needed its critical section wrapped in
     `irq_save_disable`/`irq_restore` (promoted from klog.c's old local
     helper into `arch/x86_64/io.h`, since sched.c needed the identical
     primitive) - not just `spin_lock`/`spin_unlock`. Without it, the
     periodic scheduler-tick IPI (see below) landing on a CPU that
     already held `sched_lock` would reenter `schedule()` from inside its
     own interrupt handler and deadlock on a lock it already held. Caught
     by an actual multi-hour debugging session against a genuine `-smp 4`
     hang, not anticipated in advance.
  2. `lapic_vector_handler` (`smp.c`) sent the Local APIC's EOI *after*
     dispatching to `scheduler_tick_cpu` - the exact same mistake, in the
     exact same shape, that M7's own progress log already recorded once
     for the 8259 PIC (`pic_send_eoi`) and fixed the same way:
     `scheduler_tick_cpu` can call `schedule()`, which can
     `context_switch` that CPU away entirely, and that call doesn't
     "return" until the exact interrupted context is resumed - which
     itself needs *another* interrupt on that CPU to happen, one EOI was
     withholding. Every AP would run fine for a handful of ticks, then go
     silent forever the first time its own 5th-tick reschedule actually
     switched tasks. Fixed the same way M7's version was: send EOI first.
  **AP preemption is a broadcast off the BSP's one real hardware timer,
  not per-core APIC timers** - a deliberate simplification, not an
  oversight: `scheduler_tick` (still driven only by the real PIT IRQ0,
  which the 8259 only ever delivers to the BSP) calls
  `smp_broadcast_schedule_tick()` every tick, sending `IPI_SCHEDULE_
  VECTOR` (0xF0) to every other online CPU via the "all excluding self"
  ICR destination shorthand; each receiving CPU's
  `lapic_vector_handler` runs the identical `scheduler_tick_cpu` logic
  (per-tick SIGKILL/SIGTERM check, per-5th-tick quantum reschedule) a
  real per-core APIC timer would, just off one shared clock instead of
  N independent ones.
  **`panic.c` gained an SMP-safety hook**: the first CPU to panic
  broadcasts an NMI to every other online CPU (guarded by an atomic
  exchange so only the *first* panicking CPU ever broadcasts - otherwise
  every other core's own NMI-triggered panic would re-broadcast to
  everyone else, including cores already spinning in their own
  `cli;hlt`, forever, since NMI isn't maskable by `cli`). No new
  fault-handling code needed for the receiving side: `isr.c`'s
  `isr_handler` already treats any non-breakpoint exception - NMI
  included - as fatal, so a halted-via-NMI core just prints and spins
  exactly like any other unrecoverable exception would.
  **The M20-M22 compositor self-tests turned out to depend on
  single-core scheduling determinism they'd never had to name out loud
  until SMP could break it** - their own comments already said "spawned
  one at a time... deterministic instead of a race", an assumption that
  held on one core (however many tasks exist, only one instruction
  stream ever executes) and doesn't automatically hold once genuine
  hardware parallelism exists. `smp_init()` is deliberately called
  *after* every M-numbered self-test, not right after M7's scheduler one
  where SMP bring-up would otherwise naturally have landed - every
  earlier milestone keeps the exact single-core-equivalent environment it
  was written and verified against, and real multi-core support stands up
  as additive capability from that point in boot onward. This also
  surfaced a second, unrelated pre-existing gap while building the SMP
  self-test's own cleanup: M20-M22's compositor/client processes are
  killed via `SYS_kill(SIGKILL)` rather than let exit normally (a real
  window manager isn't expected to exit on its own), and nothing before
  this point ever called `SYS_wait(-1)` afterward - so those signals sat
  pending, unnoticed, on tasks nothing ever rescheduled again. Once SMP's
  idle cores started actually picking them back up, they *did* eventually
  notice and terminate, just slowly under heavy scheduler contention -
  harmless, since nothing depended on it, but slow enough that the SMP
  self-test's own cleanup was rewritten to reap its 4 probe tasks by
  specific pid rather than draining every unreaped child of task 0 with
  `SYS_wait(-1)`.
  Verified via `tools/qemu-serial-test.sh` across `-smp 1/2/4/8/9`: every
  configuration reaches the shell prompt cleanly with zero panics; `-smp
  N` (N>1) shows `[smp] N CPU(s) online` and the probe self-test
  reporting exactly N distinct physical CPUs observed (1 for the
  single-core/default-QEMU case, correctly skipping the "more than one
  actually ran it" assertion there); `-smp 9` is silently capped to
  `MAX_CPUS`=8 by `acpi.c`'s own MADT-parse-time bound. Confirmed via
  QEMU's `-d int,cpu_reset` trace during the debugging session (no
  faults, no resets, the real PIT vector firing continuously throughout)
  that the earlier hangs were genuine software deadlocks, not silent CPU
  resets - which is what pointed at the EOI-ordering bug specifically
  once the "stuck AP receives no further IPIs" pattern matched M7's own
  documented PIC bug shape.
  Next: whichever of the remaining stretch goals (networking, UEFI,
  real-hardware boot, package tooling) is picked next - still an open,
  independent decision, same as before.
