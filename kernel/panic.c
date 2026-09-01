#include "panic.h"

#include "arch/x86_64/smp.h"
#include "drivers/fb.h"
#include "drivers/font8x16.h"
#include "drivers/klog.h"

/* SMP: only the first CPU to panic broadcasts the halt-everyone-else NMI
 * (smp_halt_other_cpus) - every other core's NMI handler ends up calling
 * panic() too (isr.c's isr_handler treats any non-breakpoint exception,
 * NMI included, as fatal), and without this guard each of *those* would
 * re-broadcast to every other core (including the original one, whose
 * `cli` doesn't block a genuinely non-maskable interrupt) forever. One
 * broadcast, from whichever CPU got here first, is enough - everyone else
 * only needs to stop, not to also tell each other to stop. */
static volatile int panic_broadcast_sent;

/* One glyph, straight from the compiled-in 8x16 font into the
 * framebuffer. Deliberately not console.c's renderer: that one takes
 * klog_lock and maintains a cursor, and a panic must assume both are
 * unusable. */
static void panic_draw_char(uint32_t x, uint32_t y, char c, uint32_t rgb) {
    unsigned char ch = (unsigned char)c;
    if (ch > 0x7Eu || ch < 0x20u) {
        ch = '?'; /* the table is blank outside 0x20-0x7E - see font8x16.h */
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
            return; /* clipped rather than wrapped: a panic message is one line long */
        }
        panic_draw_char(x, y, *p, rgb);
        x += FONT_WIDTH;
    }
}

/* ---- M70: a panic that paints -----------------------------------------
 *
 * Until this milestone a panic wrote to a serial port and to a console
 * the compositor had almost certainly painted over. On a machine with no
 * serial cable - which is every real machine - the result was a frozen
 * desktop and nothing else. The last words of this kernel were written
 * somewhere nobody could read.
 *
 * So panic takes the screen back and writes on it. The constraints are
 * unusual and they shape the whole function:
 *
 *   - allocate nothing. The reason this is running may be that the heap
 *     is corrupt.
 *   - take no lock. The reason this is running may be that a lock is
 *     held by a task that is never going to release it - and a panic that
 *     deadlocks is strictly worse than one that prints nothing.
 *   - assume the framebuffer is mapped but that everything drawn on it is
 *     somebody else's. Painting a full-width band and writing into that
 *     is what makes the message readable over a desktop.
 *
 * console.c cannot be used for this: it takes klog_lock, it tracks a
 * cursor, and it is exactly the "some other subsystem is fine" assumption
 * a panic cannot make. fb_fill_rect and the raw 8x16 font are the
 * smallest things that are still legible.
 */
void panic_render(const char *msg) {
    uint32_t w = fb_width();
    uint32_t h = fb_height();
    if (w == 0 || h == 0) {
        return; /* no framebuffer - serial is all there is, and it already has this */
    }

    /* A band across the middle rather than the whole screen: it is faster
     * (a full clear at 1024x768 is three megabytes of writes through a
     * possibly-uncached mapping), it is unmistakably a takeover rather
     * than a repaint, and leaving the desktop visible around it is a
     * small kindness to whoever has to photograph this. */
    const uint32_t band_h = 8 * FONT_HEIGHT;
    uint32_t band_y = (h > band_h) ? (h - band_h) / 2 : 0;
    if (band_y + band_h > h) {
        return;
    }
    fb_fill_rect(0, band_y, w, band_h, 0x00800000u); /* dark red - not a colour this desktop uses */

    uint32_t x = 16, y = band_y + FONT_HEIGHT;
    panic_draw_text(x, y, "*** KERNEL PANIC ***", 0x00FFFFFFu);
    y += FONT_HEIGHT * 2;
    panic_draw_text(x, y, msg, 0x00FFE0E0u);
    y += FONT_HEIGHT * 2;
    panic_draw_text(x, y, "The machine has stopped. Nothing was written to disk after this.",
                     0x00FFC0C0u);
}

void panic(const char *msg) {
    /* Q2: before the first word. panic_render below was already written
     * to take no lock; klog_puts was not, and the NMI this function is
     * about to broadcast can land on a core that is holding klog_lock.
     * See klog.c's own note. */
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
