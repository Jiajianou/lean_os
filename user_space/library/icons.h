#pragma once

#include <stdint.h>

#include "graphics.h"

#define ICON_LARGE_SIZE 48
#define ICON_SMALL_SIZE 24

void icon_draw(graphics_context_t *context, int32_t x, int32_t y, const uint8_t *blob, int32_t scale);

void icon_draw_tinted(graphics_context_t *context, int32_t x, int32_t y, const uint8_t *blob,
                      int32_t scale, uint32_t tint, uint32_t tint_percent);

extern const uint8_t ICON_TERMINAL[];
extern const uint8_t ICON_TERMINAL_SMALL[];
extern const uint8_t ICON_EDITOR[];
extern const uint8_t ICON_EDITOR_SMALL[];
extern const uint8_t ICON_FILES[];
extern const uint8_t ICON_FILES_SMALL[];
extern const uint8_t ICON_SETTINGS[];
extern const uint8_t ICON_SETTINGS_SMALL[];
extern const uint8_t ICON_CLOCK[];
extern const uint8_t ICON_CLOCK_SMALL[];
extern const uint8_t ICON_PAINT[];
extern const uint8_t ICON_PAINT_SMALL[];
extern const uint8_t ICON_TASKS[];
extern const uint8_t ICON_TASKS_SMALL[];
extern const uint8_t ICON_BROWSER[];
extern const uint8_t ICON_BROWSER_SMALL[];
extern const uint8_t ICON_APPLICATION[];
extern const uint8_t ICON_APPLICATION_SMALL[];
