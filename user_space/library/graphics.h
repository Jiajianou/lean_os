#pragma once

#include <stdint.h>

#include "user_interface_font.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t *pixels;
    int32_t width;
    int32_t height;
} graphics_context_t;

void graphics_put_pixel(graphics_context_t *context, int32_t x, int32_t y, uint32_t color);
#define GRAPHICS_OVER_255(v) (((v) + 128u + (((v) + 128u) >> 8)) >> 8)

void graphics_blend_pixel(graphics_context_t *context, int32_t x, int32_t y, uint32_t color, uint32_t alpha);
void graphics_fill_rect(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void graphics_draw_rect(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void graphics_draw_line(graphics_context_t *context, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);

typedef struct {
    int32_t width;
    int32_t height;
    int32_t ascent;
    int32_t descent;
} graphics_text_metrics_t;

void graphics_set_ui_font(const ui_font_t *font);
const ui_font_t *graphics_ui_font(void);

int32_t graphics_text_width(const ui_font_t *font, const char *s);
int32_t graphics_text_width_n(const ui_font_t *font, const char *s, int32_t n);
int32_t graphics_char_advance(const ui_font_t *font, char c);

int32_t graphics_glyph_advance(const ui_font_t *font, uint32_t cp);
void graphics_draw_glyph_font(graphics_context_t *context, int32_t x, int32_t y, uint32_t cp, uint32_t color,
                          const ui_font_t *font, int bold);
void graphics_draw_glyph_mono(graphics_context_t *context, int32_t x, int32_t y, uint32_t cp, uint32_t color);
int32_t graphics_text_fit(const ui_font_t *font, const char *s, int32_t max_w);
void graphics_text_measure(const ui_font_t *font, const char *s, graphics_text_metrics_t *out);

void graphics_draw_char_font(graphics_context_t *context, int32_t x, int32_t y, char c, uint32_t color,
                         const ui_font_t *font, int bold);
void graphics_draw_text_font(graphics_context_t *context, int32_t x, int32_t y, const char *s, uint32_t color,
                         const ui_font_t *font, int bold);

void graphics_draw_char(graphics_context_t *context, int32_t x, int32_t y, char c, uint32_t color);
void graphics_draw_text(graphics_context_t *context, int32_t x, int32_t y, const char *s, uint32_t color);

void graphics_draw_char_mono(graphics_context_t *context, int32_t x, int32_t y, char c, uint32_t color);
void graphics_draw_text_mono(graphics_context_t *context, int32_t x, int32_t y, const char *s, uint32_t color);

int graphics_point_in_rect(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h);

void graphics_draw_button(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t bg_color, uint32_t border_color,
                      const char *label, uint32_t label_color);

void graphics_draw_menu(graphics_context_t *context, int32_t x, int32_t y, int32_t item_w, int32_t item_h,
                    const char *const *items, int count, int hover_index,
                    uint32_t bg_color, uint32_t hover_bg_color, uint32_t border_color, uint32_t text_color);

int graphics_menu_hit_test(int32_t px, int32_t py, int32_t x, int32_t y, int32_t item_w, int32_t item_h, int count);

void graphics_draw_scrollbar(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h,
                         int32_t total_items, int32_t visible_items, int32_t scroll_top,
                         uint32_t track_color, uint32_t thumb_color);

#define GRAPHICS_CORNER_R 4
int32_t graphics_corner_inset(int32_t row_from_edge);

#define GRAPHICS_PAD 12

#define GRAPHICS_CIRCLE_D 14
int32_t graphics_circle_inset(int32_t row_from_edge);

void graphics_fill_rect_rounded(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

void graphics_draw_rect_rounded(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

void graphics_draw_text_alpha(graphics_context_t *context, int32_t x, int32_t y, const char *s,
                              uint32_t color, uint32_t alpha);

void graphics_draw_text_shadowed(graphics_context_t *context, int32_t x, int32_t y, const char *s,
                                 uint32_t color, uint32_t shadow_color, uint32_t shadow_alpha);

#define GRAPHICS_SUBPIXEL 256

int32_t graphics_rounded_edge_subpixels(int32_t row, int32_t h, int32_t radius);

uint32_t graphics_span_coverage(int32_t column, int32_t left_subpixels, int32_t right_subpixels);

int32_t graphics_clamp_radius(int32_t w, int32_t h, int32_t radius);

void graphics_fill_rounded(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h,
                           int32_t radius, uint32_t color, uint32_t alpha);

void graphics_stroke_rounded(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h,
                             int32_t radius, uint32_t color, uint32_t alpha);

void graphics_draw_button_state(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h,
                            uint32_t bg_color, uint32_t border_color,
                            const char *label, uint32_t label_color, int pressed);

#ifdef __cplusplus
}
#endif
