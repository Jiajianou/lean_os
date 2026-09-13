#pragma once

#include <stdint.h>

#include "graphics.h"

void icon_draw(gfx_ctx_t *ctx, int32_t x, int32_t y, const uint8_t *blob, int32_t scale);

extern const uint8_t ICON_TERMINAL[];
extern const uint8_t ICON_EDITOR[];
extern const uint8_t ICON_FILES[];
extern const uint8_t ICON_SETTINGS[];
extern const uint8_t ICON_CLOCK[];
extern const uint8_t ICON_PAINT[];
extern const uint8_t ICON_TASKS[];
extern const uint8_t ICON_BROWSER[];
