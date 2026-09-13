#include "isr.h"
#include "dev/random.h"

#include "drivers/klog.h"
#include "panic.h"
#include "mm/pmm.h"
#include "ioapic.h"
#include "lapic.h"
#include "pic.h"
#include "gdt.h"
#include "mm/vmm.h"
#include "smp.h"
#include "sched/sched.h"
#include "signal.h"
#include "arch/x86_64/syscall_entry.h"

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
        sched_dump_cpus();
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

void isr_handler(isr_regs_t *r) {
    if (r->vector == BREAKPOINT_VECTOR) {
        uint64_t msg = klog_begin();
        klog_puts("[isr] breakpoint (int3) hit - resuming\n");
        dump_regs(r);
        klog_end(msg);
        return;
    }

    if (r->vector == PAGE_FAULT_VECTOR) {
        int filled = sched_fault_fill(read_cr2(), r->error_code, r->rsp);
        if (filled == 1) {
            return;
        }
        if (filled == FILL_NO_MEMORY) {
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

    if ((r->cs & 3) == 3) {
        task_t *t = sched_current();
        int fault_signo;
        switch (r->vector) {
        case 0:
        case 16:
        case 19:
            fault_signo = SIGFPE;
            break;
        case 6:
            fault_signo = SIGILL;
            break;
        case 17:
            fault_signo = SIGBUS;
            break;
        default:
            fault_signo = SIGSEGV;
            break;
        }
        uint64_t fault_addr = (r->vector == PAGE_FAULT_VECTOR) ? read_cr2() : 0;
        if (signal_deliver_fault(r, fault_signo, fault_addr)) {
            return;
        }
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
        task_exit_with_signal(fault_signo);
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

void irq_disable_line(uint8_t irq) {
    if (ioapic_available()) {
        ioapic_mask_irq(irq);
    } else {
        pic_set_mask(irq);
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
        klog_puts("[irq] unhandled IRQ ");
        klog_put_hex64(irq);
        klog_putc('\n');
    }
}
