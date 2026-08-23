/* kernel/drivers/klog.h
 *
 * Debug-log fan-out: every call writes to both VGA (vga.h, what's visible
 * in a running QEMU window/screendump) and the COM1 serial port
 * (serial.h, what `-serial file:...`/stdio capture sees). This is the
 * single logging facade the rest of the kernel uses instead of calling
 * vga_* directly, so "parallel to VGA" (M6) is true of every message, not
 * just the ones someone remembered to duplicate by hand.
 *
 * klog_putc/klog_puts/klog_put_hex32/klog_put_hex64 are the raw primitives
 * above: unconditional, always both outputs. They're also what sys_write
 * uses to route a user program's real stdout, so they can't be filtered by
 * level - anything that's actual program output, or a fault/panic dump
 * that must never be silently dropped, calls these directly.
 *
 * klog_log (+ klog_debug/info/warn/error below) is the leveled facade for
 * kernel-internal diagnostics: it's the same fan-out, except a message
 * below the current threshold (klog_set_level, default KLOG_INFO) only
 * reaches serial, not the screen. This is what routine "here's what I'm
 * doing" prints should use, so a busy diagnostic doesn't flash across the
 * visible console every time it fires - serial capture still sees it.
 */
#pragma once

#include <stdint.h>

void klog_init(void);
void klog_putc(char c);
void klog_puts(const char *s);
void klog_put_hex32(uint32_t value);
void klog_put_hex64(uint64_t value);

typedef enum {
    KLOG_DEBUG = 0,
    KLOG_INFO,
    KLOG_WARN,
    KLOG_ERROR,
} klog_level_t;

/* Messages at `level` or above reach the screen (and always reach serial).
 * Default is KLOG_INFO. */
void klog_set_level(klog_level_t level);
void klog_log(klog_level_t level, const char *s);
void klog_log_hex32(klog_level_t level, uint32_t value);
void klog_log_hex64(klog_level_t level, uint64_t value);

static inline void klog_debug(const char *s) { klog_log(KLOG_DEBUG, s); }
static inline void klog_info(const char *s) { klog_log(KLOG_INFO, s); }
static inline void klog_warn(const char *s) { klog_log(KLOG_WARN, s); }
static inline void klog_error(const char *s) { klog_log(KLOG_ERROR, s); }

/* M17: switches the visual half of the fan-out from VGA text mode to the
 * framebuffer console (console.h) - called once, right after
 * console_init(), from the earliest point graphics are actually ready.
 * Everything logged before this call (GDT/IDT/E820/pmm/vmm/heap/fb setup
 * itself - console_init needs vmm live, so it can't come any earlier)
 * only ever reached VGA text mode; serial output is unaffected either
 * way, so nothing is lost from tools/qemu-serial-test.sh's perspective. */
void klog_use_console(void);
