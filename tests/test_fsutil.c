/* tests/test_fsutil.c - M112
 *
 * The parts of the Files app that can be wrong quietly.
 *
 * A file manager fails loudly when it fails at all: a window that does
 * not open, a row that does not appear. What it does *not* do loudly is
 * write "8.4K" beside a file of 8,601 bytes and "8.4K" beside one of
 * 8,808 - both plausible, one of them the reason somebody deletes the
 * wrong file. Nor does it announce a New File box that accepted
 * "../settings.conf" and created something one directory up. Nor a
 * recursive delete that removed four of five levels and reported
 * success, in a window that has already re-listed and shows the folder
 * gone.
 *
 * Three groups below, in that order: the formatters, the name rule, and
 * the two tree walks. The last group runs against a real directory tree
 * on the host (tests/fakes/fake_user_fs.c), so what says "it is gone" is
 * the host's own stat rather than this code's opinion of itself.
 */
#include "check.h"

#include "fakes.h"

#include "../user_space/lib/fsutil.h"

#include <string.h>

/* ---- the size column -------------------------------------------------- */

TEST(fsutil, small_sizes_are_written_out_exactly) {
    char out[FSUTIL_SIZE_MAX];
    fsutil_format_size(0, out);
    CHECK_STREQ(out, "0");
    fsutil_format_size(1, out);
    CHECK_STREQ(out, "1");
    fsutil_format_size(999, out);
    CHECK_STREQ(out, "999");
}

/* The rollover, which is the boundary the column's width depends on:
 * everything below 1000 prints as itself, and 1000 is the first value
 * that takes a suffix. */
TEST(fsutil, the_suffix_starts_at_a_thousand) {
    char out[FSUTIL_SIZE_MAX];
    fsutil_format_size(999, out);
    CHECK_STREQ(out, "999");
    fsutil_format_size(1000, out);
    CHECK_STREQ(out, "0.9K");
    fsutil_format_size(1024, out);
    CHECK_STREQ(out, "1.0K");
}

/* The decimal is dropped at ten, and that is the rule this checks - not
 * a coincidence of these three numbers. */
TEST(fsutil, a_decimal_below_ten_and_none_above) {
    char out[FSUTIL_SIZE_MAX];
    fsutil_format_size(9 * 1024, out);
    CHECK_STREQ(out, "9.0K");
    fsutil_format_size(10 * 1024, out);
    CHECK_STREQ(out, "10K");
    fsutil_format_size(999 * 1024, out);
    CHECK_STREQ(out, "999K");
}

TEST(fsutil, megabytes_and_gigabytes) {
    char out[FSUTIL_SIZE_MAX];
    fsutil_format_size(1024u * 1024u, out);
    CHECK_STREQ(out, "1.0M");
    fsutil_format_size(8u * 1024u * 1024u + 512u * 1024u, out);
    CHECK_STREQ(out, "8.5M");
    fsutil_format_size(1024u * 1024u * 1024u, out);
    CHECK_STREQ(out, "1.0G");
}

/* The largest value a uint32_t size can hold, which is also leanfs's own
 * 4 GiB file ceiling. It must not run past FSUTIL_SIZE_MAX and must not
 * roll over into a fourth unit there is no suffix letter for. */
TEST(fsutil, the_biggest_file_this_filesystem_can_hold_still_fits) {
    char out[FSUTIL_SIZE_MAX];
    memset(out, 0x7F, sizeof(out));
    fsutil_format_size(0xFFFFFFFFu, out);
    CHECK_STREQ(out, "3.9G");
    CHECK(strlen(out) < FSUTIL_SIZE_MAX);
}

/* ---- the exact count -------------------------------------------------- */

TEST(fsutil, exact_sizes_group_in_threes) {
    char out[FSUTIL_EXACT_MAX];
    fsutil_format_exact(0, out);
    CHECK_STREQ(out, "0");
    fsutil_format_exact(7, out);
    CHECK_STREQ(out, "7");
    fsutil_format_exact(999, out);
    CHECK_STREQ(out, "999");
    fsutil_format_exact(1000, out);
    CHECK_STREQ(out, "1,000");
}

/* The boundary the separator rule gets wrong when it is written as "a
 * comma every three digits": 100 has three digits and takes none, and
 * 1000000 takes two rather than one at the front. */
TEST(fsutil, no_leading_separator_and_one_per_group) {
    char out[FSUTIL_EXACT_MAX];
    fsutil_format_exact(100, out);
    CHECK_STREQ(out, "100");
    fsutil_format_exact(100000, out);
    CHECK_STREQ(out, "100,000");
    fsutil_format_exact(1000000, out);
    CHECK_STREQ(out, "1,000,000");
    fsutil_format_exact(4294967295u, out);
    CHECK_STREQ(out, "4,294,967,295");
}

TEST(fsutil, the_widest_exact_count_fits_its_buffer) {
    char out[FSUTIL_EXACT_MAX];
    fsutil_format_exact(4294967295u, out);
    CHECK(strlen(out) < FSUTIL_EXACT_MAX);
}

/* ---- the date column -------------------------------------------------- */

/* A file older than this machine's ability to know the date reports 0,
 * and a column of identical zeroes would be worse than a column of
 * dashes - see M59, which is the milestone that made the date real. */
TEST(fsutil, a_file_with_no_timestamp_shows_a_dash) {
    char out[FSUTIL_DATE_MAX];
    fsutil_format_date(0, out);
    CHECK_STREQ(out, "-");
}

TEST(fsutil, a_timestamp_becomes_month_day_and_time) {
    char out[FSUTIL_DATE_MAX];
    /* 2026-09-09 14:03:00 UTC = 1788962580. Checked against the same
     * civil-time helper the clock uses, which is what the machine will
     * agree with. */
    fsutil_format_date(1788962580u, out);
    CHECK_STREQ(out, "09-09 14:03");
    /* The zero-padding, which is the half a naive formatter gets wrong:
     * 2026-01-02 03:04 must not print as "1-2 3:4". */
    fsutil_format_date(1767323040u, out);
    CHECK_STREQ(out, "01-02 03:04");
}

/* ---- the name rule ---------------------------------------------------- */

TEST(fsutil, ordinary_names_are_accepted) {
    CHECK_EQ(fsutil_name_ok("notes.txt"), 1);
    CHECK_EQ(fsutil_name_ok("a"), 1);
    CHECK_EQ(fsutil_name_ok(".hidden"), 1);
    CHECK_EQ(fsutil_name_ok("two words.md"), 1);
}

/* The rule the whole check exists for. Every path this program builds is
 * cwd + '/' + name, so a name carrying a separator is a name that
 * escapes the directory the window is showing. */
TEST(fsutil, a_name_with_a_separator_is_refused) {
    CHECK_EQ(fsutil_name_ok("../settings.conf"), 0);
    CHECK_EQ(fsutil_name_ok("sub/file"), 0);
    CHECK_EQ(fsutil_name_ok("/etc/settings.conf"), 0);
    CHECK_EQ(fsutil_name_ok("trailing/"), 0);
}

TEST(fsutil, the_two_dot_names_are_refused) {
    CHECK_EQ(fsutil_name_ok("."), 0);
    CHECK_EQ(fsutil_name_ok(".."), 0);
    /* But not names that merely begin that way - "..." is a legal, if
     * eccentric, filename and refusing it would be a rule about
     * appearances. */
    CHECK_EQ(fsutil_name_ok("..."), 1);
    CHECK_EQ(fsutil_name_ok("..bashrc"), 1);
}

TEST(fsutil, an_empty_name_is_refused) {
    CHECK_EQ(fsutil_name_ok(""), 0);
    CHECK_EQ(fsutil_name_ok(NULL), 0);
}

/* A name with a newline in it lists as two rows and a name with a
 * backspace in it cannot be read back at all - so neither can be
 * deleted by the person who made it. */
TEST(fsutil, control_characters_are_refused) {
    CHECK_EQ(fsutil_name_ok("two\nlines"), 0);
    CHECK_EQ(fsutil_name_ok("bell\a"), 0);
    CHECK_EQ(fsutil_name_ok("del\x7f"), 0);
    CHECK_EQ(fsutil_name_ok("\ttab"), 0);
}

TEST(fsutil, a_name_longer_than_the_filesystem_allows_is_refused) {
    char name[OS_NAME_MAX + 8];
    memset(name, 'a', sizeof(name));
    name[OS_NAME_MAX] = '\0';
    CHECK_EQ(fsutil_name_ok(name), 1); /* exactly at the limit */
    name[OS_NAME_MAX] = 'a';
    name[OS_NAME_MAX + 1] = '\0';
    CHECK_EQ(fsutil_name_ok(name), 0); /* one past it */
}

/* ---- the tree walks --------------------------------------------------- */

/* /home
 *   readme        (10 bytes)
 *   notes/
 *     a           (100 bytes)
 *     b           (200 bytes)
 *     deep/
 *       c         (1 byte)
 *
 * Five entries under /home and 311 bytes. Deliberately more than one
 * entry per directory and more than FAKE_BATCH in one of them, so the
 * "read a batch, act, read again" loop is actually exercised rather than
 * being a path nothing takes. */
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

TEST(fsutil, counting_a_tree_reaches_every_level) {
    build_tree();
    fsutil_tree_t t;
    CHECK_EQ(fsutil_count_tree("/home", &t), 0);
    /* readme, notes, a, b, deep, c */
    CHECK_EQ((long long)t.entries, 6);
    CHECK_EQ((long long)t.bytes, 311);
    CHECK_EQ(t.deep, 0);
}

TEST(fsutil, counting_an_empty_directory_is_zero_not_an_error) {
    fake_user_fs_reset();
    CHECK_EQ(fake_user_fs_mkdir("/empty"), 0);
    fsutil_tree_t t;
    CHECK_EQ(fsutil_count_tree("/empty", &t), 0);
    CHECK_EQ((long long)t.entries, 0);
    CHECK_EQ((long long)t.bytes, 0);
}

/* A directory that is not there is a failure and not a zero. The
 * difference matters because the confirm dialog shows this number: "0
 * items" about a folder that has vanished is a sentence that invites a
 * delete of something else. */
TEST(fsutil, counting_a_missing_directory_fails) {
    fake_user_fs_reset();
    fsutil_tree_t t;
    CHECK_EQ(fsutil_count_tree("/nothing-here", &t), -1);
}

/* One batch of SYS_getdents is three entries in the fake and 1024 bytes
 * on the machine. A directory holding more than that must still be
 * counted in full - the loop that re-reads is the only thing making
 * that true, and this is the case that fails if it is dropped. */
TEST(fsutil, a_directory_larger_than_one_batch_is_counted_in_full) {
    fake_user_fs_reset();
    CHECK_EQ(fake_user_fs_mkdir("/many"), 0);
    for (int i = 0; i < 25; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/many/f%02d", i);
        CHECK_EQ(fake_user_fs_write(p, 4), 0);
    }
    fsutil_tree_t t;
    CHECK_EQ(fsutil_count_tree("/many", &t), 0);
    CHECK_EQ((long long)t.entries, 25);
    CHECK_EQ((long long)t.bytes, 100);
}

TEST(fsutil, removing_a_tree_removes_all_of_it) {
    build_tree();
    CHECK_EQ(fsutil_remove_tree("/home/notes"), 0);
    /* Asked of the host, not of the walk that did the deleting. */
    CHECK_EQ(fake_user_fs_exists("/home/notes"), 0);
    CHECK_EQ(fake_user_fs_exists("/home/notes/a"), 0);
    CHECK_EQ(fake_user_fs_exists("/home/notes/deep"), 0);
    CHECK_EQ(fake_user_fs_exists("/home/notes/deep/c"), 0);
    /* And nothing outside it. A recursive delete that takes a sibling
     * with it is the single worst outcome available here. */
    CHECK_EQ(fake_user_fs_exists("/home/readme"), 1);
    CHECK_EQ(fake_user_fs_exists("/home"), 1);
}

TEST(fsutil, removing_a_directory_bigger_than_one_batch_empties_it) {
    fake_user_fs_reset();
    CHECK_EQ(fake_user_fs_mkdir("/many"), 0);
    for (int i = 0; i < 25; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/many/f%02d", i);
        CHECK_EQ(fake_user_fs_write(p, 4), 0);
    }
    CHECK_EQ(fsutil_remove_tree("/many"), 0);
    CHECK_EQ(fake_user_fs_exists("/many"), 0);
}

TEST(fsutil, removing_an_empty_directory_works_too) {
    fake_user_fs_reset();
    CHECK_EQ(fake_user_fs_mkdir("/empty"), 0);
    CHECK_EQ(fsutil_remove_tree("/empty"), 0);
    CHECK_EQ(fake_user_fs_exists("/empty"), 0);
}

TEST(fsutil, removing_something_that_is_not_there_fails) {
    fake_user_fs_reset();
    CHECK_EQ(fsutil_remove_tree("/nothing-here"), -1);
}

/* The depth ceiling, from both ends. A tree at exactly the limit must
 * still be removed, and one past it must be refused rather than run off
 * the stack - and the refusal must not have half-deleted the tree's own
 * root, because a caller that re-lists after a failure has to see what
 * is actually still there. */
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

TEST(fsutil, a_tree_at_the_depth_limit_is_still_removed) {
    /* FSUTIL_MAX_DEPTH levels of directory below the one being removed
     * is the deepest walk that is allowed to succeed: remove_at is
     * called with depth 0 for the root itself. */
    build_deep(FSUTIL_MAX_DEPTH);
    CHECK_EQ(fsutil_remove_tree("/d0"), 0);
    CHECK_EQ(fake_user_fs_exists("/d0"), 0);
}

TEST(fsutil, a_tree_past_the_depth_limit_is_refused_rather_than_followed) {
    build_deep(FSUTIL_MAX_DEPTH + 2);
    CHECK_EQ(fsutil_remove_tree("/d0"), -1);
    /* Partly emptied is the honest outcome and is what the caller
     * re-lists to see - but the root it was asked about is still there,
     * so the window shows a folder that is still a folder. */
    CHECK_EQ(fake_user_fs_exists("/d0"), 1);
}

TEST(fsutil, counting_past_the_depth_limit_says_so_rather_than_lying) {
    build_deep(FSUTIL_MAX_DEPTH + 2);
    fsutil_tree_t t;
    CHECK_EQ(fsutil_count_tree("/d0", &t), 0);
    /* The flag is the whole point: the count is a floor, and a confirm
     * dialog that said "4 items" about a tree it could not finish
     * walking would be understating what it is about to destroy. */
    CHECK_EQ(t.deep, 1);
}
