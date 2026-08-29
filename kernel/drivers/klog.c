#include "klog.h"

#include <stddef.h>
#include <stdint.h>

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

/* ---- M70: the ring buffer ---------------------------------------------
 *
 * Everything klog emits has always gone to two places: a serial port and
 * the screen. Both are write-only and both are gone the moment the
 * compositor paints over the console - so on any machine without a serial
 * cable attached, this kernel's entire diagnostic surface was a black
 * screen. Every capability denial M65 was careful to log, every driver
 * message, every spawn failure: written down where nobody on the machine
 * could read them.
 *
 * This is the third destination, and the first one that survives being
 * written to. A plain byte ring, because that is exactly what klog
 * produces - there are no records here, no levels to filter on the way
 * in, just the characters that were already going to the wire.
 *
 * 64 KiB holds a whole boot's worth of log on this machine, which is the
 * size that makes "what happened during startup" answerable after the
 * desktop is up. It costs a fixed 64 KiB of kernel image and never grows.
 *
 * `written` is the total ever emitted and never wraps in practice at
 * 64 bits - it is what gives a reader a cursor that means something
 * across wraps. A reader that falls more than KLOG_RING_SIZE behind has
 * missed bytes, and can tell, which beats silently reading a hole. */
#define KLOG_RING_SIZE 65536u

static char klog_ring[KLOG_RING_SIZE];
static uint64_t klog_written; /* total bytes ever emitted */

/* also_console decides whether this byte reaches the screen; serial always
 * gets it either way (nothing klog emits is ever lost from serial capture,
 * per the header's fan-out guarantee). klog_putc and the leveled path in
 * klog_log_putc below both fall through to this. */
static void klog_emit(char c, int also_console) {
    /* cli has to happen *before* taking the lock, not after: an interrupt
     * landing on this CPU in the gap between them, whose handler also
     * calls klog_puts (a fault reported by isr_handler, say), would try to
     * spin_lock a lock this exact CPU already holds and deadlock on
     * itself - the identical mistake sched_lock had, see sched.c's own
     * note on why. */
    uint64_t flags = irq_save_disable();
    spin_lock(&klog_lock);
    if (also_console) {
        if (use_console) {
            console_putc(c);
        } else {
            vga_putc(c);
        }
    }
    serial_putc(c);
    /* M70: and into the ring, under the same lock that already serialises
     * the other two destinations - so the ring can never contain a
     * half-written line that the serial capture does not. */
    klog_ring[klog_written % KLOG_RING_SIZE] = c;
    klog_written++;
    spin_unlock(&klog_lock);
    irq_restore(flags);
}

void klog_putc(char c) {
    klog_emit(c, 1);
}

void klog_puts(const char *s) {
    while (*s) {
        klog_putc(*s++);
    }
}

static char hex_digit(uint8_t nibble) {
    return (char)(nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10));
}

void klog_put_dec_pad(uint32_t value, int width) {
    char buf[10];
    int n = 0;
    do {
        buf[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value && n < (int)sizeof(buf));
    for (int pad = n; pad < width; pad++) {
        klog_putc('0');
    }
    while (n > 0) {
        klog_putc(buf[--n]);
    }
}

void klog_put_dec(uint32_t value) {
    klog_put_dec_pad(value, 1);
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

static klog_level_t current_level = KLOG_INFO;

void klog_set_level(klog_level_t level) {
    current_level = level;
}

static void klog_log_putc(klog_level_t level, char c) {
    klog_emit(c, level >= current_level);
}

void klog_log(klog_level_t level, const char *s) {
    while (*s) {
        klog_log_putc(level, *s++);
    }
}

void klog_log_hex32(klog_level_t level, uint32_t value) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        klog_log_putc(level, hex_digit((value >> shift) & 0xF));
    }
}

void klog_log_hex64(klog_level_t level, uint64_t value) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        klog_log_putc(level, hex_digit((value >> shift) & 0xF));
    }
}

/* M70: see klog.h. Copies out at most `max` bytes starting at absolute
 * position `from`, and returns how many. `*next` is set to the position
 * to ask for next time, which is how a reader follows the log rather than
 * re-reading it.
 *
 * A `from` older than the ring can still reach is silently advanced to
 * the oldest byte still held, and *next tells the caller where it
 * actually resumed - so a reader that fell behind sees a jump in the
 * cursor rather than a seamless stream with a hole in it. */
size_t klog_read(uint64_t from, char *out, size_t max, uint64_t *next) {
    uint64_t flags = irq_save_disable();
    spin_lock(&klog_lock);

    uint64_t total = klog_written;
    uint64_t oldest = total > KLOG_RING_SIZE ? total - KLOG_RING_SIZE : 0;
    if (from < oldest) {
        from = oldest;
    }
    size_t n = 0;
    while (from + n < total && n < max) {
        out[n] = klog_ring[(from + n) % KLOG_RING_SIZE];
        n++;
    }
    if (next) {
        *next = from + n;
    }

    spin_unlock(&klog_lock);
    irq_restore(flags);
    return n;
}

uint64_t klog_written_total(void) {
    uint64_t flags = irq_save_disable();
    spin_lock(&klog_lock);
    uint64_t v = klog_written;
    spin_unlock(&klog_lock);
    irq_restore(flags);
    return v;
}
