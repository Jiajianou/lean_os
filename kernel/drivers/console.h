/* kernel/drivers/console.h
 *
 * A scrolling monospace text console drawn over the M16 framebuffer,
 * using the font8x16 bitmap font. This is what M17 gives klog (klog.h)
 * to switch to once graphics are up - see klog_use_console(). Not meant
 * to be called directly by most of the kernel; go through klog instead,
 * the same way vga.h never was either.
 */
#pragma once

void console_init(void);
void console_putc(char c);
void console_puts(const char *s);
