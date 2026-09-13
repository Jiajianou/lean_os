#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ICON_MAGIC_0 'L'
#define ICON_MAGIC_1 'I'
#define ICON_MAGIC_2 'C'
#define ICON_MAGIC_3_PALETTE   '1'
#define ICON_MAGIC_3_TRUECOLOR '2'

#define ICON_HEADER_BYTES 8
#define ICON_MAX_PALETTE  16
#define ICON_MAX_SIDE     64

static inline int icon_width(const uint8_t *blob) { return blob[4]; }
static inline int icon_height(const uint8_t *blob) { return blob[5]; }
static inline int icon_palette_count(const uint8_t *blob) { return blob[6]; }

static inline int icon_is_truecolor(const uint8_t *blob) {
    return blob[3] == ICON_MAGIC_3_TRUECOLOR;
}

static inline int icon_bytes(const uint8_t *blob) {
    int pixels = blob[4] * blob[5];
    if (icon_is_truecolor(blob)) {
        return ICON_HEADER_BYTES + 4 * pixels;
    }
    return ICON_HEADER_BYTES + 3 * blob[6] + (pixels + 1) / 2;
}

#define ICON_MAX_BYTES (ICON_HEADER_BYTES + 4 * ICON_MAX_SIDE * ICON_MAX_SIDE)

static inline int icon_valid(const uint8_t *blob) {
    if (!blob || blob[0] != ICON_MAGIC_0 || blob[1] != ICON_MAGIC_1 || blob[2] != ICON_MAGIC_2) {
        return 0;
    }
    if (blob[4] <= 0 || blob[5] <= 0 || blob[4] > ICON_MAX_SIDE || blob[5] > ICON_MAX_SIDE) {
        return 0;
    }
    if (blob[3] == ICON_MAGIC_3_TRUECOLOR) {
        return 1;
    }
    if (blob[3] != ICON_MAGIC_3_PALETTE) {
        return 0;
    }
    return blob[6] > 0 && blob[6] <= ICON_MAX_PALETTE;
}

#ifdef __cplusplus
}
#endif
