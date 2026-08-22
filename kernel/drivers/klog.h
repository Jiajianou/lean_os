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
