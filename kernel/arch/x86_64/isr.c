#include "isr.h"

#include "../../drivers/vga.h"
#include "../../panic.h"
#include "pic.h"

#define PAGE_FAULT_VECTOR 14
#define BREAKPOINT_VECTOR 3

static const char *exception_name(uint64_t vector) {
    static const char *const names[32] = {
        "Divide-by-zero",  "Debug",                "NMI",             "Breakpoint",
        "Overflow",        "Bound range exceeded", "Invalid opcode",  "Device not available",
        "Double fault",    "Coprocessor overrun",  "Invalid TSS",     "Segment not present",
        "Stack-segment fault", "General protection fault", "Page fault", "Reserved",
        "x87 floating-point", "Alignment check",   "Machine check",   "SIMD floating-point",
        "Virtualization",  "Control protection",   "Reserved",        "Reserved",
        "Reserved",        "Reserved",             "Reserved",        "Reserved",
        "Reserved",        "Reserved",             "Security exception", "Reserved",
    };
    return vector < 32 ? names[vector] : "Unknown";
}

static uint64_t read_cr2(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

static void dump_regs(isr_regs_t *r) {
    vga_puts("  vector=0x");
    vga_put_hex64(r->vector);
    vga_puts(" error_code=0x");
    vga_put_hex64(r->error_code);
    vga_puts("\n  rip=0x");
    vga_put_hex64(r->rip);
    vga_puts(" cs=0x");
    vga_put_hex64(r->cs);
    vga_puts(" rflags=0x");
    vga_put_hex64(r->rflags);
    vga_puts("\n  rsp=0x");
    vga_put_hex64(r->rsp);
    vga_puts(" ss=0x");
    vga_put_hex64(r->ss);
    if (r->vector == PAGE_FAULT_VECTOR) {
        vga_puts("\n  cr2=0x");
        vga_put_hex64(read_cr2());
    }
    vga_putc('\n');
}

/* isr_handler: dispatch for CPU exceptions (vectors 0-31). Breakpoints are
 * the one recoverable case - everything else is treated as fatal, since
 * this kernel has no fault-recovery story yet (no page-fault-driven
 * demand paging, no per-process fault isolation). */
void isr_handler(isr_regs_t *r) {
    if (r->vector == BREAKPOINT_VECTOR) {
        vga_puts("[isr] breakpoint (int3) hit - resuming\n");
        dump_regs(r);
        return;
    }

    vga_puts("\n*** UNHANDLED CPU EXCEPTION: ");
    vga_puts(exception_name(r->vector));
    vga_puts(" ***\n");
    dump_regs(r);
    panic("unrecoverable CPU exception");
}

/* irq_handler: dispatch for remapped PIC IRQs (vectors 32-47). Every line
 * is masked by pic_remap() right now, so in normal operation this never
 * fires - it exists so an unmasked/spurious line has somewhere safe to
 * land instead of hitting an unset IDT entry, once M6 starts unmasking
 * real drivers one at a time. */
void irq_handler(isr_regs_t *r) {
    uint8_t irq = (uint8_t)(r->vector - 32);
    vga_puts("[irq] unhandled IRQ ");
    vga_put_hex64(irq);
    vga_putc('\n');
    pic_send_eoi(irq);
}
