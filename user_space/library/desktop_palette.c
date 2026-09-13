#include "desktop_palette.h"

#include <math.h>

#define CHANNEL_RED(c)   (((c) >> 16) & 0xFFu)
#define CHANNEL_GREEN(c) (((c) >> 8) & 0xFFu)
#define CHANNEL_BLUE(c)  ((c) & 0xFFu)

#define PALETTE_TEXT      0x00F2F5F9u
#define PALETTE_INK       0x000B0E14u
#define PALETTE_DANGER    0x00E5484Du
#define PALETTE_POSITIVE  0x0032B57Bu

#define WINDOW_DARKEN_PERCENT 58u
#define WINDOW_TINT_PERCENT    7u
#define SURFACE_LIFT_PERCENT   7u
#define RAISED_LIFT_PERCENT   13u
#define OUTLINE_LIFT_PERCENT  22u
#define DIM_TEXT_PERCENT      62u

static uint32_t mix_channel(uint32_t base, uint32_t over, uint32_t over_percent) {
    return (base * (100u - over_percent) + over * over_percent + 50u) / 100u;
}

uint32_t desktop_palette_mix(uint32_t base, uint32_t over, uint32_t over_percent) {
    if (over_percent > 100u) {
        over_percent = 100u;
    }
    uint32_t r = mix_channel(CHANNEL_RED(base), CHANNEL_RED(over), over_percent);
    uint32_t g = mix_channel(CHANNEL_GREEN(base), CHANNEL_GREEN(over), over_percent);
    uint32_t b = mix_channel(CHANNEL_BLUE(base), CHANNEL_BLUE(over), over_percent);
    return (r << 16) | (g << 8) | b;
}

static double channel_luminance(uint32_t value) {
    double c = (double)value / 255.0;
    if (c <= 0.03928) {
        return c / 12.92;
    }
    return pow((c + 0.055) / 1.055, 2.4);
}

static double relative_luminance(uint32_t color) {
    return 0.2126 * channel_luminance(CHANNEL_RED(color)) +
           0.7152 * channel_luminance(CHANNEL_GREEN(color)) +
           0.0722 * channel_luminance(CHANNEL_BLUE(color));
}

uint32_t desktop_palette_contrast_x100(uint32_t a, uint32_t b) {
    double la = relative_luminance(a);
    double lb = relative_luminance(b);
    double lighter = la > lb ? la : lb;
    double darker = la > lb ? lb : la;
    double ratio = (lighter + 0.05) / (darker + 0.05);
    return (uint32_t)(ratio * 100.0 + 0.5);
}

void desktop_palette_derive(uint32_t background, uint32_t accent, desktop_palette_t *out) {
    if (!out) {
        return;
    }
    background &= 0x00FFFFFFu;
    accent &= 0x00FFFFFFu;

    uint32_t window = desktop_palette_mix(background, 0x00000000u, WINDOW_DARKEN_PERCENT);
    window = desktop_palette_mix(window, accent, WINDOW_TINT_PERCENT);

    out->window = window;
    out->surface = desktop_palette_mix(window, 0x00FFFFFFu, SURFACE_LIFT_PERCENT);
    out->surface_raised = desktop_palette_mix(window, 0x00FFFFFFu, RAISED_LIFT_PERCENT);
    out->outline = desktop_palette_mix(window, 0x00FFFFFFu, OUTLINE_LIFT_PERCENT);
    out->text = PALETTE_TEXT;
    out->text_dim = desktop_palette_mix(window, PALETTE_TEXT, DIM_TEXT_PERCENT);
    out->accent = accent;
    out->accent_text = desktop_palette_contrast_x100(accent, PALETTE_TEXT) >=
                       desktop_palette_contrast_x100(accent, PALETTE_INK)
                           ? PALETTE_TEXT
                           : PALETTE_INK;
    out->danger = PALETTE_DANGER;
    out->positive = PALETTE_POSITIVE;
}
