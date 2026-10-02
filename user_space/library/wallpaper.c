#include "wallpaper.h"

#include "malloc.h"

typedef struct {
    const char *name;
    int32_t top_pct;
    int32_t bottom_pct;
} wallpaper_style_t;

static const wallpaper_style_t STYLES[WALLPAPER_COUNT] = {
    {"Flat",     100, 100},
    {"Gradient", 155,  60},
    {"Deep",      95,  25},
    {"Grid",     155,  60},
    {"Aurora",   100, 100},
    {"Dusk",     100, 100},
    {"Ocean",    100, 100},
};

#define GRID_STEP 64
#define GRID_PCT  190

#define WAVE_PERIOD 4096
#define WAVE_PEAK   1024

static int style_index(int id) {
    if (id == WALLPAPER_PICTURE) {
        return WALLPAPER_GRADIENT;
    }
    return (id < 0 || id >= WALLPAPER_COUNT) ? WALLPAPER_FLAT : id;
}

int wallpaper_uses_desktop_colour(int id) {
    return style_index(id) <= WALLPAPER_GRID;
}

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
    if (id == WALLPAPER_PICTURE) {
        return "Picture";
    }
    return STYLES[style_index(id)].name;
}

static uint32_t mix(uint32_t a, uint32_t b, int32_t numerator, int32_t denominator) {
    if (denominator <= 0) {
        return a;
    }
    if (numerator < 0) {
        numerator = 0;
    }
    if (numerator > denominator) {
        numerator = denominator;
    }
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        int32_t from = (int32_t)((a >> shift) & 0xFF);
        int32_t to = (int32_t)((b >> shift) & 0xFF);
        out |= (uint32_t)(from + (to - from) * numerator / denominator) << shift;
    }
    return out;
}

static uint32_t add_light(uint32_t base, uint32_t light, int32_t amount_of_256) {
    if (amount_of_256 <= 0) {
        return base;
    }
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        int32_t v = (int32_t)((base >> shift) & 0xFF) + (int32_t)((light >> shift) & 0xFF) * amount_of_256 / 256;
        out |= (uint32_t)(v > 255 ? 255 : v) << shift;
    }
    return out;
}

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

/* A sine's shape out of two parabolas, in integers, so a painted wallpaper is
   the same pixels on the machine and in the host tests. One period is 4096
   and the peak is 1024. */
static int32_t wave(int32_t phase) {
    phase &= WAVE_PERIOD - 1;
    int32_t half = phase & (WAVE_PERIOD / 2 - 1);
    int32_t v = (int32_t)((int64_t)half * (WAVE_PERIOD / 2 - half) * WAVE_PEAK / ((WAVE_PERIOD / 4) * (WAVE_PERIOD / 4)));
    return phase < WAVE_PERIOD / 2 ? v : -v;
}

static int32_t column_wave(int32_t column, int32_t width, int32_t cycles_x10, int32_t offset) {
    return wave((int32_t)((int64_t)column * WAVE_PERIOD * cycles_x10 / (10 * (int64_t)width)) + offset);
}

static uint32_t hash(uint32_t x, uint32_t y) {
    uint32_t h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static int32_t ridge(int32_t column, int32_t width, int32_t height, int32_t level_pct, int32_t depth_x10_pct,
                     int32_t cycles_x10, int32_t offset) {
    int32_t base = height * level_pct / 100;
    int32_t swing = (int32_t)((int64_t)column_wave(column, width, cycles_x10, offset) * height * depth_x10_pct /
                              (WAVE_PEAK * 1000));
    int32_t detail = (int32_t)((int64_t)column_wave(column, width, cycles_x10 * 3 + 7, offset * 2) * height /
                               (WAVE_PEAK * 90));
    return base + swing + detail;
}

/* Light that falls off with the square of the distance from a band's
   centre: 256 on the line, nothing past its half-width. */
static int32_t band(int32_t row, int32_t centre, int32_t half_width) {
    if (half_width <= 0) {
        return 0;
    }
    int32_t d = row - centre;
    if (d < 0) {
        d = -d;
    }
    if (d >= half_width) {
        return 0;
    }
    int32_t f = (half_width - d) * 256 / half_width;
    return f * f / 256;
}

static uint32_t aurora_pixel(int32_t column, int32_t row, int32_t w, int32_t h, const int32_t *centres) {
    uint32_t sky = mix(0x00080C20u, 0x00162848u, row, h);
    if (hash((uint32_t)column, (uint32_t)row) % 1400u == 0 && row < h * 2 / 3) {
        sky = add_light(sky, 0x00C8D0E8u, 120 + (int32_t)(hash((uint32_t)row, (uint32_t)column) % 120u));
    }
    int32_t thickness = h / 9 + (int32_t)((int64_t)centres[3 * column + 2] * h / (WAVE_PEAK * 30));
    int32_t streak = 640 + column_wave(column, w, 230, 0) / 3 + column_wave(column, w, 970, 1000) / 6;
    int32_t green = band(row, centres[3 * column], thickness) * streak / 1024;
    int32_t below = row > centres[3 * column] ? 2 : 1;
    sky = add_light(sky, 0x0030D896u, green * 200 / 256 / below);
    int32_t violet = band(row, centres[3 * column + 1], thickness * 2 / 3) * streak / 1024;
    sky = add_light(sky, 0x008A50D2u, violet * 120 / 256);
    return sky;
}

static uint32_t dusk_sky(int32_t column, int32_t row, int32_t w, int32_t h);

/* A hill's edge is one row a column, so where the ridge steps by more than
   a row between neighbours the edge would be a staircase; the row on the
   ridge itself is drawn half sky and half hill, which is enough to soften
   it at the sizes a desktop is. */
static uint32_t dusk_pixel(int32_t column, int32_t row, int32_t w, int32_t h, const int32_t *ridges) {
    int32_t near = ridges[2 * column + 1];
    int32_t far = ridges[2 * column];
    if (row > near) {
        return mix(0x00321C3Cu, 0x001A0E22u, row - near, h - near);
    }
    uint32_t far_hill = mix(0x006A3A6Au, 0x00502A58u, row - far, h / 6);
    if (row == near) {
        return mix(row >= far ? far_hill : dusk_sky(column, row, w, h), 0x00321C3Cu, 1, 2);
    }
    if (row > far) {
        return far_hill;
    }
    if (row == far) {
        return mix(dusk_sky(column, row, w, h), far_hill, 1, 2);
    }
    return dusk_sky(column, row, w, h);
}

static uint32_t dusk_sky(int32_t column, int32_t row, int32_t w, int32_t h) {
    uint32_t sky = row < h * 55 / 100 ? mix(0x00281E50u, 0x00C86A6Au, row, h * 55 / 100)
                                      : mix(0x00C86A6Au, 0x00F8BE7Cu, row - h * 55 / 100, h * 20 / 100);
    int32_t sun_x = w * 68 / 100;
    int32_t sun_y = h * 64 / 100;
    int32_t radius = h / 13;
    int64_t dx = column - sun_x;
    int64_t dy = row - sun_y;
    int64_t distance_squared = dx * dx + dy * dy;
    if (distance_squared <= (int64_t)radius * radius) {
        return 0x00FFE6B4u;
    }
    int64_t glow = (int64_t)radius * 4;
    if (distance_squared < glow * glow) {
        int32_t f = (int32_t)(256 - distance_squared * 256 / (glow * glow));
        sky = add_light(sky, 0x00FFB478u, f * f / 512);
    }
    return sky;
}

static uint32_t ocean_pixel(int32_t column, int32_t row, int32_t w, int32_t h, const int32_t *crests) {
    int32_t horizon = h * 42 / 100;
    if (row < horizon) {
        uint32_t sky = mix(0x0094C2E6u, 0x0050A0D2u, row, horizon);
        return add_light(sky, 0x00FFFFFFu, band(row, horizon, h / 14) / 4);
    }
    uint32_t sea = mix(0x00146490u, 0x00051A38u, row - horizon, h - horizon);
    for (int k = 0; k < 6; k++) {
        int32_t crest = crests[6 * column + k];
        int32_t half = 2 + k;
        sea = add_light(sea, 0x005AAAD8u, band(row, crest, half) * (70 - k * 6) / 256);
    }
    return add_light(sea, 0x00FFF0C8u, band(column, w * 68 / 100, w / 40) * band(row, horizon + 4, h / 3) / 2048);
}

static void paint_picture_style(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h, int style) {
    int per_column = style == WALLPAPER_AURORA ? 3 : style == WALLPAPER_DUSK ? 2 : 6;
    int32_t *columns = (int32_t *)malloc((size_t)w * (size_t)per_column * sizeof(int32_t));
    if (!columns) {
        for (int32_t row = 0; row < h; row++) {
            graphics_fill_rect(context, x, y + row, w, 1, wallpaper_row_color(WALLPAPER_DEEP, 0x00203050u, row, h));
        }
        return;
    }
    for (int32_t c = 0; c < w; c++) {
        if (style == WALLPAPER_AURORA) {
            columns[3 * c] = h * 30 / 100 + (int32_t)((int64_t)column_wave(c, w, 13, 600) * h * 8 / (WAVE_PEAK * 100));
            columns[3 * c + 1] = h * 46 / 100 + (int32_t)((int64_t)column_wave(c, w, 9, 2600) * h * 6 / (WAVE_PEAK * 100));
            columns[3 * c + 2] = column_wave(c, w, 21, 0);
        } else if (style == WALLPAPER_DUSK) {
            columns[2 * c] = ridge(c, w, h, 72, 40, 15, 900);
            columns[2 * c + 1] = ridge(c, w, h, 84, 50, 9, 2000);
        } else {
            for (int k = 0; k < 6; k++) {
                int32_t level = h * (46 + k * 9) / 100;
                columns[6 * c + k] =
                    level + (int32_t)((int64_t)column_wave(c, w, 20 + k * 9, k * 700) * h * (2 + k) / (WAVE_PEAK * 200));
            }
        }
    }
    for (int32_t row = 0; row < h; row++) {
        int32_t target_y = y + row;
        if (target_y < 0 || target_y >= context->height) {
            continue;
        }
        uint32_t *line = context->pixels + (int64_t)target_y * context->width;
        for (int32_t c = 0; c < w; c++) {
            int32_t target_x = x + c;
            if (target_x < 0 || target_x >= context->width) {
                continue;
            }
            uint32_t pixel = style == WALLPAPER_AURORA ? aurora_pixel(c, row, w, h, columns)
                           : style == WALLPAPER_DUSK   ? dusk_pixel(c, row, w, h, columns)
                                                       : ocean_pixel(c, row, w, h, columns);
            line[target_x] = pixel;
        }
    }
    free(columns);
}

void wallpaper_fill(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h, int id, uint32_t base) {
    int style = style_index(id);
    if (style >= WALLPAPER_AURORA) {
        paint_picture_style(context, x, y, w, h, style);
        return;
    }
    for (int32_t row = 0; row < h; row++) {
        graphics_fill_rect(context, x, y + row, w, 1, wallpaper_row_color(id, base, row, h));
    }
    if (style == WALLPAPER_GRID) {
        uint32_t line = scale_color(base, GRID_PCT);
        for (int32_t gx = x + GRID_STEP; gx < x + w; gx += GRID_STEP) {
            graphics_fill_rect(context, gx, y, 1, h, line);
        }
        for (int32_t gy = y + GRID_STEP; gy < y + h; gy += GRID_STEP) {
            graphics_fill_rect(context, x, gy, w, 1, line);
        }
    }
}
