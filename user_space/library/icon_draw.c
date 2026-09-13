#include "icons.h"

#include <stddef.h>

#include "icon.h"

static uint32_t icon_mix(uint32_t base, uint32_t over, uint32_t over_percent) {
    if (over_percent == 0) {
        return base;
    }
    if (over_percent >= 100) {
        return over;
    }
    uint32_t rest = 100u - over_percent;
    uint32_t red = (((base >> 16) & 0xFFu) * rest + ((over >> 16) & 0xFFu) * over_percent) / 100u;
    uint32_t green = (((base >> 8) & 0xFFu) * rest + ((over >> 8) & 0xFFu) * over_percent) / 100u;
    uint32_t blue = ((base & 0xFFu) * rest + (over & 0xFFu) * over_percent) / 100u;
    return (red << 16) | (green << 8) | blue;
}

static void icon_draw_palette(graphics_context_t *context, int32_t x, int32_t y, const uint8_t *blob,
                              int32_t scale, uint32_t tint, uint32_t tint_percent) {
    int width = icon_width(blob);
    int height = icon_height(blob);
    const uint8_t *palette = blob + ICON_HEADER_BYTES;
    const uint8_t *pixels = palette + 3 * icon_palette_count(blob);
    for (int row = 0; row < height; row++) {
        for (int column = 0; column < width; column++) {
            int i = row * width + column;
            int index = (i & 1) ? (pixels[i >> 1] & 0x0F) : (pixels[i >> 1] >> 4);
            if (index == 0 || index >= icon_palette_count(blob)) {
                continue;
            }
            uint32_t color = ((uint32_t)palette[index * 3] << 16) |
                             ((uint32_t)palette[index * 3 + 1] << 8) |
                             (uint32_t)palette[index * 3 + 2];
            color = icon_mix(color, tint, tint_percent);
            graphics_fill_rect(context, x + column * scale, y + row * scale, scale, scale, color);
        }
    }
}

static void icon_draw_truecolor(graphics_context_t *context, int32_t x, int32_t y, const uint8_t *blob,
                                int32_t scale, uint32_t tint, uint32_t tint_percent) {
    int width = icon_width(blob);
    int height = icon_height(blob);
    const uint8_t *pixels = blob + ICON_HEADER_BYTES;
    for (int row = 0; row < height; row++) {
        for (int column = 0; column < width; column++) {
            const uint8_t *pixel = pixels + ((size_t)row * (size_t)width + (size_t)column) * 4u;
            uint32_t alpha = pixel[3];
            if (alpha == 0) {
                continue;
            }
            uint32_t color = ((uint32_t)pixel[0] << 16) | ((uint32_t)pixel[1] << 8) | (uint32_t)pixel[2];
            color = icon_mix(color, tint, tint_percent);
            for (int32_t dy = 0; dy < scale; dy++) {
                for (int32_t dx = 0; dx < scale; dx++) {
                    graphics_blend_pixel(context, x + column * scale + dx, y + row * scale + dy, color, alpha);
                }
            }
        }
    }
}

void icon_draw_tinted(graphics_context_t *context, int32_t x, int32_t y, const uint8_t *blob,
                      int32_t scale, uint32_t tint, uint32_t tint_percent) {
    if (!icon_valid(blob) || scale < 1) {
        return;
    }
    if (icon_is_truecolor(blob)) {
        icon_draw_truecolor(context, x, y, blob, scale, tint, tint_percent);
        return;
    }
    icon_draw_palette(context, x, y, blob, scale, tint, tint_percent);
}

void icon_draw(graphics_context_t *context, int32_t x, int32_t y, const uint8_t *blob, int32_t scale) {
    icon_draw_tinted(context, x, y, blob, scale, 0, 0);
}
