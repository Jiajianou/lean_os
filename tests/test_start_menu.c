#include <stdio.h>
#include <string.h>

#include "check.h"

#include "../user_space/library/start_menu.c"

static start_menu_t menu;

static const char LISTING[] = "toybox\nls\ngrep\nreboot\npopuptest\nsettings\nfile_manager\ngui_terminal\n"
                              "compositor\ndesktop_shell\nlocal/\ntext_editor\nwm_crash\nsh\n";

static void open_menu(void) {
    memset(&menu, 0, sizeof(menu));
    start_menu_set_commands(&menu, LISTING, (long)strlen(LISTING));
    strcpy(menu.recent[0], "/home/notes.txt");
    strcpy(menu.recent[1], "/home/Pictures/");
    menu.recent_count = 2;
    start_menu_search(&menu);
}

static void type(const char *text) {
    for (const char *c = text; *c; c++) {
        start_menu_type(&menu, *c);
    }
}

static int find(const char *name) {
    for (int i = 0; i < menu.result_count; i++) {
        if (strcmp(start_menu_result_name(&menu, i), name) == 0) {
            return i;
        }
    }
    return -1;
}

TEST(start_menu, nothing_typed_shows_the_applications_then_what_was_open_before) {
    open_menu();
    CHECK_EQ(menu.result_count, START_MENU_APP_COUNT + 2);
    CHECK_EQ(strcmp(start_menu_result_name(&menu, 0), "Files"), 0);
    CHECK_EQ(menu.results[START_MENU_APP_COUNT].kind, START_MENU_RECENT);
    CHECK_EQ(strcmp(start_menu_result_name(&menu, START_MENU_APP_COUNT), "notes.txt"), 0);
    CHECK_EQ(find("ls"), -1);
    CHECK_EQ(menu.selected, 0);
}

TEST(start_menu, the_desktop_s_own_programs_and_applications_are_not_commands) {
    open_menu();
    for (int i = 0; i < menu.command_count; i++) {
        CHECK(strcmp(menu.commands[i], "compositor") != 0);
        CHECK(strcmp(menu.commands[i], "settings") != 0);
        CHECK(strcmp(menu.commands[i], "gui_terminal") != 0);
        CHECK(strcmp(menu.commands[i], "local/") != 0);
        CHECK(strcmp(menu.commands[i], "local") != 0);
    }
    CHECK_EQ(menu.command_count, 7);
}

TEST(start_menu, an_application_comes_before_a_setting_before_a_command) {
    open_menu();
    type("se");
    CHECK(menu.result_count >= 2);
    CHECK_EQ(menu.results[0].kind, START_MENU_APP);
    CHECK_EQ(strcmp(start_menu_result_name(&menu, 0), "Settings"), 0);
    int kind = START_MENU_APP;
    for (int i = 0; i < menu.result_count; i++) {
        CHECK((int)menu.results[i].kind >= kind);
        kind = (int)menu.results[i].kind;
    }
}

TEST(start_menu, a_setting_is_found_by_the_words_people_use_for_it) {
    open_menu();
    type("resolution");
    CHECK_EQ(menu.result_count, 1);
    CHECK_EQ(menu.results[0].kind, START_MENU_SETTING);
    CHECK_EQ(strcmp(start_menu_result_name(&menu, 0), "Display"), 0);
    char path[64];
    const char *argument = 0;
    CHECK_EQ(start_menu_result_command(&menu, 0, path, sizeof(path), &argument), 0);
    CHECK_EQ(strcmp(path, "/bin/settings"), 0);
    CHECK_EQ(strcmp(argument, "display"), 0);
}

TEST(start_menu, a_typed_program_name_still_finds_the_program) {
    open_menu();
    type("reboot");
    CHECK_EQ(menu.result_count, 1);
    char path[64];
    const char *argument = 0;
    CHECK_EQ(start_menu_result_command(&menu, 0, path, sizeof(path), &argument), 0);
    CHECK_EQ(strcmp(path, "/bin/reboot"), 0);
    CHECK_EQ(strcmp(argument, ""), 0);

    open_menu();
    type("SETTINGS");
    CHECK_EQ(menu.results[0].kind, START_MENU_APP);
    CHECK_EQ(start_menu_result_command(&menu, 0, path, sizeof(path), &argument), 0);
    CHECK_EQ(strcmp(path, "/bin/settings"), 0);
}

TEST(start_menu, a_name_that_starts_with_the_query_beats_one_that_contains_it) {
    open_menu();
    type("p");
    int popup = find("popuptest");
    int grep = find("grep");
    CHECK(popup >= 0);
    CHECK(grep >= 0);
    CHECK(popup < grep);
    CHECK_EQ(menu.results[0].kind, START_MENU_APP);
    CHECK_EQ(strcmp(start_menu_result_name(&menu, 0), "Paint"), 0);
}

TEST(start_menu, an_application_is_found_by_its_program_name_and_what_it_does) {
    open_menu();
    type("gui_term");
    CHECK_EQ(strcmp(start_menu_result_name(&menu, 0), "Terminal"), 0);
    open_menu();
    type("shell");
    CHECK_EQ(strcmp(start_menu_result_name(&menu, 0), "Terminal"), 0);
    open_menu();
    type("web");
    CHECK_EQ(strcmp(start_menu_result_name(&menu, 0), "Browser"), 0);
}

TEST(start_menu, a_recent_folder_opens_in_files_and_a_recent_file_in_the_editor) {
    open_menu();
    char path[64];
    const char *argument = 0;
    int file = START_MENU_APP_COUNT;
    CHECK_EQ(start_menu_result_command(&menu, file, path, sizeof(path), &argument), 0);
    CHECK_EQ(strcmp(path, "/bin/text_editor"), 0);
    CHECK_EQ(strcmp(argument, "/home/notes.txt"), 0);
    CHECK_EQ(start_menu_result_command(&menu, file + 1, path, sizeof(path), &argument), 0);
    CHECK_EQ(strcmp(path, "/bin/file_manager"), 0);
    CHECK_EQ(strcmp(start_menu_result_name(&menu, file + 1), "Pictures/"), 0);
}

TEST(start_menu, backspace_widens_the_search_again_and_nothing_left_is_the_grid) {
    open_menu();
    type("zzz");
    CHECK_EQ(menu.result_count, 0);
    CHECK_EQ(start_menu_type(&menu, '\b'), 1);
    CHECK_EQ(start_menu_type(&menu, '\b'), 1);
    CHECK_EQ(start_menu_type(&menu, '\b'), 1);
    CHECK_EQ(start_menu_type(&menu, '\b'), 0);
    CHECK(!start_menu_searching(&menu));
    CHECK_EQ(menu.result_count, START_MENU_APP_COUNT + 2);
}

TEST(start_menu, a_leading_space_and_control_characters_are_not_a_search) {
    open_menu();
    CHECK_EQ(start_menu_type(&menu, ' '), 0);
    CHECK_EQ(start_menu_type(&menu, 27), 0);
    CHECK(!start_menu_searching(&menu));
}

TEST(start_menu, the_grid_moves_by_rows_and_runs_on_into_the_recent_files) {
    open_menu();
    start_menu_move(&menu, 1, 0);
    CHECK_EQ(menu.selected, 1);
    start_menu_move(&menu, 0, 1);
    CHECK_EQ(menu.selected, 1 + START_MENU_COLUMNS);
    start_menu_move(&menu, 0, 10);
    CHECK_EQ(menu.selected, START_MENU_APP_COUNT);
    start_menu_move(&menu, 0, 1);
    CHECK_EQ(menu.selected, START_MENU_APP_COUNT + 1);
    start_menu_move(&menu, 0, 1);
    CHECK_EQ(menu.selected, START_MENU_APP_COUNT + 1);
    start_menu_move(&menu, 0, -1);
    CHECK_EQ(menu.selected, START_MENU_APP_COUNT);
    start_menu_move(&menu, 0, -1);
    CHECK_EQ(menu.selected, START_MENU_APP_COUNT - START_MENU_COLUMNS);
    start_menu_move(&menu, -100, 0);
    CHECK_EQ(menu.selected, 0);
}

TEST(start_menu, a_search_is_one_list_and_selection_stays_inside_it) {
    open_menu();
    type("t");
    int count = menu.result_count;
    CHECK(count > 3);
    start_menu_move(&menu, 0, 1);
    CHECK_EQ(menu.selected, 1);
    start_menu_move(&menu, 0, 1000);
    CHECK_EQ(menu.selected, count - 1);
    type("ex");
    CHECK_EQ(menu.selected, 0);
}

TEST(start_menu, more_programs_than_the_menu_holds_says_so) {
    static char listing[START_MENU_MAX_COMMANDS * 8 + 64];
    size_t n = 0;
    for (int i = 0; i < START_MENU_MAX_COMMANDS + 5; i++) {
        n += (size_t)snprintf(listing + n, sizeof(listing) - n, "p%d\n", i);
    }
    memset(&menu, 0, sizeof(menu));
    start_menu_set_commands(&menu, listing, (long)n);
    CHECK_EQ(menu.command_count, START_MENU_MAX_COMMANDS);
    CHECK_EQ(menu.commands_truncated, 1);
}

TEST(start_menu, a_name_longer_than_the_menu_keeps_is_left_out_rather_than_cut) {
    static const char listing[] = "short\nthis_program_name_is_much_longer_than_thirty_one\nafter";
    memset(&menu, 0, sizeof(menu));
    start_menu_set_commands(&menu, listing, (long)strlen(listing));
    CHECK_EQ(menu.command_count, 2);
    CHECK_EQ(strcmp(menu.commands[1], "after"), 0);
}

TEST(start_menu, the_catalogue_names_only_what_the_menu_would_start) {
    char path[64];
    const char *argument = 0;
    int date_time = start_menu_setting_named("date_time");
    CHECK(date_time >= 0);
    CHECK_EQ(start_menu_catalogue_command(START_MENU_SETTING, date_time, path, sizeof(path), &argument), 0);
    CHECK_EQ(strcmp(path, "/bin/settings"), 0);
    CHECK_EQ(strcmp(argument, "date_time"), 0);
    CHECK_EQ(start_menu_catalogue_command(START_MENU_APP, 0, path, sizeof(path), &argument), 0);
    CHECK_EQ(strcmp(path, "/bin/file_manager"), 0);
    CHECK_EQ(start_menu_catalogue_command(START_MENU_APP, START_MENU_APP_COUNT, path, sizeof(path), &argument), -1);
    CHECK_EQ(start_menu_catalogue_command(START_MENU_SETTING, -1, path, sizeof(path), &argument), -1);
    CHECK_EQ(start_menu_catalogue_command(START_MENU_COMMAND, 0, path, sizeof(path), &argument), -1);
    CHECK_EQ(start_menu_catalogue_command(START_MENU_RECENT, 0, path, sizeof(path), &argument), -1);
    CHECK_EQ(start_menu_setting_named("nonsense"), -1);
}
