#include "gfx.h"

#include "font8x16.h"
#include "str.h"

void gfx_put_pixel(gfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t color) {
    if (x < 0 || y < 0 || x >= ctx->width || y >= ctx->height) {
        return;
    }
    ctx->pixels[y * ctx->width + x] = color;
}

/* Clipped once, then written a row at a time - not gfx_put_pixel per
 * pixel, which re-did four bounds comparisons and a multiply for every
 * one of them. That mattered: desktop_icons.c fills a whole 1024x768
 * window on every redraw, and desktop_shell.c a full-width bar several
 * times a second, so this is the single hottest primitive in user space.
 * A w or h of zero (or negative) still draws nothing - x1 <= x0 or
 * y1 <= y0 simply skips both loops, same as the old bounds check did. */
void gfx_fill_rect(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w > ctx->width ? ctx->width : x + w;
    int32_t y1 = y + h > ctx->height ? ctx->height : y + h;
    for (int32_t row = y0; row < y1; row++) {
        uint32_t *dst = ctx->pixels + row * ctx->width;
        for (int32_t col = x0; col < x1; col++) {
            dst[col] = color;
        }
    }
}

void gfx_draw_rect(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    for (int32_t col = 0; col < w; col++) {
        gfx_put_pixel(ctx, x + col, y, color);
        gfx_put_pixel(ctx, x + col, y + h - 1, color);
    }
    for (int32_t row = 0; row < h; row++) {
        gfx_put_pixel(ctx, x, y + row, color);
        gfx_put_pixel(ctx, x + w - 1, y + row, color);
    }
}

void gfx_draw_line(gfx_ctx_t *ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    int32_t dx = x1 - x0;
    dx = dx < 0 ? -dx : dx;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 - y0;
    dy = dy < 0 ? dy : -dy; /* kept negative, per the classic Bresenham formulation */
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;

    for (;;) {
        gfx_put_pixel(ctx, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* ---- text ---------------------------------------------------------
 *
 * M57. Two fonts with two jobs, and the split is deliberate: the
 * monospace pair at the bottom is M39's 8x16 cell exactly as it was, for
 * the two programs whose column arithmetic is real, and everything above
 * it draws the proportional family with a per-glyph advance. See gfx.h.
 */

/* Defaults to the 16-row face, which is what chrome and labels want.
 * Non-const so a program can pick a size once at startup; nothing
 * changes it mid-frame, and nothing here is re-entrant anyway (a user
 * program in this OS is single-threaded). */
static const ui_font_t *ui_font_current = &ui_font_ui;

void gfx_set_ui_font(const ui_font_t *font) {
    if (font) {
        ui_font_current = font;
    }
}

const ui_font_t *gfx_ui_font(void) {
    return ui_font_current;
}

/* ---- M88: UTF-8, and the box that stands for what this font lacks ----
 *
 * The encoding is UTF-8 everywhere now (see <wchar.h>), and this font is
 * 128 glyphs. Those two facts meet here, and the way they met before this
 * milestone was the worst of the three options: a byte over 0x7F drew
 * nothing and advanced nothing, so a filename with an accent in it did
 * not render as wrong text - it rendered as *shorter* text, silently, and
 * a name that differed only above U+007F was indistinguishable from one
 * that did not.
 *
 * Three things change. The text walkers decode UTF-8 rather than stepping
 * one byte at a time, so a multi-byte character is one character. Anything
 * this font has no glyph for draws as a hollow box, which is the
 * convention every system uses and which says "there is a character here
 * that I cannot show you" rather than saying nothing. And a malformed
 * byte gets the same box and advances exactly one byte, so a decoder
 * cannot be walked off the end of a string by bad input.
 *
 * This is not the glyphs. The font is still ASCII, and a font that covers
 * more is a font project rather than a libc one - see <wchar.h>'s note on
 * where that split is drawn.
 */
#define GFX_REPLACEMENT 0xFFFDu /* what a code point with no glyph becomes */

/* Decodes one character, stores it through `out`, and returns the number
 * of BYTES it consumed - always at least 1, which is the property that
 * makes every loop below terminate. A malformed or truncated sequence
 * yields the replacement code point and consumes a single byte, so the
 * next character is still found if the damage was one bad byte. */
static int32_t utf8_step(const char *p, uint32_t *out) {
    uint8_t b0 = (uint8_t)p[0];
    if (b0 < 0x80u) {
        *out = b0;
        return 1;
    }
    int32_t need;
    uint32_t wc;
    if (b0 >= 0xC2u && b0 < 0xE0u) {
        need = 1;
        wc = b0 & 0x1Fu;
    } else if (b0 >= 0xE0u && b0 < 0xF0u) {
        need = 2;
        wc = b0 & 0x0Fu;
    } else if (b0 >= 0xF0u && b0 < 0xF5u) {
        need = 3;
        wc = b0 & 0x07u;
    } else {
        *out = GFX_REPLACEMENT; /* a continuation byte with nothing to continue, or an overlong lead */
        return 1;
    }
    for (int32_t i = 1; i <= need; i++) {
        uint8_t b = (uint8_t)p[i];
        if ((b & 0xC0u) != 0x80u) {
            *out = GFX_REPLACEMENT; /* truncated - including by the string's own NUL */
            return 1;
        }
        wc = (wc << 6) | (b & 0x3Fu);
    }
    *out = wc;
    return need + 1;
}

/* The box is as wide as a digit and as tall as the font's ascent, which
 * makes a run of them line up with the text around them. Digits are the
 * one class of glyph a proportional font keeps at a single width, which
 * is why '0' is the measurement rather than 'M' or an average. */
static int32_t box_advance(const ui_font_t *font) {
    return font->advance['0'];
}

int32_t gfx_glyph_advance(const ui_font_t *font, uint32_t cp) {
    if (cp >= 128) {
        return box_advance(font);
    }
    return font->advance[cp];
}

int32_t gfx_char_advance(const ui_font_t *font, char c) {
    uint8_t code = (uint8_t)c;
    if (code >= 128) {
        /* A single byte out of context: it is either part of a sequence
         * this caller is walking one byte at a time, or it is malformed.
         * Either way it is the box's width, which is what the decoding
         * walkers above would charge for it. */
        return box_advance(font);
    }
    return font->advance[code];
}

int32_t gfx_text_width_n(const ui_font_t *font, const char *s, int32_t n) {
    int32_t w = 0, widest = 0;
    for (int32_t i = 0; i < n && s[i];) {
        if (s[i] == '\n') {
            if (w > widest) {
                widest = w;
            }
            w = 0;
            i++;
            continue;
        }
        uint32_t cp;
        int32_t used = utf8_step(s + i, &cp);
        /* `n` counts BYTES, which is what every caller of this has always
         * passed it. A sequence that would run past the limit is charged
         * as one box rather than being decoded out of bounds. */
        if (i + used > n) {
            w += box_advance(font);
            break;
        }
        i += used;
        w += gfx_glyph_advance(font, cp);
    }
    return w > widest ? w : widest;
}

int32_t gfx_text_width(const ui_font_t *font, const char *s) {
    return gfx_text_width_n(font, s, (int32_t)strlen(s));
}

/* Returns a BYTE count, and one that never lands inside a character -
 * every caller uses it to truncate a string, and a cut through the middle
 * of a sequence would produce bytes that are not text. */
int32_t gfx_text_fit(const ui_font_t *font, const char *s, int32_t max_w) {
    int32_t w = 0;
    int32_t i = 0;
    while (s[i]) {
        uint32_t cp;
        int32_t used = utf8_step(s + i, &cp);
        int32_t adv = gfx_glyph_advance(font, cp);
        if (w + adv > max_w) {
            break;
        }
        w += adv;
        i += used;
    }
    return i;
}

void gfx_text_measure(const ui_font_t *font, const char *s, gfx_text_metrics_t *out) {
    out->width   = gfx_text_width(font, s);
    out->height  = font->height;
    out->ascent  = font->baseline;
    out->descent = font->height - font->baseline;
}

/* The hollow box, drawn where a glyph would have gone. Inset by a pixel
 * on each side so two adjacent boxes read as two characters rather than
 * as one grid. */
static void draw_box(gfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t color,
                     const ui_font_t *font) {
    int32_t w = box_advance(font) - 1;
    int32_t top = font->height - font->baseline;
    int32_t h = font->baseline - 1;
    if (w < 3 || h < 3) {
        return;
    }
    for (int32_t col = 0; col < w; col++) {
        gfx_put_pixel(ctx, x + col, y + top, color);
        gfx_put_pixel(ctx, x + col, y + top + h - 1, color);
    }
    for (int32_t row = 1; row < h - 1; row++) {
        gfx_put_pixel(ctx, x, y + top + row, color);
        gfx_put_pixel(ctx, x + w - 1, y + top + row, color);
    }
}

/* M88: the code-point form. gfx_draw_char_font below is this with a
 * single byte widened, which is what every caller that draws one
 * character at a time is really doing. */
void gfx_draw_glyph_font(gfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t cp, uint32_t color,
                          const ui_font_t *font, int bold) {
    if (cp >= 128) {
        draw_box(ctx, x, y, color, font);
        return;
    }
    uint8_t code = (uint8_t)cp;
    const uint16_t *rows = (bold && font->rows_bold) ? font->rows_bold : font->rows;
    const uint16_t *glyph = rows + (int32_t)code * font->height;
    /* The bold weight is a one-column dilation (tools/gen-font.c), so it
     * reaches one column past the regular glyph's box - into the single
     * column of letter spacing the advance provides, which is exactly
     * what bold is meant to spend. */
    int32_t w = font->width[code] + ((bold && font->rows_bold) ? 1 : 0);
    if (w > UI_FONT_MAX_COLS) {
        w = UI_FONT_MAX_COLS;
    }
    for (int32_t row = 0; row < font->height; row++) {
        uint16_t bits = glyph[row];
        if (!bits) {
            continue;
        }
        for (int32_t col = 0; col < w; col++) {
            if (bits & (uint16_t)(0x8000u >> col)) {
                gfx_put_pixel(ctx, x + col, y + row, color);
            }
        }
    }
}

void gfx_draw_char_font(gfx_ctx_t *ctx, int32_t x, int32_t y, char c, uint32_t color,
                         const ui_font_t *font, int bold) {
    gfx_draw_glyph_font(ctx, x, y, (uint8_t)c, color, font, bold);
}

void gfx_draw_text_font(gfx_ctx_t *ctx, int32_t x, int32_t y, const char *s, uint32_t color,
                         const ui_font_t *font, int bold) {
    int32_t cx = x;
    for (const char *p = s; *p;) {
        if (*p == '\n') {
            cx = x;
            y += font->height;
            p++;
            continue;
        }
        uint32_t cp;
        p += utf8_step(p, &cp);
        gfx_draw_glyph_font(ctx, cx, y, cp, color, font, bold);
        cx += gfx_glyph_advance(font, cp);
    }
}

void gfx_draw_char(gfx_ctx_t *ctx, int32_t x, int32_t y, char c, uint32_t color) {
    gfx_draw_char_font(ctx, x, y, c, color, ui_font_current, 0);
}

void gfx_draw_text(gfx_ctx_t *ctx, int32_t x, int32_t y, const char *s, uint32_t color) {
    gfx_draw_text_font(ctx, x, y, s, color, ui_font_current, 0);
}

/* The mono font's box. One cell wide, so a terminal's grid survives -
 * which is the whole reason the mono path is separate from the
 * proportional one. */
static void draw_box_mono(gfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t color) {
    for (int32_t col = 1; col < FONT_WIDTH - 1; col++) {
        gfx_put_pixel(ctx, x + col, y + 2, color);
        gfx_put_pixel(ctx, x + col, y + FONT_HEIGHT - 3, color);
    }
    for (int32_t row = 3; row < FONT_HEIGHT - 3; row++) {
        gfx_put_pixel(ctx, x + 1, y + row, color);
        gfx_put_pixel(ctx, x + FONT_WIDTH - 2, y + row, color);
    }
}

void gfx_draw_glyph_mono(gfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t cp, uint32_t color) {
    if (cp >= 128) {
        draw_box_mono(ctx, x, y, color);
        return;
    }
    const uint8_t *glyph = font8x16[cp];
    for (int32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int32_t col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                gfx_put_pixel(ctx, x + col, y + row, color);
            }
        }
    }
}

void gfx_draw_char_mono(gfx_ctx_t *ctx, int32_t x, int32_t y, char c, uint32_t color) {
    gfx_draw_glyph_mono(ctx, x, y, (uint8_t)c, color);
}

void gfx_draw_text_mono(gfx_ctx_t *ctx, int32_t x, int32_t y, const char *s, uint32_t color) {
    int32_t cx = x;
    for (const char *p = s; *p;) {
        if (*p == '\n') {
            cx = x;
            y += FONT_HEIGHT;
            p++;
            continue;
        }
        uint32_t cp;
        p += utf8_step(p, &cp);
        gfx_draw_glyph_mono(ctx, cx, y, cp, color);
        cx += FONT_WIDTH;
    }
}

int gfx_point_in_rect(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* M46: two thirds of each channel - dark enough to read as "held" at a
 * glance, shallow enough that a button's own color still identifies it
 * (task_manager.c's Force Quit stays visibly red while pressed). Integer,
 * per channel, the same arithmetic every blend in this project uses. */
static uint32_t darken(uint32_t color) {
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        out |= (((color >> shift) & 0xFF) * 2 / 3) << shift;
    }
    return out;
}

void gfx_draw_button_state(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h,
                            uint32_t bg_color, uint32_t border_color,
                            const char *label, uint32_t label_color, int pressed) {
    gfx_fill_rect(ctx, x, y, w, h, pressed ? darken(bg_color) : bg_color);
    gfx_draw_rect(ctx, x, y, w, h, border_color);
    if (label) {
        const ui_font_t *font = ui_font_current;
        int32_t label_w = gfx_text_width(font, label);
        int32_t label_x = x + (w - label_w) / 2;
        int32_t label_y = y + (h - font->height) / 2;
        /* One pixel down and right while held - the oldest "the surface
         * moved under your finger" cue there is, and the only one
         * available without a second border color. */
        gfx_draw_text(ctx, label_x + (pressed ? 1 : 0), label_y + (pressed ? 1 : 0), label, label_color);
    }
}

void gfx_draw_button(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t bg_color, uint32_t border_color,
                      const char *label, uint32_t label_color) {
    gfx_draw_button_state(ctx, x, y, w, h, bg_color, border_color, label, label_color, 0);
}

void gfx_draw_menu(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t item_w, int32_t item_h,
                    const char *const *items, int count, int hover_index,
                    uint32_t bg_color, uint32_t hover_bg_color, uint32_t border_color, uint32_t text_color) {
    for (int i = 0; i < count; i++) {
        gfx_draw_button(ctx, x, y + i * item_h, item_w, item_h,
                         i == hover_index ? hover_bg_color : bg_color, border_color,
                         items[i], text_color);
    }
}

int gfx_menu_hit_test(int32_t px, int32_t py, int32_t x, int32_t y, int32_t item_w, int32_t item_h, int count) {
    if (!gfx_point_in_rect(px, py, x, y, item_w, item_h * count)) {
        return -1;
    }
    return (py - y) / item_h;
}

void gfx_draw_scrollbar(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h,
                         int32_t total_items, int32_t visible_items, int32_t scroll_top,
                         uint32_t track_color, uint32_t thumb_color) {
    gfx_fill_rect(ctx, x, y, w, h, track_color);
    if (total_items <= visible_items || total_items <= 0) {
        gfx_fill_rect(ctx, x, y, w, h, thumb_color);
        return;
    }
    int32_t thumb_h = h * visible_items / total_items;
    if (thumb_h < 4) {
        thumb_h = 4;
    }
    int32_t max_top = total_items - visible_items;
    int32_t thumb_y = y + (h - thumb_h) * scroll_top / max_top;
    gfx_fill_rect(ctx, x, thumb_y, w, thumb_h, thumb_color);
}

/* {2, 1, 0, 0} is the integer circle of radius GFX_CORNER_R evaluated at
 * half-pixel centers: a pixel is in the corner if (2dx+1)^2 + (2dy+1)^2
 * <= (2r)^2, with everything doubled to stay integer. A table rather
 * than a runtime square root - four numbers are easier to check by eye
 * than the code that would generate them, and the radius is a design
 * constant, not a parameter. */
static const int32_t CORNER_INSET[GFX_CORNER_R] = {2, 1, 0, 0};

int32_t gfx_corner_inset(int32_t row_from_edge) {
    if (row_from_edge < 0 || row_from_edge >= GFX_CORNER_R) {
        return 0;
    }
    return CORNER_INSET[row_from_edge];
}

/* M46: the same idea one size up - the horizontal inset of each row of a
 * GFX_CIRCLE_D-diameter disc, top half only (the bottom mirrors it).
 * Evaluated at half-pixel centers from the circle equation with r = 7:
 * row k's center is (k - 6.5) from the middle, and the inset is
 * 7 - floor(sqrt(49 - (k - 6.5)^2)). Tabulated for the same reason
 * CORNER_INSET is: seven numbers are easier to check by eye than the
 * integer-sqrt loop that would produce them, and the diameter is a design
 * constant rather than a parameter. */
static const int32_t CIRCLE_INSET[GFX_CIRCLE_D / 2] = {4, 3, 2, 1, 0, 0, 0};

int32_t gfx_circle_inset(int32_t row_from_edge) {
    if (row_from_edge < 0) {
        return GFX_CIRCLE_D / 2;
    }
    if (row_from_edge >= GFX_CIRCLE_D / 2) {
        row_from_edge = GFX_CIRCLE_D - 1 - row_from_edge; /* mirror the bottom half onto the top */
    }
    if (row_from_edge < 0 || row_from_edge >= GFX_CIRCLE_D / 2) {
        return GFX_CIRCLE_D / 2;
    }
    return CIRCLE_INSET[row_from_edge];
}

/* How much row `row` of an h-tall rect is inset - nonzero only within
 * GFX_CORNER_R of either end, and measured from whichever end is
 * nearer. */
static int32_t row_inset(int32_t row, int32_t h) {
    if (row < GFX_CORNER_R) {
        return gfx_corner_inset(row);
    }
    if (row >= h - GFX_CORNER_R) {
        return gfx_corner_inset(h - 1 - row);
    }
    return 0;
}

void gfx_fill_rect_rounded(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (w < 2 * GFX_CORNER_R || h < 2 * GFX_CORNER_R) {
        gfx_fill_rect(ctx, x, y, w, h, color);
        return;
    }
    for (int32_t row = 0; row < h; row++) {
        int32_t inset = row_inset(row, h);
        gfx_fill_rect(ctx, x + inset, y + row, w - 2 * inset, 1, color);
    }
}

/* How wide a horizontal run this row's outline needs at each end: enough
 * to close the gap left by whichever vertical neighbour is further inset
 * than this row is, so the corner reads as a continuous edge instead of a
 * ladder of disconnected dots. A row with no neighbour above or below
 * (the top and bottom rows) reaches the full width and so draws a solid
 * span; every ordinary middle row reaches nothing and draws a single
 * pixel per side.
 *
 * Getting this wrong is not subtle-looking, but it *is* subtle to spot in
 * a screenshot: the first version drew the whole span on any row where
 * the inset changed, which turned the top two rows of every taskbar
 * button into a solid bar of border color. A pixel self-test probing the
 * button's fill caught it. */
static int32_t outline_run(int32_t row, int32_t w, int32_t h) {
    int32_t inset = row_inset(row, h);
    int32_t above = (row == 0) ? w : row_inset(row - 1, h);
    int32_t below = (row == h - 1) ? w : row_inset(row + 1, h);
    int32_t reach = above > below ? above : below;
    if (reach > w - inset) {
        reach = w - inset;
    }
    int32_t run = reach - inset;
    return run < 1 ? 1 : run;
}

void gfx_draw_rect_rounded(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (w < 2 * GFX_CORNER_R || h < 2 * GFX_CORNER_R) {
        gfx_draw_rect(ctx, x, y, w, h, color);
        return;
    }
    for (int32_t row = 0; row < h; row++) {
        int32_t inset = row_inset(row, h);
        int32_t run = outline_run(row, w, h);
        gfx_fill_rect(ctx, x + inset, y + row, run, 1, color);
        gfx_fill_rect(ctx, x + w - inset - run, y + row, run, 1, color);
    }
}
