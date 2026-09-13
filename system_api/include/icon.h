#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ICON_MAGIC_0 'L'
#define ICON_MAGIC_1 'I'
#define ICON_MAGIC_2 'C'
#define ICON_MAGIC_3 '1'

#define ICON_HEADER_BYTES 8
#define ICON_MAX_PALETTE  16

static inline int icon_width(const uint8_t *blob) { return blob[4]; }
static inline int icon_height(const uint8_t *blob) { return blob[5]; }
static inline int icon_palette_count(const uint8_t *blob) { return blob[6]; }

static inline int icon_bytes(const uint8_t *blob) {
    int pixels = blob[4] * blob[5];
    return ICON_HEADER_BYTES + 3 * blob[6] + (pixels + 1) / 2;
}

#define ICON_MAX_BYTES (ICON_HEADER_BYTES + 3 * ICON_MAX_PALETTE + (255 * 255 + 1) / 2)

static inline int icon_valid(const uint8_t *blob) {
    return blob && blob[0] == ICON_MAGIC_0 && blob[1] == ICON_MAGIC_1 &&
           blob[2] == ICON_MAGIC_2 && blob[3] == ICON_MAGIC_3 &&
           blob[4] > 0 && blob[5] > 0 &&
           blob[6] > 0 && blob[6] <= ICON_MAX_PALETTE;
}

#ifdef __cplusplus
}
#endif
