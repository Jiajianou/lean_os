#include "check.h"

#include "../user_space/library/lvgl_keys.c"

TEST(lvgl_keys, arrow_keys_do_not_collide_with_home_and_end) {
    CHECK_EQ(lvgl_translate_key(KEYBOARD_KEY_UP, 0), (uint32_t)LV_KEY_UP);
    CHECK_EQ(lvgl_translate_key(KEYBOARD_KEY_DOWN, 0), (uint32_t)LV_KEY_DOWN);
    CHECK_EQ(lvgl_translate_key(KEYBOARD_KEY_LEFT, 0), (uint32_t)LV_KEY_LEFT);
    CHECK_EQ(lvgl_translate_key(KEYBOARD_KEY_RIGHT, 0), (uint32_t)LV_KEY_RIGHT);

    CHECK_NE(lvgl_translate_key(KEYBOARD_KEY_DOWN, 0), (uint32_t)LV_KEY_HOME);
    CHECK_NE(lvgl_translate_key(KEYBOARD_KEY_LEFT, 0), (uint32_t)LV_KEY_END);
}

TEST(lvgl_keys, editing_keys_map_to_their_lvgl_names) {
    CHECK_EQ(lvgl_translate_key('\n', 0), (uint32_t)LV_KEY_ENTER);
    CHECK_EQ(lvgl_translate_key('\r', 0), (uint32_t)LV_KEY_ENTER);
    CHECK_EQ(lvgl_translate_key('\b', 0), (uint32_t)LV_KEY_BACKSPACE);
    CHECK_EQ(lvgl_translate_key(0x7F, 0), (uint32_t)LV_KEY_BACKSPACE);
    CHECK_EQ(lvgl_translate_key(27, 0), (uint32_t)LV_KEY_ESC);
}

TEST(lvgl_keys, tab_moves_focus_and_shift_tab_moves_it_back) {
    CHECK_EQ(lvgl_translate_key('\t', 0), (uint32_t)LV_KEY_NEXT);
    CHECK_EQ(lvgl_translate_key('\t', KEYBOARD_MOD_SHIFT), (uint32_t)LV_KEY_PREV);
    CHECK_EQ(lvgl_translate_key('\t', KEYBOARD_MOD_CTRL), (uint32_t)LV_KEY_NEXT);
}

TEST(lvgl_keys, printable_characters_pass_through_unchanged) {
    CHECK_EQ(lvgl_translate_key('a', 0), (uint32_t)'a');
    CHECK_EQ(lvgl_translate_key('Z', 0), (uint32_t)'Z');
    CHECK_EQ(lvgl_translate_key('0', 0), (uint32_t)'0');
    CHECK_EQ(lvgl_translate_key(' ', 0), (uint32_t)' ');
    CHECK_EQ(lvgl_translate_key('~', 0), (uint32_t)'~');
}

TEST(lvgl_keys, unmapped_control_characters_are_dropped) {
    CHECK_EQ(lvgl_translate_key(16, 0), 0u);
    CHECK_EQ(lvgl_translate_key(0x1E, 0), 0u);
    CHECK_EQ(lvgl_translate_key(KEYBOARD_KEY_SUPER, 0), 0u);
    CHECK_EQ(lvgl_translate_key(KEYBOARD_KEY_PAGE_UP, 0), 0u);
}

TEST(lvgl_keys, home_end_and_delete_reach_a_text_field) {
    CHECK_EQ(lvgl_translate_key(KEYBOARD_KEY_HOME, 0), (uint32_t)LV_KEY_HOME);
    CHECK_EQ(lvgl_translate_key(KEYBOARD_KEY_END, 0), (uint32_t)LV_KEY_END);
    CHECK_EQ(lvgl_translate_key(KEYBOARD_KEY_DELETE, 0), (uint32_t)LV_KEY_DEL);
    CHECK_NE(lvgl_translate_key(KEYBOARD_KEY_DELETE, 0), (uint32_t)LV_KEY_BACKSPACE);
}
