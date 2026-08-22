#include "panic.h"

#include "arch/x86_64/smp.h"
#include "drivers/klog.h"

/* SMP: only the first CPU to panic broadcasts the halt-everyone-else NMI
 * (smp_halt_other_cpus) - every other core's NMI handler ends up calling
 * panic() too (isr.c's isr_handler treats any non-breakpoint exception,
 * NMI included, as fatal), and without this guard each of *those* would
 * re-broadcast to every other core (including the original one, whose
 * `cli` doesn't block a genuinely non-maskable interrupt) forever. One
 * broadcast, from whichever CPU got here first, is enough - everyone else
 * only needs to stop, not to also tell each other to stop. */
static volatile int panic_broadcast_sent;

void panic(const char *msg) {
    klog_puts("\n*** KERNEL PANIC: ");
    klog_puts(msg);
    klog_puts(" ***\n");
    if (smp_is_initialized() && __atomic_exchange_n(&panic_broadcast_sent, 1, __ATOMIC_ACQ_REL) == 0) {
        smp_halt_other_cpus();
    }
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
