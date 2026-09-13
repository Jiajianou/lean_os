#include "graphics.h"

#include "font8x16.h"
#include "string_utilities.h"

void gfx_put_pixel(gfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t color) {
    if (x < 0 || y < 0 || x >= ctx->width || y >= ctx->height) {
        return;
    }
    ctx->pixels[y * ctx->width + x] = color;
}

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
    dy = dy < 0 ? dy : -dy;
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

static const ui_font_t *ui_font_current = &ui_font_ui;

void gfx_set_ui_font(const ui_font_t *font) {
    if (font) {
        ui_font_current = font;
    }
}

const ui_font_t *gfx_ui_font(void) {
    return ui_font_current;
}

#define GFX_REPLACEMENT 0xFFFDu

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
        *out = GFX_REPLACEMENT;
        return 1;
    }
    for (int32_t i = 1; i <= need; i++) {
        uint8_t b = (uint8_t)p[i];
        if ((b & 0xC0u) != 0x80u) {
            *out = GFX_REPLACEMENT;
            return 1;
        }
        wc = (wc << 6) | (b & 0x3Fu);
    }
    *out = wc;
    return need + 1;
}

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

void gfx_draw_glyph_font(gfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t cp, uint32_t color,
                          const ui_font_t *font, int bold) {
    if (cp >= 128) {
        draw_box(ctx, x, y, color, font);
        return;
    }
    uint8_t code = (uint8_t)cp;
    const uint16_t *rows = (bold && font->rows_bold) ? font->rows_bold : font->rows;
    const uint16_t *glyph = rows + (int32_t)code * font->height;
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

static const int32_t CORNER_INSET[GFX_CORNER_R] = {2, 1, 0, 0};

int32_t gfx_corner_inset(int32_t row_from_edge) {
    if (row_from_edge < 0 || row_from_edge >= GFX_CORNER_R) {
        return 0;
    }
    return CORNER_INSET[row_from_edge];
}

static const int32_t CIRCLE_INSET[GFX_CIRCLE_D / 2] = {4, 3, 2, 1, 0, 0, 0};

int32_t gfx_circle_inset(int32_t row_from_edge) {
    if (row_from_edge < 0) {
        return GFX_CIRCLE_D / 2;
    }
    if (row_from_edge >= GFX_CIRCLE_D / 2) {
        row_from_edge = GFX_CIRCLE_D - 1 - row_from_edge;
    }
    if (row_from_edge < 0 || row_from_edge >= GFX_CIRCLE_D / 2) {
        return GFX_CIRCLE_D / 2;
    }
    return CIRCLE_INSET[row_from_edge];
}

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
