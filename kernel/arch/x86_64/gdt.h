/* kernel/arch/x86_64/gdt.h
 *
 * Kernel-owned GDT + TSS. Replaces the flat 32-bit GDT stage2.asm built
 * just to get into protected/long mode - that one is scratch memory the
 * bootloader owned; this is the kernel's real, permanent descriptor table.
 */
#pragma once

/* Selectors (byte offsets into the GDT) - shared with idt.c, which needs
 * the kernel code selector for every interrupt gate. */
#define GDT_KERNEL_CODE_SEL 0x08
#define GDT_KERNEL_DATA_SEL 0x10
#define GDT_TSS_SEL         0x18

void gdt_init(void);
