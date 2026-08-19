/* kernel/arch/x86_64/idt.h
 *
 * Kernel IDT: 32 CPU exception vectors + 16 remapped PIC IRQ vectors
 * (32..47), all installed as 64-bit interrupt gates pointing at the
 * trampolines in isr.asm.
 */
#pragma once

void idt_init(void);
