#include "idt.h"

#include <stdint.h>

#include "gdt.h"
#include "isr.h"

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} table_ptr_t;

#define IDT_ENTRIES 256
#define IDT_GATE_INTERRUPT_RING0 0x8E /* present, DPL0, 64-bit interrupt gate */

#define DOUBLE_FAULT_VECTOR 8
#define DOUBLE_FAULT_IST    1 /* matches the IST1 slot gdt.c points at double_fault_stack */

static idt_entry_t idt[IDT_ENTRIES];
static table_ptr_t idtp;

extern void idt_flush(table_ptr_t *ptr);

static void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist) {
    idt[vector].offset_low = handler & 0xFFFF;
    idt[vector].selector = GDT_KERNEL_CODE_SEL;
    idt[vector].ist = ist;
    idt[vector].type_attr = IDT_GATE_INTERRUPT_RING0;
    idt[vector].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].reserved = 0;
}

void idt_init(void) {
    for (int vector = 0; vector < 32; vector++) {
        uint8_t ist = (vector == DOUBLE_FAULT_VECTOR) ? DOUBLE_FAULT_IST : 0;
        idt_set_gate((uint8_t)vector, isr_stub_table[vector], ist);
    }
    for (int line = 0; line < 16; line++) {
        idt_set_gate((uint8_t)(32 + line), irq_stub_table[line], 0);
    }

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint64_t)&idt;
    idt_flush(&idtp);
}
