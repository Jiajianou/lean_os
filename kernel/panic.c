#include "panic.h"

#include "architecture/x86_64/smp.h"
#include "drivers/fb.h"
#include "drivers/font8x16.h"
#include "drivers/klog.h"

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
                fb_put_pixel(x + col, y + row, rgb);
            }
        }
    }
}

static void panic_draw_text(uint32_t x, uint32_t y, const char *s, uint32_t rgb) {
    uint32_t max_x = fb_width();
    for (const char *p = s; *p; p++) {
        if (x + FONT_WIDTH > max_x) {
            return;
        }
        panic_draw_char(x, y, *p, rgb);
        x += FONT_WIDTH;
    }
}

void panic_render(const char *msg) {
    uint32_t w = fb_width();
    uint32_t h = fb_height();
    if (w == 0 || h == 0) {
        return;
    }

    const uint32_t band_h = 8 * FONT_HEIGHT;
    uint32_t band_y = (h > band_h) ? (h - band_h) / 2 : 0;
    if (band_y + band_h > h) {
        return;
    }
    fb_fill_rect(0, band_y, w, band_h, 0x00800000u);

    uint32_t x = 16, y = band_y + FONT_HEIGHT;
    panic_draw_text(x, y, "*** KERNEL PANIC ***", 0x00FFFFFFu);
    y += FONT_HEIGHT * 2;
    panic_draw_text(x, y, msg, 0x00FFE0E0u);
    y += FONT_HEIGHT * 2;
    panic_draw_text(x, y, "The machine has stopped. Nothing was written to disk after this.",
                     0x00FFC0C0u);
}

void panic(const char *msg) {
    klog_enter_panic();
    klog_puts("\n*** KERNEL PANIC: ");
    klog_puts(msg);
    klog_puts(" ***\n");
    panic_render(msg);
    if (smp_is_initialized() && __atomic_exchange_n(&panic_broadcast_sent, 1, __ATOMIC_ACQ_REL) == 0) {
        smp_halt_other_cpus();
    }
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
