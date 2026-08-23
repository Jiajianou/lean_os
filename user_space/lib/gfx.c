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

void gfx_draw_char(gfx_ctx_t *ctx, int32_t x, int32_t y, char c, uint32_t color) {
    uint8_t code = (uint8_t)c;
    if (code >= 128) {
        return;
    }
    const uint8_t *glyph = font8x16[code];
    for (int32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int32_t col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                gfx_put_pixel(ctx, x + col, y + row, color);
            }
        }
    }
}

void gfx_draw_text(gfx_ctx_t *ctx, int32_t x, int32_t y, const char *s, uint32_t color) {
    int32_t cx = x;
    for (const char *p = s; *p; p++) {
        if (*p == '\n') {
            cx = x;
            y += FONT_HEIGHT;
            continue;
        }
        gfx_draw_char(ctx, cx, y, *p, color);
        cx += FONT_WIDTH;
    }
}

int gfx_point_in_rect(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

void gfx_draw_button(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t bg_color, uint32_t border_color,
                      const char *label, uint32_t label_color) {
    gfx_fill_rect(ctx, x, y, w, h, bg_color);
    gfx_draw_rect(ctx, x, y, w, h, border_color);
    if (label) {
        int32_t label_w = (int32_t)strlen(label) * FONT_WIDTH;
        int32_t label_x = x + (w - label_w) / 2;
        int32_t label_y = y + (h - FONT_HEIGHT) / 2;
        gfx_draw_text(ctx, label_x, label_y, label, label_color);
    }
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
