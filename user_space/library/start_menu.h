#pragma once

#include <stddef.h>
#include <stdint.h>

#include "recent.h"

#define START_MENU_NAME_MAX     32
#define START_MENU_QUERY_MAX    32
#define START_MENU_MAX_COMMANDS 512
#define START_MENU_COLUMNS      2

typedef enum {
    START_MENU_APP = 0,
    START_MENU_SETTING,
    START_MENU_RECENT,
    START_MENU_COMMAND,
} start_menu_kind_t;

typedef enum {
    START_MENU_ICON_APPLICATION = 0,
    START_MENU_ICON_FILES,
    START_MENU_ICON_BROWSER,
    START_MENU_ICON_TERMINAL,
    START_MENU_ICON_EDITOR,
    START_MENU_ICON_SETTINGS,
    START_MENU_ICON_TASKS,
    START_MENU_ICON_PAINT,
    START_MENU_ICON_CLOCK,
} start_menu_icon_t;

typedef struct {
    const char *name;
    const char *program;
    const char *argument;
    const char *words;
    start_menu_icon_t icon;
} start_menu_entry_t;

typedef struct {
    start_menu_kind_t kind;
    int index;
    int score;
} start_menu_result_t;

#define START_MENU_MAX_RESULTS (32 + RECENT_MAX + START_MENU_MAX_COMMANDS)

typedef struct {
    char commands[START_MENU_MAX_COMMANDS][START_MENU_NAME_MAX];
    int command_count;
    int commands_truncated;
    char recent[RECENT_MAX][PATH_MAX_LENGTH];
    int recent_count;
    char query[START_MENU_QUERY_MAX];
    int query_length;
    start_menu_result_t results[START_MENU_MAX_RESULTS];
    int result_count;
    int selected;
} start_menu_t;

extern const start_menu_entry_t START_MENU_APPS[];
extern const int START_MENU_APP_COUNT;
extern const start_menu_entry_t START_MENU_SETTINGS[];
extern const int START_MENU_SETTING_COUNT;

void start_menu_set_commands(start_menu_t *menu, const char *listing, long length);

void start_menu_search(start_menu_t *menu);

int start_menu_type(start_menu_t *menu, char character);

void start_menu_move(start_menu_t *menu, int dx, int dy);

int start_menu_searching(const start_menu_t *menu);

const char *start_menu_result_name(const start_menu_t *menu, int result);

const char *start_menu_result_kind(const start_menu_t *menu, int result);

start_menu_icon_t start_menu_result_icon(const start_menu_t *menu, int result);

int start_menu_catalogue_command(int kind, int index, char *path, size_t path_capacity, const char **argument);

int start_menu_setting_named(const char *argument);

int start_menu_result_command(const start_menu_t *menu, int result, char *path, size_t path_capacity,
                              const char **argument);
