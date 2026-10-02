#pragma once

#include <stdint.h>

#include "input.h"
#include "window_manager.h"

#define POINTER_SETTINGS_SLOWEST_GAP_MS 50
#define POINTER_SETTINGS_ACCELERATION_THRESHOLD 100
#define POINTER_SETTINGS_ACCELERATION_CEILING 3

typedef struct {
    uint32_t speed;
    uint32_t acceleration;
    uint32_t natural_scrolling;
    uint32_t scroll_speed;
    uint32_t tap_to_click;
} pointer_profile_t;

typedef struct {
    int32_t remainder_x;
    int32_t remainder_y;
    int32_t remainder_wheel;
    uint32_t last_ms;
} pointer_motion_t;

static inline uint32_t pointer_settings_clamp_speed(uint32_t speed) {
    if (speed < WINDOW_MANAGER_POINTER_SPEED_MINIMUM) {
        return WINDOW_MANAGER_POINTER_SPEED_MINIMUM;
    }
    if (speed > WINDOW_MANAGER_POINTER_SPEED_MAXIMUM) {
        return WINDOW_MANAGER_POINTER_SPEED_MAXIMUM;
    }
    return speed;
}

static inline uint32_t pointer_settings_clamp_scroll_speed(uint32_t speed) {
    if (speed < WINDOW_MANAGER_SCROLL_SPEED_MINIMUM) {
        return WINDOW_MANAGER_SCROLL_SPEED_MINIMUM;
    }
    if (speed > WINDOW_MANAGER_SCROLL_SPEED_MAXIMUM) {
        return WINDOW_MANAGER_SCROLL_SPEED_MAXIMUM;
    }
    return speed;
}

static inline uint32_t pointer_settings_clamp_acceleration(uint32_t acceleration) {
    return acceleration > WINDOW_MANAGER_ACCELERATION_MAXIMUM ? WINDOW_MANAGER_ACCELERATION_MAXIMUM : acceleration;
}

static inline int32_t pointer_settings_scale_by(int32_t delta, int32_t percent, int32_t *remainder) {
    int32_t total = delta * percent + *remainder;
    int32_t moved = total / 100;
    *remainder = total - moved * 100;
    return moved;
}

static inline int32_t pointer_settings_scale(int32_t delta, uint32_t speed, int32_t *remainder) {
    return pointer_settings_scale_by(delta, (int32_t)pointer_settings_clamp_speed(speed), remainder);
}

static inline int32_t pointer_settings_scroll(int32_t detents, uint32_t speed, int32_t *remainder) {
    return pointer_settings_scale_by(detents, (int32_t)pointer_settings_clamp_scroll_speed(speed), remainder);
}

/* M211. Acceleration is a gain on how fast the hand is moving rather than on
   how far: below about a thousand counts a second - a finger placing the
   pointer on a button - the pointer moves exactly as far as the setting says,
   and above it the gain grows with the speed, so a flick crosses the screen
   without the slow movements losing their precision. An event after a pause
   is measured against the slowest gap rather than the pause, or the first
   movement after resting would always count as slow. */
static inline uint32_t pointer_settings_gain(int32_t dx, int32_t dy, uint32_t elapsed_ms, uint32_t acceleration) {
    acceleration = pointer_settings_clamp_acceleration(acceleration);
    if (acceleration == 0) {
        return 100;
    }
    if (elapsed_ms == 0) {
        elapsed_ms = 1;
    }
    if (elapsed_ms > POINTER_SETTINGS_SLOWEST_GAP_MS) {
        elapsed_ms = POINTER_SETTINGS_SLOWEST_GAP_MS;
    }
    uint32_t ax = (uint32_t)(dx < 0 ? -dx : dx);
    uint32_t ay = (uint32_t)(dy < 0 ? -dy : dy);
    uint32_t distance = ax > ay ? ax + ay / 2 : ay + ax / 2;
    uint32_t velocity = distance * 100u / elapsed_ms;
    if (velocity <= POINTER_SETTINGS_ACCELERATION_THRESHOLD) {
        return 100;
    }
    uint32_t gain = 100u + acceleration * (velocity - POINTER_SETTINGS_ACCELERATION_THRESHOLD) / 100u;
    uint32_t ceiling = 100u + acceleration * POINTER_SETTINGS_ACCELERATION_CEILING;
    return gain > ceiling ? ceiling : gain;
}

static inline uint8_t pointer_settings_buttons(uint8_t buttons, uint32_t swap) {
    if (!swap) {
        return buttons;
    }
    return (uint8_t)((buttons & ~3u) | ((buttons & 1u) << 1) | ((buttons & 2u) >> 1));
}

static inline int32_t pointer_settings_wheel(int32_t wheel, uint32_t natural) {
    return natural ? -wheel : wheel;
}

/* Everything one pointer event goes through on its way to the cursor, for
   whichever device it came from. Returns 0 for an event that is to be
   dropped - a tap on a trackpad whose tap-to-click is off. */
static inline int pointer_settings_apply(const pointer_profile_t *profile, uint32_t swap, pointer_motion_t *motion,
                                         mouse_event_t *event) {
    if ((event->flags & MOUSE_FLAG_TAP) && !profile->tap_to_click) {
        return 0;
    }
    uint32_t elapsed = event->time_ms - motion->last_ms;
    motion->last_ms = event->time_ms;
    uint32_t gain = pointer_settings_gain(event->dx, event->dy, elapsed, profile->acceleration);
    int32_t percent = (int32_t)(pointer_settings_clamp_speed(profile->speed) * gain / 100u);
    event->buttons = pointer_settings_buttons(event->buttons, swap);
    event->wheel = pointer_settings_scroll(pointer_settings_wheel(event->wheel, profile->natural_scrolling),
                                           profile->scroll_speed, &motion->remainder_wheel);
    event->dx = pointer_settings_scale_by(event->dx, percent, &motion->remainder_x);
    event->dy = pointer_settings_scale_by(event->dy, percent, &motion->remainder_y);
    return 1;
}
