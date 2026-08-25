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

#include "uifont.h"

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
/* ---- text (M57) ---------------------------------------------------
 *
 * Until M57 there was one font at one size and text drawing advanced a
 * compile-time eight pixels per character, so every caller that needed
 * to centre or right-align a label wrote `strlen(s) * FONT_WIDTH`. That
 * idiom is gone from this tree, and it has to stay gone: with a
 * per-glyph advance it is not an approximation, it is simply a different
 * number, and the label lands somewhere else.
 *
 * Measure with gfx_text_width(). Everything here takes an explicit
 * ui_font_t (uifont.h) except the plain gfx_draw_text/gfx_draw_char
 * pair, which use the process's current UI font - see gfx_set_ui_font.
 */

/* The shape a caller needs to lay text out against something else: the
 * advance width of the whole string, the font's line height, and where
 * the baseline sits inside it (ascent + descent == height). */
typedef struct {
    int32_t width;
    int32_t height;
    int32_t ascent;   /* top of the cell down to the baseline */
    int32_t descent;  /* baseline down to the bottom of the cell */
} gfx_text_metrics_t;

/* The font gfx_draw_text, gfx_draw_button and gfx_draw_menu use. Per
 * process, set once at startup (file_manager.c and task_manager.c pick
 * ui_font_small for their list rows); defaults to ui_font_ui. Deliberate
 * hidden state, and the reason it is worth it: the alternative is
 * threading a font pointer through every helper in this header for a
 * value that is constant for the life of a program. */
void gfx_set_ui_font(const ui_font_t *font);
const ui_font_t *gfx_ui_font(void);

/* Advance width of `s` - the exact number of pixels gfx_draw_text_font
 * will move the pen. Counts the widest line if `s` contains newlines. */
int32_t gfx_text_width(const ui_font_t *font, const char *s);
/* The same for the first `n` characters, for callers laying out a
 * substring (a cursor position inside an edit field, say). */
int32_t gfx_text_width_n(const ui_font_t *font, const char *s, int32_t n);
int32_t gfx_char_advance(const ui_font_t *font, char c);
/* How many leading characters of `s` fit in `max_w` pixels - what every
 * "truncate this to the space I have" site needs now that it cannot
 * divide by a constant. */
int32_t gfx_text_fit(const ui_font_t *font, const char *s, int32_t max_w);
void gfx_text_measure(const ui_font_t *font, const char *s, gfx_text_metrics_t *out);

void gfx_draw_char_font(gfx_ctx_t *ctx, int32_t x, int32_t y, char c, uint32_t color,
                         const ui_font_t *font, int bold);
void gfx_draw_text_font(gfx_ctx_t *ctx, int32_t x, int32_t y, const char *s, uint32_t color,
                         const ui_font_t *font, int bold);

/* The process's current UI font, regular weight. */
void gfx_draw_char(gfx_ctx_t *ctx, int32_t x, int32_t y, char c, uint32_t color);
void gfx_draw_text(gfx_ctx_t *ctx, int32_t x, int32_t y, const char *s, uint32_t color);

/* The 8x16 monospace cell (font8x16.h), unchanged since M39 and staying
 * that way: gui_terminal.c's character grid and text_editor.c's column
 * arithmetic both genuinely depend on a fixed advance, and a proportional
 * font underneath either of them would not be an improvement, it would be
 * a bug. Everything *around* the grid in those two programs - their
 * menus, dialogs and status lines - is ordinary UI text and uses the
 * proportional pair above. */
void gfx_draw_char_mono(gfx_ctx_t *ctx, int32_t x, int32_t y, char c, uint32_t color);
void gfx_draw_text_mono(gfx_ctx_t *ctx, int32_t x, int32_t y, const char *s, uint32_t color);

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

/* M46: one circle shape for the whole desktop, in exactly the form M44
 * settled on for rounded corners and for the same reason - compositor.c
 * draws its titlebar buttons through its own clip-rect-aware put_pixel
 * (see gfx_point_in_rect's note) and cannot use a gfx.c drawing
 * primitive, so two *implementations* are unavoidable. Two *tables* are
 * not, and M44's note about them eventually disagreeing is why this is
 * exported rather than kept private to whoever draws first.
 *
 * gfx_circle_inset is how much narrower row `row_from_center_edge` (0 is
 * the topmost row of a GFX_CIRCLE_D-diameter circle, valid up to
 * GFX_CIRCLE_D/2 - 1) is at each end; the bottom half mirrors it. Derived
 * once from the circle equation and tabulated, since this project has no
 * floating point (see gfx_draw_line's note) and a per-pixel integer
 * sqrt for a 14px button would be worse than seven numbers.
 *
 * Deliberately no gfx_fill_circle here yet: nothing drawing into an app's
 * own window wants a circle, and the shared thing that mattered was the
 * table. */
#define GFX_CIRCLE_D 14
int32_t gfx_circle_inset(int32_t row_from_edge);

/* A filled rect with GFX_CORNER_R-rounded corners. Falls back to a plain
 * fill when the rect is too small to round without eating itself. */
void gfx_fill_rect_rounded(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

/* The 1px outline companion, same corners. */
void gfx_draw_rect_rounded(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

/* M46: gfx_draw_button with a real pressed look - a darkened fill and the
 * label nudged one pixel down and right, the standard "this is being
 * held" cue. Until this milestone a button in this project reacted only
 * once the click had already been acted on, which is the one moment
 * feedback is no longer useful. gfx_draw_button is this with pressed = 0
 * (and stays the name every existing call site uses, since most buttons
 * are drawn from a redraw that has no idea what the mouse is doing). */
void gfx_draw_button_state(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h,
                            uint32_t bg_color, uint32_t border_color,
                            const char *label, uint32_t label_color, int pressed);
