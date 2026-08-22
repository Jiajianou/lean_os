/* kernel/arch/x86_64/isr.h
 *
 * C-side of the interrupt pipeline: the register-dump struct isr.asm's
 * common stubs build on the stack before calling into C, the two dispatch
 * entry points those stubs call, and the stub address tables idt.c installs
 * into the IDT.
 */
#pragma once

#include <stdint.h>

/* Layout matches the push order in isr_common_stub/irq_common_stub
 * (isr.asm) exactly: general regs (last pushed = lowest address = first
 * field), then vector/error_code, then the CPU-pushed iretq frame. */
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} isr_regs_t;

extern uint64_t isr_stub_table[32];
extern uint64_t irq_stub_table[16];
extern uint64_t syscall_stub_addr; /* vector 0x80's stub - installed separately, see idt.c */

typedef void (*irq_handler_fn)(isr_regs_t *regs);

void isr_handler(isr_regs_t *regs);
void irq_handler(isr_regs_t *regs);

/* Registers a C handler for a remapped IRQ line (0-15, i.e. vectors
 * 32-47). irq_handler dispatches to it and still sends the PIC EOI
 * afterward either way - drivers don't need to (and shouldn't) send
 * their own. Line still has to be unmasked separately via
 * pic_clear_mask(). */
void irq_register_handler(uint8_t irq, irq_handler_fn handler);
