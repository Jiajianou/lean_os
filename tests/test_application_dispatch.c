#include "check.h"

#include "../user_space/library/application_dispatch.c"

static const char *const NAMES[] = {"settings", "task_manager", "lvgl_demo"};
#define NAME_COUNT ((int)(sizeof(NAMES) / sizeof(NAMES[0])))

static int index_of(const char *path) {
    return application_dispatch_index(path, NAMES, NAME_COUNT);
}

TEST(application_dispatch, a_link_in_bin_chooses_the_application) {
    CHECK_EQ(index_of("/bin/settings"), 0);
    CHECK_EQ(index_of("/bin/task_manager"), 1);
    CHECK_EQ(index_of("/bin/lvgl_demo"), 2);
}

TEST(application_dispatch, the_bare_name_chooses_the_same_application) {
    CHECK_EQ(index_of("settings"), 0);
    CHECK_EQ(index_of("task_manager"), 1);
}

TEST(application_dispatch, a_deeper_path_still_dispatches_on_the_last_component) {
    CHECK_EQ(index_of("/pkg/bin/settings"), 0);
    CHECK_EQ(index_of("/a/b/c/d/task_manager"), 1);
}

TEST(application_dispatch, the_binary_under_its_own_name_is_not_an_application) {
    CHECK_EQ(index_of("/bin/desktop_applications"), -1);
}

TEST(application_dispatch, a_name_that_only_shares_a_prefix_is_not_a_match) {
    CHECK_EQ(index_of("/bin/setting"), -1);
    CHECK_EQ(index_of("/bin/settings2"), -1);
    CHECK_EQ(index_of("/bin/task"), -1);
    CHECK_EQ(index_of("/bin/task_manager_old"), -1);
}

TEST(application_dispatch, nothing_at_all_dispatches_to_nothing) {
    CHECK_EQ(index_of(""), -1);
    CHECK_EQ(index_of(NULL), -1);
    CHECK_EQ(index_of("/bin/"), -1);
    CHECK_EQ(index_of("/"), -1);
    CHECK_EQ(index_of("/bin/settings/"), -1);
}

TEST(application_dispatch, an_empty_table_dispatches_to_nothing) {
    CHECK_EQ(application_dispatch_index("/bin/settings", NAMES, 0), -1);
    CHECK_EQ(application_dispatch_index("/bin/settings", NULL, NAME_COUNT), -1);
}

TEST(application_dispatch, the_name_is_the_text_after_the_last_slash) {
    CHECK_STREQ(application_dispatch_name("/bin/settings"), "settings");
    CHECK_STREQ(application_dispatch_name("settings"), "settings");
    CHECK_STREQ(application_dispatch_name("/bin/"), "");
    CHECK_STREQ(application_dispatch_name(NULL), "");
}
