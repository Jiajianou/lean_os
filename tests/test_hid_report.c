#include "check.h"

#include "drivers/hid_report.h"

static const uint8_t precision_touchpad_mouse_collection[] = {
    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x85, 0x01,
    0x09, 0x01,
    0xA1, 0x00,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x02,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x02,
    0x81, 0x02,
    0x95, 0x06,
    0x81, 0x03,
    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x02,
    0x81, 0x06,
    0x09, 0x38,
    0x95, 0x01,
    0x81, 0x06,
    0xC0,
    0xC0,

    0x05, 0x0D,
    0x09, 0x05,
    0xA1, 0x01,
    0x85, 0x04,
    0x09, 0x22,
    0xA1, 0x02,
    0x09, 0x42,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x01,
    0x81, 0x02,
    0x95, 0x07,
    0x81, 0x03,
    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x75, 0x10,
    0x95, 0x02,
    0x81, 0x02,
    0xC0,
    0xC0,
};

static const uint8_t digitizer_only[] = {
    0x05, 0x0D,
    0x09, 0x05,
    0xA1, 0x01,
    0x85, 0x04,
    0x09, 0x42,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x01,
    0x81, 0x02,
    0xC0,
};

TEST(hid_report, the_mouse_collection_is_found_past_the_digitizer_one) {
    hid_mouse_layout_t layout;
    CHECK_EQ(hid_report_find_mouse(precision_touchpad_mouse_collection,
                                   sizeof(precision_touchpad_mouse_collection), &layout), 1);
    CHECK_EQ(layout.has_report_id, 1);
    CHECK_EQ(layout.report_id, 1);
    CHECK_EQ(layout.button_offset, 0);
    CHECK_EQ(layout.button_count, 2);
    CHECK_EQ(layout.has_x, 1);
    CHECK_EQ(layout.x_offset, 8);
    CHECK_EQ(layout.x_bits, 8);
    CHECK_EQ(layout.y_offset, 16);
    CHECK_EQ(layout.has_wheel, 1);
    CHECK_EQ(layout.wheel_offset, 24);
    CHECK_EQ(layout.report_bits, 32);
}

TEST(hid_report, the_digitizer_x_and_y_do_not_become_the_mouse_s) {
    hid_mouse_layout_t layout;
    CHECK_EQ(hid_report_find_mouse(precision_touchpad_mouse_collection,
                                   sizeof(precision_touchpad_mouse_collection), &layout), 1);
    CHECK_EQ(layout.x_bits, 8);
    CHECK_EQ(layout.report_id, 1);
}

TEST(hid_report, a_descriptor_with_no_mouse_collection_reports_none) {
    hid_mouse_layout_t layout;
    CHECK_EQ(hid_report_find_mouse(digitizer_only, sizeof(digitizer_only), &layout), 0);
}

TEST(hid_report, a_report_decodes_to_movement_buttons_and_a_wheel) {
    hid_mouse_layout_t layout;
    REQUIRE(hid_report_find_mouse(precision_touchpad_mouse_collection,
                                  sizeof(precision_touchpad_mouse_collection), &layout) == 1);

    const uint8_t report[] = {0x01, 0x01, 0x05, 0xFB, 0xFF};
    hid_mouse_report_t decoded;
    CHECK_EQ(hid_mouse_decode(&layout, report, sizeof(report), &decoded), 1);
    CHECK_EQ(decoded.buttons, 1);
    CHECK_EQ(decoded.dx, 5);
    CHECK_EQ(decoded.dy, -5);
    CHECK_EQ(decoded.wheel, -1);
}

TEST(hid_report, a_report_for_a_different_collection_is_not_decoded_as_a_mouse) {
    hid_mouse_layout_t layout;
    REQUIRE(hid_report_find_mouse(precision_touchpad_mouse_collection,
                                  sizeof(precision_touchpad_mouse_collection), &layout) == 1);

    const uint8_t touch_report[] = {0x04, 0x01, 0x40, 0x06, 0x80, 0x04};
    hid_mouse_report_t decoded;
    CHECK_EQ(hid_mouse_decode(&layout, touch_report, sizeof(touch_report), &decoded), 0);
}

TEST(hid_report, a_report_shorter_than_the_layout_reads_zero_rather_than_past_it) {
    hid_mouse_layout_t layout;
    REQUIRE(hid_report_find_mouse(precision_touchpad_mouse_collection,
                                  sizeof(precision_touchpad_mouse_collection), &layout) == 1);

    const uint8_t truncated[] = {0x01, 0x02, 0x07};
    hid_mouse_report_t decoded;
    CHECK_EQ(hid_mouse_decode(&layout, truncated, sizeof(truncated), &decoded), 1);
    CHECK_EQ(decoded.buttons, 2);
    CHECK_EQ(decoded.dx, 7);
    CHECK_EQ(decoded.dy, 0);
    CHECK_EQ(decoded.wheel, 0);
}

TEST(hid_report, a_truncated_descriptor_is_survivable_rather_than_fatal) {
    hid_mouse_layout_t layout;
    for (uint32_t n = 0; n < sizeof(precision_touchpad_mouse_collection); n++) {
        hid_report_find_mouse(precision_touchpad_mouse_collection, n, &layout);
    }
    CHECK_EQ(hid_report_find_mouse(NULL, 0, &layout), 0);
}

TEST(hid_report, a_mouse_without_report_ids_starts_at_the_first_byte) {
    static const uint8_t plain_mouse[] = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
        0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
        0x75, 0x05, 0x95, 0x01, 0x81, 0x03,
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
        0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
        0xC0,
    };
    hid_mouse_layout_t layout;
    REQUIRE(hid_report_find_mouse(plain_mouse, sizeof(plain_mouse), &layout) == 1);
    CHECK_EQ(layout.has_report_id, 0);
    CHECK_EQ(layout.button_count, 3);

    const uint8_t report[] = {0x04, 0xFE, 0x02};
    hid_mouse_report_t decoded;
    CHECK_EQ(hid_mouse_decode(&layout, report, sizeof(report), &decoded), 1);
    CHECK_EQ(decoded.buttons, 4);
    CHECK_EQ(decoded.dx, -2);
    CHECK_EQ(decoded.dy, 2);
}
