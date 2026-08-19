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

## M5 — Memory management

- [ ] Physical frame allocator, seeded from the E820 map
- [ ] Virtual memory manager (page table manipulation, map/unmap)
- [ ] Kernel heap allocator (`kmalloc`/`kfree`)

## M6 — Timer & core drivers

- [ ] PIT or APIC timer, tick counter, `sleep`-style busy wait
- [ ] Serial port (COM1) driver for debug logging (parallel to VGA)
- [ ] PS/2 keyboard driver

## M7 — Multitasking basics

- [ ] Task/thread control block struct
- [ ] Context switch (assembly) between kernel threads
- [ ] Simple round-robin scheduler driven by the timer interrupt

## M8 — System call interface (`system_api/`)

- [ ] Define syscall ABI: numbering, register convention, `syscall`/`sysret`
      (or `int 0x80`) entry
- [ ] `system_api/include/` headers shared by kernel and user_space
- [ ] Kernel-side syscall dispatch table
- [ ] First syscalls: `write`, `exit`, `getpid`

## M9 — User mode & process loading

- [ ] Ring 3 transition, TSS configured for privilege-level switches
- [ ] Minimal ELF64 loader in the kernel
- [ ] User stack + address space setup per process

## M10 — User-space runtime

- [ ] `user_space/lib` crt0 (`_start`) for user binaries
- [ ] Thin syscall wrapper functions (built from `system_api/` headers)
- [ ] Bare string/mem helpers (`memcpy`, `strlen`, ...) — hand-written, no
      external libc

## M11 — First user program end-to-end

- [ ] "Hello world" built against `user_space/lib` + `system_api`
- [ ] Kernel loads and runs it, output visible via syscall `write`
- [ ] This is the milestone that proves the whole stack (boot → kernel →
      syscall → user space) works together

## M12 — Storage & filesystem

- [ ] ATA (PIO mode) disk driver
- [ ] Minimal custom filesystem format (or a simple FAT-like layout) + VFS
      layer in the kernel
- [ ] Load user binaries from disk instead of an embedded/in-memory blob

## M13 — Init & shell

- [ ] `fork`/`exec`-equivalent syscalls, `wait`
- [ ] `user_space/init` as PID 1
- [ ] Minimal shell (`user_space/shell`)
- [ ] A couple of coreutils-style programs (`ls`, `cat`, `echo`)

## M14 — IPC & process management

- [ ] Pipes
- [ ] Signals (basic set: kill, term, etc.)
- [ ] Process groups / more complete `wait` semantics

## Stretch goals (unordered, post-M14)

- [ ] SMP (multi-core) support
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
