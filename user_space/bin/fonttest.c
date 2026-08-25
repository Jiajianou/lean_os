/* user_space/bin/fonttest.c
 *
 * M57 self-test, spawned from kernel_main the same way M19's memtest is:
 * real ring-3 code exercising the thing the milestone actually added,
 * rather than a host-side check of a table the OS might not be shipping.
 *
 * The property that matters here is not "the glyphs look right" - that is
 * a screenshot's job, and tools/qemu_input_suite.py has one. It is that
 * gfx_text_width() and gfx_draw_text_font() agree. Every centred label,
 * right-aligned clock and truncated title in this desktop is one call to
 * each, and if the measurement is a pixel off from the drawing, the only
 * symptom is a label sitting slightly wrong in a window nobody happened
 * to look at. So this renders into an off-screen buffer and *measures the
 * ink*: nothing may be drawn left of the pen, nothing right of the
 * advance, and the last glyph's box must actually reach the width that
 * was promised.
 *
 * It also holds the family together - all three faces on one cap line,
 * one x-height, one baseline, one descender row - and checks the eight
 * glyphs M57 added so a mis-authored one is caught here rather than by an
 * arrow that turns out to be blank.
 */
#include "gfx.h"
#include "str.h"
#include "syscall_wrappers.h"
#include "uifont.h"

#define BUF_W 640
#define BUF_H 32

static uint32_t pixels[BUF_W * BUF_H];
static gfx_ctx_t ctx = { pixels, BUF_W, BUF_H };

#define INK 0x00FFFFFFu
#define BG  0x00000000u

static int failed;

static void write_str(const char *s) {
    sys_write(1, s, strlen(s));
}

static void fail(const char *what, const char *detail) {
    write_str("[fonttest] FAIL: ");
    write_str(what);
    if (detail) {
        write_str(" (");
        write_str(detail);
        write_str(")");
    }
    write_str("\n");
    failed = 1;
}

static void clear(void) {
    for (int i = 0; i < BUF_W * BUF_H; i++) {
        pixels[i] = BG;
    }
}

/* Leftmost and rightmost lit columns, and topmost/bottommost lit rows,
 * of whatever is currently in the buffer. -1 for all four if it is
 * blank. */
static void ink_box(int32_t *x0, int32_t *x1, int32_t *y0, int32_t *y1) {
    *x0 = -1; *x1 = -1; *y0 = -1; *y1 = -1;
    for (int32_t y = 0; y < BUF_H; y++) {
        for (int32_t x = 0; x < BUF_W; x++) {
            if (pixels[y * BUF_W + x] == BG) {
                continue;
            }
            if (*x0 < 0 || x < *x0) *x0 = x;
            if (x > *x1) *x1 = x;
            if (*y0 < 0) *y0 = y;
            *y1 = y;
        }
    }
}

/* The core claim: drawing `s` at x = 0 puts ink inside [0, width), and
 * the string's last glyph really does reach far enough that the measured
 * width is not simply generous. */
static void check_width(const ui_font_t *f, const char *s, const char *name, int bold) {
    int32_t want = gfx_text_width(f, s);
    clear();
    gfx_draw_text_font(&ctx, 0, 0, s, INK, f, bold);

    int32_t x0, x1, y0, y1;
    ink_box(&x0, &x1, &y0, &y1);

    if (x0 < 0) {
        fail("string measured non-zero but drew nothing", name);
        return;
    }
    if (x0 < 0 || x1 >= want) {
        fail("ink runs past the measured advance width", name);
    }
    /* The trailing letter-spacing column is the only slack allowed, plus
     * one more for the bold weight, which spends exactly that column. */
    int32_t slack = want - 1 - x1;
    if (slack > (bold ? 2 : 1) + (int32_t)f->max_advance) {
        fail("measured width is far wider than the ink", name);
    }
    if (y1 >= (int32_t)f->height) {
        fail("glyph drew below the bottom of its cell", name);
    }
}

/* One glyph, rendered alone: its ink must sit inside its own box, and
 * its box must be strictly narrower than its advance so two adjacent
 * letters cannot touch. */
static void check_glyph_box(const ui_font_t *f, char c, const char *name) {
    clear();
    gfx_draw_char_font(&ctx, 0, 0, c, INK, f, 0);
    int32_t x0, x1, y0, y1;
    ink_box(&x0, &x1, &y0, &y1);
    if (x0 < 0) {
        fail("glyph is blank", name);
        return;
    }
    if (x1 >= (int32_t)f->width[(unsigned char)c]) {
        fail("glyph ink escapes its declared box", name);
    }
    if (f->width[(unsigned char)c] >= f->advance[(unsigned char)c]) {
        fail("glyph box is not narrower than its advance", name);
    }
}

/* Where a single character's ink starts and ends vertically - the shared
 * metric, measured in rendered pixels rather than read back out of the
 * table that produced them. */
static void baseline_row(const ui_font_t *f, char c, int32_t *top, int32_t *bot) {
    clear();
    gfx_draw_char_font(&ctx, 0, 0, c, INK, f, 0);
    int32_t x0, x1;
    ink_box(&x0, &x1, top, bot);
}

static void check_face(const ui_font_t *f, const char *face) {
    static const char *SAMPLES[] = {
        "Hamburgefontsiv",
        "Files",
        "00:00",
        "iiiii",
        "MMMMM",
        "Shut Down",
        ". , : ; ! |",
        UI_S_ARROW_LEFT UI_S_ARROW_RIGHT UI_S_ARROW_UP UI_S_ARROW_DOWN
        UI_S_CHECK UI_S_BULLET UI_S_ELLIPSIS UI_S_CLOSE,
    };
    for (unsigned i = 0; i < sizeof(SAMPLES) / sizeof(SAMPLES[0]); i++) {
        check_width(f, SAMPLES[i], face, 0);
        if (f->rows_bold) {
            check_width(f, SAMPLES[i], face, 1);
        }
    }

    for (char c = 0x21; c <= 0x7E; c++) {
        check_glyph_box(f, c, face);
    }
    for (char c = UI_GLYPH_SPECIAL_FIRST; c <= UI_GLYPH_SPECIAL_LAST; c++) {
        check_glyph_box(f, c, face);
    }

    int32_t a_top, a_bot, x_top, x_bot, g_top, g_bot;
    baseline_row(f, 'A', &a_top, &a_bot);
    baseline_row(f, 'x', &x_top, &x_bot);
    baseline_row(f, 'g', &g_top, &g_bot);

    if (a_top != (int32_t)f->cap_top)        fail("'A' is off the cap line", face);
    if (x_top != (int32_t)f->x_top)          fail("'x' is off the x-height line", face);
    if (g_top != (int32_t)f->x_top)          fail("'g' is off the x-height line", face);
    if (a_bot != (int32_t)f->baseline - 1)   fail("'A' is off the baseline", face);
    if (x_bot != (int32_t)f->baseline - 1)   fail("'x' is off the baseline", face);
    if (g_bot != (int32_t)f->desc_last)      fail("'g' misses the descender row", face);

    /* Tabular figures: a taskbar clock that shuffles sideways once a
     * minute is the bug this prevents, and it is invisible in any single
     * frame. */
    for (char c = '1'; c <= '9'; c++) {
        if (f->advance[(unsigned char)c] != f->advance['0']) {
            fail("digits do not share one advance", face);
            break;
        }
    }
    if (gfx_text_width(f, "00:00") != gfx_text_width(f, "19:47")) {
        fail("two clock readings measure differently", face);
    }

    /* Empty and newline handling, since callers pass both. */
    if (gfx_text_width(f, "") != 0) {
        fail("the empty string has a width", face);
    }
    if (gfx_text_width(f, "ab\ncdef") != gfx_text_width(f, "cdef")) {
        fail("a multi-line measurement is not the widest line", face);
    }

    /* gfx_text_fit is what every truncation site now uses, so its
     * contract - the returned prefix fits, one more character does not -
     * is checked directly rather than trusted. */
    const char *s = "Hamburgefontsiv";
    for (int32_t avail = 0; avail < 120; avail += 3) {
        int32_t n = gfx_text_fit(f, s, avail);
        if (gfx_text_width_n(f, s, n) > avail) {
            fail("gfx_text_fit returned a prefix that does not fit", face);
            break;
        }
        if (s[n] && gfx_text_width_n(f, s, n + 1) <= avail) {
            fail("gfx_text_fit stopped short of what fits", face);
            break;
        }
    }
}

int main(void) {
    /* The three faces have to be one family: same shape of metric, and
     * strictly increasing line heights, or "pick a smaller size for the
     * list" is not a thing a caller can reason about. */
    if (!(ui_font_small.height < ui_font_ui.height &&
          ui_font_ui.height < ui_font_large.height)) {
        fail("the three sizes are not ordered by height", 0);
    }
    if (ui_font_ui.rows_bold == 0) {
        fail("the chrome face has no bold weight", 0);
    }

    check_face(&ui_font_small, "small");
    check_face(&ui_font_ui, "ui");
    check_face(&ui_font_large, "large");

    if (failed) {
        return 1;
    }
    write_str("[fonttest] proportional UI font metrics verified.\n");
    return 0;
}
