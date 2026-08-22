/* kernel/arch/x86_64/gdt.h
 *
 * Kernel-owned GDT + TSS. Replaces the flat 32-bit GDT stage2.asm built
 * just to get into protected/long mode - that one is scratch memory the
 * bootloader owned; this is the kernel's real, permanent descriptor table.
 */
#pragma once

#include <stdint.h>

/* Selectors (byte offsets into the GDT) - shared with idt.c, which needs
 * the kernel code selector for every interrupt gate, and (M9) proc.c,
 * which needs the user selectors for the ring-3 iretq frame. User
 * selectors need `| 3` (RPL 3) applied by the caller before loading them
 * into a segment register or an iretq frame - the GDT entry's DPL alone
 * doesn't imply that. */
#define GDT_KERNEL_CODE_SEL 0x08
#define GDT_KERNEL_DATA_SEL 0x10
#define GDT_TSS_SEL         0x18
#define GDT_USER_CODE_SEL   0x28
#define GDT_USER_DATA_SEL   0x30

void gdt_init(void);

/* Updates TSS.RSP0 - the kernel stack the CPU switches to automatically
 * on any ring3->ring0 transition (interrupt, exception, or `int 0x80`
 * while running user code). The scheduler (kernel/sched/sched.c) calls
 * this on every context switch so it always points at whichever task is
 * about to run's own kernel stack - critical the moment more than one
 * user-mode task exists, since they can't share one RSP0. No separate
 * "flush" needed: the CPU reads this field live out of TSS memory, only
 * the TSS *descriptor* (unchanged here) needs `ltr`. */
void tss_set_rsp0(uint64_t rsp0);
