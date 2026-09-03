/* kernel/arch/x86_64/gdt.h
 *
 * Kernel-owned GDT + TSS. Replaces whatever GDT the boot loader (UEFI
 * firmware) had installed - that one is firmware-owned and goes away with
 * ExitBootServices; this is the kernel's real, permanent descriptor table.
 *
 * SMP (stretch goal): the TSS is what holds RSP0, the kernel stack the CPU
 * switches to on any ring3->ring0 transition - and RSP0 is inherently
 * per-CPU the moment two cores can each be running a different user-mode
 * task at once (a single shared TSS would have one core's RSP0 stomped by
 * the other's every context switch). So the GDT carries MAX_CPUS TSS
 * descriptors, one per possible core, all built by the BSP's gdt_init()
 * up front; each CPU (BSP included) just points its own TR at its own
 * slot via gdt_tss_selector(cpu_id) - the table itself is shared and
 * read-only after init, so no locking is needed for that part.
 */
#pragma once

#include <stdint.h>

#include "cpu.h"

/* Selectors (byte offsets into the GDT) - shared with idt.c, which needs
 * the kernel code selector for every interrupt gate, and (M9) proc.c,
 * which needs the user selectors for the ring-3 iretq frame. User
 * selectors need `| 3` (RPL 3) applied by the caller before loading them
 * into a segment register or an iretq frame - the GDT entry's DPL alone
 * doesn't imply that. GDT_TSS_SEL_BASE and GDT_USER_CODE_SEL both shift if
 * MAX_CPUS (cpu.h) ever changes - computed from it, not hand-maintained,
 * so they can't silently drift out of sync the way two independent
 * hardcoded constants could. */
#define GDT_KERNEL_CODE_SEL 0x08
#define GDT_KERNEL_DATA_SEL 0x10
#define GDT_TSS_SEL_BASE    0x18 /* cpu 0's TSS selector; cpu i's is GDT_TSS_SEL_BASE + i*16 (a TSS descriptor is 16 bytes in long mode) */
#define GDT_USER_CODE_SEL   (GDT_TSS_SEL_BASE + MAX_CPUS * 16)
#define GDT_USER_DATA_SEL   (GDT_USER_CODE_SEL + 8)

void gdt_init(void);

/* Loads this AP's own TR from the shared GDT the BSP already built and
 * flushed - every table *entry* already exists (gdt_init built all
 * MAX_CPUS TSS descriptors up front), an AP just needs LGDT (its own GDTR
 * is a per-CPU register even though it points at the same shared table)
 * and LTR for its own slot. */
void gdt_init_ap(int cpu_id);

/* Selector for a given CPU's TSS - GDT_TSS_SEL_BASE for cpu 0, matching
 * the plain GDT_TSS_SEL constant this used to be before SMP. */
static inline uint16_t gdt_tss_selector(int cpu_id) {
    return (uint16_t)(GDT_TSS_SEL_BASE + cpu_id * 16);
}

/* ---- M106: which CPU am I, asked of the CPU itself -------------------
 *
 * The task register is per-CPU and every core loaded its own TSS
 * selector with `ltr` at bring-up, so `str` IS this core's identity - one
 * instruction, no memory, no MMIO, and correct by construction rather
 * than by a table lookup agreeing with reality.
 *
 * What it replaces read the Local APIC's ID register over MMIO and then
 * linearly scanned smp_cpus[] for a match, returning 0 - the BSP's index
 * - when it found none. Every SMP invariant in this kernel is indexed by
 * the number this returns: current_task[], the TSS whose rsp0 a ring-3
 * transition lands on, the per-CPU slice counter. A core that answered 0
 * when it was not core 0 would alias all three onto the boot CPU's, and
 * would do it silently.
 *
 * Returns 0 before `ltr` has run on this core, which is the right
 * bootstrap answer: the BSP is cpu 0 and nothing else is running yet. */
/* Q13: `str` is an x86 instruction and the host test tier compiles this
 * header on a machine that has no such thing - see the same #if in
 * cpu.h for the full argument. The host side is a number a test sets,
 * which is what lets one process drive a multi-core scheduler one CPU at
 * a time. */
#if defined(__x86_64__)
static inline int gdt_current_cpu(void) {
    uint16_t sel;
    __asm__ volatile("str %0" : "=r"(sel));
    if (sel < GDT_TSS_SEL_BASE) {
        return 0;
    }
    return (int)((sel - GDT_TSS_SEL_BASE) / 16);
}
#else
int gdt_current_cpu(void);
#endif

/* Updates TSS.RSP0 for the given CPU - the kernel stack the CPU switches
 * to automatically on any ring3->ring0 transition (interrupt, exception,
 * or `int 0x80` while running user code). The scheduler (kernel/sched/
 * sched.c) calls this on every context switch, for whichever CPU is doing
 * the switching, so that CPU's TSS always points at whichever task is
 * about to run there's own kernel stack. No separate "flush" needed: the
 * CPU reads this field live out of TSS memory, only the TSS *descriptor*
 * (unchanged here) needs `ltr`. */
void tss_set_rsp0(int cpu_id, uint64_t rsp0);
uint64_t tss_get_rsp0(int cpu_id); /* M106 - see gdt.c */

/* Copies the {limit, base} descriptor gdt_flush uses into the caller's
 * two out-params - smp.c needs this to hand an AP the exact same GDT
 * pointer to lgdt from inside kernel/arch/x86_64/ap_trampoline.asm. */
void gdt_get_table_ptr(uint16_t *limit_out, uint64_t *base_out);
