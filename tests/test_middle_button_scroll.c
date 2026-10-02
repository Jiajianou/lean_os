#include "check.h"

#include "../kernel/drivers/middle_button_scroll.c"

TEST(middle_button_scroll, without_the_middle_button_everything_passes_through) {
    middle_button_scroll_t state = {0};
    middle_button_scroll_event_t out[2];
    REQUIRE(middle_button_scroll_filter(&state, 5, -3, 1, out) == 1);
    CHECK_EQ(out[0].dx, 5);
    CHECK_EQ(out[0].dy, -3);
    CHECK_EQ(out[0].buttons, 1);
    CHECK_EQ(out[0].wheel, 0);
}

TEST(middle_button_scroll, holding_the_middle_button_turns_movement_into_the_wheel) {
    middle_button_scroll_t state = {0};
    middle_button_scroll_event_t out[2];
    CHECK_EQ(middle_button_scroll_filter(&state, 0, 0, 4, out), 0);
    int32_t wheel = 0;
    for (int i = 0; i < 6; i++) {
        int n = middle_button_scroll_filter(&state, 1, -5, 4, out);
        for (int k = 0; k < n; k++) {
            CHECK_EQ(out[k].dx, 0);
            CHECK_EQ(out[k].dy, 0);
            CHECK_EQ(out[k].buttons & 4, 0);
            wheel += out[k].wheel;
        }
    }
    CHECK_EQ(wheel, 3);
    CHECK_EQ(middle_button_scroll_filter(&state, 0, 0, 0, out), 0);
}

TEST(middle_button_scroll, pushing_down_scrolls_the_other_way) {
    middle_button_scroll_t state = {0};
    middle_button_scroll_event_t out[2];
    int32_t wheel = 0;
    for (int i = 0; i < 4; i++) {
        if (middle_button_scroll_filter(&state, 0, 5, 4, out) == 1) {
            wheel += out[0].wheel;
        }
    }
    CHECK_EQ(wheel, -2);
}

TEST(middle_button_scroll, a_middle_press_that_never_moved_is_a_middle_click) {
    middle_button_scroll_t state = {0};
    middle_button_scroll_event_t out[2];
    CHECK_EQ(middle_button_scroll_filter(&state, 0, 0, 4, out), 0);
    CHECK_EQ(middle_button_scroll_filter(&state, 1, 1, 4, out), 0);
    REQUIRE(middle_button_scroll_filter(&state, 0, 0, 0, out) == 2);
    CHECK_EQ(out[0].buttons, 4);
    CHECK_EQ(out[1].buttons, 0);
    REQUIRE(middle_button_scroll_filter(&state, 2, 2, 0, out) == 1);
    CHECK_EQ(out[0].dx, 2);
}

TEST(middle_button_scroll, a_press_that_scrolled_sends_no_click_when_let_go) {
    middle_button_scroll_t state = {0};
    middle_button_scroll_event_t out[2];
    middle_button_scroll_filter(&state, 0, -20, 4, out);
    CHECK_EQ(middle_button_scroll_filter(&state, 0, 0, 0, out), 0);
    REQUIRE(middle_button_scroll_filter(&state, 0, 0, 1, out) == 1);
    CHECK_EQ(out[0].buttons, 1);
}

TEST(middle_button_scroll, the_other_buttons_still_reach_the_desktop_while_scrolling) {
    middle_button_scroll_t state = {0};
    middle_button_scroll_event_t out[2];
    middle_button_scroll_filter(&state, 0, 0, 4, out);
    REQUIRE(middle_button_scroll_filter(&state, 0, 0, 5, out) == 1);
    CHECK_EQ(out[0].buttons, 1);
    REQUIRE(middle_button_scroll_filter(&state, 0, 0, 4, out) == 1);
    CHECK_EQ(out[0].buttons, 0);
}

TEST(middle_button_scroll, a_little_tremor_is_still_a_click_and_a_little_more_is_a_scroll) {
    middle_button_scroll_t state = {0};
    middle_button_scroll_event_t out[2];
    middle_button_scroll_filter(&state, 0, 0, 4, out);
    middle_button_scroll_filter(&state, 1, 0, 4, out);
    middle_button_scroll_filter(&state, 0, 2, 4, out);
    CHECK_EQ(middle_button_scroll_filter(&state, 0, 0, 0, out), 2);
    middle_button_scroll_filter(&state, 0, 0, 4, out);
    middle_button_scroll_filter(&state, 2, 2, 4, out);
    CHECK_EQ(middle_button_scroll_filter(&state, 0, 0, 0, out), 0);
}
