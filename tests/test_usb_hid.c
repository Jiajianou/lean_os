#include "check.h"

#include "drivers/usb_hid.h"
#include "input.h"

#include <stdlib.h>
#include <string.h>

static void report(uint8_t out[8], uint8_t mods, uint8_t k0, uint8_t k1, uint8_t k2) {
    memset(out, 0, 8);
    out[0] = mods;
    out[2] = k0;
    out[3] = k1;
    out[4] = k2;
}

#define K_A 0x04
#define K_B 0x05
#define K_C 0x06
#define K_RET 0x28
#define K_SPACE 0x2C
#define MOD_LSHIFT 0x02
#define MOD_RSHIFT 0x20
#define MOD_LCTRL  0x01
#define MOD_LALT   0x04

TEST(usb_hid, a_single_press_is_one_character) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, 0, K_A, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(keys.ch[0], 'a');
    CHECK_EQ(keys.mods[0], 0);
}

TEST(usb_hid, a_held_key_does_not_repeat) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, 0, K_A, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);

    for (int i = 0; i < 100; i++) {
        usb_hid_decode_keyboard(&st, r, &keys);
        CHECK_EQ(keys.count, 0);
    }
}

TEST(usb_hid, a_key_released_and_pressed_again_is_two_characters) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t down[8], up[8];
    report(down, 0, K_A, 0, 0);
    report(up, 0, 0, 0, 0);

    usb_hid_decode_keyboard(&st, down, &keys);
    CHECK_EQ(keys.count, 1);
    usb_hid_decode_keyboard(&st, up, &keys);
    CHECK_EQ(keys.count, 0);
    usb_hid_decode_keyboard(&st, down, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(keys.ch[0], 'a');
}

TEST(usb_hid, one_of_two_held_keys_released) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t both[8], just_b[8];
    report(both, 0, K_A, K_B, 0);
    report(just_b, 0, K_B, 0, 0);

    usb_hid_decode_keyboard(&st, both, &keys);
    CHECK_EQ(keys.count, 2);
    CHECK_EQ(keys.ch[0], 'a');
    CHECK_EQ(keys.ch[1], 'b');

    usb_hid_decode_keyboard(&st, just_b, &keys);
    CHECK_EQ(keys.count, 0);
}

TEST(usb_hid, a_key_that_changes_slot_is_not_a_new_press) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, 0, K_A, K_B, K_C);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 3);

    report(r, 0, K_C, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 0);
}

TEST(usb_hid, shift_selects_the_shifted_table) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, MOD_LSHIFT, K_A, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(keys.ch[0], 'A');
    CHECK_EQ(keys.mods[0] & KBD_MOD_SHIFT, KBD_MOD_SHIFT);
}

TEST(usb_hid, right_hand_modifiers_count) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, MOD_RSHIFT, K_A, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(keys.ch[0], 'A');
    CHECK_EQ(keys.mods[0] & KBD_MOD_SHIFT, KBD_MOD_SHIFT);
}

TEST(usb_hid, ctrl_and_alt_travel_with_the_character) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, MOD_LCTRL, K_C, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(keys.ch[0], 'c');
    CHECK_EQ(keys.mods[0] & KBD_MOD_CTRL, KBD_MOD_CTRL);
    CHECK_EQ(keys.mods[0] & KBD_MOD_SHIFT, 0);

    usb_hid_state_t st2 = {{0}, 0, 0};
    report(r, MOD_LALT, K_A, 0, 0);
    usb_hid_decode_keyboard(&st2, r, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(keys.mods[0] & KBD_MOD_ALT, KBD_MOD_ALT);
}

TEST(usb_hid, error_rollover_is_not_a_key) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, 0, 0x01, 0x01, 0x01);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 0);
}

TEST(usb_hid, an_out_of_range_usage_reads_nothing) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, 0, 0xFF, 0xE7, 0x68);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 0);
}

TEST(usb_hid, return_space_and_backspace_are_the_control_characters) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, 0, K_RET, K_SPACE, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 2);
    CHECK_EQ(keys.ch[0], '\n');
    CHECK_EQ(keys.ch[1], ' ');
}

TEST(usb_hid, the_two_ascii_tables_agree_on_which_keys_exist) {
    for (int i = 0; i < 104; i++) {
        CHECK_EQ(usb_hid_ascii[i] == 0, usb_hid_ascii_shift[i] == 0);
    }
}

TEST(usb_hid, the_letter_block_is_a_case_pair) {
    for (int usage = 0x04; usage <= 0x1D; usage++) {
        char lower = usb_hid_ascii[usage];
        char upper = usb_hid_ascii_shift[usage];
        CHECK(lower >= 'a' && lower <= 'z');
        CHECK_EQ(upper, lower - 'a' + 'A');
    }
}

TEST(usb_hid, the_digit_row_is_in_keyboard_order) {
    CHECK_EQ(usb_hid_ascii[0x1E], '1');
    CHECK_EQ(usb_hid_ascii[0x26], '9');
    CHECK_EQ(usb_hid_ascii[0x27], '0');
    CHECK_EQ(usb_hid_ascii_shift[0x1E], '!');
    CHECK_EQ(usb_hid_ascii_shift[0x27], ')');
}

TEST(usb_hid, a_mouse_report_with_nothing_in_it_is_not_delivered) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_mouse_t m;
    uint8_t r[4] = {0, 0, 0, 0};

    usb_hid_decode_mouse(&st, r, 4, &m);
    CHECK_EQ(m.deliver, 1);

    for (int i = 0; i < 50; i++) {
        usb_hid_decode_mouse(&st, r, 4, &m);
        CHECK_EQ(m.deliver, 0);
    }
}

TEST(usb_hid, movement_is_signed) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_mouse_t m;
    uint8_t r[4] = {0, 0xFF, 0xFE, 0};

    usb_hid_decode_mouse(&st, r, 4, &m);
    CHECK_EQ(m.deliver, 1);
    CHECK_EQ(m.dx, -1);
    CHECK_EQ(m.dy, -2);
}

TEST(usb_hid, a_button_change_is_delivered_without_movement) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_mouse_t m;
    uint8_t idle[4] = {0, 0, 0, 0};
    uint8_t click[4] = {1, 0, 0, 0};

    usb_hid_decode_mouse(&st, idle, 4, &m);
    usb_hid_decode_mouse(&st, idle, 4, &m);
    CHECK_EQ(m.deliver, 0);

    usb_hid_decode_mouse(&st, click, 4, &m);
    CHECK_EQ(m.deliver, 1);
    CHECK_EQ(m.buttons, 1);
    CHECK_EQ(m.dx, 0);

    usb_hid_decode_mouse(&st, click, 4, &m);
    CHECK_EQ(m.deliver, 0);

    usb_hid_decode_mouse(&st, idle, 4, &m);
    CHECK_EQ(m.deliver, 1);
    CHECK_EQ(m.buttons, 0);
}

TEST(usb_hid, a_three_byte_report_has_no_wheel) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_mouse_t m;
    uint8_t r[3] = {0, 5, 5};

    usb_hid_decode_mouse(&st, r, 3, &m);
    CHECK_EQ(m.wheel, 0);
    CHECK_EQ(m.dx, 5);
    CHECK_EQ(m.dy, 5);
}

TEST(usb_hid, the_wheel_is_signed_on_a_four_byte_report) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_mouse_t m;
    uint8_t r[4] = {0, 0, 0, 0xFF};

    usb_hid_decode_mouse(&st, r, 4, &m);
    CHECK_EQ(m.deliver, 1);
    CHECK_EQ(m.wheel, -1);
}

TEST(usb_hid, the_decoder_reads_exactly_eight_bytes) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t *r = malloc(8);
    REQUIRE(r != NULL);
    memset(r, 0, 8);
    r[2] = K_A;
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);
    for (int i = 2; i < 8; i++) {
        r[i] = (uint8_t)(K_A + i - 2);
    }
    usb_hid_state_t st2 = {{0}, 0, 0};
    usb_hid_decode_keyboard(&st2, r, &keys);
    CHECK_EQ(keys.count, 6);
    free(r);
}

TEST(usb_hid, the_table_bound_is_exactly_one_past_the_last_entry) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, 0, 103, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 0);

    usb_hid_state_t st2 = {{0}, 0, 0};
    report(r, 0, 104, 0, 0);
    usb_hid_decode_keyboard(&st2, r, &keys);
    CHECK_EQ(keys.count, 0);

    usb_hid_state_t st3 = {{0}, 0, 0};
    report(r, 0, 0x38, 0, 0);
    usb_hid_decode_keyboard(&st3, r, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(keys.ch[0], '/');
}

TEST(usb_hid, the_status_codes_below_four_are_not_keys_and_four_is) {
    usb_hid_keys_t keys;
    uint8_t r[8];
    for (uint8_t usage = 0; usage <= 3; usage++) {
        usb_hid_state_t st = {{0}, 0, 0};
        report(r, 0, usage, 0, 0);
        usb_hid_decode_keyboard(&st, r, &keys);
        CHECK_EQ(keys.count, 0);
    }
    usb_hid_state_t st = {{0}, 0, 0};
    report(r, 0, 4, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(keys.ch[0], 'a');
}

TEST(usb_hid, six_simultaneous_presses_are_all_reported) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];
    memset(r, 0, 8);
    for (int i = 0; i < 6; i++) {
        r[2 + i] = (uint8_t)(K_A + i);
    }
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 6);
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(keys.ch[i], 'a' + i);
    }
}

TEST(usb_hid, the_first_report_is_not_diffed_against_nothing) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    CHECK_EQ(st.have_last, 0);
    report(r, 0, K_A, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(st.have_last, 1);
}

TEST(usb_hid, the_whole_report_is_remembered_including_the_last_slot) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];
    memset(r, 0, 8);
    r[0] = MOD_LSHIFT;
    r[7] = K_A;

    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(keys.ch[0], 'A');
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(st.last_keys[i], r[i]);
    }
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 0);
}

TEST(usb_hid, a_key_held_in_any_slot_is_found_by_the_held_search) {
    for (int slot = 2; slot < 8; slot++) {
        usb_hid_state_t st = {{0}, 0, 0};
        usb_hid_keys_t keys;
        uint8_t r[8];
        memset(r, 0, 8);
        r[slot] = K_A;

        usb_hid_decode_keyboard(&st, r, &keys);
        CHECK_EQ(keys.count, 1);
        usb_hid_decode_keyboard(&st, r, &keys);
        CHECK_EQ(keys.count, 0);
    }
}

TEST(usb_hid, a_key_with_no_character_injects_nothing) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];
    report(r, 0, 0x65, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 0);

    usb_hid_state_t st2 = {{0}, 0, 0};
    report(r, MOD_LSHIFT, 0x32, 0, 0);
    usb_hid_decode_keyboard(&st2, r, &keys);
    CHECK_EQ(keys.count, 0);
}

TEST(usb_hid, the_punctuation_block_is_the_us_layout) {
    static const struct { uint8_t usage; char plain; char shifted; } expect[] = {
        {0x28, '\n', '\n'}, {0x29, 27, 27},   {0x2A, '\b', '\b'},
        {0x2B, '\t', '\t'}, {0x2C, ' ', ' '}, {0x2D, '-', '_'},
        {0x2E, '=', '+'},   {0x2F, '[', '{'}, {0x30, ']', '}'},
        {0x31, '\\', '|'},  {0x33, ';', ':'}, {0x34, '\'', '"'},
        {0x35, '`', '~'},   {0x36, ',', '<'}, {0x37, '.', '>'},
        {0x38, '/', '?'},
    };
    for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        CHECK_EQ(usb_hid_ascii[expect[i].usage], expect[i].plain);
        CHECK_EQ(usb_hid_ascii_shift[expect[i].usage], expect[i].shifted);
    }
    static const char digits[] = "1234567890";
    static const char symbols[] = "!@#$%^&*()";
    for (int i = 0; i < 10; i++) {
        CHECK_EQ(usb_hid_ascii[0x1E + i], digits[i]);
        CHECK_EQ(usb_hid_ascii_shift[0x1E + i], symbols[i]);
    }
}

TEST(usb_hid, the_mouse_remembers_that_it_has_seen_a_report) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_mouse_t m;
    uint8_t r[4] = {0, 0, 0, 0};
    usb_hid_decode_mouse(&st, r, 4, &m);
    CHECK_EQ(st.have_last, 1);
    CHECK_EQ(m.deliver, 1);
    usb_hid_decode_mouse(&st, r, 4, &m);
    CHECK_EQ(m.deliver, 0);
}
