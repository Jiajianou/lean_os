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
    int32_t slack = want - 1 - x1;
    if (slack > (bold ? 2 : 1) + (int32_t)f->max_advance) {
        fail("measured width is far wider than the ink", name);
    }
    if (y1 >= (int32_t)f->height) {
        fail("glyph drew below the bottom of its cell", name);
    }
}

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

    for (char c = '1'; c <= '9'; c++) {
        if (f->advance[(unsigned char)c] != f->advance['0']) {
            fail("digits do not share one advance", face);
            break;
        }
    }
    if (gfx_text_width(f, "00:00") != gfx_text_width(f, "19:47")) {
        fail("two clock readings measure differently", face);
    }

    if (gfx_text_width(f, "") != 0) {
        fail("the empty string has a width", face);
    }
    if (gfx_text_width(f, "ab\ncdef") != gfx_text_width(f, "cdef")) {
        fail("a multi-line measurement is not the widest line", face);
    }

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
