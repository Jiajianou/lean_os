#include "display_scale.h"

/* M213. How many physical pixels one desktop pixel is, in percent. Only a
   scale that divides the panel exactly is offered: a desktop of 2194.3
   pixels would leave the compositor rounding every edge it draws, and the
   last column of the screen would belong to nobody. */
static const uint32_t CANDIDATES[] = {100, 125, 150, 175, 200, 250, 300};

#define CANDIDATE_COUNT ((int)(sizeof(CANDIDATES) / sizeof(CANDIDATES[0])))

int display_scale_fits(uint32_t width, uint32_t height, uint32_t percent) {
    if (percent < 100 || width == 0 || height == 0) {
        return 0;
    }
    uint64_t scaled_width = (uint64_t)width * 100u;
    uint64_t scaled_height = (uint64_t)height * 100u;
    if (scaled_width % percent != 0 || scaled_height % percent != 0) {
        return 0;
    }
    if (percent == 100) {
        return 1;
    }
    return scaled_width / percent >= DISPLAY_SCALE_MINIMUM_WIDTH &&
           scaled_height / percent >= DISPLAY_SCALE_MINIMUM_HEIGHT;
}

/* scale= in lean_os.cfg pins a factor; without it a panel of 2560x1440 or
   more is doubled, because at about 300 pixels to the inch a desktop drawn
   one to one is a third of the size anybody can read (M196). */
uint32_t display_scale_automatic(uint32_t width, uint32_t height, uint32_t boot_factor) {
    if (boot_factor == 1) {
        return 100;
    }
    if ((boot_factor == 2 || (width >= 2560 && height >= 1440)) && display_scale_fits(width, height, 200)) {
        return 200;
    }
    return 100;
}

uint32_t display_scale_effective(uint32_t width, uint32_t height, uint32_t requested, uint32_t boot_factor) {
    if (display_scale_fits(width, height, requested)) {
        return requested;
    }
    return display_scale_automatic(width, height, boot_factor);
}

int display_scale_choices(uint32_t width, uint32_t height, uint32_t *out, int max) {
    int count = 0;
    for (int i = 0; i < CANDIDATE_COUNT && count < max; i++) {
        if (display_scale_fits(width, height, CANDIDATES[i])) {
            out[count++] = CANDIDATES[i];
        }
    }
    return count;
}
