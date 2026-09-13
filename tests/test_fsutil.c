#include "check.h"

#include "fakes.h"

#include "../user_space/library/file_system_utilities.h"

#include <string.h>

TEST(file_system_utilities, small_sizes_are_written_out_exactly) {
    char out[FILE_SYSTEM_UTILITIES_SIZE_MAX];
    file_system_utilities_format_size(0, out);
    CHECK_STREQ(out, "0");
    file_system_utilities_format_size(1, out);
    CHECK_STREQ(out, "1");
    file_system_utilities_format_size(999, out);
    CHECK_STREQ(out, "999");
}

TEST(file_system_utilities, the_suffix_starts_at_a_thousand) {
    char out[FILE_SYSTEM_UTILITIES_SIZE_MAX];
    file_system_utilities_format_size(999, out);
    CHECK_STREQ(out, "999");
    file_system_utilities_format_size(1000, out);
    CHECK_STREQ(out, "0.9K");
    file_system_utilities_format_size(1024, out);
    CHECK_STREQ(out, "1.0K");
}

TEST(file_system_utilities, a_decimal_below_ten_and_none_above) {
    char out[FILE_SYSTEM_UTILITIES_SIZE_MAX];
    file_system_utilities_format_size(9 * 1024, out);
    CHECK_STREQ(out, "9.0K");
    file_system_utilities_format_size(10 * 1024, out);
    CHECK_STREQ(out, "10K");
    file_system_utilities_format_size(999 * 1024, out);
    CHECK_STREQ(out, "999K");
}

TEST(file_system_utilities, megabytes_and_gigabytes) {
    char out[FILE_SYSTEM_UTILITIES_SIZE_MAX];
    file_system_utilities_format_size(1024u * 1024u, out);
    CHECK_STREQ(out, "1.0M");
    file_system_utilities_format_size(8u * 1024u * 1024u + 512u * 1024u, out);
    CHECK_STREQ(out, "8.5M");
    file_system_utilities_format_size(1024u * 1024u * 1024u, out);
    CHECK_STREQ(out, "1.0G");
}

TEST(file_system_utilities, the_biggest_file_this_filesystem_can_hold_still_fits) {
    char out[FILE_SYSTEM_UTILITIES_SIZE_MAX];
    memset(out, 0x7F, sizeof(out));
    file_system_utilities_format_size(0xFFFFFFFFu, out);
    CHECK_STREQ(out, "3.9G");
    CHECK(strlen(out) < FILE_SYSTEM_UTILITIES_SIZE_MAX);
}

TEST(file_system_utilities, exact_sizes_group_in_threes) {
    char out[FILE_SYSTEM_UTILITIES_EXACT_MAX];
    file_system_utilities_format_exact(0, out);
    CHECK_STREQ(out, "0");
    file_system_utilities_format_exact(7, out);
    CHECK_STREQ(out, "7");
    file_system_utilities_format_exact(999, out);
    CHECK_STREQ(out, "999");
    file_system_utilities_format_exact(1000, out);
    CHECK_STREQ(out, "1,000");
}

TEST(file_system_utilities, no_leading_separator_and_one_per_group) {
    char out[FILE_SYSTEM_UTILITIES_EXACT_MAX];
    file_system_utilities_format_exact(100, out);
    CHECK_STREQ(out, "100");
    file_system_utilities_format_exact(100000, out);
    CHECK_STREQ(out, "100,000");
    file_system_utilities_format_exact(1000000, out);
    CHECK_STREQ(out, "1,000,000");
    file_system_utilities_format_exact(4294967295u, out);
    CHECK_STREQ(out, "4,294,967,295");
}

TEST(file_system_utilities, the_widest_exact_count_fits_its_buffer) {
    char out[FILE_SYSTEM_UTILITIES_EXACT_MAX];
    file_system_utilities_format_exact(4294967295u, out);
    CHECK(strlen(out) < FILE_SYSTEM_UTILITIES_EXACT_MAX);
}

TEST(file_system_utilities, a_file_with_no_timestamp_shows_a_dash) {
    char out[FILE_SYSTEM_UTILITIES_DATE_MAX];
    file_system_utilities_format_date(0, out);
    CHECK_STREQ(out, "-");
}

TEST(file_system_utilities, a_timestamp_becomes_month_day_and_time) {
    char out[FILE_SYSTEM_UTILITIES_DATE_MAX];
    file_system_utilities_format_date(1788962580u, out);
    CHECK_STREQ(out, "09-09 14:03");
    file_system_utilities_format_date(1767323040u, out);
    CHECK_STREQ(out, "01-02 03:04");
}

TEST(file_system_utilities, ordinary_names_are_accepted) {
    CHECK_EQ(file_system_utilities_name_ok("notes.txt"), 1);
    CHECK_EQ(file_system_utilities_name_ok("a"), 1);
    CHECK_EQ(file_system_utilities_name_ok(".hidden"), 1);
    CHECK_EQ(file_system_utilities_name_ok("two words.md"), 1);
}

TEST(file_system_utilities, a_name_with_a_separator_is_refused) {
    CHECK_EQ(file_system_utilities_name_ok("../settings.conf"), 0);
    CHECK_EQ(file_system_utilities_name_ok("sub/file"), 0);
    CHECK_EQ(file_system_utilities_name_ok("/etc/settings.conf"), 0);
    CHECK_EQ(file_system_utilities_name_ok("trailing/"), 0);
}

TEST(file_system_utilities, the_two_dot_names_are_refused) {
    CHECK_EQ(file_system_utilities_name_ok("."), 0);
    CHECK_EQ(file_system_utilities_name_ok(".."), 0);
    CHECK_EQ(file_system_utilities_name_ok("..."), 1);
    CHECK_EQ(file_system_utilities_name_ok("..bashrc"), 1);
}

TEST(file_system_utilities, an_empty_name_is_refused) {
    CHECK_EQ(file_system_utilities_name_ok(""), 0);
    CHECK_EQ(file_system_utilities_name_ok(NULL), 0);
}

TEST(file_system_utilities, control_characters_are_refused) {
    CHECK_EQ(file_system_utilities_name_ok("two\nlines"), 0);
    CHECK_EQ(file_system_utilities_name_ok("bell\a"), 0);
    CHECK_EQ(file_system_utilities_name_ok("del\x7f"), 0);
    CHECK_EQ(file_system_utilities_name_ok("\ttab"), 0);
}

TEST(file_system_utilities, a_name_longer_than_the_filesystem_allows_is_refused) {
    char name[OS_NAME_MAX + 8];
    memset(name, 'a', sizeof(name));
    name[OS_NAME_MAX] = '\0';
    CHECK_EQ(file_system_utilities_name_ok(name), 1);
    name[OS_NAME_MAX] = 'a';
    name[OS_NAME_MAX + 1] = '\0';
    CHECK_EQ(file_system_utilities_name_ok(name), 0);
}

static void build_tree(void) {
    fake_user_fs_reset();
    CHECK_EQ(fake_user_fs_mkdir("/home"), 0);
    CHECK_EQ(fake_user_fs_write("/home/readme", 10), 0);
    CHECK_EQ(fake_user_fs_mkdir("/home/notes"), 0);
    CHECK_EQ(fake_user_fs_write("/home/notes/a", 100), 0);
    CHECK_EQ(fake_user_fs_write("/home/notes/b", 200), 0);
    CHECK_EQ(fake_user_fs_mkdir("/home/notes/deep"), 0);
    CHECK_EQ(fake_user_fs_write("/home/notes/deep/c", 1), 0);
}

TEST(file_system_utilities, counting_a_tree_reaches_every_level) {
    build_tree();
    file_system_utilities_tree_t t;
    CHECK_EQ(file_system_utilities_count_tree("/home", &t), 0);
    CHECK_EQ((long long)t.entries, 6);
    CHECK_EQ((long long)t.bytes, 311);
    CHECK_EQ(t.deep, 0);
}

TEST(file_system_utilities, counting_an_empty_directory_is_zero_not_an_error) {
    fake_user_fs_reset();
    CHECK_EQ(fake_user_fs_mkdir("/empty"), 0);
    file_system_utilities_tree_t t;
    CHECK_EQ(file_system_utilities_count_tree("/empty", &t), 0);
    CHECK_EQ((long long)t.entries, 0);
    CHECK_EQ((long long)t.bytes, 0);
}

TEST(file_system_utilities, counting_a_missing_directory_fails) {
    fake_user_fs_reset();
    file_system_utilities_tree_t t;
    CHECK_EQ(file_system_utilities_count_tree("/nothing-here", &t), -1);
}

TEST(file_system_utilities, a_directory_larger_than_one_batch_is_counted_in_full) {
    fake_user_fs_reset();
    CHECK_EQ(fake_user_fs_mkdir("/many"), 0);
    for (int i = 0; i < 25; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/many/f%02d", i);
        CHECK_EQ(fake_user_fs_write(p, 4), 0);
    }
    file_system_utilities_tree_t t;
    CHECK_EQ(file_system_utilities_count_tree("/many", &t), 0);
    CHECK_EQ((long long)t.entries, 25);
    CHECK_EQ((long long)t.bytes, 100);
}

TEST(file_system_utilities, removing_a_tree_removes_all_of_it) {
    build_tree();
    CHECK_EQ(file_system_utilities_remove_tree("/home/notes"), 0);
    CHECK_EQ(fake_user_fs_exists("/home/notes"), 0);
    CHECK_EQ(fake_user_fs_exists("/home/notes/a"), 0);
    CHECK_EQ(fake_user_fs_exists("/home/notes/deep"), 0);
    CHECK_EQ(fake_user_fs_exists("/home/notes/deep/c"), 0);
    CHECK_EQ(fake_user_fs_exists("/home/readme"), 1);
    CHECK_EQ(fake_user_fs_exists("/home"), 1);
}

TEST(file_system_utilities, removing_a_directory_bigger_than_one_batch_empties_it) {
    fake_user_fs_reset();
    CHECK_EQ(fake_user_fs_mkdir("/many"), 0);
    for (int i = 0; i < 25; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/many/f%02d", i);
        CHECK_EQ(fake_user_fs_write(p, 4), 0);
    }
    CHECK_EQ(file_system_utilities_remove_tree("/many"), 0);
    CHECK_EQ(fake_user_fs_exists("/many"), 0);
}

TEST(file_system_utilities, removing_an_empty_directory_works_too) {
    fake_user_fs_reset();
    CHECK_EQ(fake_user_fs_mkdir("/empty"), 0);
    CHECK_EQ(file_system_utilities_remove_tree("/empty"), 0);
    CHECK_EQ(fake_user_fs_exists("/empty"), 0);
}

TEST(file_system_utilities, removing_something_that_is_not_there_fails) {
    fake_user_fs_reset();
    CHECK_EQ(file_system_utilities_remove_tree("/nothing-here"), -1);
}

static void build_deep(int levels) {
    char path[512];
    fake_user_fs_reset();
    int n = 0;
    path[0] = '\0';
    for (int i = 0; i < levels; i++) {
        n += snprintf(path + n, sizeof(path) - (size_t)n, "/d%d", i);
        CHECK_EQ(fake_user_fs_mkdir(path), 0);
    }
    snprintf(path + n, sizeof(path) - (size_t)n, "/leaf");
    CHECK_EQ(fake_user_fs_write(path, 3), 0);
}

TEST(file_system_utilities, a_tree_at_the_depth_limit_is_still_removed) {
    build_deep(FILE_SYSTEM_UTILITIES_MAX_DEPTH);
    CHECK_EQ(file_system_utilities_remove_tree("/d0"), 0);
    CHECK_EQ(fake_user_fs_exists("/d0"), 0);
}

TEST(file_system_utilities, a_tree_past_the_depth_limit_is_refused_rather_than_followed) {
    build_deep(FILE_SYSTEM_UTILITIES_MAX_DEPTH + 2);
    CHECK_EQ(file_system_utilities_remove_tree("/d0"), -1);
    CHECK_EQ(fake_user_fs_exists("/d0"), 1);
}

TEST(file_system_utilities, counting_past_the_depth_limit_says_so_rather_than_lying) {
    build_deep(FILE_SYSTEM_UTILITIES_MAX_DEPTH + 2);
    file_system_utilities_tree_t t;
    CHECK_EQ(file_system_utilities_count_tree("/d0", &t), 0);
    CHECK_EQ(t.deep, 1);
}
