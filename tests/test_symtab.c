#include "check.h"

#include "../user_space/library/symtab.h"

#include <string.h>

static const char SAMPLE[] =
    "0000000000100000 _start\n"
    "0000000000100050 enter_user_mode\n"
    "0000000000100070 context_switch\n"
    "0000000000101000 schedule\n"
    "0000000000102abc [end-of-text]\n";

static int parse_sample(symtab_t *st, symtab_entry_t *storage, int cap) {
    return symtab_parse(st, storage, cap, SAMPLE, strlen(SAMPLE));
}

static int name_is(const symtab_entry_t *e, const char *want) {
    if (!e) {
        return 0;
    }
    size_t n = strlen(want);
    return e->name_len == n && strncmp(e->name, want, n) == 0;
}

TEST(symtab, parses_every_line) {
    symtab_entry_t storage[16];
    symtab_t st;
    CHECK_EQ(parse_sample(&st, storage, 16), 5);
    CHECK_EQ((long long)st.entries[0].addr, 0x100000);
    CHECK(name_is(&st.entries[0], "_start"));
    CHECK_EQ((long long)st.entries[4].addr, 0x102abc);
    CHECK(name_is(&st.entries[4], "[end-of-text]"));
    CHECK_EQ(symtab_is_sorted(&st), 1);
}

TEST(symtab, an_address_on_a_boundary_belongs_to_that_symbol) {
    symtab_entry_t storage[16];
    symtab_t st;
    REQUIRE(parse_sample(&st, storage, 16) == 5);

    CHECK(name_is(symtab_lookup(&st, 0x100050), "enter_user_mode"));
    CHECK(name_is(symtab_lookup(&st, 0x10004f), "_start"));
    CHECK(name_is(symtab_lookup(&st, 0x100051), "enter_user_mode"));
    CHECK(name_is(symtab_lookup(&st, 0x100070), "context_switch"));
    CHECK(name_is(symtab_lookup(&st, 0x100fff), "context_switch"));
    CHECK(name_is(symtab_lookup(&st, 0x101000), "schedule"));
}

TEST(symtab, the_first_symbol_and_below_it) {
    symtab_entry_t storage[16];
    symtab_t st;
    REQUIRE(parse_sample(&st, storage, 16) == 5);

    CHECK(name_is(symtab_lookup(&st, 0x100000), "_start"));
    CHECK(symtab_lookup(&st, 0x0fffff) == NULL);
    CHECK(symtab_lookup(&st, 0) == NULL);
}

TEST(symtab, past_the_end_of_text_resolves_to_nothing) {
    symtab_entry_t storage[16];
    symtab_t st;
    REQUIRE(parse_sample(&st, storage, 16) == 5);

    CHECK(name_is(symtab_lookup(&st, 0x102abb), "schedule"));
    CHECK(symtab_lookup(&st, 0x102abc) == NULL);
    CHECK(symtab_lookup(&st, 0xdeadbeef) == NULL);
    CHECK(symtab_lookup(&st, 0xffffffffffffffffULL) == NULL);
}

TEST(symtab, capacity_is_a_ceiling_not_a_crash) {
    symtab_entry_t storage[2];
    symtab_t st;
    CHECK_EQ(parse_sample(&st, storage, 2), 2);
    CHECK(name_is(&st.entries[1], "enter_user_mode"));
}

TEST(symtab, malformed_lines_are_skipped) {
    static const char text[] =
        "# a comment nm would never write, but a person might\n"
        "\n"
        "0000000000100000 _start\n"
        "not_an_address_at_all\n"
        "0000000000100050\n"
        "0000000000100070 context_switch\n"
        "0000000000100080 [end-of-text]\n";
    symtab_entry_t storage[16];
    symtab_t st;
    CHECK_EQ(symtab_parse(&st, storage, 16, text, strlen(text)), 3);
    CHECK(name_is(&st.entries[0], "_start"));
    CHECK(name_is(&st.entries[1], "context_switch"));
    CHECK(name_is(symtab_lookup(&st, 0x100060), "_start"));
}

TEST(symtab, a_file_with_no_trailing_newline_still_parses) {
    static const char text[] =
        "0000000000100000 _start\n"
        "0000000000100050 [end-of-text]";
    symtab_entry_t storage[8];
    symtab_t st;
    CHECK_EQ(symtab_parse(&st, storage, 8, text, strlen(text)), 2);
    CHECK(name_is(&st.entries[1], "[end-of-text]"));
}

TEST(symtab, an_empty_or_single_entry_table_answers_nothing) {
    symtab_entry_t storage[8];
    symtab_t st;

    CHECK_EQ(symtab_parse(&st, storage, 8, "", 0), 0);
    CHECK(symtab_lookup(&st, 0x100000) == NULL);

    static const char one[] = "0000000000100000 [end-of-text]\n";
    CHECK_EQ(symtab_parse(&st, storage, 8, one, strlen(one)), 1);
    CHECK(symtab_lookup(&st, 0x100000) == NULL);
    CHECK(symtab_lookup(&st, 0x0fffff) == NULL);
}

TEST(symtab, bad_arguments_are_refused) {
    symtab_entry_t storage[8];
    symtab_t st;
    CHECK_EQ(symtab_parse(&st, storage, 0, SAMPLE, strlen(SAMPLE)), -1);
    CHECK_EQ(symtab_parse(&st, NULL, 8, SAMPLE, strlen(SAMPLE)), -1);
    CHECK_EQ(symtab_parse(NULL, storage, 8, SAMPLE, strlen(SAMPLE)), -1);
    CHECK_EQ(symtab_parse(&st, storage, 8, NULL, 10), -1);
    CHECK(symtab_lookup(NULL, 0x100000) == NULL);
}

TEST(symtab, an_unsorted_file_is_detected_not_fixed) {
    static const char text[] =
        "0000000000100070 context_switch\n"
        "0000000000100000 _start\n"
        "0000000000100080 [end-of-text]\n";
    symtab_entry_t storage[8];
    symtab_t st;
    CHECK_EQ(symtab_parse(&st, storage, 8, text, strlen(text)), 3);
    CHECK_EQ(symtab_is_sorted(&st), 0);
    CHECK_EQ(symtab_is_sorted(&(symtab_t){storage, 1, 8}), 1);
}

TEST(symtab, an_overlong_address_is_not_an_entry) {
    static const char text[] =
        "00000000001000000 nonsense\n"
        "0000000000100000 _start\n"
        "0000000000100080 [end-of-text]\n";
    symtab_entry_t storage[8];
    symtab_t st;
    CHECK_EQ(symtab_parse(&st, storage, 8, text, strlen(text)), 2);
    CHECK(name_is(&st.entries[0], "_start"));
}
