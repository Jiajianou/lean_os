#include "check.h"

#include "../user_space/library/pointer_settings.h"

TEST(pointer_settings, the_default_speed_moves_the_cursor_exactly_as_far_as_the_mouse) {
    int32_t remainder = 0;
    for (int32_t delta = -40; delta <= 40; delta++) {
        CHECK_EQ(pointer_settings_scale(delta, WINDOW_MANAGER_POINTER_SPEED_DEFAULT, &remainder), delta);
        CHECK_EQ(remainder, 0);
    }
}

TEST(pointer_settings, a_slow_pointer_still_moves_on_small_steps) {
    int32_t remainder = 0;
    int32_t travelled = 0;
    for (int i = 0; i < 100; i++) {
        travelled += pointer_settings_scale(1, 50, &remainder);
    }
    CHECK_EQ(travelled, 50);
}

TEST(pointer_settings, the_carried_fraction_does_not_drift_either_way) {
    int32_t remainder = 0;
    int32_t travelled = 0;
    for (int i = 0; i < 300; i++) {
        travelled += pointer_settings_scale(i % 2 ? 3 : -3, 150, &remainder);
    }
    CHECK(travelled >= -1 && travelled <= 1);
    remainder = 0;
    CHECK_EQ(pointer_settings_scale(10, 300, &remainder), 30);
    CHECK_EQ(pointer_settings_scale(10, 100000, &remainder), 30);
    CHECK_EQ(pointer_settings_scale(100, 0, &remainder), 25);
}

TEST(pointer_settings, swapping_exchanges_only_the_two_main_buttons) {
    CHECK_EQ(pointer_settings_buttons(1, 1), 2);
    CHECK_EQ(pointer_settings_buttons(2, 1), 1);
    CHECK_EQ(pointer_settings_buttons(3, 1), 3);
    CHECK_EQ(pointer_settings_buttons(4, 1), 4);
    CHECK_EQ(pointer_settings_buttons(5, 1), 6);
    CHECK_EQ(pointer_settings_buttons(1, 0), 1);
}

TEST(pointer_settings, natural_scrolling_turns_the_wheel_around) {
    CHECK_EQ(pointer_settings_wheel(3, 1), -3);
    CHECK_EQ(pointer_settings_wheel(-1, 1), 1);
    CHECK_EQ(pointer_settings_wheel(3, 0), 3);
}

TEST(pointer_settings, no_acceleration_is_no_gain_at_any_speed) {
    CHECK_EQ(pointer_settings_gain(1, 0, 8, 0), 100);
    CHECK_EQ(pointer_settings_gain(400, 300, 1, 0), 100);
}

/* M218: this was named slow_movement_..., and the runner takes a name that
   starts with slow_ for a slow test and skips it in every tier - so it had
   never run, and its last line asked for a pause of four seconds to count
   as slow, which the gap cap beside it deliberately does not do. */
TEST(pointer_settings, a_movement_under_the_threshold_keeps_its_precision_with_acceleration_on) {
    CHECK_EQ(pointer_settings_gain(8, 0, 8, 100), 100);
    CHECK_EQ(pointer_settings_gain(0, -8, 8, 100), 100);
    CHECK_EQ(pointer_settings_gain(16, 0, 20, 50), 100);
    CHECK_EQ(pointer_settings_gain(50, 0, 50, 100), 100);
    CHECK(pointer_settings_gain(51, 0, 50, 100) > 100);
}

TEST(pointer_settings, faster_movement_goes_further_and_never_past_the_ceiling) {
    uint32_t previous = 100;
    for (int32_t distance = 9; distance <= 400; distance++) {
        uint32_t gain = pointer_settings_gain(distance, 0, 8, 50);
        CHECK(gain >= previous);
        CHECK(gain <= 100 + 50 * POINTER_SETTINGS_ACCELERATION_CEILING);
        previous = gain;
    }
    CHECK(pointer_settings_gain(40, 0, 8, 50) > 100);
    CHECK_EQ(pointer_settings_gain(400, 0, 1, 50), 100 + 50 * POINTER_SETTINGS_ACCELERATION_CEILING);
    CHECK(pointer_settings_gain(40, 0, 8, 100) > pointer_settings_gain(40, 0, 8, 25));
}

TEST(pointer_settings, a_pause_counts_as_the_slowest_gap_not_as_a_crawl) {
    CHECK_EQ(pointer_settings_gain(100, 0, 5000, 50), pointer_settings_gain(100, 0, POINTER_SETTINGS_SLOWEST_GAP_MS, 50));
    CHECK(pointer_settings_gain(100, 0, 5000, 50) > 100);
    CHECK_EQ(pointer_settings_gain(10, 0, 0, 50), pointer_settings_gain(10, 0, 1, 50));
}

TEST(pointer_settings, diagonal_movement_is_measured_as_one_distance) {
    CHECK_EQ(pointer_settings_gain(30, 40, 20, 50), pointer_settings_gain(40, 30, 20, 50));
    CHECK_EQ(pointer_settings_gain(-30, -40, 20, 50), pointer_settings_gain(30, 40, 20, 50));
    CHECK(pointer_settings_gain(30, 40, 20, 50) > pointer_settings_gain(40, 0, 20, 50));
}

TEST(pointer_settings, scroll_speed_scales_whole_clicks_and_carries_the_rest) {
    int32_t remainder = 0;
    CHECK_EQ(pointer_settings_scroll(1, 100, &remainder), 1);
    CHECK_EQ(pointer_settings_scroll(1, 300, &remainder), 3);
    int32_t total = 0;
    for (int i = 0; i < 8; i++) {
        total += pointer_settings_scroll(1, 50, &remainder);
    }
    CHECK_EQ(total, 4);
    total = 0;
    for (int i = 0; i < 8; i++) {
        total += pointer_settings_scroll(-1, 25, &remainder);
    }
    CHECK_EQ(total, -2);
    CHECK_EQ(pointer_settings_scroll(1, 0, &remainder), 0);
    CHECK_EQ(pointer_settings_clamp_scroll_speed(100000), WINDOW_MANAGER_SCROLL_SPEED_MAXIMUM);
}

static mouse_event_t event_at(int32_t dx, int32_t dy, int32_t wheel, uint8_t buttons, uint8_t flags, uint32_t ms) {
    mouse_event_t event = {0};
    event.dx = dx;
    event.dy = dy;
    event.wheel = wheel;
    event.buttons = buttons;
    event.flags = flags;
    event.time_ms = ms;
    return event;
}

TEST(pointer_settings, a_tap_is_dropped_only_when_tap_to_click_is_off) {
    pointer_profile_t profile = {100, 0, 0, 100, 0};
    pointer_motion_t motion = {0};
    mouse_event_t tap = event_at(0, 0, 0, 1, MOUSE_FLAG_TAP, 10);
    CHECK_EQ(pointer_settings_apply(&profile, 0, &motion, &tap), 0);
    mouse_event_t press = event_at(0, 0, 0, 1, 0, 20);
    CHECK_EQ(pointer_settings_apply(&profile, 0, &motion, &press), 1);
    CHECK_EQ(press.buttons, 1);
    profile.tap_to_click = 1;
    tap = event_at(0, 0, 0, 1, MOUSE_FLAG_TAP, 30);
    CHECK_EQ(pointer_settings_apply(&profile, 1, &motion, &tap), 1);
    CHECK_EQ(tap.buttons, 2);
}

TEST(pointer_settings, a_profile_carries_its_own_speed_direction_and_scroll) {
    pointer_profile_t mouse = {100, 0, 0, 100, 1};
    pointer_profile_t trackpad = {200, 0, 1, 300, 1};
    pointer_motion_t mouse_motion = {0};
    pointer_motion_t trackpad_motion = {0};
    mouse_event_t a = event_at(5, -5, 1, 0, 0, 100);
    mouse_event_t b = event_at(5, -5, 1, 0, 0, 100);
    pointer_settings_apply(&mouse, 0, &mouse_motion, &a);
    pointer_settings_apply(&trackpad, 0, &trackpad_motion, &b);
    CHECK_EQ(a.dx, 5);
    CHECK_EQ(a.dy, -5);
    CHECK_EQ(a.wheel, 1);
    CHECK_EQ(b.dx, 10);
    CHECK_EQ(b.dy, -10);
    CHECK_EQ(b.wheel, -3);
}

TEST(pointer_settings, acceleration_multiplies_with_the_speed) {
    pointer_profile_t profile = {200, 100, 0, 100, 1};
    pointer_motion_t motion = {0};
    mouse_event_t slow = event_at(4, 0, 0, 0, 0, 100);
    pointer_settings_apply(&profile, 0, &motion, &slow);
    CHECK_EQ(slow.dx, 8);
    mouse_event_t fast = event_at(100, 0, 0, 0, 0, 120);
    pointer_settings_apply(&profile, 0, &motion, &fast);
    uint32_t gain = pointer_settings_gain(100, 0, 20, 100);
    CHECK(gain > 100);
    CHECK_EQ(fast.dx, (int32_t)(100 * (200 * gain / 100) / 100));
}

TEST(pointer_settings, a_tap_and_drag_still_moves_when_tap_to_click_is_off) {
    pointer_profile_t profile = {100, 0, 0, 100, 0};
    pointer_motion_t motion = {0, 0, 0, 0};
    mouse_event_t press = event_at(0, 0, 0, 1, MOUSE_FLAG_TAP, 10);
    CHECK_EQ(pointer_settings_apply(&profile, 0, &motion, &press), 0);
    mouse_event_t drag = event_at(12, -5, 0, 1, MOUSE_FLAG_TAP, 20);
    CHECK_EQ(pointer_settings_apply(&profile, 0, &motion, &drag), 1);
    CHECK_EQ(drag.buttons, 0);
    CHECK_EQ(drag.dx, 12);
    CHECK_EQ(drag.dy, -5);
    profile.tap_to_click = 1;
    drag = event_at(12, -5, 0, 1, MOUSE_FLAG_TAP, 30);
    CHECK_EQ(pointer_settings_apply(&profile, 0, &motion, &drag), 1);
    CHECK_EQ(drag.buttons, 1);
}
