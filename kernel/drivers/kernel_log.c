#include "kernel_log.h"

#include <stddef.h>
#include <stdint.h>

#include "architecture/x86_64/io.h"
#include "architecture/x86_64/symmetric_multiprocessing.h"
#include "architecture/x86_64/timestamp_counter.h"
#include "console.h"
#include "library/spinlock.h"
#include "serial.h"

static int use_console = 0;

static int console_released = 0;

void kernel_log_init(void) {
    serial_init();
}

void kernel_log_use_console(void) {
    use_console = 1;
}

void kernel_log_release_console(void) {
    console_released = 1;
}

static spinlock_t kernel_log_lock;

static volatile int kernel_log_panicking;
static spinlock_t kernel_log_message_lock;
static volatile int kernel_log_message_owner = -1;
static volatile int kernel_log_message_depth;

uint64_t kernel_log_begin(void) {
    uint64_t flags = irq_save_disable();
    if (kernel_log_panicking) {
        return flags;
    }
    int cpu = smp_current_cpu();
    if (kernel_log_message_owner == cpu) {
        kernel_log_message_depth++;
        return flags;
    }
    spin_lock(&kernel_log_message_lock);
    kernel_log_message_owner = cpu;
    kernel_log_message_depth = 1;
    return flags;
}

void kernel_log_end(uint64_t flags) {
    if (!kernel_log_panicking) {
        if (--kernel_log_message_depth == 0) {
            kernel_log_message_owner = -1;
            spin_unlock(&kernel_log_message_lock);
        }
    }
    irq_restore(flags);
}

void kernel_log_enter_panic(void) {
    kernel_log_panicking = 1;
}

#define KERNEL_LOG_RING_SIZE 262144u

static char kernel_log_ring[KERNEL_LOG_RING_SIZE];
static uint64_t kernel_log_written;

/* M197. The stick log said the laptop took nine seconds to reach the desktop
   and could not say where: a line carried no time. Each line's first byte is
   remembered with the TSC it was written at, so the log a machine with no
   serial port leaves behind can be read as a timeline. */
#define KERNEL_LOG_LINE_RING 16384u

static uint64_t kernel_log_line_offset[KERNEL_LOG_LINE_RING];
static uint64_t kernel_log_line_tsc[KERNEL_LOG_LINE_RING];
static uint64_t kernel_log_lines;
static int kernel_log_mid_line;

static void kernel_log_emit_locked(char c, int also_console) {
    if (!kernel_log_mid_line) {
        uint64_t slot = kernel_log_lines % KERNEL_LOG_LINE_RING;
        kernel_log_line_offset[slot] = kernel_log_written;
        kernel_log_line_tsc[slot] = tsc_read();
        kernel_log_lines++;
    }
    kernel_log_mid_line = c != '\n';
    if (also_console && use_console && !console_released) {
        console_putc(c);
    }
    serial_putc(c);
    kernel_log_ring[kernel_log_written % KERNEL_LOG_RING_SIZE] = c;
    kernel_log_written++;
}

static void kernel_log_emit(char c, int also_console) {
    uint64_t flags = irq_save_disable();
    if (kernel_log_panicking) {
        kernel_log_emit_locked(c, also_console);
        irq_restore(flags);
        return;
    }
    spin_lock(&kernel_log_lock);
    kernel_log_emit_locked(c, also_console);
    spin_unlock(&kernel_log_lock);
    irq_restore(flags);
}

void kernel_log_putc(char c) {
    kernel_log_emit(c, 1);
}

void kernel_log_puts(const char *s) {
    uint64_t flags = irq_save_disable();
    if (kernel_log_panicking) {
        while (*s) {
            kernel_log_emit_locked(*s++, 1);
        }
        irq_restore(flags);
        return;
    }
    spin_lock(&kernel_log_lock);
    while (*s) {
        kernel_log_emit_locked(*s++, 1);
    }
    spin_unlock(&kernel_log_lock);
    irq_restore(flags);
}

static char hex_digit(uint8_t nibble) {
    return (char)(nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10));
}

void kernel_log_put_dec_pad(uint32_t value, int width) {
    char buffer[10];
    int n = 0;
    do {
        buffer[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value && n < (int)sizeof(buffer));
    for (int pad = n; pad < width; pad++) {
        kernel_log_putc('0');
    }
    while (n > 0) {
        kernel_log_putc(buffer[--n]);
    }
}

void kernel_log_put_dec(uint32_t value) {
    kernel_log_put_dec_pad(value, 1);
}

void kernel_log_put_hex32(uint32_t value) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        kernel_log_putc(hex_digit((value >> shift) & 0xF));
    }
}

void kernel_log_put_hex64(uint64_t value) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        kernel_log_putc(hex_digit((value >> shift) & 0xF));
    }
}

static kernel_log_level_t current_level = KERNEL_LOG_INFO;

static void kernel_log_log_putc(kernel_log_level_t level, char c) {
    kernel_log_emit(c, level >= current_level);
}

void kernel_log_log(kernel_log_level_t level, const char *s) {
    int also_console = level >= current_level;
    uint64_t flags = irq_save_disable();
    spin_lock(&kernel_log_lock);
    while (*s) {
        kernel_log_emit_locked(*s++, also_console);
    }
    spin_unlock(&kernel_log_lock);
    irq_restore(flags);
}

void kernel_log_log_hex64(kernel_log_level_t level, uint64_t value) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        kernel_log_log_putc(level, hex_digit((value >> shift) & 0xF));
    }
}

size_t kernel_log_read(uint64_t from, char *out, size_t max, uint64_t *next) {
    uint64_t flags = irq_save_disable();
    spin_lock(&kernel_log_lock);

    uint64_t total = kernel_log_written;
    uint64_t oldest = total > KERNEL_LOG_RING_SIZE ? total - KERNEL_LOG_RING_SIZE : 0;
    if (from < oldest) {
        from = oldest;
    }
    size_t n = 0;
    while (from + n < total && n < max) {
        out[n] = kernel_log_ring[(from + n) % KERNEL_LOG_RING_SIZE];
        n++;
    }
    if (next) {
        *next = from + n;
    }

    spin_unlock(&kernel_log_lock);
    irq_restore(flags);
    return n;
}

uint64_t kernel_log_written_total(void) {
    uint64_t flags = irq_save_disable();
    spin_lock(&kernel_log_lock);
    uint64_t v = kernel_log_written;
    spin_unlock(&kernel_log_lock);
    irq_restore(flags);
    return v;
}

int kernel_log_line_time(uint64_t offset, uint64_t *tsc) {
    uint64_t flags = irq_save_disable();
    spin_lock(&kernel_log_lock);
    uint64_t oldest = kernel_log_lines > KERNEL_LOG_LINE_RING ? kernel_log_lines - KERNEL_LOG_LINE_RING : 0;
    uint64_t low = oldest;
    uint64_t high = kernel_log_lines;
    int found = 0;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2u;
        uint64_t at = kernel_log_line_offset[middle % KERNEL_LOG_LINE_RING];
        if (at == offset) {
            *tsc = kernel_log_line_tsc[middle % KERNEL_LOG_LINE_RING];
            found = 1;
            break;
        }
        if (at < offset) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    spin_unlock(&kernel_log_lock);
    irq_restore(flags);
    return found;
}
