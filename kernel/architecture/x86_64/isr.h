#pragma once

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} isr_regs_t;

extern uint64_t isr_stub_table[32];
extern uint64_t irq_stub_table[16];
extern uint64_t syscall_stub_addr;

#define IPI_SCHEDULE_VECTOR   0xF0
#define LAPIC_SPURIOUS_VECTOR 0xFF
extern uint64_t isr_ipi_schedule_addr;
extern uint64_t isr_lapic_spurious_addr;

typedef void (*irq_handler_fn)(isr_regs_t *regs);

void isr_handler(isr_regs_t *regs);
void irq_handler(isr_regs_t *regs);

void lapic_vector_handler(isr_regs_t *regs);

void irq_register_handler(uint8_t irq, irq_handler_fn handler);

void irq_enable_line(uint8_t irq);
