#include "isr.h"

#include "drivers/klog.h"
#include "panic.h"
#include "pic.h"
#include "sched/sched.h" /* M52 - task_exit_with_code/sched_current, so a ring-3 fault kills one task instead of the machine */
#include "signal.h"      /* system_api/include/signal.h - SIGSEGV, the exit code a killed-for-faulting task gets */

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
 * the one recoverable case.
 *
 * M52 splits the rest by *ring*, which is the whole milestone in one
 * branch. Until now every fault panicked, so any wild pointer in any user
 * program stopped the machine - by a distance the largest source of "you
 * have to reset it" this project has had. A fault in ring 3 is a bug in
 * one program and now kills only that program, with a distinguishable
 * exit code (128 + SIGSEGV = 139) so everything that already watches for
 * a client dying - M29's reap_dead_clients, M48's crash toast, SYS_wait -
 * treats it exactly like any other unexpected death, which is what it is.
 * A fault in ring 0 still panics, loudly: that is a kernel bug, and
 * quietly killing whatever task happened to be current would hide it.
 *
 * Every fatal exception is handled this way, not only #PF. A
 * divide-by-zero, an invalid opcode and a #GP from ring 3 are the same
 * kind of event - a program that did something impossible - and there is
 * nothing to be gained by leaving three of them able to stop the machine
 * while the fourth cannot. They share one exit code because this project
 * has no per-signal handling to tell them apart with; the log line names
 * which it was. */
void isr_handler(isr_regs_t *r) {
    if (r->vector == BREAKPOINT_VECTOR) {
        klog_puts("[isr] breakpoint (int3) hit - resuming\n");
        dump_regs(r);
        return;
    }

    /* M82: before deciding whose bug this is, ask whether it is a bug at
     * all.
     *
     * A page fault in ring 3 on a page of the caller's own mmap that has
     * never been touched is not a fault in the "something went wrong"
     * sense - it is how a reservation becomes memory. sched_fault_fill
     * answers that question and is deliberately narrow about it: it
     * refuses a present page, an address outside the arena, an address
     * inside the arena that no mapping covers, and a write to a mapping
     * that was asked for read-only. Anything it refuses falls through to
     * exactly the behaviour below, unchanged since M52.
     *
     * Ring 0 is checked too, and that is not an oversight. The kernel
     * reads and writes user buffers directly (copy_to_user and friends
     * run on the caller's own page tables), so a syscall handed a pointer
     * into a mapping the program has reserved but not yet touched faults
     * in ring 0 at an address that is perfectly legitimate. Filling it is
     * right; the alternative is that mmap'd memory works everywhere
     * except as a syscall argument. sched_fault_fill still consults the
     * *current task's* arena, so this cannot fill anything for a fault in
     * kernel memory. */
    if (r->vector == PAGE_FAULT_VECTOR && sched_fault_fill(read_cr2(), r->error_code, r->rsp)) {
        return;
    }

    /* The low two bits of the saved CS are the privilege level the fault
     * came from - 3 for user code, 0 for the kernel. This is the real
     * question ("whose bug is this?"), and it is one the interrupt frame
     * has always carried. */
    if ((r->cs & 3) == 3) {
        task_t *t = sched_current();
        klog_puts("\n[isr] ring-3 fault: ");
        klog_puts(exception_name(r->vector));
        klog_puts(" in task ");
        klog_puts(t && t->name[0] ? t->name : "(unnamed)");
        klog_puts(" pid 0x");
        klog_put_hex32((uint32_t)(t ? t->id : -1));
        klog_puts(" - terminating it, not the machine\n");
        dump_regs(r);
        /* noreturn: a TERMINATED task is never scheduled again, so the
         * interrupt frame this was called from is simply abandoned along
         * with the rest of that task's kernel stack. Exactly what the
         * SIGKILL/SIGTERM path already does from inside IRQ0's handler
         * (sched.c's deliver_pending_signal_and_exit), which is why this
         * is safe from interrupt context at all. */
        task_exit_with_signal(SIGSEGV);
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
