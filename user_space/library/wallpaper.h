#pragma once

#include <stdint.h>

#include "graphics.h"

#define WALLPAPER_FLAT     0
#define WALLPAPER_GRADIENT 1
#define WALLPAPER_DEEP     2
#define WALLPAPER_GRID     3
#define WALLPAPER_AURORA   4
#define WALLPAPER_DUSK     5
#define WALLPAPER_OCEAN    6
#define WALLPAPER_COUNT    7

#define WALLPAPER_PICTURE 64

const char *wallpaper_name(int id);

int wallpaper_uses_desktop_colour(int id);

void wallpaper_fill(graphics_context_t *context, int32_t x, int32_t y, int32_t w, int32_t h, int id, uint32_t base);
