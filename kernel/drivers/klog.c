#include "klog.h"

#include "arch/x86_64/io.h" /* irq_save_disable/irq_restore */
#include "console.h"
#include "lib/spinlock.h"
#include "serial.h"
#include "vga.h"

static int use_console = 0;

void klog_init(void) {
    vga_clear();
    serial_init();
}

void klog_use_console(void) {
    use_console = 1;
}

/* console.c's cursor position (cur_col/cur_row) is shared mutable state
 * with no locking of its own - fine for vga.c's cur_row/cur_col (same
 * shape of shared state, but a wrapped/scrolled line from an interleaved
 * write is just a cosmetic glitch there), not fine for the framebuffer
 * console, which panics on an out-of-bounds coordinate. Once M13 gave
 * this kernel more than one runnable task and both could call klog_putc
 * concurrently (e.g. init and the shell it just spawned), a timer
 * preemption landing mid-update could hand the next call a half-updated
 * cursor - caught for real during M17 verification, not hypothetical.
 *
 * Disabling interrupts around one character used to be enough on its own
 * (a single-core critical section, correct back when there was no second
 * CPU to still race with) - now that SMP means a genuinely different core
 * can be inside klog_putc at the same instant, `cli` alone only protects
 * against *this* CPU's own interrupt handlers, not another core. klog_lock
 * adds the cross-CPU half; `cli` stays too, since it's still what prevents
 * a same-CPU interrupt handler from deadlocking on a lock this same core
 * already holds. irq_save_disable/irq_restore now live in arch/x86_64/
 * io.h - kernel/sched/sched.c needs the identical primitive for
 * sched_lock, for the same "keep this CPU's own interrupt handlers out of
 * a critical section it already holds" reason. */
static spinlock_t klog_lock;

void klog_putc(char c) {
    /* cli has to happen *before* taking the lock, not after: an interrupt
     * landing on this CPU in the gap between them, whose handler also
     * calls klog_puts (a fault reported by isr_handler, say), would try to
     * spin_lock a lock this exact CPU already holds and deadlock on
     * itself - the identical mistake sched_lock had, see sched.c's own
     * note on why. */
    uint64_t flags = irq_save_disable();
    spin_lock(&klog_lock);
    if (use_console) {
        console_putc(c);
    } else {
        vga_putc(c);
    }
    serial_putc(c);
    spin_unlock(&klog_lock);
    irq_restore(flags);
}

void klog_puts(const char *s) {
    while (*s) {
        klog_putc(*s++);
    }
}

static char hex_digit(uint8_t nibble) {
    return (char)(nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10));
}

void klog_put_hex32(uint32_t value) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        klog_putc(hex_digit((value >> shift) & 0xF));
    }
}

void klog_put_hex64(uint64_t value) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        klog_putc(hex_digit((value >> shift) & 0xF));
    }
}
