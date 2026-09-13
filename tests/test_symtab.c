#include "check.h"

#include "../user_space/library/symbol_table.h"

#include <string.h>

static const char SAMPLE[] =
    "0000000000100000 _start\n"
    "0000000000100050 enter_user_mode\n"
    "0000000000100070 context_switch\n"
    "0000000000101000 schedule\n"
    "0000000000102abc [end-of-text]\n";

static int parse_sample(symbol_table_t *st, symbol_table_entry_t *storage, int cap) {
    return symbol_table_parse(st, storage, cap, SAMPLE, strlen(SAMPLE));
}

static int name_is(const symbol_table_entry_t *e, const char *want) {
    if (!e) {
        return 0;
    }
    size_t n = strlen(want);
    return e->name_length == n && strncmp(e->name, want, n) == 0;
}

TEST(symbol_table, parses_every_line) {
    symbol_table_entry_t storage[16];
    symbol_table_t st;
    CHECK_EQ(parse_sample(&st, storage, 16), 5);
    CHECK_EQ((long long)st.entries[0].address, 0x100000);
    CHECK(name_is(&st.entries[0], "_start"));
    CHECK_EQ((long long)st.entries[4].address, 0x102abc);
    CHECK(name_is(&st.entries[4], "[end-of-text]"));
    CHECK_EQ(symbol_table_is_sorted(&st), 1);
}

TEST(symbol_table, an_address_on_a_boundary_belongs_to_that_symbol) {
    symbol_table_entry_t storage[16];
    symbol_table_t st;
    REQUIRE(parse_sample(&st, storage, 16) == 5);

    CHECK(name_is(symbol_table_lookup(&st, 0x100050), "enter_user_mode"));
    CHECK(name_is(symbol_table_lookup(&st, 0x10004f), "_start"));
    CHECK(name_is(symbol_table_lookup(&st, 0x100051), "enter_user_mode"));
    CHECK(name_is(symbol_table_lookup(&st, 0x100070), "context_switch"));
    CHECK(name_is(symbol_table_lookup(&st, 0x100fff), "context_switch"));
    CHECK(name_is(symbol_table_lookup(&st, 0x101000), "schedule"));
}

TEST(symbol_table, the_first_symbol_and_below_it) {
    symbol_table_entry_t storage[16];
    symbol_table_t st;
    REQUIRE(parse_sample(&st, storage, 16) == 5);

    CHECK(name_is(symbol_table_lookup(&st, 0x100000), "_start"));
    CHECK(symbol_table_lookup(&st, 0x0fffff) == NULL);
    CHECK(symbol_table_lookup(&st, 0) == NULL);
}

TEST(symbol_table, past_the_end_of_text_resolves_to_nothing) {
    symbol_table_entry_t storage[16];
    symbol_table_t st;
    REQUIRE(parse_sample(&st, storage, 16) == 5);

    CHECK(name_is(symbol_table_lookup(&st, 0x102abb), "schedule"));
    CHECK(symbol_table_lookup(&st, 0x102abc) == NULL);
    CHECK(symbol_table_lookup(&st, 0xdeadbeef) == NULL);
    CHECK(symbol_table_lookup(&st, 0xffffffffffffffffULL) == NULL);
}

TEST(symbol_table, capacity_is_a_ceiling_not_a_crash) {
    symbol_table_entry_t storage[2];
    symbol_table_t st;
    CHECK_EQ(parse_sample(&st, storage, 2), 2);
    CHECK(name_is(&st.entries[1], "enter_user_mode"));
}

TEST(symbol_table, malformed_lines_are_skipped) {
    static const char text[] =
        "# a comment nm would never write, but a person might\n"
        "\n"
        "0000000000100000 _start\n"
        "not_an_address_at_all\n"
        "0000000000100050\n"
        "0000000000100070 context_switch\n"
        "0000000000100080 [end-of-text]\n";
    symbol_table_entry_t storage[16];
    symbol_table_t st;
    CHECK_EQ(symbol_table_parse(&st, storage, 16, text, strlen(text)), 3);
    CHECK(name_is(&st.entries[0], "_start"));
    CHECK(name_is(&st.entries[1], "context_switch"));
    CHECK(name_is(symbol_table_lookup(&st, 0x100060), "_start"));
}

TEST(symbol_table, a_file_with_no_trailing_newline_still_parses) {
    static const char text[] =
        "0000000000100000 _start\n"
        "0000000000100050 [end-of-text]";
    symbol_table_entry_t storage[8];
    symbol_table_t st;
    CHECK_EQ(symbol_table_parse(&st, storage, 8, text, strlen(text)), 2);
    CHECK(name_is(&st.entries[1], "[end-of-text]"));
}

TEST(symbol_table, an_empty_or_single_entry_table_answers_nothing) {
    symbol_table_entry_t storage[8];
    symbol_table_t st;

    CHECK_EQ(symbol_table_parse(&st, storage, 8, "", 0), 0);
    CHECK(symbol_table_lookup(&st, 0x100000) == NULL);

    static const char one[] = "0000000000100000 [end-of-text]\n";
    CHECK_EQ(symbol_table_parse(&st, storage, 8, one, strlen(one)), 1);
    CHECK(symbol_table_lookup(&st, 0x100000) == NULL);
    CHECK(symbol_table_lookup(&st, 0x0fffff) == NULL);
}

TEST(symbol_table, bad_arguments_are_refused) {
    symbol_table_entry_t storage[8];
    symbol_table_t st;
    CHECK_EQ(symbol_table_parse(&st, storage, 0, SAMPLE, strlen(SAMPLE)), -1);
    CHECK_EQ(symbol_table_parse(&st, NULL, 8, SAMPLE, strlen(SAMPLE)), -1);
    CHECK_EQ(symbol_table_parse(NULL, storage, 8, SAMPLE, strlen(SAMPLE)), -1);
    CHECK_EQ(symbol_table_parse(&st, storage, 8, NULL, 10), -1);
    CHECK(symbol_table_lookup(NULL, 0x100000) == NULL);
}

TEST(symbol_table, an_unsorted_file_is_detected_not_fixed) {
    static const char text[] =
        "0000000000100070 context_switch\n"
        "0000000000100000 _start\n"
        "0000000000100080 [end-of-text]\n";
    symbol_table_entry_t storage[8];
    symbol_table_t st;
    CHECK_EQ(symbol_table_parse(&st, storage, 8, text, strlen(text)), 3);
    CHECK_EQ(symbol_table_is_sorted(&st), 0);
    CHECK_EQ(symbol_table_is_sorted(&(symbol_table_t){storage, 1, 8}), 1);
}

TEST(symbol_table, an_overlong_address_is_not_an_entry) {
    static const char text[] =
        "00000000001000000 nonsense\n"
        "0000000000100000 _start\n"
        "0000000000100080 [end-of-text]\n";
    symbol_table_entry_t storage[8];
    symbol_table_t st;
    CHECK_EQ(symbol_table_parse(&st, storage, 8, text, strlen(text)), 2);
    CHECK(name_is(&st.entries[0], "_start"));
}
