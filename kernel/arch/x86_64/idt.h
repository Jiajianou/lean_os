/* kernel/arch/x86_64/idt.h
 *
 * Kernel IDT: 32 CPU exception vectors + 16 remapped PIC IRQ vectors
 * (32..47), all installed as 64-bit interrupt gates pointing at the
 * trampolines in isr.asm.
 */
#pragma once

void idt_init(void);

/* Loads this AP's own IDTR from the already-built, shared IDT (idt_init
 * has to run once, on the BSP, before any AP calls this - the table
 * content is identical for every core, only the per-CPU IDTR register
 * needs repeating, the same "shared table, per-CPU register" situation
 * gdt_init_ap is in). */
void idt_load_ap(void);
