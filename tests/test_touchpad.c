#include "check.h"

#include <string.h>

#include "drivers/hid_report.h"
#include "drivers/i2c_hid.h"
#include "drivers/touchpad_gestures.h"

/* A pad in the shape the Precision Touchpad specification's samples have and
   the laptop's Elan part reports in: a mouse collection first, then a touch
   pad that carries one finger a report ("hybrid"), then the configuration
   collection whose Input Mode feature is what switches it. X is 4095 units
   over 120 mm and Y 4095 over 80 mm. */
static const uint8_t hybrid_pad[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x02, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0,

    0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x04,
    0x09, 0x22, 0xA1, 0x02,
    0x15, 0x00, 0x25, 0x01, 0x09, 0x47, 0x09, 0x42, 0x95, 0x02, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x02, 0x25, 0x02, 0x09, 0x51, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x04, 0x81, 0x03,
    0x05, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x0F, 0x75, 0x10, 0x55, 0x0E, 0x65, 0x11,
    0x09, 0x30, 0x35, 0x00, 0x46, 0xB0, 0x04, 0x95, 0x01, 0x81, 0x02,
    0x46, 0x20, 0x03, 0x09, 0x31, 0x81, 0x02,
    0xC0,
    0x55, 0x0C, 0x66, 0x01, 0x10, 0x47, 0xFF, 0xFF, 0x00, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x01, 0x05, 0x0D, 0x09, 0x56, 0x81, 0x02,
    0x09, 0x54, 0x25, 0x7F, 0x95, 0x01, 0x75, 0x08, 0x81, 0x02,
    0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x95, 0x07, 0x81, 0x03,
    0x05, 0x0D, 0x85, 0x02, 0x09, 0x55, 0x09, 0x59, 0x75, 0x04, 0x95, 0x02, 0x25, 0x0F, 0xB1, 0x02,
    0xC0,

    0x05, 0x0D, 0x09, 0x0E, 0xA1, 0x01, 0x85, 0x03,
    0x09, 0x22, 0xA1, 0x02, 0x09, 0x52, 0x15, 0x00, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02, 0xC0,
    0x09, 0x22, 0xA1, 0x00, 0x85, 0x05, 0x09, 0x57, 0x09, 0x58, 0x75, 0x01, 0x95, 0x02, 0x25, 0x01,
    0xB1, 0x02, 0x95, 0x06, 0xB1, 0x03, 0xC0,
    0xC0,
};

/* The other shape: two fingers in every report, sizes in inches, the
   finger's globals saved and restored with Push and Pop, and the switches in
   the same feature report as the input mode. 3000 units over 4.00 in. */
static const uint8_t parallel_pad[] = {
    0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x07,
    0x09, 0x22, 0xA1, 0x02,
    0xA4,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x09, 0x42, 0x81, 0x02,
    0x09, 0x47, 0x81, 0x02, 0x75, 0x06, 0x81, 0x03,
    0x75, 0x08, 0x25, 0x0F, 0x09, 0x51, 0x81, 0x02,
    0x05, 0x01, 0x26, 0xB8, 0x0B, 0x75, 0x10, 0x55, 0x0E, 0x65, 0x13, 0x35, 0x00, 0x46, 0x90, 0x01,
    0x09, 0x30, 0x81, 0x02, 0x46, 0x2C, 0x01, 0x26, 0x08, 0x07, 0x09, 0x31, 0x81, 0x02,
    0xB4,
    0xC0,
    0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02,
    0xA4,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x09, 0x42, 0x81, 0x02,
    0x09, 0x47, 0x81, 0x02, 0x75, 0x06, 0x81, 0x03,
    0x75, 0x08, 0x25, 0x0F, 0x09, 0x51, 0x81, 0x02,
    0x05, 0x01, 0x26, 0xB8, 0x0B, 0x75, 0x10, 0x55, 0x0E, 0x65, 0x13, 0x35, 0x00, 0x46, 0x90, 0x01,
    0x09, 0x30, 0x81, 0x02, 0x46, 0x2C, 0x01, 0x26, 0x08, 0x07, 0x09, 0x31, 0x81, 0x02,
    0xB4,
    0xC0,
    0x05, 0x0D, 0x15, 0x00, 0x25, 0x05, 0x75, 0x08, 0x95, 0x01, 0x09, 0x54, 0x81, 0x02,
    0xC0,
    0x05, 0x0D, 0x09, 0x0E, 0xA1, 0x01, 0x85, 0x09,
    0x09, 0x52, 0x15, 0x00, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02,
    0x09, 0x57, 0x09, 0x58, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0xB1, 0x02, 0x95, 0x06, 0xB1, 0x03,
    0xC0,
};

static hid_touchpad_layout_t hybrid_layout(void) {
    hid_touchpad_layout_t layout;
    REQUIRE(hid_report_find_touchpad(hybrid_pad, sizeof(hybrid_pad), &layout) == 1);
    return layout;
}

static uint32_t hybrid_report(uint8_t *out, int confident, int touching, uint8_t id, uint16_t x, uint16_t y,
                              uint8_t count, int button) {
    out[0] = 0x04;
    out[1] = (uint8_t)((confident ? 1 : 0) | (touching ? 2 : 0) | (id << 2));
    out[2] = (uint8_t)x;
    out[3] = (uint8_t)(x >> 8);
    out[4] = (uint8_t)y;
    out[5] = (uint8_t)(y >> 8);
    out[6] = 0x34;
    out[7] = 0x12;
    out[8] = count;
    out[9] = button ? 1 : 0;
    return 10;
}

TEST(touchpad, the_touchpad_collection_is_found_beside_the_mouse_one) {
    hid_touchpad_layout_t layout = hybrid_layout();
    CHECK_EQ(layout.report_id, 4);
    CHECK_EQ(layout.finger_count, 1);
    CHECK_EQ(layout.fingers[0].confidence.offset, 0);
    CHECK_EQ(layout.fingers[0].tip.offset, 1);
    CHECK_EQ(layout.fingers[0].contact_id.offset, 2);
    CHECK_EQ(layout.fingers[0].contact_id.bits, 2);
    CHECK_EQ(layout.fingers[0].x.offset, 8);
    CHECK_EQ(layout.fingers[0].x.bits, 16);
    CHECK_EQ(layout.fingers[0].y.offset, 24);
    CHECK_EQ(layout.scan_time.offset, 40);
    CHECK_EQ(layout.contact_count.offset, 56);
    CHECK_EQ(layout.button.offset, 64);
    CHECK_EQ(layout.report_bits, 72);
    CHECK_EQ(layout.x_maximum, 4095);
    CHECK_EQ(layout.y_maximum, 4095);
}

TEST(touchpad, the_physical_size_comes_from_the_unit_and_its_exponent) {
    hid_touchpad_layout_t layout = hybrid_layout();
    CHECK_EQ(layout.width_tenths_mm, 1200);
    CHECK_EQ(layout.height_tenths_mm, 800);

    hid_touchpad_layout_t inches;
    REQUIRE(hid_report_find_touchpad(parallel_pad, sizeof(parallel_pad), &inches) == 1);
    CHECK_EQ(inches.width_tenths_mm, 1016);
    CHECK_EQ(inches.height_tenths_mm, 762);
}

TEST(touchpad, the_input_mode_and_the_switches_are_found_in_their_own_feature_reports) {
    hid_touchpad_layout_t layout = hybrid_layout();
    CHECK_EQ(layout.input_mode.present, 1);
    CHECK_EQ(layout.input_mode_report_id, 3);
    CHECK_EQ(layout.input_mode.offset, 0);
    CHECK_EQ(layout.input_mode.bits, 8);
    CHECK_EQ(layout.input_mode_report_bits, 8);
    CHECK_EQ(layout.switches_report_id, 5);
    CHECK_EQ(layout.surface_switch.offset, 0);
    CHECK_EQ(layout.button_switch.offset, 1);
    CHECK_EQ(layout.switches_report_bits, 8);
}

TEST(touchpad, the_feature_reports_that_switch_it_are_built_byte_for_byte) {
    hid_touchpad_layout_t layout = hybrid_layout();
    uint8_t payload[8];
    CHECK_EQ(hid_touchpad_build_input_mode(&layout, HID_TOUCHPAD_INPUT_MODE_TOUCHPAD, payload, sizeof(payload)), 1);
    CHECK_EQ(payload[0], 3);
    CHECK_EQ(hid_touchpad_build_input_mode(&layout, HID_TOUCHPAD_INPUT_MODE_MOUSE, payload, sizeof(payload)), 1);
    CHECK_EQ(payload[0], 0);
    CHECK_EQ(hid_touchpad_build_switches(&layout, payload, sizeof(payload)), 1);
    CHECK_EQ(payload[0], 3);
}

TEST(touchpad, switches_in_the_input_mode_report_are_set_with_it_and_not_sent_twice) {
    hid_touchpad_layout_t layout;
    REQUIRE(hid_report_find_touchpad(parallel_pad, sizeof(parallel_pad), &layout) == 1);
    CHECK_EQ(layout.input_mode_report_id, 9);
    CHECK_EQ(layout.switches_report_id, 9);
    uint8_t payload[8];
    CHECK_EQ(hid_touchpad_build_input_mode(&layout, HID_TOUCHPAD_INPUT_MODE_TOUCHPAD, payload, sizeof(payload)), 2);
    CHECK_EQ(payload[0], 3);
    CHECK_EQ(payload[1], 3);
    CHECK_EQ(hid_touchpad_build_switches(&layout, payload, sizeof(payload)), 0);
}

TEST(touchpad, push_and_pop_give_every_finger_the_same_fields) {
    hid_touchpad_layout_t layout;
    REQUIRE(hid_report_find_touchpad(parallel_pad, sizeof(parallel_pad), &layout) == 1);
    CHECK_EQ(layout.report_id, 7);
    CHECK_EQ(layout.finger_count, 2);
    CHECK_EQ(layout.fingers[0].tip.offset, 0);
    CHECK_EQ(layout.fingers[0].contact_id.offset, 8);
    CHECK_EQ(layout.fingers[0].x.offset, 16);
    CHECK_EQ(layout.fingers[1].tip.offset, 48);
    CHECK_EQ(layout.fingers[1].contact_id.offset, 56);
    CHECK_EQ(layout.fingers[1].x.offset, 64);
    CHECK_EQ(layout.fingers[1].y.offset, 80);
    CHECK_EQ(layout.contact_count.offset, 96);
    CHECK_EQ(layout.contact_count.bits, 8);
    CHECK_EQ(layout.x_maximum, 3000);
    CHECK_EQ(layout.y_maximum, 1800);
    CHECK_EQ(layout.button.present, 0);
}

TEST(touchpad, a_descriptor_without_a_touch_pad_collection_has_no_touchpad) {
    hid_touchpad_layout_t layout;
    CHECK_EQ(hid_report_find_touchpad(hybrid_pad, 50, &layout), 0);
    CHECK_EQ(hid_report_find_touchpad(hybrid_pad, 0, &layout), 0);
}

TEST(touchpad, a_descriptor_cut_short_anywhere_is_never_read_past_its_end) {
    hid_touchpad_layout_t layout;
    for (uint32_t length = 0; length < sizeof(hybrid_pad); length++) {
        int found = hid_report_find_touchpad(hybrid_pad, length, &layout);
        CHECK(found == 0 || found == 1);
    }
    for (uint32_t length = 0; length < sizeof(parallel_pad); length++) {
        hid_report_find_touchpad(parallel_pad, length, &layout);
    }
}

TEST(touchpad, a_report_decodes_to_its_contact) {
    hid_touchpad_layout_t layout = hybrid_layout();
    uint8_t bytes[16];
    uint32_t length = hybrid_report(bytes, 1, 1, 2, 1000, 3000, 1, 1);
    hid_touchpad_report_t report;
    CHECK_EQ(hid_touchpad_decode(&layout, bytes, length, &report), 1);
    CHECK_EQ(report.slots, 1);
    CHECK_EQ(report.has_contact_count, 1);
    CHECK_EQ(report.contact_count, 1);
    CHECK_EQ(report.button, 1);
    CHECK_EQ(report.contacts[0].touching, 1);
    CHECK_EQ(report.contacts[0].confident, 1);
    CHECK_EQ(report.contacts[0].id, 2);
    CHECK_EQ(report.contacts[0].x, 1000);
    CHECK_EQ(report.contacts[0].y, 3000);
}

TEST(touchpad, a_mouse_report_or_a_short_report_is_not_a_touchpad_report) {
    hid_touchpad_layout_t layout = hybrid_layout();
    uint8_t bytes[16];
    uint32_t length = hybrid_report(bytes, 1, 1, 0, 10, 10, 1, 0);
    hid_touchpad_report_t report;
    CHECK_EQ(hid_touchpad_decode(&layout, bytes, length - 1, &report), 0);
    bytes[0] = 0x01;
    CHECK_EQ(hid_touchpad_decode(&layout, bytes, length, &report), 0);
}

TEST(touchpad, set_report_carries_the_command_the_data_register_and_a_length_that_counts_itself) {
    uint8_t payload[] = {0x03};
    uint8_t out[32];
    uint32_t n = i2c_hid_build_set_feature(0x0022, 0x0023, 3, payload, sizeof(payload), out, sizeof(out));
    const uint8_t expected[] = {0x22, 0x00, 0x33, 0x03, 0x23, 0x00, 0x04, 0x00, 0x03, 0x03};
    CHECK_EQ(n, sizeof(expected));
    CHECK_MEMEQ(out, expected, sizeof(expected));
}

TEST(touchpad, a_report_id_of_fifteen_or_more_follows_the_opcode) {
    uint8_t payload[] = {0x03, 0x00};
    uint8_t out[32];
    uint32_t n = i2c_hid_build_set_feature(0x0005, 0x0006, 0x20, payload, sizeof(payload), out, sizeof(out));
    const uint8_t expected[] = {0x05, 0x00, 0x3F, 0x03, 0x20, 0x06, 0x00, 0x05, 0x00, 0x20, 0x03, 0x00};
    CHECK_EQ(n, sizeof(expected));
    CHECK_MEMEQ(out, expected, sizeof(expected));
    CHECK_EQ(i2c_hid_build_set_feature(0x0005, 0x0006, 0x20, payload, sizeof(payload), out, 11), 0);
}

static touchpad_gestures_t gestures_for_hybrid(void) {
    hid_touchpad_layout_t layout = hybrid_layout();
    touchpad_gestures_t state;
    touchpad_gestures_init(&state, &layout);
    return state;
}

static int one(touchpad_gestures_t *state, int32_t x, int32_t y, uint8_t button, uint32_t at,
               touchpad_event_t *out) {
    touchpad_point_t point = {0, x, y};
    return touchpad_gestures_frame(state, &point, 1, button, at, out);
}

static int two(touchpad_gestures_t *state, int32_t x, int32_t y, int32_t x2, int32_t y2, uint8_t button,
               uint32_t at, touchpad_event_t *out) {
    touchpad_point_t points[2] = {{0, x, y}, {1, x2, y2}};
    return touchpad_gestures_frame(state, points, 2, button, at, out);
}

static int lift(touchpad_gestures_t *state, uint32_t at, touchpad_event_t *out) {
    return touchpad_gestures_frame(state, 0, 0, 0, at, out);
}

TEST(touchpad, one_finger_moves_the_pointer_twelve_counts_a_millimetre) {
    touchpad_gestures_t state = gestures_for_hybrid();
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    CHECK_EQ(one(&state, 1000, 1000, 0, 0, events), 0);
    int32_t moved_x = 0;
    int32_t moved_y = 0;
    for (int step = 1; step <= 10; step++) {
        int n = one(&state, 1000 + 34 * step, 1000 + 51 * step, 0, (uint32_t)(8 * step), events);
        for (int i = 0; i < n; i++) {
            moved_x += events[i].dx;
            moved_y += events[i].dy;
            CHECK_EQ(events[i].wheel, 0);
            CHECK_EQ(events[i].buttons, 0);
        }
    }
    CHECK(moved_x >= 118 && moved_x <= 120);
    CHECK(moved_y >= 118 && moved_y <= 120);
}

TEST(touchpad, the_first_frame_of_a_touch_does_not_jump_the_pointer) {
    touchpad_gestures_t state = gestures_for_hybrid();
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    CHECK_EQ(one(&state, 100, 100, 0, 0, events), 0);
    CHECK_EQ(lift(&state, 400, events), 0);
    CHECK_EQ(one(&state, 4000, 4000, 0, 1000, events), 0);
}

TEST(touchpad, two_fingers_up_the_pad_are_the_wheel_rolled_away) {
    touchpad_gestures_t state = gestures_for_hybrid();
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    two(&state, 1000, 3000, 1500, 3000, 0, 0, events);
    int32_t wheel = 0;
    for (int step = 1; step <= 10; step++) {
        int n = two(&state, 1000, 3000 - 52 * step, 1500, 3000 - 52 * step, 0, (uint32_t)(10 * step), events);
        for (int i = 0; i < n; i++) {
            CHECK_EQ(events[i].dx, 0);
            CHECK_EQ(events[i].dy, 0);
            wheel += events[i].wheel;
        }
    }
    CHECK_EQ(wheel, 4);
    two(&state, 1000, 3000, 1500, 3000, 0, 200, events);
    CHECK_EQ(lift(&state, 400, events), 0);
}

TEST(touchpad, two_fingers_down_the_pad_scroll_the_other_way) {
    touchpad_gestures_t state = gestures_for_hybrid();
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    two(&state, 1000, 1000, 1500, 1000, 0, 0, events);
    int32_t wheel = 0;
    for (int step = 1; step <= 5; step++) {
        int n = two(&state, 1000, 1000 + 104 * step, 1500, 1000 + 104 * step, 0, (uint32_t)(10 * step), events);
        for (int i = 0; i < n; i++) {
            wheel += events[i].wheel;
        }
    }
    CHECK_EQ(wheel, -4);
}

TEST(touchpad, a_quick_still_touch_is_a_left_click) {
    touchpad_gestures_t state = gestures_for_hybrid();
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    one(&state, 2000, 2000, 0, 0, events);
    one(&state, 2005, 2003, 0, 40, events);
    int n = lift(&state, 90, events);
    REQUIRE(n == 2);
    CHECK_EQ(events[0].buttons, 1);
    CHECK_EQ(events[0].tap, 1);
    CHECK_EQ(events[1].buttons, 0);
    CHECK_EQ(events[1].tap, 1);
    for (int i = 0; i < 2; i++) {
        CHECK_EQ(events[i].dx, 0);
        CHECK_EQ(events[i].dy, 0);
        CHECK_EQ(events[i].wheel, 0);
    }
}

TEST(touchpad, two_fingers_resting_still_for_a_few_frames_are_still_a_tap) {
    touchpad_gestures_t state = gestures_for_hybrid();
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    for (uint32_t at = 0; at <= 60; at += 10) {
        CHECK_EQ(two(&state, 2000, 2000, 2400, 2000, 0, at, events), 0);
    }
    int n = lift(&state, 80, events);
    REQUIRE(n == 2);
    CHECK_EQ(events[0].buttons, 2);
}

TEST(touchpad, a_report_naming_a_new_count_starts_a_new_frame_even_mid_frame) {
    hid_touchpad_layout_t layout = hybrid_layout();
    touchpad_gestures_t state;
    touchpad_gestures_init(&state, &layout);
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    uint8_t bytes[16];
    hid_touchpad_report_t report;
    hybrid_report(bytes, 1, 1, 0, 1000, 1000, 2, 0);
    REQUIRE(hid_touchpad_decode(&layout, bytes, 10, &report));
    touchpad_gestures_report(&state, &report, 0, events);
    CHECK_EQ(state.frame_collected, 1);
    hybrid_report(bytes, 1, 1, 1, 1500, 1000, 1, 0);
    REQUIRE(hid_touchpad_decode(&layout, bytes, 10, &report));
    touchpad_gestures_report(&state, &report, 10, events);
    CHECK_EQ(state.frame_expected, 1);
    CHECK_EQ(state.previous_count, 1);
    CHECK_EQ(state.previous[0].id, 1);
}

TEST(touchpad, a_pad_with_no_range_does_not_divide_by_zero) {
    hid_touchpad_layout_t layout = hybrid_layout();
    layout.x_maximum = layout.x_minimum;
    layout.y_maximum = layout.y_minimum;
    touchpad_gestures_t state;
    touchpad_gestures_init(&state, &layout);
    CHECK_EQ(state.x_range, 1);
    CHECK_EQ(state.y_range, 1);
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    one(&state, 0, 0, 0, 0, events);
    int n = one(&state, 1, 1, 0, 10, events);
    REQUIRE(n == 1);
    CHECK_EQ(events[0].dx, TOUCHPAD_COUNTS_PER_MM * 120);
    CHECK_EQ(events[0].dy, TOUCHPAD_COUNTS_PER_MM * 80);
}

TEST(touchpad, a_two_finger_tap_is_the_secondary_button_and_three_the_middle) {
    touchpad_gestures_t state = gestures_for_hybrid();
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    two(&state, 2000, 2000, 2400, 2000, 0, 0, events);
    int n = lift(&state, 80, events);
    REQUIRE(n == 2);
    CHECK_EQ(events[0].buttons, 2);

    touchpad_point_t three[3] = {{0, 1000, 1000}, {1, 1400, 1000}, {2, 1800, 1000}};
    touchpad_gestures_frame(&state, three, 3, 0, 1000, events);
    n = lift(&state, 1100, events);
    REQUIRE(n == 2);
    CHECK_EQ(events[0].buttons, 4);
}

TEST(touchpad, a_long_touch_or_a_moving_one_is_not_a_tap) {
    touchpad_gestures_t state = gestures_for_hybrid();
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    one(&state, 2000, 2000, 0, 0, events);
    CHECK_EQ(lift(&state, TOUCHPAD_TAP_MAXIMUM_MS + 1, events), 0);

    one(&state, 2000, 2000, 0, 1000, events);
    one(&state, 2200, 2000, 0, 1020, events);
    int n = lift(&state, 1040, events);
    for (int i = 0; i < n; i++) {
        CHECK_EQ(events[i].tap, 0);
    }
}

TEST(touchpad, a_two_finger_scroll_that_comes_back_to_where_it_started_is_not_a_tap) {
    touchpad_gestures_t state = gestures_for_hybrid();
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    two(&state, 2000, 2000, 2400, 2000, 0, 0, events);
    two(&state, 2000, 2020, 2400, 2020, 0, 20, events);
    two(&state, 2000, 2000, 2400, 2000, 0, 40, events);
    CHECK_EQ(lift(&state, 60, events), 0);
}

TEST(touchpad, pressing_the_pad_clicks_and_two_fingers_down_make_it_the_secondary_button) {
    touchpad_gestures_t state = gestures_for_hybrid();
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    one(&state, 2000, 2000, 0, 0, events);
    int n = one(&state, 2000, 2000, 1, 20, events);
    REQUIRE(n == 1);
    CHECK_EQ(events[0].buttons, 1);
    n = one(&state, 2000, 2000, 0, 40, events);
    REQUIRE(n == 1);
    CHECK_EQ(events[0].buttons, 0);
    CHECK_EQ(lift(&state, 60, events), 0);

    two(&state, 2000, 2000, 2400, 2000, 0, 1000, events);
    n = two(&state, 2000, 2000, 2400, 2000, 1, 1020, events);
    REQUIRE(n == 1);
    CHECK_EQ(events[0].buttons, 2);
}

TEST(touchpad, a_press_held_while_a_finger_moves_is_a_drag) {
    touchpad_gestures_t state = gestures_for_hybrid();
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    one(&state, 2000, 2000, 1, 0, events);
    two(&state, 2000, 2000, 1000, 1000, 1, 20, events);
    int n = two(&state, 2000, 2000, 1340, 1000, 1, 40, events);
    REQUIRE(n == 1);
    CHECK_EQ(events[0].buttons, 1);
    CHECK(events[0].dx >= 118 && events[0].dx <= 120);
    CHECK_EQ(events[0].wheel, 0);
}

TEST(touchpad, hybrid_reports_are_put_back_together_into_one_frame) {
    hid_touchpad_layout_t layout = hybrid_layout();
    touchpad_gestures_t state;
    touchpad_gestures_init(&state, &layout);
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    uint8_t bytes[16];
    hid_touchpad_report_t report;

    hybrid_report(bytes, 1, 1, 0, 1000, 3000, 2, 0);
    REQUIRE(hid_touchpad_decode(&layout, bytes, 10, &report));
    CHECK_EQ(touchpad_gestures_report(&state, &report, 0, events), 0);
    CHECK_EQ(state.frame_collected, 1);
    hybrid_report(bytes, 1, 1, 1, 1500, 3000, 0, 0);
    REQUIRE(hid_touchpad_decode(&layout, bytes, 10, &report));
    CHECK_EQ(touchpad_gestures_report(&state, &report, 0, events), 0);
    CHECK_EQ(state.previous_count, 2);

    int32_t wheel = 0;
    for (int step = 1; step <= 10; step++) {
        hybrid_report(bytes, 1, 1, 0, 1000, (uint16_t)(3000 - 52 * step), 2, 0);
        REQUIRE(hid_touchpad_decode(&layout, bytes, 10, &report));
        int n = touchpad_gestures_report(&state, &report, (uint32_t)(10 * step), events);
        CHECK_EQ(n, 0);
        hybrid_report(bytes, 1, 1, 1, 1500, (uint16_t)(3000 - 52 * step), 0, 0);
        REQUIRE(hid_touchpad_decode(&layout, bytes, 10, &report));
        n = touchpad_gestures_report(&state, &report, (uint32_t)(10 * step), events);
        for (int i = 0; i < n; i++) {
            wheel += events[i].wheel;
            CHECK_EQ(events[i].dx, 0);
        }
    }
    CHECK_EQ(wheel, 4);
}

TEST(touchpad, a_palm_is_not_a_finger) {
    hid_touchpad_layout_t layout = hybrid_layout();
    touchpad_gestures_t state;
    touchpad_gestures_init(&state, &layout);
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    uint8_t bytes[16];
    hid_touchpad_report_t report;
    hybrid_report(bytes, 0, 1, 0, 1000, 1000, 1, 0);
    REQUIRE(hid_touchpad_decode(&layout, bytes, 10, &report));
    touchpad_gestures_report(&state, &report, 0, events);
    hybrid_report(bytes, 0, 1, 0, 2000, 1000, 1, 0);
    REQUIRE(hid_touchpad_decode(&layout, bytes, 10, &report));
    CHECK_EQ(touchpad_gestures_report(&state, &report, 10, events), 0);
    CHECK_EQ(state.touching, 0);
}

TEST(touchpad, a_lift_reported_with_a_count_of_zero_ends_the_touch) {
    hid_touchpad_layout_t layout = hybrid_layout();
    touchpad_gestures_t state;
    touchpad_gestures_init(&state, &layout);
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    uint8_t bytes[16];
    hid_touchpad_report_t report;
    hybrid_report(bytes, 1, 1, 0, 1000, 1000, 1, 0);
    REQUIRE(hid_touchpad_decode(&layout, bytes, 10, &report));
    touchpad_gestures_report(&state, &report, 0, events);
    CHECK_EQ(state.touching, 1);
    hybrid_report(bytes, 1, 0, 0, 1000, 1000, 0, 0);
    REQUIRE(hid_touchpad_decode(&layout, bytes, 10, &report));
    int n = touchpad_gestures_report(&state, &report, 50, events);
    CHECK_EQ(state.touching, 0);
    CHECK_EQ(n, 2);
}

TEST(touchpad, a_pad_that_does_not_say_its_size_is_taken_as_a_hundred_millimetres) {
    hid_touchpad_layout_t layout = hybrid_layout();
    layout.width_tenths_mm = 0;
    layout.height_tenths_mm = 0;
    touchpad_gestures_t state;
    touchpad_gestures_init(&state, &layout);
    CHECK_EQ(state.width_tenths_mm, TOUCHPAD_DEFAULT_WIDTH_TENTHS_MM);
    CHECK_EQ(state.height_tenths_mm, TOUCHPAD_DEFAULT_HEIGHT_TENTHS_MM);
}

TEST(touchpad, slots_past_the_contact_count_are_not_fingers) {
    hid_touchpad_layout_t layout;
    REQUIRE(hid_report_find_touchpad(parallel_pad, sizeof(parallel_pad), &layout) == 1);
    touchpad_gestures_t state;
    touchpad_gestures_init(&state, &layout);
    uint8_t bytes[14] = {0x07, 0x03, 0x00, 0xE8, 0x03, 0xE8, 0x03, 0x03, 0x01, 0xD0, 0x07, 0xD0, 0x07, 0x01};
    hid_touchpad_report_t report;
    REQUIRE(hid_touchpad_decode(&layout, bytes, sizeof(bytes), &report) == 1);
    CHECK_EQ(report.slots, 2);
    CHECK_EQ(report.contacts[1].touching, 1);
    CHECK_EQ(report.contacts[1].x, 2000);
    touchpad_event_t events[TOUCHPAD_MAX_EVENTS];
    touchpad_gestures_report(&state, &report, 0, events);
    CHECK_EQ(state.previous_count, 1);
    CHECK_EQ(state.previous[0].x, 1000);
    bytes[13] = 2;
    REQUIRE(hid_touchpad_decode(&layout, bytes, sizeof(bytes), &report) == 1);
    touchpad_gestures_report(&state, &report, 10, events);
    CHECK_EQ(state.previous_count, 2);
}

TEST(touchpad, a_second_touch_pad_collection_does_not_add_to_the_first) {
    uint8_t both[sizeof(hybrid_pad) + sizeof(parallel_pad)];
    memcpy(both, hybrid_pad, sizeof(hybrid_pad));
    memcpy(both + sizeof(hybrid_pad), parallel_pad, sizeof(parallel_pad));
    hid_touchpad_layout_t layout;
    REQUIRE(hid_report_find_touchpad(both, sizeof(both), &layout) == 1);
    CHECK_EQ(layout.report_id, 4);
    CHECK_EQ(layout.finger_count, 1);
    CHECK_EQ(layout.x_maximum, 4095);
    CHECK_EQ(layout.contact_count.offset, 56);
}

TEST(touchpad, sizes_in_other_exponents_and_units_are_converted_or_left_unknown) {
    uint8_t pad[sizeof(hybrid_pad)];
    memcpy(pad, hybrid_pad, sizeof(pad));
    uint32_t at = 0;
    while (!(pad[at] == 0x55 && pad[at + 1] == 0x0E)) {
        at++;
    }
    hid_touchpad_layout_t layout;
    pad[at + 1] = 0x0F;
    REQUIRE(hid_report_find_touchpad(pad, sizeof(pad), &layout) == 1);
    CHECK_EQ(layout.width_tenths_mm, 12000);
    pad[at + 1] = 0x0D;
    REQUIRE(hid_report_find_touchpad(pad, sizeof(pad), &layout) == 1);
    CHECK_EQ(layout.width_tenths_mm, 120);
    pad[at + 1] = 0x0E;
    pad[at + 3] = 0x12;
    REQUIRE(hid_report_find_touchpad(pad, sizeof(pad), &layout) == 1);
    CHECK_EQ(layout.width_tenths_mm, 0);
    CHECK_EQ(layout.height_tenths_mm, 0);
}
