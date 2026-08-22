/* kernel/drivers/serial.h
 *
 * 16550 UART driver for COM1 (0x3F8), polling mode (no RX/TX interrupts -
 * nothing here needs asynchronous I/O yet). Exists as a debug-logging
 * channel that runs in parallel to VGA (see klog.h) and, unlike VGA, is
 * something QEMU can capture directly to a file/stdio (`-serial
 * file:...`) - much faster to verify against than a screendump.
 */
#pragma once

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
