#pragma once

#include <stdint.h>

#include "gfx.h"

#define WALLPAPER_FLAT     0
#define WALLPAPER_GRADIENT 1
#define WALLPAPER_DEEP     2
#define WALLPAPER_GRID     3
#define WALLPAPER_COUNT    4

const char *wallpaper_name(int id);

void wallpaper_fill(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, int id, uint32_t base);
