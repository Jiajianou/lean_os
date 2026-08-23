/* user_space/lib/wallpaper.h
 *
 * M44: the desktop background, as a shared definition rather than a
 * color constant baked into whichever program happens to paint it.
 *
 * Two programs need to agree about it - desktop_icons.c draws it, and
 * settings.c offers the choice - and the compositor stores which one is
 * chosen (system_api/include/wm.h's wm_settings_request_t.wallpaper, the
 * same place the background and accent colors already live). Putting the
 * styles here is what keeps "what does Gradient look like" from existing
 * in two places that could disagree; the compositor still knows nothing
 * about any of it beyond relaying an integer.
 *
 * Every style is derived from the *chosen background color* rather than
 * being its own fixed palette, so the existing color picker keeps
 * meaning something after this milestone instead of being superseded by
 * it - pick the hue with one control, the treatment with the other.
 *
 * No floating point anywhere (the same constraint M38/M39 worked within -
 * see the Makefile's -mgeneral-regs-only comment): the vertical ramp is
 * one integer interpolation per row.
 */
#pragma once

#include <stdint.h>

#include "gfx.h"

#define WALLPAPER_FLAT     0 /* what every desktop before M44 looked like */
#define WALLPAPER_GRADIENT 1
#define WALLPAPER_DEEP     2
#define WALLPAPER_GRID     3
#define WALLPAPER_COUNT    4

/* Short label for a wallpaper style, for settings.c's picker. Returns
 * the flat style's name for an out-of-range id, the same way every other
 * function here treats one. */
const char *wallpaper_name(int id);

/* Paints the w x h rect at (x, y) with style `id` derived from `base`. */
void wallpaper_fill(gfx_ctx_t *ctx, int32_t x, int32_t y, int32_t w, int32_t h, int id, uint32_t base);
