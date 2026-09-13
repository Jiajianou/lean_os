#include "graphics.h"

#include "font8x16.h"
#include "string_utilities.h"

void graphics_put_pixel(graphics_context_t *context, int32_t x, int32_t y, uint32_t color) {
    if (x < 0 || y < 0 || x >= context->width || y >= context->height) {
        return;
    }
    context->pixels[y * context->width + x] = color;
}

void graphics_blend_pixel(graphics_context_t *context, int32_t x, int32_t y, uint32_t color,
                          uint32_t alpha) {
    if (x < 0 || y < 0 || x >= context->width || y >= context->height) {
        return;
    }
    if (alpha >= 255) {
        context->pixels[y * context->width + x] = color;
        return;
    }
    if (alpha == 0) {
        return;
    }
    uint32_t *at = &context->pixels[y * context->width + x];
    uint32_t behind = *at;
    uint32_t inverse = 255u - alpha;
    uint32_t r = GRAPHICS_OVER_255((color >> 16 & 0xFFu) * alpha + (behind >> 16 & 0xFFu) * inverse);
    uint32_t g = GRAPHICS_OVER_255((color >> 8 & 0xFFu) * alpha + (behind >> 8 & 0xFFu) * inverse);
    uint32_t b = GRAPHICS_OVER_255((color & 0xFFu) * alpha + (behind & 0xFFu) * inverse);
    *at = (behind & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

void graphics_fill_rect(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + w > context->width ? context->width : x + w;
    int32_t y1 = y + h > context->height ? context->height : y + h;
    for (int32_t row = y0; row < y1; row++) {
        uint32_t *destination = context->pixels + row * context->width;
        for (int32_t col = x0; col < x1; col++) {
            destination[col] = color;
        }
    }
}

void graphics_draw_rect(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    for (int32_t col = 0; col < w; col++) {
        graphics_put_pixel(context, x + col, y, color);
        graphics_put_pixel(context, x + col, y + h - 1, color);
    }
    for (int32_t row = 0; row < h; row++) {
        graphics_put_pixel(context, x, y + row, color);
        graphics_put_pixel(context, x + w - 1, y + row, color);
    }
}

void graphics_draw_line(graphics_context_t *context, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    int32_t dx = x1 - x0;
    dx = dx < 0 ? -dx : dx;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 - y0;
    dy = dy < 0 ? dy : -dy;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;

    for (;;) {
        graphics_put_pixel(context, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int32_t e2 = 2 * error;
        if (e2 >= dy) {
            error += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static const ui_font_t *ui_font_current = &ui_font_ui;

static uint32_t text_alpha_scale = 255;

void graphics_set_ui_font(const ui_font_t *font) {
    if (font) {
        ui_font_current = font;
    }
}

const ui_font_t *graphics_ui_font(void) {
    return ui_font_current;
}

#define GRAPHICS_REPLACEMENT 0xFFFDu

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
        *out = GRAPHICS_REPLACEMENT;
        return 1;
    }
    for (int32_t i = 1; i <= need; i++) {
        uint8_t b = (uint8_t)p[i];
        if ((b & 0xC0u) != 0x80u) {
            *out = GRAPHICS_REPLACEMENT;
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

int32_t graphics_glyph_advance(const ui_font_t *font, uint32_t cp) {
    if (cp >= 128) {
        return box_advance(font);
    }
    return font->advance[cp];
}

int32_t graphics_char_advance(const ui_font_t *font, char c) {
    uint8_t code = (uint8_t)c;
    if (code >= 128) {
        return box_advance(font);
    }
    return font->advance[code];
}

int32_t graphics_text_width_n(const ui_font_t *font, const char *s, int32_t n) {
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
        w += graphics_glyph_advance(font, cp);
    }
    return w > widest ? w : widest;
}

int32_t graphics_text_width(const ui_font_t *font, const char *s) {
    return graphics_text_width_n(font, s, (int32_t)strlen(s));
}

int32_t graphics_text_fit(const ui_font_t *font, const char *s, int32_t max_w) {
    int32_t w = 0;
    int32_t i = 0;
    while (s[i]) {
        uint32_t cp;
        int32_t used = utf8_step(s + i, &cp);
        int32_t adv = graphics_glyph_advance(font, cp);
        if (w + adv > max_w) {
            break;
        }
        w += adv;
        i += used;
    }
    return i;
}

void graphics_text_measure(const ui_font_t *font, const char *s, graphics_text_metrics_t *out) {
    out->width   = graphics_text_width(font, s);
    out->height  = font->height;
    out->ascent  = font->baseline;
    out->descent = font->height - font->baseline;
}

static void draw_box(graphics_context_t *context, int32_t x, int32_t y, uint32_t color,
                     const ui_font_t *font) {
    int32_t w = box_advance(font) - 1;
    int32_t top = font->height - font->baseline;
    int32_t h = font->baseline - 1;
    if (w < 3 || h < 3) {
        return;
    }
    for (int32_t col = 0; col < w; col++) {
        graphics_put_pixel(context, x + col, y + top, color);
        graphics_put_pixel(context, x + col, y + top + h - 1, color);
    }
    for (int32_t row = 1; row < h - 1; row++) {
        graphics_put_pixel(context, x, y + top + row, color);
        graphics_put_pixel(context, x + w - 1, y + top + row, color);
    }
}

void graphics_draw_glyph_font(graphics_context_t *context, int32_t x, int32_t y, uint32_t cp, uint32_t color,
                          const ui_font_t *font, int bold) {
    if (cp >= 128) {
        draw_box(context, x, y, color, font);
        return;
    }
    uint8_t code = (uint8_t)cp;
    int use_bold = (bold && font->coverage_bold) ? 1 : 0;
    const uint8_t *coverage = use_bold ? font->coverage_bold : font->coverage;
    const uint16_t *offset = use_bold ? font->offset_bold : font->offset;
    int32_t w = font->width[code] + use_bold;
    if (font->width[code] == 0) {
        return;
    }
    const uint8_t *glyph = coverage + offset[code];
    for (int32_t row = 0; row < font->height; row++) {
        const uint8_t *line = glyph + (int32_t)row * w;
        for (int32_t col = 0; col < w; col++) {
            uint32_t alpha = line[col];
            if (alpha) {
                if (text_alpha_scale != 255) {
                    alpha = alpha * text_alpha_scale / 255u;
                }
                graphics_blend_pixel(context, x + col, y + row, color, alpha);
            }
        }
    }
}

void graphics_draw_char_font(graphics_context_t *context, int32_t x, int32_t y, char c, uint32_t color,
                         const ui_font_t *font, int bold) {
    graphics_draw_glyph_font(context, x, y, (uint8_t)c, color, font, bold);
}

void graphics_draw_text_font(graphics_context_t *context, int32_t x, int32_t y, const char *s, uint32_t color,
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
        graphics_draw_glyph_font(context, cx, y, cp, color, font, bold);
        cx += graphics_glyph_advance(font, cp);
    }
}

void graphics_draw_char(graphics_context_t *context, int32_t x, int32_t y, char c, uint32_t color) {
    graphics_draw_char_font(context, x, y, c, color, ui_font_current, 0);
}

void graphics_draw_text(graphics_context_t *context, int32_t x, int32_t y, const char *s, uint32_t color) {
    graphics_draw_text_font(context, x, y, s, color, ui_font_current, 0);
}

static void draw_box_mono(graphics_context_t *context, int32_t x, int32_t y, uint32_t color) {
    for (int32_t col = 1; col < FONT_WIDTH - 1; col++) {
        graphics_put_pixel(context, x + col, y + 2, color);
        graphics_put_pixel(context, x + col, y + FONT_HEIGHT - 3, color);
    }
    for (int32_t row = 3; row < FONT_HEIGHT - 3; row++) {
        graphics_put_pixel(context, x + 1, y + row, color);
        graphics_put_pixel(context, x + FONT_WIDTH - 2, y + row, color);
    }
}

void graphics_draw_glyph_mono(graphics_context_t *context, int32_t x, int32_t y, uint32_t cp, uint32_t color) {
    if (cp >= 128) {
        draw_box_mono(context, x, y, color);
        return;
    }
    const uint8_t *glyph = font8x16[cp];
    for (int32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int32_t col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                graphics_put_pixel(context, x + col, y + row, color);
            }
        }
    }
}

void graphics_draw_char_mono(graphics_context_t *context, int32_t x, int32_t y, char c, uint32_t color) {
    graphics_draw_glyph_mono(context, x, y, (uint8_t)c, color);
}

void graphics_draw_text_mono(graphics_context_t *context, int32_t x, int32_t y, const char *s, uint32_t color) {
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
        graphics_draw_glyph_mono(context, cx, y, cp, color);
        cx += FONT_WIDTH;
    }
}

int graphics_point_in_rect(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static uint32_t darken(uint32_t color) {
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        out |= (((color >> shift) & 0xFF) * 2 / 3) << shift;
    }
    return out;
}

void graphics_draw_button_state(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h,
                            uint32_t bg_color, uint32_t border_color,
                            const char *label, uint32_t label_color, int pressed) {
    graphics_fill_rect(context, x, y, w, h, pressed ? darken(bg_color) : bg_color);
    graphics_draw_rect(context, x, y, w, h, border_color);
    if (label) {
        const ui_font_t *font = ui_font_current;
        int32_t label_w = graphics_text_width(font, label);
        int32_t label_x = x + (w - label_w) / 2;
        int32_t label_y = y + (h - font->height) / 2;
        graphics_draw_text(context, label_x + (pressed ? 1 : 0), label_y + (pressed ? 1 : 0), label, label_color);
    }
}

void graphics_draw_button(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t bg_color, uint32_t border_color,
                      const char *label, uint32_t label_color) {
    graphics_draw_button_state(context, x, y, w, h, bg_color, border_color, label, label_color, 0);
}

void graphics_draw_menu(graphics_context_t *context, int32_t x, int32_t y, int32_t item_w, int32_t item_h,
                    const char *const *items, int count, int hover_index,
                    uint32_t bg_color, uint32_t hover_bg_color, uint32_t border_color, uint32_t text_color) {
    for (int i = 0; i < count; i++) {
        graphics_draw_button(context, x, y + i * item_h, item_w, item_h,
                         i == hover_index ? hover_bg_color : bg_color, border_color,
                         items[i], text_color);
    }
}

int graphics_menu_hit_test(int32_t px, int32_t py, int32_t x, int32_t y, int32_t item_w, int32_t item_h, int count) {
    if (!graphics_point_in_rect(px, py, x, y, item_w, item_h * count)) {
        return -1;
    }
    return (py - y) / item_h;
}

void graphics_draw_scrollbar(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h,
                         int32_t total_items, int32_t visible_items, int32_t scroll_top,
                         uint32_t track_color, uint32_t thumb_color) {
    graphics_fill_rect(context, x, y, w, h, track_color);
    if (total_items <= visible_items || total_items <= 0) {
        graphics_fill_rect(context, x, y, w, h, thumb_color);
        return;
    }
    int32_t thumb_h = h * visible_items / total_items;
    if (thumb_h < 4) {
        thumb_h = 4;
    }
    int32_t max_top = total_items - visible_items;
    int32_t thumb_y = y + (h - thumb_h) * scroll_top / max_top;
    graphics_fill_rect(context, x, thumb_y, w, thumb_h, thumb_color);
}

static const int32_t CORNER_INSET[GRAPHICS_CORNER_R] = {2, 1, 0, 0};

int32_t graphics_corner_inset(int32_t row_from_edge) {
    if (row_from_edge < 0 || row_from_edge >= GRAPHICS_CORNER_R) {
        return 0;
    }
    return CORNER_INSET[row_from_edge];
}

static const int32_t CIRCLE_INSET[GRAPHICS_CIRCLE_D / 2] = {4, 3, 2, 1, 0, 0, 0};

int32_t graphics_circle_inset(int32_t row_from_edge) {
    if (row_from_edge < 0) {
        return GRAPHICS_CIRCLE_D / 2;
    }
    if (row_from_edge >= GRAPHICS_CIRCLE_D / 2) {
        row_from_edge = GRAPHICS_CIRCLE_D - 1 - row_from_edge;
    }
    if (row_from_edge < 0 || row_from_edge >= GRAPHICS_CIRCLE_D / 2) {
        return GRAPHICS_CIRCLE_D / 2;
    }
    return CIRCLE_INSET[row_from_edge];
}

static int32_t row_inset(int32_t row, int32_t h) {
    if (row < GRAPHICS_CORNER_R) {
        return graphics_corner_inset(row);
    }
    if (row >= h - GRAPHICS_CORNER_R) {
        return graphics_corner_inset(h - 1 - row);
    }
    return 0;
}

void graphics_fill_rect_rounded(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (w < 2 * GRAPHICS_CORNER_R || h < 2 * GRAPHICS_CORNER_R) {
        graphics_fill_rect(context, x, y, w, h, color);
        return;
    }
    for (int32_t row = 0; row < h; row++) {
        int32_t inset = row_inset(row, h);
        graphics_fill_rect(context, x + inset, y + row, w - 2 * inset, 1, color);
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

void graphics_draw_rect_rounded(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (w < 2 * GRAPHICS_CORNER_R || h < 2 * GRAPHICS_CORNER_R) {
        graphics_draw_rect(context, x, y, w, h, color);
        return;
    }
    for (int32_t row = 0; row < h; row++) {
        int32_t inset = row_inset(row, h);
        int32_t run = outline_run(row, w, h);
        graphics_fill_rect(context, x + inset, y + row, run, 1, color);
        graphics_fill_rect(context, x + w - inset - run, y + row, run, 1, color);
    }
}

static uint32_t integer_square_root(uint64_t value) {
    if (value == 0) {
        return 0;
    }
    uint64_t guess = value;
    uint64_t previous = 0;
    while (guess != previous) {
        previous = guess;
        guess = (guess + value / guess) / 2;
    }
    while (guess * guess > value) {
        guess--;
    }
    while ((guess + 1) * (guess + 1) <= value) {
        guess++;
    }
    return (uint32_t)guess;
}

int32_t graphics_clamp_radius(int32_t w, int32_t h, int32_t radius) {
    int32_t maximum = (w < h ? w : h) / 2;
    if (radius > maximum) {
        radius = maximum;
    }
    return radius < 0 ? 0 : radius;
}

int32_t graphics_rounded_edge_subpixels(int32_t row, int32_t h, int32_t radius) {
    if (radius <= 0) {
        return 0;
    }
    int32_t distance_from_corner;
    if (row < radius) {
        distance_from_corner = radius - row;
    } else if (row >= h - radius) {
        distance_from_corner = radius - (h - 1 - row);
    } else {
        return 0;
    }
    int64_t dy = (int64_t)distance_from_corner * GRAPHICS_SUBPIXEL - GRAPHICS_SUBPIXEL / 2;
    int64_t r = (int64_t)radius * GRAPHICS_SUBPIXEL;
    if (dy <= 0) {
        return 0;
    }
    if (dy >= r) {
        return radius * GRAPHICS_SUBPIXEL;
    }
    int64_t inside = r * r - dy * dy;
    int64_t horizontal = (int64_t)integer_square_root((uint64_t)inside);
    return (int32_t)(r - horizontal);
}

uint32_t graphics_span_coverage(int32_t column, int32_t left_subpixels, int32_t right_subpixels) {
    int32_t pixel_left = column * GRAPHICS_SUBPIXEL;
    int32_t pixel_right = pixel_left + GRAPHICS_SUBPIXEL;
    int32_t covered_left = left_subpixels > pixel_left ? left_subpixels : pixel_left;
    int32_t covered_right = right_subpixels < pixel_right ? right_subpixels : pixel_right;
    if (covered_right <= covered_left) {
        return 0;
    }
    return (uint32_t)((covered_right - covered_left) * 255 / GRAPHICS_SUBPIXEL);
}

void graphics_fill_rounded(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h,
                           int32_t radius, uint32_t color, uint32_t alpha) {
    if (w <= 0 || h <= 0 || alpha == 0) {
        return;
    }
    radius = graphics_clamp_radius(w, h, radius);
    for (int32_t row = 0; row < h; row++) {
        int32_t left = graphics_rounded_edge_subpixels(row, h, radius);
        if (left == 0) {
            if (alpha >= 255) {
                graphics_fill_rect(context, x, y + row, w, 1, color);
            } else {
                for (int32_t column = 0; column < w; column++) {
                    graphics_blend_pixel(context, x + column, y + row, color, alpha);
                }
            }
            continue;
        }
        int32_t right = w * GRAPHICS_SUBPIXEL - left;
        int32_t first = left / GRAPHICS_SUBPIXEL;
        int32_t last = (right + GRAPHICS_SUBPIXEL - 1) / GRAPHICS_SUBPIXEL;
        for (int32_t column = first; column < last && column < w; column++) {
            uint32_t coverage = graphics_span_coverage(column, left, right);
            if (coverage == 0) {
                continue;
            }
            graphics_blend_pixel(context, x + column, y + row, color, coverage * alpha / 255);
        }
    }
}

void graphics_stroke_rounded(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h,
                             int32_t radius, uint32_t color, uint32_t alpha) {
    if (w <= 2 || h <= 2 || alpha == 0) {
        return;
    }
    radius = graphics_clamp_radius(w, h, radius);
    int32_t inner_radius = radius > 0 ? radius - 1 : 0;
    for (int32_t row = 0; row < h; row++) {
        int32_t outer_left = graphics_rounded_edge_subpixels(row, h, radius);
        if (outer_left == 0) {
            if (row == 0 || row == h - 1) {
                for (int32_t column = 0; column < w; column++) {
                    graphics_blend_pixel(context, x + column, y + row, color, alpha);
                }
            } else {
                graphics_blend_pixel(context, x, y + row, color, alpha);
                graphics_blend_pixel(context, x + w - 1, y + row, color, alpha);
            }
            continue;
        }
        int32_t outer_right = w * GRAPHICS_SUBPIXEL - outer_left;
        int32_t inner_left;
        int32_t inner_right;
        if (row == 0 || row == h - 1) {
            inner_left = outer_right;
            inner_right = outer_right;
        } else {
            inner_left = GRAPHICS_SUBPIXEL + graphics_rounded_edge_subpixels(row - 1, h - 2, inner_radius);
            inner_right = (w - 1) * GRAPHICS_SUBPIXEL - (inner_left - GRAPHICS_SUBPIXEL);
        }
        int32_t bands[2][2];
        int band_count;
        if (inner_left >= inner_right) {
            bands[0][0] = outer_left / GRAPHICS_SUBPIXEL;
            bands[0][1] = (outer_right + GRAPHICS_SUBPIXEL - 1) / GRAPHICS_SUBPIXEL;
            band_count = 1;
        } else {
            bands[0][0] = outer_left / GRAPHICS_SUBPIXEL;
            bands[0][1] = (inner_left + GRAPHICS_SUBPIXEL - 1) / GRAPHICS_SUBPIXEL;
            bands[1][0] = inner_right / GRAPHICS_SUBPIXEL;
            bands[1][1] = (outer_right + GRAPHICS_SUBPIXEL - 1) / GRAPHICS_SUBPIXEL;
            band_count = 2;
        }
        for (int band = 0; band < band_count; band++) {
            int32_t from = bands[band][0] < 0 ? 0 : bands[band][0];
            int32_t to = bands[band][1] > w ? w : bands[band][1];
            for (int32_t column = from; column < to; column++) {
                int32_t outer = (int32_t)graphics_span_coverage(column, outer_left, outer_right);
                int32_t inner = (int32_t)graphics_span_coverage(column, inner_left, inner_right);
                int32_t coverage = outer - inner;
                if (coverage > 0) {
                    graphics_blend_pixel(context, x + column, y + row, color, (uint32_t)coverage * alpha / 255);
                }
            }
        }
    }
}

void graphics_draw_text_alpha(graphics_context_t *context, int32_t x, int32_t y, const char *s,
                              uint32_t color, uint32_t alpha) {
    if (alpha > 255) {
        alpha = 255;
    }
    text_alpha_scale = alpha;
    graphics_draw_text(context, x, y, s, color);
    text_alpha_scale = 255;
}

void graphics_draw_text_shadowed(graphics_context_t *context, int32_t x, int32_t y, const char *s,
                                 uint32_t color, uint32_t shadow_color, uint32_t shadow_alpha) {
    graphics_draw_text_alpha(context, x, y + 1, s, shadow_color, shadow_alpha);
    graphics_draw_text(context, x, y, s, color);
}
