#pragma once

#include <stdint.h>

#include "uifont.h"

typedef struct {
    uint32_t *pixels;
    int32_t width;
    int32_t height;
} gfx_ctx_t;

void gfx_put_pixel(gfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t color);
void gfx_fill_rect(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gfx_draw_rect(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gfx_draw_line(gfx_ctx_t *ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);

typedef struct {
    int32_t width;
    int32_t height;
    int32_t ascent;
    int32_t descent;
} gfx_text_metrics_t;

void gfx_set_ui_font(const ui_font_t *font);
const ui_font_t *gfx_ui_font(void);

int32_t gfx_text_width(const ui_font_t *font, const char *s);
int32_t gfx_text_width_n(const ui_font_t *font, const char *s, int32_t n);
int32_t gfx_char_advance(const ui_font_t *font, char c);

int32_t gfx_glyph_advance(const ui_font_t *font, uint32_t cp);
void gfx_draw_glyph_font(gfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t cp, uint32_t color,
                          const ui_font_t *font, int bold);
void gfx_draw_glyph_mono(gfx_ctx_t *ctx, int32_t x, int32_t y, uint32_t cp, uint32_t color);
int32_t gfx_text_fit(const ui_font_t *font, const char *s, int32_t max_w);
void gfx_text_measure(const ui_font_t *font, const char *s, gfx_text_metrics_t *out);

void gfx_draw_char_font(gfx_ctx_t *ctx, int32_t x, int32_t y, char c, uint32_t color,
                         const ui_font_t *font, int bold);
void gfx_draw_text_font(gfx_ctx_t *ctx, int32_t x, int32_t y, const char *s, uint32_t color,
                         const ui_font_t *font, int bold);

void gfx_draw_char(gfx_ctx_t *ctx, int32_t x, int32_t y, char c, uint32_t color);
void gfx_draw_text(gfx_ctx_t *ctx, int32_t x, int32_t y, const char *s, uint32_t color);

void gfx_draw_char_mono(gfx_ctx_t *ctx, int32_t x, int32_t y, char c, uint32_t color);
void gfx_draw_text_mono(gfx_ctx_t *ctx, int32_t x, int32_t y, const char *s, uint32_t color);

int gfx_point_in_rect(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h);

void gfx_draw_button(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t bg_color, uint32_t border_color,
                      const char *label, uint32_t label_color);

void gfx_draw_menu(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t item_w, int32_t item_h,
                    const char *const *items, int count, int hover_index,
                    uint32_t bg_color, uint32_t hover_bg_color, uint32_t border_color, uint32_t text_color);

int gfx_menu_hit_test(int32_t px, int32_t py, int32_t x, int32_t y, int32_t item_w, int32_t item_h, int count);

void gfx_draw_scrollbar(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h,
                         int32_t total_items, int32_t visible_items, int32_t scroll_top,
                         uint32_t track_color, uint32_t thumb_color);

#define GFX_CORNER_R 4
int32_t gfx_corner_inset(int32_t row_from_edge);

#define GFX_PAD 12

#define GFX_CIRCLE_D 14
int32_t gfx_circle_inset(int32_t row_from_edge);

void gfx_fill_rect_rounded(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

void gfx_draw_rect_rounded(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

void gfx_draw_button_state(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h,
                            uint32_t bg_color, uint32_t border_color,
                            const char *label, uint32_t label_color, int pressed);
