#include "touchpad_gestures.h"

static int32_t absolute(int32_t v) {
    return v < 0 ? -v : v;
}

void touchpad_gestures_init(touchpad_gestures_t *state, const hid_touchpad_layout_t *layout) {
    *state = (touchpad_gestures_t){0};
    int32_t x_range = layout->x_maximum - layout->x_minimum;
    int32_t y_range = layout->y_maximum - layout->y_minimum;
    state->x_range = x_range > 0 ? (uint32_t)x_range : 1;
    state->y_range = y_range > 0 ? (uint32_t)y_range : 1;
    state->width_tenths_mm = layout->width_tenths_mm ? layout->width_tenths_mm : TOUCHPAD_DEFAULT_WIDTH_TENTHS_MM;
    state->height_tenths_mm =
        layout->height_tenths_mm ? layout->height_tenths_mm : TOUCHPAD_DEFAULT_HEIGHT_TENTHS_MM;
}

/* A Precision Touchpad may report one finger at a time - "hybrid" mode,
   which is what a pad whose largest report is fourteen bytes is doing - so a
   report is a piece of a frame rather than a frame. The first report of a
   frame carries how many contacts the frame has; the rest carry zero. */
int touchpad_gestures_report(touchpad_gestures_t *state, const hid_touchpad_report_t *report, uint32_t now_ms,
                             touchpad_event_t *out) {
    if (!report->has_contact_count || report->contact_count > 0 || state->frame_collected >= state->frame_expected) {
        uint8_t expected = report->has_contact_count ? report->contact_count : report->slots;
        state->frame_expected = expected > HID_TOUCHPAD_MAX_CONTACTS ? HID_TOUCHPAD_MAX_CONTACTS : expected;
        state->frame_collected = 0;
        state->frame_points = 0;
        state->frame_button = 0;
    }
    state->frame_button |= report->button;
    for (uint8_t slot = 0; slot < report->slots && state->frame_collected < state->frame_expected; slot++) {
        const hid_touchpad_contact_t *contact = &report->contacts[slot];
        state->frame_collected++;
        if (contact->touching && contact->confident) {
            touchpad_point_t *point = &state->frame[state->frame_points++];
            point->id = contact->id;
            point->x = contact->x;
            point->y = contact->y;
        }
    }
    if (state->frame_collected < state->frame_expected) {
        return 0;
    }
    return touchpad_gestures_frame(state, state->frame, state->frame_points, state->frame_button, now_ms, out);
}

static int32_t carry(int64_t numerator, int64_t denominator, int64_t *remainder) {
    int64_t total = numerator + *remainder;
    int64_t moved = total / denominator;
    *remainder = total - moved * denominator;
    return (int32_t)moved;
}

int touchpad_gestures_frame(touchpad_gestures_t *state, const touchpad_point_t *points, uint8_t count,
                            uint8_t button, uint32_t now_ms, touchpad_event_t *out) {
    int events = 0;
    if (count > HID_TOUCHPAD_MAX_CONTACTS) {
        count = HID_TOUCHPAD_MAX_CONTACTS;
    }

    if (count > 0 && !state->touching) {
        state->touching = 1;
        state->touch_started_ms = now_ms;
        state->touch_most_fingers = 0;
        state->touch_travel_tenths_mm = 0;
        state->touch_clicked = 0;
        state->touch_scrolled = 0;
        state->remainder_x = 0;
        state->remainder_y = 0;
        state->remainder_scroll = 0;
        /* M218: tap and drag. A finger that comes down again just after a
           one-finger tap holds the left button for as long as it stays, so
           sliding it drags - a window by its title, a slider, a file - without
           pressing the pad. Lifted at once it is a second click: a double tap
           is a double click, pressed as the finger lands. */
        if (count == 1 && state->tap_ended && now_ms - state->tap_ended_ms <= TOUCHPAD_TAP_DRAG_MS) {
            state->tap_dragging = 1;
            state->touch_clicked = 1;
        }
        state->tap_ended = 0;
    }
    if (count > state->touch_most_fingers) {
        state->touch_most_fingers = count;
    }

    /* Two fingers on the pad when it is pressed is the secondary click, and
       the choice is made at the press: a second finger arriving mid-drag does
       not turn a drag into a right-button drag. */
    if (button && !state->button_held) {
        state->button_held = 1;
        state->button_mask = count >= 2 ? 2 : 1;
        state->touch_clicked = 1;
    } else if (!button && state->button_held) {
        state->button_held = 0;
        state->button_mask = 0;
    }

    int32_t sum_dy = 0;
    int matched = 0;
    int32_t best_dx = 0;
    int32_t best_dy = 0;
    int32_t best_size = -1;
    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t j = 0; j < state->previous_count; j++) {
            if (state->previous[j].id != points[i].id) {
                continue;
            }
            int32_t dx = points[i].x - state->previous[j].x;
            int32_t dy = points[i].y - state->previous[j].y;
            state->touch_travel_tenths_mm +=
                (uint32_t)((int64_t)absolute(dx) * state->width_tenths_mm / state->x_range +
                           (int64_t)absolute(dy) * state->height_tenths_mm / state->y_range);
            sum_dy += dy;
            matched++;
            if (absolute(dx) + absolute(dy) > best_size) {
                best_size = absolute(dx) + absolute(dy);
                best_dx = dx;
                best_dy = dy;
            }
            break;
        }
    }

    int32_t move_x = 0;
    int32_t move_y = 0;
    int32_t wheel = 0;
    if (matched > 0 && (count == 1 || state->button_held || state->tap_dragging)) {
        move_x = carry((int64_t)best_dx * state->width_tenths_mm * TOUCHPAD_COUNTS_PER_MM,
                       (int64_t)state->x_range * 10, &state->remainder_x);
        move_y = carry((int64_t)best_dy * state->height_tenths_mm * TOUCHPAD_COUNTS_PER_MM,
                       (int64_t)state->y_range * 10, &state->remainder_y);
    } else if (matched >= 2 && count >= 2) {
        /* Fingers moving up the pad are the wheel rolled away from you, which
           is what a positive wheel is everywhere in this system; which way the
           content then goes is the compositor's natural-scrolling setting. */
        int32_t detents = carry((int64_t)(sum_dy / matched) * state->height_tenths_mm,
                                (int64_t)state->y_range * TOUCHPAD_SCROLL_TENTHS_MM_PER_DETENT,
                                &state->remainder_scroll);
        wheel = -detents;
        if (sum_dy != 0) {
            state->touch_scrolled = 1;
        }
    }

    for (uint8_t i = 0; i < count; i++) {
        state->previous[i] = points[i];
    }
    state->previous_count = count;

    int dragged_by_tap = state->tap_dragging;
    if (count == 0 && state->tap_dragging) {
        state->tap_dragging = 0;
    }
    uint8_t pressed = (uint8_t)(state->button_mask | (state->tap_dragging ? 1u : 0u));
    if (move_x != 0 || move_y != 0 || wheel != 0 || state->buttons_out != pressed) {
        out[events].dx = move_x;
        out[events].dy = move_y;
        out[events].wheel = wheel;
        out[events].buttons = pressed;
        out[events].tap = (uint8_t)dragged_by_tap;
        events++;
        state->buttons_out = pressed;
    }

    if (count == 0 && state->touching) {
        state->touching = 0;
        int quick = now_ms - state->touch_started_ms <= TOUCHPAD_TAP_MAXIMUM_MS;
        int still = state->touch_travel_tenths_mm <= TOUCHPAD_TAP_MAXIMUM_TRAVEL_TENTHS_MM;
        if (quick && still && !state->touch_clicked && !state->touch_scrolled && state->touch_most_fingers > 0) {
            uint8_t tapped = state->touch_most_fingers == 1 ? 1 : state->touch_most_fingers == 2 ? 2 : 4;
            state->tap_ended = tapped == 1;
            state->tap_ended_ms = now_ms;
            out[events].dx = 0;
            out[events].dy = 0;
            out[events].wheel = 0;
            out[events].buttons = (uint8_t)(state->buttons_out | tapped);
            out[events].tap = 1;
            events++;
            out[events].dx = 0;
            out[events].dy = 0;
            out[events].wheel = 0;
            out[events].buttons = state->buttons_out;
            out[events].tap = 1;
            events++;
        }
    }
    return events;
}
