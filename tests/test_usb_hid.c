/* tests/test_usb_hid.c - M107
 *
 * A USB boot keyboard is state, not events, and this is the file that
 * says so out loud.
 *
 * The bug these tests exist for does not stop a boot and does not appear
 * in a serial log: a decoder that forgets to diff against the previous
 * report turns every held key into a keystroke per poll - a hundred a
 * second at this driver's 10 ms tick. Typing "hello" would produce
 * "hhhhheeeeellllllllllooooo", and the machine would look like it was
 * working. Nothing else in this tree can catch that, because every other
 * instrument here grades what the machine DID with a keystroke rather
 * than how many of them it thought it got.
 */
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

/* HID usage codes used below, so the tests read as keys rather than as
 * numbers: 0x04 'a', 0x05 'b', 0x06 'c', 0x28 Return, 0x2C space. */
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

/* The test this file is for. */
TEST(usb_hid, a_held_key_does_not_repeat) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, 0, K_A, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);

    /* The same key still down, reported again and again - which is what
     * a keyboard does for as long as a finger is on it. */
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

/* Two keys in one report, and then one of them released while the other
 * is still held: the held one must not fire again. This is the case a
 * decoder that compares only report[2] gets wrong. */
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

    /* 'a' let go, 'b' still down and now in the first slot - a slot
     * change is not a keystroke. */
    usb_hid_decode_keyboard(&st, just_b, &keys);
    CHECK_EQ(keys.count, 0);
}

/* A key that moves slots because a key BEFORE it was released is the
 * commonest rolling-typing case, and the one where a positional compare
 * produces a doubled letter. */
TEST(usb_hid, a_key_that_changes_slot_is_not_a_new_press) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, 0, K_A, K_B, K_C);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 3);

    report(r, 0, K_C, 0, 0); /* a and b released, c still down */
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

/* Right-hand modifiers are the same modifiers. A decoder that masks only
 * the low nibble reports a right-Shift chord as no chord at all, which
 * presents as "Ctrl+C works on the left side of the keyboard". */
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

/* 0x01 in a key slot is ErrorRollOver - "more keys are held than I can
 * report" - and is not a key. A table lookup on it would return whatever
 * index 1 holds. */
TEST(usb_hid, error_rollover_is_not_a_key) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, 0, 0x01, 0x01, 0x01);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 0);
}

/* A usage code past the end of the table must be silence, not a read of
 * whatever follows the array - which ASan turns into a failure here and
 * a corrupt keystroke on the machine. */
TEST(usb_hid, an_out_of_range_usage_reads_nothing) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    report(r, 0, 0xFF, 0xE7, 0x68); /* past the table, a GUI key, F13 */
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

/* The two tables must have holes in exactly the same places. A character
 * present in one and absent from the other is a key that types when
 * Shift is down and not otherwise, or the reverse - which is the sort of
 * thing that gets noticed months later by somebody trying to type a
 * question mark. */
TEST(usb_hid, the_two_ascii_tables_agree_on_which_keys_exist) {
    for (int i = 0; i < 104; i++) {
        CHECK_EQ(usb_hid_ascii[i] == 0, usb_hid_ascii_shift[i] == 0);
    }
}

/* Every letter must be a letter in both tables, and the shifted one must
 * be its upper case. */
TEST(usb_hid, the_letter_block_is_a_case_pair) {
    for (int usage = 0x04; usage <= 0x1D; usage++) {
        char lower = usb_hid_ascii[usage];
        char upper = usb_hid_ascii_shift[usage];
        CHECK(lower >= 'a' && lower <= 'z');
        CHECK_EQ(upper, lower - 'a' + 'A');
    }
}

TEST(usb_hid, the_digit_row_is_in_keyboard_order) {
    /* HID puts '1' at 0x1E and '0' at 0x27 - the order on the keyboard,
     * which is NOT the order of the ASCII codes. A table built by
     * counting from '0' gets every digit wrong by one. */
    CHECK_EQ(usb_hid_ascii[0x1E], '1');
    CHECK_EQ(usb_hid_ascii[0x26], '9');
    CHECK_EQ(usb_hid_ascii[0x27], '0');
    CHECK_EQ(usb_hid_ascii_shift[0x1E], '!');
    CHECK_EQ(usb_hid_ascii_shift[0x27], ')');
}

/* ---- the mouse --------------------------------------------------------- */

TEST(usb_hid, a_mouse_report_with_nothing_in_it_is_not_delivered) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_mouse_t m;
    uint8_t r[4] = {0, 0, 0, 0};

    /* The first report is always delivered - the caller has no idea what
     * the button state is until one arrives. */
    usb_hid_decode_mouse(&st, r, 4, &m);
    CHECK_EQ(m.deliver, 1);

    /* Every identical one after it is silence. A mouse polled at 100 Hz
     * and sitting still sends a hundred of these a second, and each one
     * forwarded is a compositor wakeup to move the cursor by zero - which
     * is precisely what M117 spent a milestone removing. */
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

    /* Held, not re-pressed. */
    usb_hid_decode_mouse(&st, click, 4, &m);
    CHECK_EQ(m.deliver, 0);

    /* Released is a change too. */
    usb_hid_decode_mouse(&st, idle, 4, &m);
    CHECK_EQ(m.deliver, 1);
    CHECK_EQ(m.buttons, 0);
}

/* A three-byte report is what a mouse with no wheel sends, and reading a
 * fourth byte from it reads whatever is next in the buffer. */
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

/* ---- What the mutation harness found ----------------------------------
 *
 * `make mutate` over drivers/usb_hid.c scored 81.5% on the first run and
 * named forty survivors, most of them in this file's subject. A survivor
 * is coverage without an assertion: the line ran, and nothing checked
 * what it did. The tests below are the assertions those mutants asked
 * for, and each one names the mutation it kills - because a test whose
 * reason is not written down is a test somebody deletes later.
 */

/* Kills: `for (int i = 2; i < 8; i++)` -> `i <= 8`, twice over.
 *
 * A report is exactly eight bytes. Reading a ninth reads whatever
 * follows it, which on the machine is the next field of the device
 * struct and here is a heap redzone - so the buffer is malloc'd at
 * exactly 8 bytes rather than declared on the stack, because ASan sees
 * past the end of an allocation and does not see past the end of an
 * array a parameter has decayed from. */
TEST(usb_hid, the_decoder_reads_exactly_eight_bytes) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t *r = malloc(8);
    REQUIRE(r != NULL);
    memset(r, 0, 8);
    r[2] = K_A;
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);
    /* And again with every slot full, so the loop runs to its end. */
    for (int i = 2; i < 8; i++) {
        r[i] = (uint8_t)(K_A + i - 2);
    }
    usb_hid_state_t st2 = {{0}, 0, 0};
    usb_hid_decode_keyboard(&st2, r, &keys);
    CHECK_EQ(keys.count, 6);
    free(r);
}

/* Kills: `usage >= 104` -> `usage > 104`, and the 104 -> 105 constant
 * mutants. 103 is the last usage this table has a character for and 104
 * is the first that is past its end; a decoder that lets 104 through
 * reads one byte beyond a 104-element array. */
TEST(usb_hid, the_table_bound_is_exactly_one_past_the_last_entry) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    /* 103 is inside the table. It has no character (it is in the
     * navigation block), so what is asserted is that it is *looked up*
     * and found empty rather than rejected by the bound. */
    report(r, 0, 103, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 0);

    /* 104 is past it, and must be rejected before the lookup. Under ASan
     * a decoder that does not reject it fails here rather than returning
     * a plausible character. */
    usb_hid_state_t st2 = {{0}, 0, 0};
    report(r, 0, 104, 0, 0);
    usb_hid_decode_keyboard(&st2, r, &keys);
    CHECK_EQ(keys.count, 0);

    /* And the last usage that does produce something: 0x38 is '/'. */
    usb_hid_state_t st3 = {{0}, 0, 0};
    report(r, 0, 0x38, 0, 0);
    usb_hid_decode_keyboard(&st3, r, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(keys.ch[0], '/');
}

/* Kills: `usage <= 3` -> `usage < 3`, and the 3 -> 2/4 constants.
 *
 * 0 is "no key", 1 is ErrorRollOver, 2 is POSTFail and 3 is
 * ErrorUndefined. All four are statuses rather than keys, and 4 is 'a' -
 * the first real one. A bound one too low lets a status through; one too
 * high eats the letter 'a'. */
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

/* Kills: `if (out->count < 6)` -> `<= 6`, `< 5`, `< 7`, and the
 * condition_true mutant.
 *
 * Six is the ceiling because a boot report holds six usage codes, so six
 * simultaneous new presses is the most that can ever happen - and all
 * six must be reported. A bound of five silently drops the last key of
 * a six-finger chord; a bound of seven writes past the end of the
 * array. */
TEST(usb_hid, six_simultaneous_presses_are_all_reported) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];
    memset(r, 0, 8);
    /* 'a' through 'f' in all six slots at once. */
    for (int i = 0; i < 6; i++) {
        r[2 + i] = (uint8_t)(K_A + i);
    }
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 6);
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(keys.ch[i], 'a' + i);
    }
}

/* Kills: `if (state->have_last)` -> always true.
 *
 * On the very first report there is no previous one, and last_keys is
 * all zeroes. A decoder that consults it anyway compares against zeroes
 * - which happens to be harmless for a key, and is NOT harmless as a
 * statement about the code: the flag exists so that a device enumerated
 * mid-keystroke reports that keystroke rather than swallowing it. */
TEST(usb_hid, the_first_report_is_not_diffed_against_nothing) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];

    CHECK_EQ(st.have_last, 0);
    report(r, 0, K_A, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(st.have_last, 1); /* ...and it is set, so the next one IS diffed */
}

/* Kills: the `for (int i = 0; i < 8; i++)` mutants on the state copy, and
 * the constant mutants inside it.
 *
 * The whole report - modifier byte included - is what gets remembered,
 * and it has to be all eight bytes: a copy that stops at seven leaves
 * the last key slot holding the report before last, so a key held in
 * slot six fires again every time. */
TEST(usb_hid, the_whole_report_is_remembered_including_the_last_slot) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];
    memset(r, 0, 8);
    r[0] = MOD_LSHIFT;
    r[7] = K_A; /* the LAST key slot */

    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 1);
    CHECK_EQ(keys.ch[0], 'A');
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(st.last_keys[i], r[i]);
    }
    /* Held: it must not fire again. */
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 0);
}

/* Kills: `for (int j = 2; j < 8; j++)` in the held-key search, and its
 * constants. A search that stops early misses a key held in a late slot
 * and reports it as a new press - the doubled-letter bug, arriving by a
 * different route than the missing diff. */
TEST(usb_hid, a_key_held_in_any_slot_is_found_by_the_held_search) {
    for (int slot = 2; slot < 8; slot++) {
        usb_hid_state_t st = {{0}, 0, 0};
        usb_hid_keys_t keys;
        uint8_t r[8];
        memset(r, 0, 8);
        r[slot] = K_A;

        usb_hid_decode_keyboard(&st, r, &keys);
        CHECK_EQ(keys.count, 1);
        /* Same key, same slot, next poll: silence. */
        usb_hid_decode_keyboard(&st, r, &keys);
        CHECK_EQ(keys.count, 0);
    }
}

/* Kills: `if (ch == 0)` -> condition_false, and the 0 -> 1 constant.
 *
 * A usage inside the table but with no character must produce nothing.
 * 0x65 is the Application key; a decoder that forwards it injects a NUL
 * into the keyboard ring, which every reader in this tree will treat as
 * end-of-string. */
TEST(usb_hid, a_key_with_no_character_injects_nothing) {
    usb_hid_state_t st = {{0}, 0, 0};
    usb_hid_keys_t keys;
    uint8_t r[8];
    report(r, 0, 0x65, 0, 0);
    usb_hid_decode_keyboard(&st, r, &keys);
    CHECK_EQ(keys.count, 0);

    /* And the hole in the middle of the punctuation block - 0x32, which
     * is a non-US key this table has no character for - behaves the
     * same, shifted or not. */
    usb_hid_state_t st2 = {{0}, 0, 0};
    report(r, MOD_LSHIFT, 0x32, 0, 0);
    usb_hid_decode_keyboard(&st2, r, &keys);
    CHECK_EQ(keys.count, 0);
}

/* Kills: the constant_plus_one / constant_minus_one mutants on the
 * translation tables themselves - a character changed by one is still a
 * character, and every test above would still pass with '-' turned into
 * ',' somewhere in the punctuation block.
 *
 * So the tables are asserted against the US QWERTY layout entry by
 * entry, which is the only thing that can catch a one-off in a table:
 * there is no rule to derive them from, they are a keyboard. */
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
    /* Every digit, both rows, in keyboard order. */
    static const char digits[] = "1234567890";
    static const char symbols[] = "!@#$%^&*()";
    for (int i = 0; i < 10; i++) {
        CHECK_EQ(usb_hid_ascii[0x1E + i], digits[i]);
        CHECK_EQ(usb_hid_ascii_shift[0x1E + i], symbols[i]);
    }
}

/* Kills: `state->have_last = 1` -> 0 or 2, on the mouse path. A flag that
 * never becomes set means every idle report is delivered, which is the
 * wakeup storm the deliver flag exists to prevent. */
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
