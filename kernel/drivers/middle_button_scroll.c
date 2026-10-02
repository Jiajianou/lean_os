#include "middle_button_scroll.h"

#define MIDDLE 4u

static int32_t absolute(int32_t v) {
    return v < 0 ? -v : v;
}

/* M219. A ThinkPad's TrackPoint is a PS/2 mouse with three buttons and no
   wheel, and the way to scroll with it is to hold the middle button and
   push the stick - what every other system that drives one does. So on a
   pointer with no wheel, the middle button held turns the pointer's
   movement into wheel detents and the pointer stays where it is; let go
   without having moved, it is the middle click it always was, sent then
   as a press and a release. The other buttons pass through untouched.
   Returns how many events to deliver. */
int middle_button_scroll_filter(middle_button_scroll_t *state, int32_t dx, int32_t dy, uint8_t buttons,
                                middle_button_scroll_event_t out[2]) {
    uint8_t others = (uint8_t)(buttons & ~MIDDLE);
    int middle = (buttons & MIDDLE) != 0;
    if (!state->held && !middle) {
        out[0] = (middle_button_scroll_event_t){dx, dy, 0, buttons};
        state->buttons_out = buttons;
        return 1;
    }
    if (middle && !state->held) {
        state->held = 1;
        state->scrolled = 0;
        state->remainder = 0;
        state->travel = 0;
    }
    if (middle) {
        state->travel += absolute(dx) + absolute(dy);
        if (state->travel > MIDDLE_BUTTON_SCROLL_STILL_COUNTS) {
            state->scrolled = 1;
        }
        int32_t total = state->remainder - dy;
        int32_t detents = total / MIDDLE_BUTTON_SCROLL_COUNTS_PER_DETENT;
        state->remainder = total - detents * MIDDLE_BUTTON_SCROLL_COUNTS_PER_DETENT;
        if (detents == 0 && others == state->buttons_out) {
            return 0;
        }
        out[0] = (middle_button_scroll_event_t){0, 0, detents, others};
        state->buttons_out = others;
        return 1;
    }
    state->held = 0;
    if (state->scrolled) {
        if (others == state->buttons_out && dx == 0 && dy == 0) {
            return 0;
        }
        out[0] = (middle_button_scroll_event_t){dx, dy, 0, others};
        state->buttons_out = others;
        return 1;
    }
    out[0] = (middle_button_scroll_event_t){0, 0, 0, (uint8_t)(others | MIDDLE)};
    out[1] = (middle_button_scroll_event_t){dx, dy, 0, others};
    state->buttons_out = others;
    return 2;
}
