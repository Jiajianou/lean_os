#include "isr.h"

#include "drivers/klog.h"
#include "panic.h"
#include "mm/pmm.h"   /* M102 - the free-frame count in the OOM line */
#include "ioapic.h" /* M103 - which controller this machine has */
#include "lapic.h"  /* M103 - lapic_send_eoi */
#include "pic.h"
#include "gdt.h"
#include "mm/vmm.h"
#include "smp.h"    /* M103 - smp_current_cpu, for the per-CPU counters */
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
    /* M106: which core, and which task. On one CPU both were implicit;
     * on four, a register dump that does not say whose it is cannot be
     * matched to the other three lines around it. */
    {
        task_t *t = sched_current();
        klog_puts("  cpu=");
        klog_put_dec((uint32_t)smp_current_cpu());
        klog_puts(" task=");
        klog_puts(t && t->name[0] ? t->name : "(none)");
        klog_puts(" pid=0x");
        klog_put_hex32((uint32_t)(t ? t->id : -1));
        klog_puts(" tss.rsp0=0x");
        klog_put_hex64(tss_get_rsp0(smp_current_cpu()));
        klog_puts(" kstack_top=0x");
        klog_put_hex64(t ? t->kernel_stack_top : 0);
        klog_putc('\n');
        sched_dump_cpus(); /* M106 - every core's view, not just this one */
    }
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
    /* M106: the general registers, which this dump never had. A fault at
     * an address that is one byte inside an instruction says the control
     * transfer that got there was wrong, and the only way to tell a bad
     * `ret` from a bad `call *%rax` apart is to see what was in the
     * registers. */
    klog_puts("\n  rax=0x");
    klog_put_hex64(r->rax);
    klog_puts(" rbx=0x");
    klog_put_hex64(r->rbx);
    klog_puts(" rcx=0x");
    klog_put_hex64(r->rcx);
    klog_puts("\n  rdx=0x");
    klog_put_hex64(r->rdx);
    klog_puts(" rsi=0x");
    klog_put_hex64(r->rsi);
    klog_puts(" rdi=0x");
    klog_put_hex64(r->rdi);
    klog_puts("\n  rbp=0x");
    klog_put_hex64(r->rbp);
    klog_puts(" r10=0x");
    klog_put_hex64(r->r10);
    /* M106: the bytes AT the faulting rip. A fault whose rip is one byte
     * inside a known instruction has two possible causes that look
     * identical in a register dump - a control transfer that went to the
     * wrong place, or a text page that is no longer this program's - and
     * sixteen bytes of memory tells them apart in one boot instead of
     * three. Only for a fault whose rip is mapped in the address space
     * this CPU is already running under, so reading it cannot fault. */
    {
        task_t *ft = sched_current();
        if (r->rip != 0 && ft &&
            vmm_user_range_ok(ft->pml4_phys, r->rip, 16, 0)) {
            const uint8_t *code = (const uint8_t *)(uintptr_t)r->rip;
            klog_puts("\n  code@rip=");
            for (int i = 0; i < 16; i++) {
                klog_put_hex32(code[i]);
                klog_putc(' ');
            }
        }
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
        uint64_t msg = klog_begin();
        klog_puts("[isr] breakpoint (int3) hit - resuming\n");
        dump_regs(r);
        klog_end(msg);
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
    if (r->vector == PAGE_FAULT_VECTOR) {
        int filled = sched_fault_fill(read_cr2(), r->error_code, r->rsp);
        if (filled == 1) {
            return;
        }
        if (filled == FILL_NO_MEMORY) {
            /* M102: the machine is out of memory, and this process is the
             * one that asked for the page it could not have.
             *
             * The victim is the asking process, chosen by not choosing:
             * no heuristic, no scoring, no scan for the largest resident
             * set. A machine with one principal that kills the program
             * that could not be given what it asked for is telling the
             * truth about what happened, which is the same argument M65
             * made about the permission model.
             *
             * SIGKILL rather than SIGSEGV, and the two lines below are
             * why: SIGSEGV says "your pointer was wrong", which is a
             * statement about the program and is false here - and it is
             * catchable, so a program with a handler could ignore the
             * news and carry on in an address space that cannot give it
             * another page. SIGKILL is uncatchable and says what
             * happened.
             *
             * Logged unconditionally. An OOM kill that leaves no record
             * is indistinguishable from a crash, and the first question
             * anybody asks about a process that vanished is which of the
             * two it was. */
            task_t *t = sched_current();
            klog_puts("\n[oom] out of physical memory filling 0x");
            klog_put_hex64(read_cr2());
            klog_puts(" for task ");
            klog_puts(t && t->name[0] ? t->name : "(unnamed)");
            klog_puts(" pid 0x");
            klog_put_hex32((uint32_t)(t ? t->id : -1));
            klog_puts(" - killing it, not the machine. ");
            klog_put_dec((uint32_t)pmm_free_frame_count());
            klog_puts(" frames free.\n");
            task_exit_with_signal(SIGKILL);
        }
    }

    /* The low two bits of the saved CS are the privilege level the fault
     * came from - 3 for user code, 0 for the kernel. This is the real
     * question ("whose bug is this?"), and it is one the interrupt frame
     * has always carried. */
    if ((r->cs & 3) == 3) {
        task_t *t = sched_current();
        /* M106: one report, not four cores' worth of lines shuffled
         * together. See klog.c on why this bracket exists and why it has
         * to be re-entrant - the same report can end in panic(). */
        uint64_t msg = klog_begin();
        klog_puts("\n[isr] ring-3 fault: ");
        klog_puts(exception_name(r->vector));
        klog_puts(" in task ");
        klog_puts(t && t->name[0] ? t->name : "(unnamed)");
        klog_puts(" pid 0x");
        klog_put_hex32((uint32_t)(t ? t->id : -1));
        klog_puts(" - terminating it, not the machine\n");
        dump_regs(r);
        klog_end(msg);
        /* noreturn: a TERMINATED task is never scheduled again, so the
         * interrupt frame this was called from is simply abandoned along
         * with the rest of that task's kernel stack. Exactly what the
         * SIGKILL/SIGTERM path already does from inside IRQ0's handler
         * (sched.c's deliver_pending_signal_and_exit), which is why this
         * is safe from interrupt context at all. */
        task_exit_with_signal(SIGSEGV);
    }

    uint64_t msg = klog_begin();
    klog_puts("\n*** UNHANDLED CPU EXCEPTION: ");
    klog_puts(exception_name(r->vector));
    klog_puts(" ***\n");
    dump_regs(r);
    klog_end(msg);
    panic("unrecoverable CPU exception");
}

static irq_handler_fn irq_handlers[16];

void irq_register_handler(uint8_t irq, irq_handler_fn handler) {
    irq_handlers[irq] = handler;
}

/* M103 - see isr.h. Routed to the boot CPU's LAPIC: spreading interrupts
 * across cores is a policy, and M69's rule is that a policy without a
 * measurement is a guess. The measurement now exists per vector per CPU
 * (/proc/interrupts), so the day it says the boot CPU is saturated, this
 * is the line that changes. */
void irq_enable_line(uint8_t irq) {
    /* IRQ 2 is the 8259's cascade: the wire that carries the second
     * PIC's output into the first, which is why mouse.c unmasks it
     * before IRQ 12. An I/O APIC has no cascade - there is one
     * controller, and GSI 2 is where the TIMER lives on every PC (see
     * ioapic.h on the override table). Enabling it here would unmask a
     * second entry for the timer's line with a vector nothing handles,
     * which is exactly what happened once: a storm of "unhandled IRQ 2"
     * beginning at the instant the mouse came up.
     *
     * Refused here rather than in mouse.c, because "which lines exist"
     * is a fact about the controller and this is the function that knows
     * which controller there is. */
    if (ioapic_available() && irq == 2) {
        return;
    }
    if (ioapic_available()) {
        /* This CPU's LAPIC id, read from the LAPIC itself rather than
         * from a table - every driver's init runs on the boot CPU, so
         * this is the boot CPU's id, and asking the hardware avoids a
         * second place that has to agree about which id that is. */
        ioapic_route_irq(irq, (uint8_t)lapic_id());
    } else {
        pic_clear_mask(irq);
    }
}

void irq_disable_line(uint8_t irq) {
    if (ioapic_available()) {
        ioapic_mask_irq(irq);
    } else {
        pic_set_mask(irq);
    }
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
    /* M103: whichever controller delivered it gets the acknowledgement.
     * The LAPIC's EOI is a single register write with no line number in
     * it - the local APIC knows which vector it is servicing - which is
     * why this is a choice of function rather than a choice of argument.
     *
     * Still BEFORE dispatching, for exactly the reason the paragraph
     * above gives about the scheduler's tick hook never returning. */
    if (ioapic_available()) {
        lapic_send_eoi();
    } else {
        pic_send_eoi(irq);
    }
    /* M103: counted per vector per CPU, because an interrupt that stops
     * arriving is otherwise indistinguishable from a device that has
     * nothing to say. Read out through /proc/interrupts. */
    ioapic_count_irq(r->vector, smp_current_cpu());
    if (irq_handlers[irq]) {
        irq_handlers[irq](r);
    } else {
        klog_puts("[irq] unhandled IRQ ");
        klog_put_hex64(irq);
        klog_putc('\n');
    }
}
