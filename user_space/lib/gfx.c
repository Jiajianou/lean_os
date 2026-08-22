#include "gfx.h"

#include "font8x16.h"

void gfx_put_pixel(gfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t color) {
    if (x < 0 || y < 0 || x >= ctx->width || y >= ctx->height) {
        return;
    }
    ctx->pixels[y * ctx->width + x] = color;
}

void gfx_fill_rect(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    for (int32_t row = 0; row < h; row++) {
        for (int32_t col = 0; col < w; col++) {
            gfx_put_pixel(ctx, x + col, y + row, color);
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
