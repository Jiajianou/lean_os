/* kernel/drivers/klog.h
 *
 * Debug-log fan-out: every call writes to both VGA (vga.h, what's visible
 * in a running QEMU window/screendump) and the COM1 serial port
 * (serial.h, what `-serial file:...`/stdio capture sees). This is the
 * single logging facade the rest of the kernel uses instead of calling
 * vga_* directly, so "parallel to VGA" (M6) is true of every message, not
 * just the ones someone remembered to duplicate by hand.
 */
#pragma once

#include <stdint.h>

void klog_init(void);
void klog_putc(char c);
void klog_puts(const char *s);
void klog_put_hex32(uint32_t value);
void klog_put_hex64(uint64_t value);

/* M17: switches the visual half of the fan-out from VGA text mode to the
 * framebuffer console (console.h) - called once, right after
 * console_init(), from the earliest point graphics are actually ready.
 * Everything logged before this call (GDT/IDT/E820/pmm/vmm/heap/fb setup
 * itself - console_init needs vmm live, so it can't come any earlier)
 * only ever reached VGA text mode; serial output is unaffected either
 * way, so nothing is lost from tools/qemu-serial-test.sh's perspective. */
void klog_use_console(void);
