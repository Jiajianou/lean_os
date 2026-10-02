#pragma once

#include <stdint.h>

#include "window_manager.h"

static inline uint32_t pointer_settings_clamp_speed(uint32_t speed) {
    if (speed < WINDOW_MANAGER_POINTER_SPEED_MINIMUM) {
        return WINDOW_MANAGER_POINTER_SPEED_MINIMUM;
    }
    if (speed > WINDOW_MANAGER_POINTER_SPEED_MAXIMUM) {
        return WINDOW_MANAGER_POINTER_SPEED_MAXIMUM;
    }
    return speed;
}

static inline int32_t pointer_settings_scale(int32_t delta, uint32_t speed, int32_t *remainder) {
    int32_t total = delta * (int32_t)pointer_settings_clamp_speed(speed) + *remainder;
    int32_t moved = total / 100;
    *remainder = total - moved * 100;
    return moved;
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
