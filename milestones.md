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
