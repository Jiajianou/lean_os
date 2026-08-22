#include "isr.h"

#include "drivers/klog.h"
#include "panic.h"
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
    klog_puts("  vector=0x");
    klog_put_hex64(r->vector);
    klog_puts(" error_code=0x");
    klog_put_hex64(r->error_code);
    klog_puts("\n  rip=0x");
    klog_put_hex64(r->rip);
    klog_puts(" cs=0x");
    klog_put_hex64(r->cs);
    klog_puts(" rflags=0x");
    klog_put_hex64(r->rflags);
    klog_puts("\n  rsp=0x");
    klog_put_hex64(r->rsp);
    klog_puts(" ss=0x");
    klog_put_hex64(r->ss);
    if (r->vector == PAGE_FAULT_VECTOR) {
        klog_puts("\n  cr2=0x");
        klog_put_hex64(read_cr2());
    }
    klog_putc('\n');
}

/* isr_handler: dispatch for CPU exceptions (vectors 0-31). Breakpoints are
 * the one recoverable case - everything else is treated as fatal, since
 * this kernel has no fault-recovery story yet (no page-fault-driven
 * demand paging, no per-process fault isolation). */
void isr_handler(isr_regs_t *r) {
    if (r->vector == BREAKPOINT_VECTOR) {
        klog_puts("[isr] breakpoint (int3) hit - resuming\n");
        dump_regs(r);
        return;
    }

    klog_puts("\n*** UNHANDLED CPU EXCEPTION: ");
    klog_puts(exception_name(r->vector));
    klog_puts(" ***\n");
    dump_regs(r);
    panic("unrecoverable CPU exception");
}

static irq_handler_fn irq_handlers[16];

void irq_register_handler(uint8_t irq, irq_handler_fn handler) {
    irq_handlers[irq] = handler;
}

/* irq_handler: dispatch for remapped PIC IRQs (vectors 32-47). A line with
 * no registered handler (still masked by pic_remap(), or simply nothing
 * cares about it) falls back to a generic "unhandled" print instead of
 * hitting an unset IDT entry. EOI is sent here, once, on every path - a
 * registered handler doesn't send its own, so there's exactly one place
 * that can get it wrong instead of one per driver.
 *
 * EOI goes out *before* dispatching, not after: the scheduler's tick hook
 * (kernel/sched/sched.c) can context-switch away from here entirely, and
 * that switch doesn't "return" until some later, unrelated point in time
 * (when this exact task is next resumed) - if EOI were sent afterward, on
 * the normal fall-through path, the 8259 would never see it for this
 * occurrence and would withhold every further interrupt on the line,
 * deadlocking the timer. Sending it first means the PIC is happy
 * regardless of whether or when this call ever "returns". */
void irq_handler(isr_regs_t *r) {
    uint8_t irq = (uint8_t)(r->vector - 32);
    pic_send_eoi(irq);
    if (irq_handlers[irq]) {
        irq_handlers[irq](r);
    } else {
        klog_puts("[irq] unhandled IRQ ");
        klog_put_hex64(irq);
        klog_putc('\n');
    }
}
