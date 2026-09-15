#include "interrupt_service_routines.h"
#include "device/random.h"

#include "drivers/kernel_log.h"
#include "panic.h"
#include "memory_management/physical_memory.h"
#include "ioapic.h"
#include "lapic.h"
#include "pic.h"
#include "global_descriptor_table.h"
#include "memory_management/virtual_memory.h"
#include "symmetric_multiprocessing.h"
#include "scheduler/scheduler.h"
#include "signal.h"
#include "architecture/x86_64/syscall_entry.h"

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

/* The x87 status word and MXCSR each carry one bit per exception, and the
   order below is the order the hardware reports them in when more than one is
   pending - invalid first, because it is the one that makes the others
   meaningless. */
static int fp_exception_code(uint32_t status) {
    if (status & 0x01) {
        return FPE_FLTINV;
    }
    if (status & 0x04) {
        return FPE_FLTDIV;
    }
    if (status & 0x08) {
        return FPE_FLTOVF;
    }
    if (status & 0x10) {
        return FPE_FLTUND;
    }
    if (status & 0x20) {
        return FPE_FLTRES;
    }
    if (status & 0x02) {
        return FPE_FLTSUB;
    }
    return SI_KERNEL;
}

static int x87_exception_code(void) {
    uint16_t status = 0;
    __asm__ volatile("fnstsw %0" : "=m"(status));
    return fp_exception_code(status);
}

static int sse_exception_code(void) {
    uint32_t mxcsr = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    return fp_exception_code(mxcsr);
}

static void dump_regs(isr_regs_t *r) {
    {
        task_t *t = scheduler_current();
        kernel_log_puts("  cpu=");
        kernel_log_put_dec((uint32_t)smp_current_cpu());
        kernel_log_puts(" task=");
        kernel_log_puts(t && t->name[0] ? t->name : "(none)");
        kernel_log_puts(" pid=0x");
        kernel_log_put_hex32((uint32_t)(t ? t->id : -1));
        kernel_log_puts(" tss.rsp0=0x");
        kernel_log_put_hex64(tss_get_rsp0(smp_current_cpu()));
        kernel_log_puts(" kstack_top=0x");
        kernel_log_put_hex64(t ? t->kernel_stack_top : 0);
        kernel_log_putc('\n');
        scheduler_dump_cpus();
    }
    kernel_log_puts("  vector=0x");
    kernel_log_put_hex64(r->vector);
    kernel_log_puts(" error_code=0x");
    kernel_log_put_hex64(r->error_code);
    kernel_log_puts("\n  rip=0x");
    kernel_log_put_hex64(r->rip);
    kernel_log_puts(" cs=0x");
    kernel_log_put_hex64(r->cs);
    kernel_log_puts(" rflags=0x");
    kernel_log_put_hex64(r->rflags);
    kernel_log_puts("\n  rsp=0x");
    kernel_log_put_hex64(r->rsp);
    kernel_log_puts(" ss=0x");
    kernel_log_put_hex64(r->ss);
    if (r->vector == PAGE_FAULT_VECTOR) {
        kernel_log_puts("\n  cr2=0x");
        kernel_log_put_hex64(read_cr2());
    }
    kernel_log_puts("\n  rax=0x");
    kernel_log_put_hex64(r->rax);
    kernel_log_puts(" rbx=0x");
    kernel_log_put_hex64(r->rbx);
    kernel_log_puts(" rcx=0x");
    kernel_log_put_hex64(r->rcx);
    kernel_log_puts("\n  rdx=0x");
    kernel_log_put_hex64(r->rdx);
    kernel_log_puts(" rsi=0x");
    kernel_log_put_hex64(r->rsi);
    kernel_log_puts(" rdi=0x");
    kernel_log_put_hex64(r->rdi);
    kernel_log_puts("\n  rbp=0x");
    kernel_log_put_hex64(r->rbp);
    kernel_log_puts(" r10=0x");
    kernel_log_put_hex64(r->r10);
    {
        task_t *ft = scheduler_current();
        if (r->rip != 0 && ft &&
            virtual_memory_user_range_ok(ft->pml4_phys, r->rip, 16, 0)) {
            const uint8_t *code = (const uint8_t *)(uintptr_t)r->rip;
            kernel_log_puts("\n  code@rip=");
            for (int i = 0; i < 16; i++) {
                kernel_log_put_hex32(code[i]);
                kernel_log_putc(' ');
            }
        }
    }
    kernel_log_putc('\n');
}

void isr_handler(isr_regs_t *r) {
    if (r->vector == BREAKPOINT_VECTOR) {
        uint64_t message = kernel_log_begin();
        kernel_log_puts("[isr] breakpoint (int3) hit - resuming\n");
        dump_regs(r);
        kernel_log_end(message);
        return;
    }

    if (r->vector == PAGE_FAULT_VECTOR) {
        int filled = scheduler_fault_fill(read_cr2(), r->error_code, r->rsp);
        if (filled == 1) {
            return;
        }
        if (filled == FILL_NO_MEMORY) {
            task_t *t = scheduler_current();
            kernel_log_puts("\n[oom] out of physical memory filling 0x");
            kernel_log_put_hex64(read_cr2());
            kernel_log_puts(" for task ");
            kernel_log_puts(t && t->name[0] ? t->name : "(unnamed)");
            kernel_log_puts(" pid 0x");
            kernel_log_put_hex32((uint32_t)(t ? t->id : -1));
            kernel_log_puts(" - killing it, not the machine. ");
            kernel_log_put_dec((uint32_t)physical_memory_free_frame_count());
            kernel_log_puts(" frames free.\n");
            task_exit_with_signal(SIGKILL);
        }
    }

    if ((r->cs & 3) == 3) {
        task_t *t = scheduler_current();
        int fault_signo;
        int fault_code = SI_KERNEL;
        switch (r->vector) {
        case 0:
            fault_signo = SIGFPE;
            fault_code = FPE_INTDIV;
            break;
        case 16:
            fault_signo = SIGFPE;
            fault_code = x87_exception_code();
            break;
        case 19:
            fault_signo = SIGFPE;
            fault_code = sse_exception_code();
            break;
        case 6:
            fault_signo = SIGILL;
            fault_code = ILL_ILLOPC;
            break;
        case 17:
            fault_signo = SIGBUS;
            fault_code = BUS_ADRALN;
            break;
        case PAGE_FAULT_VECTOR:
            fault_signo = SIGSEGV;
            /* Not the hardware's present bit. This kernel fills pages on
               demand, so a write to a read-only page nobody has touched
               faults with that bit CLEAR - and calling it SEGV_MAPERR would
               report no mapping where there was one that refused the access.
               The question the two codes actually name is whether the process
               asked for this address, and the scheduler is what knows. */
            fault_code = scheduler_address_is_mapped(read_cr2()) ? SEGV_ACCERR
                                                                 : SEGV_MAPERR;
            break;
        default:
            fault_signo = SIGSEGV;
            break;
        }
        uint64_t fault_address = (r->vector == PAGE_FAULT_VECTOR) ? read_cr2() : 0;
        if (signal_deliver_fault_with_code(r, fault_signo, fault_address,
                                           fault_code)) {
            return;
        }
        uint64_t message = kernel_log_begin();
        kernel_log_puts("\n[isr] ring-3 fault: ");
        kernel_log_puts(exception_name(r->vector));
        kernel_log_puts(" in task ");
        kernel_log_puts(t && t->name[0] ? t->name : "(unnamed)");
        kernel_log_puts(" pid 0x");
        kernel_log_put_hex32((uint32_t)(t ? t->id : -1));
        kernel_log_puts(" - terminating it, not the machine\n");
        dump_regs(r);
        kernel_log_end(message);
        task_exit_with_signal(fault_signo);
    }

    uint64_t message = kernel_log_begin();
    kernel_log_puts("\n*** UNHANDLED CPU EXCEPTION: ");
    kernel_log_puts(exception_name(r->vector));
    kernel_log_puts(" ***\n");
    dump_regs(r);
    kernel_log_end(message);
    panic("unrecoverable CPU exception");
}

static irq_handler_function irq_handlers[16];

void irq_register_handler(uint8_t irq, irq_handler_function handler) {
    irq_handlers[irq] = handler;
}

void irq_enable_line(uint8_t irq) {
    if (ioapic_available() && irq == 2) {
        return;
    }
    if (ioapic_available()) {
        ioapic_route_irq(irq, (uint8_t)lapic_id());
    } else {
        pic_clear_mask(irq);
    }
}

void irq_handler(isr_regs_t *r) {
    uint8_t irq = (uint8_t)(r->vector - 32);
    if (ioapic_available()) {
        lapic_send_eoi();
    } else {
        pic_send_eoi(irq);
    }
    ioapic_count_irq(r->vector, smp_current_cpu());
    random_feed(&r->vector, 1);
    if (irq_handlers[irq]) {
        irq_handlers[irq](r);
    } else {
        kernel_log_puts("[irq] unhandled IRQ ");
        kernel_log_put_hex64(irq);
        kernel_log_putc('\n');
    }
}
