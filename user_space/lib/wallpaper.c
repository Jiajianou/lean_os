#include "wallpaper.h"

/* Each style is a pair of percentages applied to the chosen background
 * color - what the top row and the bottom row scale it to - and the rows
 * in between are the straight-line interpolation of those two. Deriving
 * both ends from one color is what keeps every style in the set
 * recognizably the *same* desktop rather than four unrelated palettes,
 * and it is why the color picker and this picker compose instead of
 * competing.
 *
 * WALLPAPER_FLAT is 100/100 on purpose: it is exactly the flat fill
 * every desktop before M44 had, kept as a real choice rather than
 * removed, so "I don't want a gradient" stays available. */
typedef struct {
    const char *name;
    int32_t top_pct;
    int32_t bottom_pct;
} wallpaper_style_t;

static const wallpaper_style_t STYLES[WALLPAPER_COUNT] = {
    {"Flat",     100, 100},
    {"Gradient", 155,  60},
    {"Deep",      95,  25},
    {"Grid",     155,  60}, /* the same ramp as Gradient, plus the lines below */
};

#define GRID_STEP 64
#define GRID_PCT  190 /* the grid lines, brighter than the ramp's own top */

static int style_index(int id) {
    return (id < 0 || id >= WALLPAPER_COUNT) ? WALLPAPER_FLAT : id;
}

/* base * pct / 100 per channel, clamped - the one place a style's
 * percentages turn into a real color. */
static uint32_t scale_color(uint32_t c, int32_t pct) {
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        int32_t v = (int32_t)((c >> shift) & 0xFF) * pct / 100;
        if (v > 255) {
            v = 255;
        }
        out |= (uint32_t)v << shift;
    }
    return out;
}

const char *wallpaper_name(int id) {
    return STYLES[style_index(id)].name;
}

/* The color of row `row` of a `height`-tall wallpaper - the whole of the
 * gradient math. Internal: the only caller is wallpaper_fill just below,
 * and the interpolation is checked end to end from real framebuffer
 * pixels by kernel.c's [m44] self-test rather than by calling this. */
static uint32_t wallpaper_row_color(int id, uint32_t base, int32_t row, int32_t height) {
    const wallpaper_style_t *style = &STYLES[style_index(id)];
    uint32_t top = scale_color(base, style->top_pct);
    uint32_t bottom = scale_color(base, style->bottom_pct);
    if (height <= 1) {
        return top;
    }
    if (row < 0) {
        row = 0;
    }
    if (row >= height) {
        row = height - 1;
    }
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        int32_t a = (int32_t)((top >> shift) & 0xFF);
        int32_t b = (int32_t)((bottom >> shift) & 0xFF);
        out |= (uint32_t)(a + (b - a) * row / (height - 1)) << shift;
    }
    return out;
}

void wallpaper_fill(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, int id, uint32_t base) {
    /* One gfx_fill_rect per row rather than a per-pixel loop - M44's own
     * perf pass made that a clipped row-store, so a full-screen wallpaper
     * is `height` tight row fills and nothing else. */
    for (int32_t row = 0; row < h; row++) {
        gfx_fill_rect(ctx, x, y + row, w, 1, wallpaper_row_color(id, base, row, h));
    }
    if (style_index(id) == WALLPAPER_GRID) {
        uint32_t line = scale_color(base, GRID_PCT);
        for (int32_t gx = x + GRID_STEP; gx < x + w; gx += GRID_STEP) {
            gfx_fill_rect(ctx, gx, y, 1, h, line);
        }
        for (int32_t gy = y + GRID_STEP; gy < y + h; gy += GRID_STEP) {
            gfx_fill_rect(ctx, x, gy, w, 1, line);
        }
    }
}
