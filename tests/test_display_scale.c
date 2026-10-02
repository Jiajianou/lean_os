#include "check.h"

#include "../kernel/drivers/display_scale.c"

TEST(display_scale, the_laptop_panel_is_offered_every_scale_that_divides_it) {
    uint32_t choices[8];
    int count = display_scale_choices(3840, 2400, choices, 8);
    CHECK_EQ(count, 6);
    CHECK_EQ(choices[0], 100u);
    CHECK_EQ(choices[1], 125u);
    CHECK_EQ(choices[2], 150u);
    CHECK_EQ(choices[3], 200u);
    CHECK_EQ(choices[4], 250u);
    CHECK_EQ(choices[5], 300u);
}

TEST(display_scale, a_scale_that_leaves_a_fraction_of_a_pixel_is_refused) {
    CHECK(!display_scale_fits(3840, 2400, 175));
    CHECK(!display_scale_fits(2048, 1536, 150));
    CHECK(display_scale_fits(1920, 1440, 150));
    CHECK(display_scale_fits(1920, 1440, 125));
}

TEST(display_scale, a_desktop_smaller_than_the_minimum_is_not_offered) {
    CHECK(!display_scale_fits(1024, 768, 200));
    CHECK(!display_scale_fits(1920, 1440, 250));
    CHECK(display_scale_fits(1600, 1200, 200));
    CHECK(display_scale_fits(1024, 768, 100));
    uint32_t choices[8];
    CHECK_EQ(display_scale_choices(1024, 768, choices, 8), 1);
    CHECK_EQ(choices[0], 100u);
}

TEST(display_scale, below_one_to_one_is_never_a_scale) {
    CHECK(!display_scale_fits(3840, 2400, 50));
    CHECK(!display_scale_fits(3840, 2400, 0));
    CHECK(!display_scale_fits(0, 2400, 100));
}

TEST(display_scale, a_large_panel_is_doubled_unless_the_boot_option_says_otherwise) {
    CHECK_EQ(display_scale_automatic(3840, 2400, 0), 200u);
    CHECK_EQ(display_scale_automatic(2560, 1440, 0), 200u);
    CHECK_EQ(display_scale_automatic(1920, 1080, 0), 100u);
    CHECK_EQ(display_scale_automatic(3840, 2400, 1), 100u);
    CHECK_EQ(display_scale_automatic(1600, 1200, 2), 200u);
    CHECK_EQ(display_scale_automatic(1024, 768, 2), 100u);
}

TEST(display_scale, a_request_wins_only_while_it_still_fits_the_panel) {
    CHECK_EQ(display_scale_effective(3840, 2400, 150, 0), 150u);
    CHECK_EQ(display_scale_effective(3840, 2400, 0, 0), 200u);
    CHECK_EQ(display_scale_effective(1024, 768, 150, 0), 100u);
    CHECK_EQ(display_scale_effective(1920, 1080, 175, 0), 100u);
}

TEST(display_scale, the_list_stops_at_the_room_it_was_given) {
    uint32_t choices[2] = {0, 0};
    CHECK_EQ(display_scale_choices(3840, 2400, choices, 2), 2);
    CHECK_EQ(choices[1], 125u);
}

TEST(display_scale, a_panel_divisible_by_seven_is_offered_one_and_three_quarters) {
    uint32_t choices[8];
    int count = display_scale_choices(2800, 1750, choices, 8);
    CHECK_EQ(count, 5);
    CHECK_EQ(choices[0], 100u);
    CHECK_EQ(choices[1], 125u);
    CHECK_EQ(choices[2], 175u);
    CHECK_EQ(choices[3], 200u);
    CHECK_EQ(choices[4], 250u);
}

TEST(display_scale, one_to_one_always_fits_however_small_the_screen) {
    CHECK_EQ(display_scale_fits(640, 480, 100), 1);
    uint32_t choices[8];
    CHECK_EQ(display_scale_choices(640, 480, choices, 8), 1);
    CHECK_EQ(choices[0], 100u);
    CHECK_EQ(display_scale_fits(3840, 0, 100), 0);
    CHECK_EQ(display_scale_fits(1980, 1188, 99), 0);
}

TEST(display_scale, a_desktop_must_be_both_wide_enough_and_tall_enough) {
    CHECK(!display_scale_fits(3200, 1000, 200));
    CHECK(!display_scale_fits(1000, 3200, 200));
    CHECK(display_scale_fits(1600, 1200, 200));
}

TEST(display_scale, a_screen_is_doubled_only_when_it_is_large_in_both_directions) {
    CHECK_EQ(display_scale_automatic(1600, 1200, 0), 100u);
    CHECK_EQ(display_scale_automatic(3000, 1300, 0), 100u);
    CHECK_EQ(display_scale_automatic(2000, 1600, 0), 100u);
    CHECK_EQ(display_scale_automatic(2560, 1600, 0), 200u);
}
