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
