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
#define IDT_GATE_INTERRUPT_RING3 0xEE /* present, DPL3, 64-bit interrupt gate - callable via `int n` from ring 3 */
/* M67: present, DPL3, 64-bit *trap* gate. The single bit of difference
 * from 0xEE is what the CPU does to IF on entry: an interrupt gate clears
 * it, a trap gate leaves it exactly as the caller had it. See idt_init. */
#define IDT_GATE_TRAP_RING3      0xEF

#define DOUBLE_FAULT_VECTOR 8
#define DOUBLE_FAULT_IST    1 /* matches the IST1 slot gdt.c points at double_fault_stack */

#define SYSCALL_VECTOR 0x80

/* IPI_SCHEDULE_VECTOR/LAPIC_SPURIOUS_VECTOR (SMP: the two vectors every
 * CPU's Local APIC can raise once more than one core exists) are defined
 * in isr.h, shared with lapic.c and smp.c. */

static idt_entry_t idt[IDT_ENTRIES];
static table_ptr_t idtp;

extern void idt_flush(table_ptr_t *ptr);

static void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist, uint8_t type_attr) {
    idt[vector].offset_low = handler & 0xFFFF;
    idt[vector].selector = GDT_KERNEL_CODE_SEL;
    idt[vector].ist = ist;
    idt[vector].type_attr = type_attr;
    idt[vector].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].reserved = 0;
}

void idt_init(void) {
    for (int vector = 0; vector < 32; vector++) {
        uint8_t ist = (vector == DOUBLE_FAULT_VECTOR) ? DOUBLE_FAULT_IST : 0;
        idt_set_gate((uint8_t)vector, isr_stub_table[vector], ist, IDT_GATE_INTERRUPT_RING0);
    }
    for (int line = 0; line < 16; line++) {
        idt_set_gate((uint8_t)(32 + line), irq_stub_table[line], 0, IDT_GATE_INTERRUPT_RING0);
    }
    /* DPL3 so ring-3 user code (M9+) can `int 0x80` without a #GP - ring
     * 0 can invoke any gate regardless of its DPL, so this doesn't change
     * anything for the kernel-only self-test M8 verifies with today.
     *
     * M67: a TRAP gate, not an interrupt gate. This is the one-line half
     * of that milestone and the entire reason the other half exists.
     *
     * For sixty-six milestones this was 0xEE, so the CPU cleared IF on
     * entry and every syscall in this kernel ran to completion with
     * interrupts off. Nothing wrote that down, and everything depended on
     * it: IF=0 was this kernel's only mutual exclusion, and every handler
     * was written against an implicit guarantee that no timer tick, no
     * NIC interrupt and no other task could observe it half-done.
     *
     * Two things were wrong with that. The visible one is latency: a task
     * blocked inside a syscall could not be preempted, so a shell parked
     * on an empty keyboard buffer had to spin through schedule() by hand,
     * and M64 found the sharper version - a `hlt` inside a syscall halts
     * the CPU with the timer that would wake it disabled, which is a hang
     * with no output and no clue. The structural one is that "correct
     * because interrupts are off" stops being true the moment a second
     * core exists, and this kernel has had SMP since long before M67.
     *
     * What replaces it is written down rather than implied: fs_lock
     * (kernel/fs/vfs.c), pipe_lock (kernel/ipc/pipe.c), shm_lock,
     * openfile_lock, clipboard_lock and net_lock (kernel/net/net.h), each
     * with a comment naming what it protects and against whom. */
    idt_set_gate(SYSCALL_VECTOR, syscall_stub_addr, 0, IDT_GATE_TRAP_RING3);

    idt_set_gate(IPI_SCHEDULE_VECTOR, isr_ipi_schedule_addr, 0, IDT_GATE_INTERRUPT_RING0);
    idt_set_gate(LAPIC_SPURIOUS_VECTOR, isr_lapic_spurious_addr, 0, IDT_GATE_INTERRUPT_RING0);

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint64_t)&idt;
    idt_flush(&idtp);
}

void idt_load_ap(void) {
    idt_flush(&idtp);
}
