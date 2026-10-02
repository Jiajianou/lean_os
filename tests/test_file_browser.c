#include "check.h"

#include "fakes.h"

#include "../user_space/library/file_browser.h"
#include "../user_space/library/syscall_wrappers.h"

#include <string.h>

static void fresh_home(void) {
    fake_user_fs_reset();
    REQUIRE(fake_user_fs_mkdir("/home") == 0);
}

TEST(file_browser, join_adds_exactly_one_separator) {
    char out[64];
    CHECK_EQ(file_browser_join("/home", "a", out, sizeof(out)), FILE_BROWSER_OK);
    CHECK_STREQ(out, "/home/a");
    CHECK_EQ(file_browser_join("/", "bin", out, sizeof(out)), FILE_BROWSER_OK);
    CHECK_STREQ(out, "/bin");
    CHECK_EQ(file_browser_join("/home/", "a", out, sizeof(out)), FILE_BROWSER_OK);
    CHECK_STREQ(out, "/home/a");
}

TEST(file_browser, join_refuses_rather_than_truncating) {
    char out[8];
    CHECK_EQ(file_browser_join("/home", "abc", out, sizeof(out)), FILE_BROWSER_ERROR_TOO_LONG);
    CHECK_EQ(file_browser_join("/home", "a", out, sizeof(out)), FILE_BROWSER_OK);
    CHECK_STREQ(out, "/home/a");
}

TEST(file_browser, parent_and_basename_agree_with_the_path) {
    char out[64];
    file_browser_parent("/home/docs/a.txt", out, sizeof(out));
    CHECK_STREQ(out, "/home/docs");
    CHECK_STREQ(file_browser_basename("/home/docs/a.txt"), "a.txt");
    file_browser_parent("/home", out, sizeof(out));
    CHECK_STREQ(out, "/");
    file_browser_parent("/", out, sizeof(out));
    CHECK_STREQ(out, "/");
    file_browser_parent("/home/docs/", out, sizeof(out));
    CHECK_STREQ(out, "/home");
}

TEST(file_browser, inside_means_a_whole_component_not_a_prefix) {
    CHECK(file_browser_is_inside("/home/a/b", "/home/a"));
    CHECK(file_browser_is_inside("/home/a", "/home/a"));
    CHECK(!file_browser_is_inside("/home/ab", "/home/a"));
    CHECK(!file_browser_is_inside("/home", "/home/a"));
    CHECK(file_browser_is_inside("/anything", "/"));
}

TEST(file_browser, names_sort_the_way_a_person_counts) {
    CHECK(file_browser_compare_names("file2", "file10") < 0);
    CHECK(file_browser_compare_names("file10", "file2") > 0);
    CHECK(file_browser_compare_names("Apple", "banana") < 0);
    CHECK(file_browser_compare_names("apple", "Banana") < 0);
    CHECK(file_browser_compare_names("a007", "a7") == 0);
    CHECK(file_browser_compare_names("a", "ab") < 0);
    CHECK(file_browser_compare_names("x 9.txt", "x 10.txt") < 0);
}

TEST(file_browser, a_search_ignores_case_and_finds_the_middle) {
    CHECK(file_browser_name_matches("README.txt", "readme"));
    CHECK(file_browser_name_matches("my_Notes.md", "NOTES"));
    CHECK(!file_browser_name_matches("notes", "notesx"));
    CHECK(!file_browser_name_matches("anything", ""));
}

TEST(file_browser, text_is_told_apart_from_a_program) {
    static const unsigned char TEXT[] = "hello\nworld\t\n";
    static const unsigned char UTF8[] = "caf\xc3\xa9 \xe2\x82\xac";
    static const unsigned char ELF[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
    static const unsigned char BROKEN[] = {'a', 0xC3, 'b'};
    CHECK(file_browser_looks_like_text(TEXT, sizeof(TEXT) - 1));
    CHECK(file_browser_looks_like_text(UTF8, sizeof(UTF8) - 1));
    CHECK(!file_browser_looks_like_text(ELF, sizeof(ELF)));
    CHECK(!file_browser_looks_like_text(BROKEN, sizeof(BROKEN)));
}

TEST(file_browser, a_character_cut_off_at_the_end_of_the_sample_is_still_text) {
    static const unsigned char CUT[] = {'a', 'b', 0xE2, 0x82};
    CHECK(file_browser_looks_like_text(CUT, sizeof(CUT)));
}

TEST(file_browser, the_contents_outrank_the_name) {
    static const unsigned char ELF[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
    static const unsigned char PNG[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    const char *kind_name = 0;
    CHECK_EQ(file_browser_classify("ls", 0, 0, ELF, sizeof(ELF), &kind_name), FILE_BROWSER_KIND_PROGRAM);
    CHECK_STREQ(kind_name, "Program");
    CHECK_EQ(file_browser_classify("picture.txt", 0, 0, PNG, sizeof(PNG), &kind_name), FILE_BROWSER_KIND_IMAGE);
    CHECK_EQ(file_browser_classify("notes.TXT", 0, 0, 0, 0, &kind_name), FILE_BROWSER_KIND_TEXT);
    CHECK_STREQ(kind_name, "Plain Text");
    CHECK_EQ(file_browser_classify("docs", 1, 0, 0, 0, &kind_name), FILE_BROWSER_KIND_FOLDER);
    CHECK_EQ(file_browser_classify(".bashrc", 0, 0, (const unsigned char *)"x=1\n", 4, &kind_name),
             FILE_BROWSER_KIND_TEXT);
    CHECK_EQ(file_browser_classify("empty", 0, 0, (const unsigned char *)"", 0, &kind_name),
             FILE_BROWSER_KIND_TEXT);
    CHECK_STREQ(kind_name, "Empty File");
}

TEST(file_browser, a_preview_is_chosen_by_the_bytes) {
    static const unsigned char PNG[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    static const unsigned char ELF[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
    CHECK_EQ(file_browser_preview_kind(PNG, sizeof(PNG)), FILE_BROWSER_PREVIEW_IMAGE);
    CHECK_EQ(file_browser_preview_kind(ELF, sizeof(ELF)), FILE_BROWSER_PREVIEW_BYTES);
    CHECK_EQ(file_browser_preview_kind((const unsigned char *)"hi\n", 3), FILE_BROWSER_PREVIEW_TEXT);
}

TEST(file_browser, a_taken_name_gets_the_next_number_before_its_extension) {
    fresh_home();
    char out[FILE_BROWSER_NAME_MAX];
    CHECK_EQ(file_browser_available_name("/home", "untitled folder", out, sizeof(out)), FILE_BROWSER_OK);
    CHECK_STREQ(out, "untitled folder");
    fake_user_fs_mkdir("/home/untitled folder");
    CHECK_EQ(file_browser_available_name("/home", "untitled folder", out, sizeof(out)), FILE_BROWSER_OK);
    CHECK_STREQ(out, "untitled folder 2");
    fake_user_fs_write("/home/report.txt", 1);
    fake_user_fs_write("/home/report 2.txt", 1);
    CHECK_EQ(file_browser_available_name("/home", "report.txt", out, sizeof(out)), FILE_BROWSER_OK);
    CHECK_STREQ(out, "report 3.txt");
}

TEST(file_browser, a_copy_says_copy_and_then_counts) {
    fresh_home();
    char out[FILE_BROWSER_NAME_MAX];
    fake_user_fs_write("/home/report.txt", 1);
    CHECK_EQ(file_browser_copy_name("/home", "report.txt", out, sizeof(out)), FILE_BROWSER_OK);
    CHECK_STREQ(out, "report copy.txt");
    fake_user_fs_write("/home/report copy.txt", 1);
    CHECK_EQ(file_browser_copy_name("/home", "report.txt", out, sizeof(out)), FILE_BROWSER_OK);
    CHECK_STREQ(out, "report copy 2.txt");
    CHECK_EQ(file_browser_copy_name("/home", "fresh.txt", out, sizeof(out)), FILE_BROWSER_OK);
    CHECK_STREQ(out, "fresh.txt");
    fake_user_fs_write("/home/.profile", 1);
    CHECK_EQ(file_browser_copy_name("/home", ".profile", out, sizeof(out)), FILE_BROWSER_OK);
    CHECK_STREQ(out, ".profile copy");
}

TEST(file_browser, a_listing_hides_dot_files_unless_asked) {
    fresh_home();
    fake_user_fs_write("/home/visible.txt", 3);
    fake_user_fs_write("/home/.hidden", 3);
    fake_user_fs_mkdir("/home/folder");
    static file_browser_entry_t entries[16];
    CHECK_EQ(file_browser_list("/home", 0, entries, 16), 2);
    CHECK_EQ(file_browser_list("/home", FILE_BROWSER_LIST_HIDDEN, entries, 16), 3);
}

TEST(file_browser, a_listing_reports_size_kind_and_folders) {
    fresh_home();
    fake_user_fs_write("/home/notes.txt", 42);
    fake_user_fs_write_text("/home/program", "\x7f" "ELF....");
    fake_user_fs_mkdir("/home/folder");
    static file_browser_entry_t entries[16];
    int n = file_browser_list("/home", FILE_BROWSER_LIST_SNIFF, entries, 16);
    REQUIRE(n == 3);
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, "notes.txt") == 0) {
            CHECK_EQ(entries[i].size, 42);
            CHECK_EQ(entries[i].kind, FILE_BROWSER_KIND_TEXT);
            seen |= 1;
        } else if (strcmp(entries[i].name, "program") == 0) {
            CHECK_EQ(entries[i].kind, FILE_BROWSER_KIND_PROGRAM);
            seen |= 2;
        } else if (strcmp(entries[i].name, "folder") == 0) {
            CHECK(entries[i].is_directory);
            CHECK_STREQ(entries[i].kind_name, "Folder");
            seen |= 4;
        }
    }
    CHECK_EQ(seen, 7);
    CHECK_EQ(fake_user_fs_open_count(), 0);
}

TEST(file_browser, a_listing_stops_at_the_array_it_was_given) {
    fresh_home();
    for (int i = 0; i < 10; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/home/f%d", i);
        fake_user_fs_write(path, 1);
    }
    static file_browser_entry_t entries[4];
    CHECK_EQ(file_browser_list("/home", 0, entries, 4), 4);
}

static file_browser_entry_t sample(const char *name, uint32_t size, uint32_t mtime, int folder) {
    file_browser_entry_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.name, sizeof(e.name), "%s", name);
    e.size = size;
    e.mtime = mtime;
    e.is_directory = (uint8_t)folder;
    e.kind_name = folder ? "Folder" : "Plain Text";
    return e;
}

TEST(file_browser, folders_come_first_whichever_way_the_column_points) {
    file_browser_entry_t entries[4] = {
        sample("b.txt", 10, 3, 0), sample("zeta", 0, 1, 1), sample("a.txt", 30, 2, 0), sample("alpha", 0, 9, 1),
    };
    int order[4];
    file_browser_sort(entries, order, 4, FILE_BROWSER_SORT_NAME, 0);
    CHECK_STREQ(entries[order[0]].name, "alpha");
    CHECK_STREQ(entries[order[1]].name, "zeta");
    CHECK_STREQ(entries[order[2]].name, "a.txt");
    CHECK_STREQ(entries[order[3]].name, "b.txt");
    file_browser_sort(entries, order, 4, FILE_BROWSER_SORT_NAME, 1);
    CHECK(entries[order[0]].is_directory && entries[order[1]].is_directory);
    CHECK_STREQ(entries[order[2]].name, "b.txt");
}

TEST(file_browser, size_and_date_order_the_files) {
    file_browser_entry_t entries[3] = {sample("a", 10, 30, 0), sample("b", 30, 10, 0), sample("c", 20, 20, 0)};
    int order[3];
    file_browser_sort(entries, order, 3, FILE_BROWSER_SORT_SIZE, 1);
    CHECK_STREQ(entries[order[0]].name, "b");
    CHECK_STREQ(entries[order[2]].name, "a");
    file_browser_sort(entries, order, 3, FILE_BROWSER_SORT_DATE, 0);
    CHECK_STREQ(entries[order[0]].name, "b");
    CHECK_STREQ(entries[order[2]].name, "a");
}

TEST(file_browser, sorting_a_large_folder_is_a_permutation) {
    static file_browser_entry_t entries[300];
    static int order[300];
    for (int i = 0; i < 300; i++) {
        char name[16];
        snprintf(name, sizeof(name), "f%d", (i * 7919) % 300);
        entries[i] = sample(name, (uint32_t)i, 0, 0);
    }
    file_browser_sort(entries, order, 300, FILE_BROWSER_SORT_NAME, 0);
    static int seen[300];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < 300; i++) {
        seen[order[i]]++;
        if (i > 0) {
            CHECK(file_browser_compare_names(entries[order[i - 1]].name, entries[order[i]].name) < 0);
        }
    }
    for (int i = 0; i < 300; i++) {
        CHECK_EQ(seen[i], 1);
    }
}

TEST(file_browser, a_copied_tree_has_every_byte_and_the_original_stays) {
    fresh_home();
    fake_user_fs_mkdir("/home/src");
    fake_user_fs_mkdir("/home/src/inner");
    fake_user_fs_write_text("/home/src/a.txt", "alpha");
    fake_user_fs_write_text("/home/src/inner/b.txt", "bravo bravo");
    fake_user_fs_symlink("a.txt", "/home/src/link");
    CHECK_EQ(file_browser_copy("/home/src", "/home/dst"), FILE_BROWSER_OK);
    char text[64];
    CHECK_EQ(fake_user_fs_read_text("/home/dst/a.txt", text, sizeof(text)), 5);
    CHECK_STREQ(text, "alpha");
    CHECK_EQ(fake_user_fs_read_text("/home/dst/inner/b.txt", text, sizeof(text)), 11);
    CHECK_STREQ(text, "bravo bravo");
    os_stat_t status;
    CHECK_EQ(sys_lstat("/home/dst/link", &status), 0);
    CHECK(status.is_link);
    CHECK(fake_user_fs_exists("/home/src/inner/b.txt"));
    CHECK_EQ(fake_user_fs_open_count(), 0);
}

TEST(file_browser, a_folder_cannot_be_copied_into_itself) {
    fresh_home();
    fake_user_fs_mkdir("/home/src");
    fake_user_fs_write_text("/home/src/a.txt", "alpha");
    CHECK_EQ(file_browser_copy("/home/src", "/home/src/again"), FILE_BROWSER_ERROR_INTO_ITSELF);
    CHECK_EQ(file_browser_copy_into("/home/src", "/home/src", 0, 0), FILE_BROWSER_ERROR_INTO_ITSELF);
    CHECK(!fake_user_fs_exists("/home/src/again"));
    CHECK(!fake_user_fs_exists("/home/src/src"));
}

TEST(file_browser, a_copy_never_overwrites) {
    fresh_home();
    fake_user_fs_write_text("/home/a.txt", "new");
    fake_user_fs_write_text("/home/b.txt", "precious");
    CHECK_EQ(file_browser_copy("/home/a.txt", "/home/b.txt"), FILE_BROWSER_ERROR_EXISTS);
    char text[32];
    fake_user_fs_read_text("/home/b.txt", text, sizeof(text));
    CHECK_STREQ(text, "precious");
}

TEST(file_browser, a_copy_that_runs_out_of_room_leaves_nothing_half_made) {
    fresh_home();
    fake_user_fs_mkdir("/home/src");
    fake_user_fs_write("/home/src/a", 100);
    fake_user_fs_write("/home/src/b", 100);
    fake_user_fs_write_limit = 150;
    int rc = file_browser_copy("/home/src", "/home/dst");
    fake_user_fs_write_limit = -1;
    CHECK_EQ(rc, FILE_BROWSER_ERROR_WRITE);
    CHECK(!fake_user_fs_exists("/home/dst"));
    CHECK_EQ(fake_user_fs_open_count(), 0);
}

TEST(file_browser, duplicating_beside_the_original_names_it_a_copy) {
    fresh_home();
    fake_user_fs_write_text("/home/a.txt", "x");
    char made[256];
    CHECK_EQ(file_browser_copy_into("/home/a.txt", "/home", made, sizeof(made)), FILE_BROWSER_OK);
    CHECK_STREQ(made, "/home/a copy.txt");
    CHECK_EQ(file_browser_copy_into("/home/a.txt", "/home", made, sizeof(made)), FILE_BROWSER_OK);
    CHECK_STREQ(made, "/home/a copy 2.txt");
}

TEST(file_browser, a_move_refuses_its_own_inside_and_an_occupied_name) {
    fresh_home();
    fake_user_fs_mkdir("/home/outer");
    fake_user_fs_mkdir("/home/outer/inner");
    fake_user_fs_mkdir("/home/other");
    fake_user_fs_write("/home/other/outer", 1);
    CHECK_EQ(file_browser_move_into("/home/outer", "/home/outer/inner", 0, 0), FILE_BROWSER_ERROR_INTO_ITSELF);
    CHECK_EQ(file_browser_move_into("/home/outer", "/home/outer", 0, 0), FILE_BROWSER_ERROR_INTO_ITSELF);
    CHECK_EQ(file_browser_move_into("/home/outer", "/home", 0, 0), FILE_BROWSER_ERROR_SAME);
    CHECK_EQ(file_browser_move_into("/home/outer", "/home/other", 0, 0), FILE_BROWSER_ERROR_EXISTS);
    CHECK(fake_user_fs_exists("/home/outer/inner"));
}

TEST(file_browser, a_move_takes_the_whole_folder) {
    fresh_home();
    fake_user_fs_mkdir("/home/outer");
    fake_user_fs_write("/home/outer/a", 3);
    fake_user_fs_mkdir("/home/target");
    char moved[256];
    CHECK_EQ(file_browser_move_into("/home/outer", "/home/target", moved, sizeof(moved)), FILE_BROWSER_OK);
    CHECK_STREQ(moved, "/home/target/outer");
    CHECK(fake_user_fs_exists("/home/target/outer/a"));
    CHECK(!fake_user_fs_exists("/home/outer"));
}

TEST(file_browser, the_trash_remembers_where_things_came_from) {
    fresh_home();
    fake_user_fs_mkdir("/home/docs");
    fake_user_fs_write_text("/home/docs/plan.txt", "plan");
    char trashed[256];
    CHECK_EQ(file_browser_trash("/home/docs/plan.txt", trashed, sizeof(trashed)), FILE_BROWSER_OK);
    CHECK_STREQ(trashed, FILE_BROWSER_TRASH "/plan.txt");
    CHECK(!fake_user_fs_exists("/home/docs/plan.txt"));
    char origin[256];
    CHECK_EQ(file_browser_trash_origin(trashed, origin, sizeof(origin)), FILE_BROWSER_OK);
    CHECK_STREQ(origin, "/home/docs/plan.txt");
    char restored[256];
    CHECK_EQ(file_browser_put_back(trashed, restored, sizeof(restored)), FILE_BROWSER_OK);
    CHECK_STREQ(restored, "/home/docs/plan.txt");
    char text[16];
    fake_user_fs_read_text("/home/docs/plan.txt", text, sizeof(text));
    CHECK_STREQ(text, "plan");
    CHECK(!fake_user_fs_exists(FILE_BROWSER_TRASH_ORIGINS "/plan.txt"));
}

TEST(file_browser, two_things_with_one_name_both_fit_in_the_trash) {
    fresh_home();
    fake_user_fs_mkdir("/home/a");
    fake_user_fs_mkdir("/home/b");
    fake_user_fs_write_text("/home/a/notes", "from a");
    fake_user_fs_write_text("/home/b/notes", "from b");
    char first[256];
    char second[256];
    CHECK_EQ(file_browser_trash("/home/a/notes", first, sizeof(first)), FILE_BROWSER_OK);
    CHECK_EQ(file_browser_trash("/home/b/notes", second, sizeof(second)), FILE_BROWSER_OK);
    CHECK(strcmp(first, second) != 0);
    char restored[256];
    CHECK_EQ(file_browser_put_back(second, restored, sizeof(restored)), FILE_BROWSER_OK);
    CHECK_STREQ(restored, "/home/b/notes");
    char text[16];
    fake_user_fs_read_text("/home/b/notes", text, sizeof(text));
    CHECK_STREQ(text, "from b");
}

TEST(file_browser, putting_back_onto_a_taken_name_does_not_overwrite_it) {
    fresh_home();
    fake_user_fs_write_text("/home/notes", "old");
    char trashed[256];
    CHECK_EQ(file_browser_trash("/home/notes", trashed, sizeof(trashed)), FILE_BROWSER_OK);
    fake_user_fs_write_text("/home/notes", "new");
    char restored[256];
    CHECK_EQ(file_browser_put_back(trashed, restored, sizeof(restored)), FILE_BROWSER_OK);
    CHECK_STREQ(restored, "/home/notes 2");
    char text[16];
    fake_user_fs_read_text("/home/notes", text, sizeof(text));
    CHECK_STREQ(text, "new");
}

TEST(file_browser, put_back_refuses_when_the_folder_is_gone) {
    fresh_home();
    fake_user_fs_mkdir("/home/gone");
    fake_user_fs_write("/home/gone/f", 1);
    char trashed[256];
    CHECK_EQ(file_browser_trash("/home/gone/f", trashed, sizeof(trashed)), FILE_BROWSER_OK);
    CHECK_EQ(sys_rmdir("/home/gone"), 0);
    CHECK_EQ(file_browser_put_back(trashed, 0, 0), FILE_BROWSER_ERROR_ORIGIN_GONE);
    CHECK(fake_user_fs_exists(trashed));
}

TEST(file_browser, the_trash_cannot_be_trashed) {
    fresh_home();
    fake_user_fs_write("/home/f", 1);
    char trashed[256];
    CHECK_EQ(file_browser_trash("/home/f", trashed, sizeof(trashed)), FILE_BROWSER_OK);
    CHECK_EQ(file_browser_trash(trashed, 0, 0), FILE_BROWSER_ERROR_IN_TRASH);
    CHECK_EQ(file_browser_trash(FILE_BROWSER_TRASH, 0, 0), FILE_BROWSER_ERROR_IN_TRASH);
    CHECK_EQ(file_browser_trash("/home", 0, 0), FILE_BROWSER_ERROR_INTO_ITSELF);
    CHECK(fake_user_fs_exists("/home"));
}

TEST(file_browser, emptying_the_trash_removes_trees_and_records) {
    fresh_home();
    fake_user_fs_mkdir("/home/tree");
    fake_user_fs_mkdir("/home/tree/deep");
    fake_user_fs_write("/home/tree/deep/x", 9);
    fake_user_fs_write("/home/loose", 1);
    fake_user_fs_write("/home/keep", 1);
    CHECK_EQ(file_browser_trash("/home/tree", 0, 0), FILE_BROWSER_OK);
    CHECK_EQ(file_browser_trash("/home/loose", 0, 0), FILE_BROWSER_OK);
    CHECK_EQ(file_browser_empty_trash(), 2);
    static file_browser_entry_t entries[8];
    CHECK_EQ(file_browser_list(FILE_BROWSER_TRASH, FILE_BROWSER_LIST_HIDDEN, entries, 8), 0);
    CHECK(fake_user_fs_exists("/home/keep"));
}

TEST(file_browser, deleting_from_the_trash_forgets_the_origin) {
    fresh_home();
    fake_user_fs_write("/home/f", 1);
    char trashed[256];
    CHECK_EQ(file_browser_trash("/home/f", trashed, sizeof(trashed)), FILE_BROWSER_OK);
    CHECK_EQ(file_browser_delete(trashed), FILE_BROWSER_OK);
    CHECK(!fake_user_fs_exists(trashed));
    CHECK(!fake_user_fs_exists(FILE_BROWSER_TRASH_ORIGINS "/f"));
}

typedef struct {
    char found[8][256];
    int count;
    int stop_after;
} search_log_t;

static int collect(void *context, const char *path, const os_stat_t *status) {
    (void)status;
    search_log_t *log = (search_log_t *)context;
    if (log->count < 8) {
        snprintf(log->found[log->count], sizeof(log->found[0]), "%s", path);
    }
    log->count++;
    return log->stop_after && log->count >= log->stop_after;
}

static int found_path(const search_log_t *log, const char *path) {
    for (int i = 0; i < log->count && i < 8; i++) {
        if (strcmp(log->found[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

TEST(file_browser, a_search_reaches_every_level_and_only_matches) {
    fresh_home();
    fake_user_fs_mkdir("/home/a");
    fake_user_fs_mkdir("/home/a/b");
    fake_user_fs_write("/home/Report.txt", 1);
    fake_user_fs_write("/home/a/b/old report", 1);
    fake_user_fs_write("/home/a/unrelated", 1);
    search_log_t log;
    memset(&log, 0, sizeof(log));
    long visited = file_browser_search("/home", "report", 0, 1000, collect, &log);
    CHECK_EQ(log.count, 2);
    CHECK(found_path(&log, "/home/Report.txt"));
    CHECK(found_path(&log, "/home/a/b/old report"));
    CHECK_EQ(visited, 5);
}

TEST(file_browser, a_search_skips_hidden_trees_unless_asked) {
    fresh_home();
    fake_user_fs_mkdir("/home/.secret");
    fake_user_fs_write("/home/.secret/match", 1);
    search_log_t log;
    memset(&log, 0, sizeof(log));
    file_browser_search("/home", "match", 0, 1000, collect, &log);
    CHECK_EQ(log.count, 0);
    file_browser_search("/home", "match", 1, 1000, collect, &log);
    CHECK_EQ(log.count, 1);
}

TEST(file_browser, a_search_stops_when_told_and_when_its_budget_runs_out) {
    fresh_home();
    for (int i = 0; i < 10; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/home/hit%d", i);
        fake_user_fs_write(path, 1);
    }
    search_log_t log;
    memset(&log, 0, sizeof(log));
    log.stop_after = 3;
    file_browser_search("/home", "hit", 0, 1000, collect, &log);
    CHECK_EQ(log.count, 3);
    memset(&log, 0, sizeof(log));
    long visited = file_browser_search("/home", "hit", 0, 4, collect, &log);
    CHECK_EQ(log.count, 4);
    CHECK(visited < 0);
}

TEST(file_browser, without_sniffing_a_program_is_only_named_not_opened) {
    fresh_home();
    fake_user_fs_write_text("/home/program", "\x7f" "ELF....");
    static file_browser_entry_t entries[4];
    REQUIRE(file_browser_list("/home", 0, entries, 4) == 1);
    CHECK(entries[0].kind != FILE_BROWSER_KIND_PROGRAM);
    CHECK(file_browser_needs_sniff(&entries[0]));
    file_browser_sniff_entry("/home/program", &entries[0]);
    CHECK_EQ(entries[0].kind, FILE_BROWSER_KIND_PROGRAM);
    CHECK_EQ(fake_user_fs_open_count(), 0);
}

TEST(file_browser, join_fits_a_path_that_exactly_fills_the_buffer) {
    char out[10];
    CHECK_EQ(file_browser_join("/home", "abc", out, sizeof(out)), FILE_BROWSER_OK);
    CHECK_STREQ(out, "/home/abc");
    CHECK_EQ(file_browser_join("/home", "abcd", out, sizeof(out)), FILE_BROWSER_ERROR_TOO_LONG);
}

TEST(file_browser, one_nul_among_plain_words_is_not_text) {
    static const unsigned char WORDS[] = "plenty of ordinary words here\0and more after it";
    CHECK(!file_browser_looks_like_text(WORDS, sizeof(WORDS) - 1));
    CHECK(file_browser_looks_like_text(WORDS, 29));
}

TEST(file_browser, equal_sizes_fall_back_to_the_name) {
    file_browser_entry_t entries[3] = {sample("c", 10, 0, 0), sample("a", 10, 0, 0), sample("b", 10, 0, 0)};
    int order[3];
    file_browser_sort(entries, order, 3, FILE_BROWSER_SORT_SIZE, 0);
    CHECK_STREQ(entries[order[0]].name, "a");
    CHECK_STREQ(entries[order[1]].name, "b");
    CHECK_STREQ(entries[order[2]].name, "c");
}

TEST(file_browser, a_copy_into_a_folder_that_is_not_there_fails_cleanly) {
    fresh_home();
    fake_user_fs_write_text("/home/a.txt", "alpha");
    CHECK_EQ(file_browser_copy("/home/a.txt", "/home/missing/a.txt"), FILE_BROWSER_ERROR_WRITE);
    CHECK(!fake_user_fs_exists("/home/missing"));
    CHECK(fake_user_fs_exists("/home/a.txt"));
    CHECK_EQ(fake_user_fs_open_count(), 0);
}
