#pragma once

#include <stdint.h>

typedef struct {
    uint32_t window;
    uint32_t surface;
    uint32_t surface_raised;
    uint32_t outline;
    uint32_t text;
    uint32_t text_dim;
    uint32_t accent;
    uint32_t accent_text;
    uint32_t danger;
    uint32_t positive;
} desktop_palette_t;

#define DESKTOP_PALETTE_DEFAULT_BACKGROUND 0x001A1A2Eu
#define DESKTOP_PALETTE_DEFAULT_ACCENT     0x004C99E6u

uint32_t desktop_palette_mix(uint32_t base, uint32_t over, uint32_t over_percent);

uint32_t desktop_palette_contrast_x100(uint32_t a, uint32_t b);

void desktop_palette_derive(uint32_t background, uint32_t accent, desktop_palette_t *out);
