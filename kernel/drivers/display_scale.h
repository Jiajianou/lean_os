#pragma once

#include <stdint.h>

#define DISPLAY_SCALE_MINIMUM_WIDTH  800
#define DISPLAY_SCALE_MINIMUM_HEIGHT 600

int display_scale_fits(uint32_t width, uint32_t height, uint32_t percent);

uint32_t display_scale_automatic(uint32_t width, uint32_t height, uint32_t boot_factor);

uint32_t display_scale_effective(uint32_t width, uint32_t height, uint32_t requested, uint32_t boot_factor);

int display_scale_choices(uint32_t width, uint32_t height, uint32_t *out, int max);
