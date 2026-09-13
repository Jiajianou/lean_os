#include "klog.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/io.h"
#include "arch/x86_64/smp.h"
#include "console.h"
#include "lib/spinlock.h"
#include "serial.h"
#include "vga.h"

static int use_console = 0;

static int console_released = 0;

void klog_init(void) {
    vga_clear();
    serial_init();
}

void klog_use_console(void) {
    use_console = 1;
}

void klog_release_console(void) {
    console_released = 1;
}

int klog_console_released(void) {
    return console_released;
}

static spinlock_t klog_lock;

static volatile int klog_panicking;
static spinlock_t klog_msg_lock;
static volatile int klog_msg_owner = -1;
static volatile int klog_msg_depth;

uint64_t klog_begin(void) {
    uint64_t flags = irq_save_disable();
    if (klog_panicking) {
        return flags;
    }
    int cpu = smp_current_cpu();
    if (klog_msg_owner == cpu) {
        klog_msg_depth++;
        return flags;
    }
    spin_lock(&klog_msg_lock);
    klog_msg_owner = cpu;
    klog_msg_depth = 1;
    return flags;
}

void klog_end(uint64_t flags) {
    if (!klog_panicking) {
        if (--klog_msg_depth == 0) {
            klog_msg_owner = -1;
            spin_unlock(&klog_msg_lock);
        }
    }
    irq_restore(flags);
}

void klog_enter_panic(void) {
    klog_panicking = 1;
}

#define KLOG_RING_SIZE 65536u

static char klog_ring[KLOG_RING_SIZE];
static uint64_t klog_written;

static void klog_emit_locked(char c, int also_console) {
    if (also_console && !console_released) {
        if (use_console) {
            console_putc(c);
        } else {
            vga_putc(c);
        }
    }
    serial_putc(c);
    klog_ring[klog_written % KLOG_RING_SIZE] = c;
    klog_written++;
}

static void klog_emit(char c, int also_console) {
    uint64_t flags = irq_save_disable();
    if (klog_panicking) {
        klog_emit_locked(c, also_console);
        irq_restore(flags);
        return;
    }
    spin_lock(&klog_lock);
    klog_emit_locked(c, also_console);
    spin_unlock(&klog_lock);
    irq_restore(flags);
}

void klog_putc(char c) {
    klog_emit(c, 1);
}

void klog_puts(const char *s) {
    uint64_t flags = irq_save_disable();
    if (klog_panicking) {
        while (*s) {
            klog_emit_locked(*s++, 1);
        }
        irq_restore(flags);
        return;
    }
    spin_lock(&klog_lock);
    while (*s) {
        klog_emit_locked(*s++, 1);
    }
    spin_unlock(&klog_lock);
    irq_restore(flags);
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
    int also_console = level >= current_level;
    uint64_t flags = irq_save_disable();
    spin_lock(&klog_lock);
    while (*s) {
        klog_emit_locked(*s++, also_console);
    }
    spin_unlock(&klog_lock);
    irq_restore(flags);
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
