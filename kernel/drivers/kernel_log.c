#include "kernel_log.h"

#include <stddef.h>
#include <stdint.h>

#include "architecture/x86_64/io.h"
#include "architecture/x86_64/symmetric_multiprocessing.h"
#include "console.h"
#include "library/spinlock.h"
#include "serial.h"
#include "vga.h"

static int use_console = 0;

static int console_released = 0;

void kernel_log_init(void) {
    vga_clear();
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

static void kernel_log_emit_locked(char c, int also_console) {
    if (also_console && !console_released) {
        if (use_console) {
            console_putc(c);
        } else {
            vga_putc(c);
        }
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
