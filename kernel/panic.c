#include "panic.h"

#include "architecture/x86_64/symmetric_multiprocessing.h"
#include "drivers/framebuffer.h"
#include "drivers/font8x16.h"
#include "drivers/kernel_log.h"

static volatile int panic_broadcast_sent;

static void panic_draw_char(uint32_t x, uint32_t y, char c, uint32_t rgb) {
    unsigned char ch = (unsigned char)c;
    if (ch > 0x7Eu || ch < 0x20u) {
        ch = '?';
    }
    const uint8_t *glyph = font8x16[ch];
    for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80u >> col)) {
                framebuffer_put_pixel(x + col, y + row, rgb);
            }
        }
    }
}

static void panic_draw_text(uint32_t x, uint32_t y, const char *s, uint32_t rgb) {
    uint32_t max_x = framebuffer_width();
    for (const char *p = s; *p; p++) {
        if (x + FONT_WIDTH > max_x) {
            return;
        }
        panic_draw_char(x, y, *p, rgb);
        x += FONT_WIDTH;
    }
}

static void panic_render_detail(const char *message, const char *detail) {
    uint32_t w = framebuffer_width();
    uint32_t h = framebuffer_height();
    if (w == 0 || h == 0) {
        return;
    }

    const uint32_t band_h = (detail ? 10 : 8) * FONT_HEIGHT;
    uint32_t band_y = (h > band_h) ? (h - band_h) / 2 : 0;
    if (band_y + band_h > h) {
        return;
    }
    framebuffer_fill_rect(0, band_y, w, band_h, 0x00800000u);

    uint32_t x = 16, y = band_y + FONT_HEIGHT;
    panic_draw_text(x, y, "*** KERNEL PANIC ***", 0x00FFFFFFu);
    y += FONT_HEIGHT * 2;
    panic_draw_text(x, y, message, 0x00FFE0E0u);
    y += FONT_HEIGHT * 2;
    if (detail) {
        panic_draw_text(x, y, detail, 0x00FFFFFFu);
        y += FONT_HEIGHT * 2;
    }
    panic_draw_text(x, y, "The machine has stopped. Nothing was written to disk after this.",
                     0x00FFC0C0u);
}

void panic_render(const char *message) {
    panic_render_detail(message, (const char *)0);
}

void panic(const char *message) {
    panic_with_detail(message, (const char *)0);
}

void panic_with_detail(const char *message, const char *detail) {
    kernel_log_enter_panic();
    kernel_log_puts("\n*** KERNEL PANIC: ");
    kernel_log_puts(message);
    kernel_log_puts(" ***\n");
    if (detail) {
        kernel_log_puts(detail);
        kernel_log_putc('\n');
    }
    panic_render_detail(message, detail);
    if (smp_is_initialized() && __atomic_exchange_n(&panic_broadcast_sent, 1, __ATOMIC_ACQ_REL) == 0) {
        smp_halt_other_cpus();
    }
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
