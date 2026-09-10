/* user_space/lib/icons.h
 *
 * M56: the loader for system_api/include/icon.h's format, plus the
 * icons this desktop ships.
 *
 * One entry point. `scale` is an integer nearest-neighbour multiplier so
 * a single 24x24 blob serves both the 48px desktop icon and a 24px one
 * elsewhere - two sizes of the same picture rather than two pictures that
 * can disagree, which is the whole reason the old hand-drawn rectangles
 * had to be written out per call site.
 */
#pragma once

#include <stdint.h>

#include "gfx.h"

/* Draws `blob` with its top-left at (x, y), each source pixel becoming a
 * scale x scale block. Palette index 0 is transparent - nothing is
 * written for it, so whatever is underneath shows through. Silently draws
 * nothing for a malformed blob: an icon is decoration, and a client that
 * had to check would just be checking a constant. */
void icon_draw(gfx_ctx_t *ctx, int32_t x, int32_t y, const uint8_t *blob, int32_t scale);

/* The seven the desktop uses, in the order desktop_icons.c lists them. */
extern const uint8_t ICON_TERMINAL[];
extern const uint8_t ICON_EDITOR[];
extern const uint8_t ICON_FILES[];
extern const uint8_t ICON_SETTINGS[];
extern const uint8_t ICON_CLOCK[];
extern const uint8_t ICON_PAINT[];
extern const uint8_t ICON_TASKS[];
extern const uint8_t ICON_BROWSER[]; /* M100 */
