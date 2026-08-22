/* user_space/lib/gfx.h
 *
 * M21: `user_space/lib` drawing API targeting an app's own window
 * buffer - the piece M20's compositor/wm_demo didn't have yet (wm_demo
 * wrote raw pixels by hand, one-off, since it only ever drew two flat
 * rectangles). Every function here operates on a gfx_ctx_t the caller
 * supplies (a window's shm-mapped pixel buffer plus its width/height,
 * from wmclient.h's wm_window_t) - this library has no idea a
 * compositor or the framebuffer exist at all, the same separation
 * kernel/drivers/fb.c (physical framebuffer) and console.c (text over
 * it) already keep on the kernel side.
 */
#pragma once

#include <stdint.h>

typedef struct {
    uint32_t *pixels; /* tightly packed, row-major, `width` pixels per row - matches how compositor.c sizes a window's shm segment (width * height * 4 bytes) */
    int32_t width;
    int32_t height;
} gfx_ctx_t;

void gfx_put_pixel(gfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t color);
void gfx_fill_rect(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
/* One-pixel-wide outline, not a filled rect. */
void gfx_draw_rect(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
/* Bresenham's integer line algorithm - no floating point anywhere in this
 * kernel/toolchain (SSE/FPU state is never set up, see the Makefile's
 * -mgeneral-regs-only comment), so this is the only kind of line drawing
 * that was ever an option here. */
void gfx_draw_line(gfx_ctx_t *ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
void gfx_draw_char(gfx_ctx_t *ctx, int32_t x, int32_t y, char c, uint32_t color);
void gfx_draw_text(gfx_ctx_t *ctx, int32_t x, int32_t y, const char *s, uint32_t color);
