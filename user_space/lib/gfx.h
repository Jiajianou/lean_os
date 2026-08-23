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

/* M34: pure geometry, no ctx needed - the same "is this point inside this
 * rect" test compositor.c, settings.c, and desktop_icons.c each used to
 * hand-roll their own (identical) copy of. Safe for compositor.c to adopt
 * for hit-testing (this is O(1), not a per-pixel draw loop) even though
 * compositor.c's own *drawing* stays on its own clip-rect-aware fill_rect
 * for the partial-redraw performance reasons documented there - only the
 * comparison logic was ever duplicated for no reason. */
int gfx_point_in_rect(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h);

/* M34: a filled rect + 1px border + centered label - the shape every
 * hand-rolled "button" in this codebase (settings.c's Clear button was
 * the first) already drew by hand, one gfx_fill_rect/gfx_draw_text pair
 * at a time with the label's x offset picked by eye. */
void gfx_draw_button(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t bg_color, uint32_t border_color,
                      const char *label, uint32_t label_color);

/* M35: a vertical stack of `count` gfx_draw_button-drawn rows, each
 * item_w x item_h, top-left at (x, y) - the dropdown/context-menu shape
 * both text_editor.c's File menu and desktop_icons.c's right-click menu
 * need. hover_index (or -1 for none) draws that one row in hover_bg
 * instead of bg. Two real, simultaneous consumers landing in the same
 * milestone is what earns this its own helper rather than each caller
 * looping over gfx_draw_button by hand. */
void gfx_draw_menu(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t item_w, int32_t item_h,
                    const char *const *items, int count, int hover_index,
                    uint32_t bg_color, uint32_t hover_bg_color, uint32_t border_color, uint32_t text_color);

/* Which item index (0..count-1) contains (px, py), or -1 if none - the
 * hit-test companion to gfx_draw_menu, same rect math. */
int gfx_menu_hit_test(int32_t px, int32_t py, int32_t x, int32_t y, int32_t item_w, int32_t item_h, int count);

/* M37: a track (filled x,y,w,h) plus a thumb sized/positioned by
 * visible_items/total_items and scroll_top - the on-screen position/
 * extent indicator file_manager.c's file list previously had no visual
 * cue for at all (it scrolled, but nothing on screen showed there was
 * more above/below). total_items <= visible_items draws a full-track
 * thumb (nothing to scroll). Draw-only, no hit-test companion yet - this
 * milestone only promises a visible indicator, not thumb-drag scrolling. */
void gfx_draw_scrollbar(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h,
                         int32_t total_items, int32_t visible_items, int32_t scroll_top,
                         uint32_t track_color, uint32_t thumb_color);

/* M44: one corner radius for every rounded surface on this desktop -
 * taskbar buttons, the launcher overlay and its selection, the editor's
 * dialogs - so "rounded" means the same thing everywhere instead of each
 * caller picking a number by eye. Small on purpose: at 8x16 glyphs and
 * 20-24px controls, anything larger eats the corners of the content.
 *
 * gfx_corner_inset is how much narrower row `row_from_edge` (0 is the
 * outermost row, valid up to GFX_CORNER_R-1) is at each end. It is
 * exported rather than kept private because compositor.c draws its own
 * rounded rects - it has to, its primitives are clip-rect-aware and
 * gfx.c's aren't (see gfx_point_in_rect's note) - and the two must round
 * identically or the launcher would not match the taskbar. */
#define GFX_CORNER_R 4
int32_t gfx_corner_inset(int32_t row_from_edge);

/* M44: and one inset from a panel or dialog's edge to its content, for
 * the same reason - the launcher, the editor's prompts and the settings
 * window each had their own number (12, 8 and 10) picked independently,
 * which is exactly the sort of thing nobody notices individually and
 * everybody notices together. Deliberately *not* used by desktop_shell.c:
 * a bar packed with buttons is a denser surface than a dialog, and its
 * own 4/8 margins are a considered choice rather than an oversight. */
#define GFX_PAD 12

/* A filled rect with GFX_CORNER_R-rounded corners. Falls back to a plain
 * fill when the rect is too small to round without eating itself. */
void gfx_fill_rect_rounded(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

/* The 1px outline companion, same corners. */
void gfx_draw_rect_rounded(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
