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

/* Updates TSS.RSP0 for the given CPU - the kernel stack the CPU switches
 * to automatically on any ring3->ring0 transition (interrupt, exception,
 * or `int 0x80` while running user code). The scheduler (kernel/sched/
 * sched.c) calls this on every context switch, for whichever CPU is doing
 * the switching, so that CPU's TSS always points at whichever task is
 * about to run there's own kernel stack. No separate "flush" needed: the
 * CPU reads this field live out of TSS memory, only the TSS *descriptor*
 * (unchanged here) needs `ltr`. */
void tss_set_rsp0(int cpu_id, uint64_t rsp0);

/* Copies the {limit, base} descriptor gdt_flush uses into the caller's
 * two out-params - smp.c needs this to hand an AP the exact same GDT
 * pointer to lgdt from inside kernel/arch/x86_64/ap_trampoline.asm. */
void gdt_get_table_ptr(uint16_t *limit_out, uint64_t *base_out);
