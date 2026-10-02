#include "start_menu.h"

#include "paths.h"

/* M214. The Start menu used to be every name in /bin - 276 of them on the
   laptop, toybox's commands and the self-tests among them - in a box in the
   middle of the screen. What a person opens is an application, a setting or
   something they had open before; a command is still one search away, under
   them. This is the menu's catalogue, its search and its keyboard, kept apart
   from the compositor so a host test can grade the order things come back in. */

const start_menu_entry_t START_MENU_APPS[] = {
    {"Files", "file_manager", "", "finder folders documents browse disk", START_MENU_ICON_FILES},
    {"Browser", "browser", "", "web internet chromium chrome www", START_MENU_ICON_BROWSER},
    {"Terminal", "gui_terminal", "", "shell console command prompt", START_MENU_ICON_TERMINAL},
    {"Editor", "text_editor", "", "text notes write edit", START_MENU_ICON_EDITOR},
    {"Settings", "settings", "", "preferences control panel options system", START_MENU_ICON_SETTINGS},
    {"Tasks", "task_manager", "", "task manager processes activity monitor end", START_MENU_ICON_TASKS},
    {"Paint", "gui_paint", "", "draw picture drawing", START_MENU_ICON_PAINT},
    {"Clock", "gui_clock", "", "time watch", START_MENU_ICON_CLOCK},
};

const int START_MENU_APP_COUNT = (int)(sizeof(START_MENU_APPS) / sizeof(START_MENU_APPS[0]));

const start_menu_entry_t START_MENU_SETTINGS[] = {
    {"Display", "settings", "display", "screen resolution scale scaling size monitor bigger text",
     START_MENU_ICON_SETTINGS},
    {"Wallpaper", "settings", "wallpaper", "background desktop picture photo", START_MENU_ICON_SETTINGS},
    {"Appearance", "settings", "appearance", "colour color theme accent dark animations motion",
     START_MENU_ICON_SETTINGS},
    {"Sound", "settings", "sound", "volume audio speaker mute", START_MENU_ICON_SETTINGS},
    {"Mouse", "settings", "mouse", "pointer cursor speed buttons scrolling wheel", START_MENU_ICON_SETTINGS},
    {"Trackpad", "settings", "trackpad", "touchpad tap click gestures scrolling", START_MENU_ICON_SETTINGS},
    {"Keyboard", "settings", "keyboard", "shortcuts keys", START_MENU_ICON_SETTINGS},
    {"Date & Time", "settings", "date_time", "clock time zone 24-hour hour", START_MENU_ICON_SETTINGS},
    {"Network", "settings", "network", "internet address dns ethernet", START_MENU_ICON_SETTINGS},
    {"Wi-Fi", "wifi", "", "wireless network join password internet", START_MENU_ICON_SETTINGS},
    {"Storage", "settings", "storage", "disk space trash usage", START_MENU_ICON_SETTINGS},
    {"Privacy & Security", "settings", "privacy", "capabilities permissions security",
     START_MENU_ICON_SETTINGS},
    {"Startup", "settings", "startup", "session restore reopen windows boot", START_MENU_ICON_SETTINGS},
    {"About This Machine", "settings", "general", "general processor memory version system info",
     START_MENU_ICON_SETTINGS},
};

const int START_MENU_SETTING_COUNT = (int)(sizeof(START_MENU_SETTINGS) / sizeof(START_MENU_SETTINGS[0]));

/* Programs that are the desktop itself, or another name for an application
   already in the list, are not offered as commands. */
static const char *const NOT_COMMANDS[] = {
    "init", "compositor", "desktop_shell", "desktop_icons", "desktop_applications", "lvgl_demo", "wifi",
};

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int same(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int starts_with(const char *text, const char *query) {
    for (int i = 0; query[i]; i++) {
        if (lower(text[i]) != lower(query[i])) {
            return 0;
        }
    }
    return 1;
}

static int is_separator(char c) {
    return c == ' ' || c == '_' || c == '-' || c == '&' || c == '/' || c == '.';
}

static int word_starts_with(const char *text, const char *query) {
    for (int i = 0; text[i]; i++) {
        if ((i == 0 || is_separator(text[i - 1])) && !is_separator(text[i]) && starts_with(text + i, query)) {
            return 1;
        }
    }
    return 0;
}

static int contains(const char *text, const char *query) {
    for (int i = 0; text[i]; i++) {
        if (starts_with(text + i, query)) {
            return 1;
        }
    }
    return 0;
}

/* Higher is better; 0 is no match. A name that starts with what was typed
   beats a word inside it, which beats a word it is known by, which beats the
   letters turning up anywhere in the name. */
static int score_name(const char *name, const char *query) {
    if (starts_with(name, query)) {
        return 4;
    }
    if (word_starts_with(name, query)) {
        return 3;
    }
    if (contains(name, query)) {
        return 1;
    }
    return 0;
}

static int score_entry(const start_menu_entry_t *entry, const char *query) {
    int score = score_name(entry->name, query);
    if (score < 3 && starts_with(entry->program, query)) {
        score = 3;
    }
    if (score < 2 && word_starts_with(entry->words, query)) {
        score = 2;
    }
    return score;
}

static const char *basename_of(const char *path) {
    const char *base = path;
    for (const char *c = path; *c; c++) {
        if (*c == '/' && c[1]) {
            base = c + 1;
        }
    }
    return base;
}

static int offered_as_command(const char *name) {
    for (int i = 0; i < (int)(sizeof(NOT_COMMANDS) / sizeof(NOT_COMMANDS[0])); i++) {
        if (same(name, NOT_COMMANDS[i])) {
            return 0;
        }
    }
    for (int i = 0; i < START_MENU_APP_COUNT; i++) {
        if (same(name, START_MENU_APPS[i].program)) {
            return 0;
        }
    }
    return 1;
}

void start_menu_set_commands(start_menu_t *menu, const char *listing, long length) {
    menu->command_count = 0;
    menu->commands_truncated = 0;
    char name[START_MENU_NAME_MAX];
    int column = 0;
    int too_long = 0;
    for (long i = 0; i <= length; i++) {
        char c = i < length ? listing[i] : '\n';
        if (c != '\n') {
            if (column < START_MENU_NAME_MAX - 1) {
                name[column++] = c;
            } else {
                too_long = 1;
            }
            continue;
        }
        name[column] = '\0';
        int directory = column > 0 && name[column - 1] == '/';
        if (column > 0 && !directory && !too_long && offered_as_command(name)) {
            if (menu->command_count >= START_MENU_MAX_COMMANDS) {
                menu->commands_truncated = 1;
                return;
            }
            for (int k = 0; k <= column; k++) {
                menu->commands[menu->command_count][k] = name[k];
            }
            menu->command_count++;
        }
        column = 0;
        too_long = 0;
    }
}

static void add_result(start_menu_t *menu, start_menu_kind_t kind, int index, int score) {
    if (menu->result_count >= START_MENU_MAX_RESULTS) {
        return;
    }
    int at = menu->result_count++;
    while (at > 0 && menu->results[at - 1].kind == kind && menu->results[at - 1].score < score) {
        menu->results[at] = menu->results[at - 1];
        at--;
    }
    menu->results[at].kind = kind;
    menu->results[at].index = index;
    menu->results[at].score = score;
}

int start_menu_searching(const start_menu_t *menu) {
    return menu->query_length > 0;
}

void start_menu_search(start_menu_t *menu) {
    menu->result_count = 0;
    menu->selected = 0;
    const char *query = menu->query;
    if (!start_menu_searching(menu)) {
        for (int i = 0; i < START_MENU_APP_COUNT; i++) {
            add_result(menu, START_MENU_APP, i, 1);
        }
        for (int i = 0; i < menu->recent_count; i++) {
            add_result(menu, START_MENU_RECENT, i, 1);
        }
        return;
    }
    for (int i = 0; i < START_MENU_APP_COUNT; i++) {
        int score = score_entry(&START_MENU_APPS[i], query);
        if (score) {
            add_result(menu, START_MENU_APP, i, score);
        }
    }
    for (int i = 0; i < START_MENU_SETTING_COUNT; i++) {
        int score = score_entry(&START_MENU_SETTINGS[i], query);
        if (score) {
            add_result(menu, START_MENU_SETTING, i, score);
        }
    }
    for (int i = 0; i < menu->recent_count; i++) {
        int score = score_name(basename_of(menu->recent[i]), query);
        if (score) {
            add_result(menu, START_MENU_RECENT, i, score);
        }
    }
    for (int i = 0; i < menu->command_count; i++) {
        int score = score_name(menu->commands[i], query);
        if (score) {
            add_result(menu, START_MENU_COMMAND, i, score);
        }
    }
}

/* Returns 1 when the query changed and the results were made again. */
int start_menu_type(start_menu_t *menu, char character) {
    if (character == '\b' || character == 0x7F) {
        if (menu->query_length == 0) {
            return 0;
        }
        menu->query[--menu->query_length] = '\0';
        start_menu_search(menu);
        return 1;
    }
    if (character < 0x20 || character >= 0x7F || menu->query_length >= START_MENU_QUERY_MAX - 1) {
        return 0;
    }
    if (character == ' ' && menu->query_length == 0) {
        return 0;
    }
    menu->query[menu->query_length++] = character;
    menu->query[menu->query_length] = '\0';
    start_menu_search(menu);
    return 1;
}

static int app_results(const start_menu_t *menu) {
    int apps = 0;
    while (apps < menu->result_count && menu->results[apps].kind == START_MENU_APP) {
        apps++;
    }
    return apps;
}

/* The applications are a grid when nothing has been typed: left and right
   move along a row, up and down by a row, and down from the last row goes
   on into the recent files below it. A search is one list. */
void start_menu_move(start_menu_t *menu, int dx, int dy) {
    if (menu->result_count == 0) {
        menu->selected = 0;
        return;
    }
    int at = menu->selected;
    if (start_menu_searching(menu)) {
        at += dx + dy;
    } else {
        int apps = app_results(menu);
        if (at < apps) {
            at += dx + dy * START_MENU_COLUMNS;
            if (dy > 0 && at >= apps) {
                at = apps;
            }
        } else {
            at += dx + dy;
            if (dy < 0 && at < apps) {
                at = apps - 1 - ((apps - 1) % START_MENU_COLUMNS);
            }
        }
    }
    if (at < 0) {
        at = 0;
    }
    if (at >= menu->result_count) {
        at = menu->result_count - 1;
    }
    menu->selected = at;
}

const char *start_menu_result_name(const start_menu_t *menu, int result) {
    const start_menu_result_t *r = &menu->results[result];
    switch (r->kind) {
    case START_MENU_APP:
        return START_MENU_APPS[r->index].name;
    case START_MENU_SETTING:
        return START_MENU_SETTINGS[r->index].name;
    case START_MENU_RECENT:
        return basename_of(menu->recent[r->index]);
    default:
        return menu->commands[r->index];
    }
}

const char *start_menu_result_kind(const start_menu_t *menu, int result) {
    static const char *const KINDS[] = {"Application", "Settings", "Recent", "Command"};
    return KINDS[menu->results[result].kind];
}

start_menu_icon_t start_menu_result_icon(const start_menu_t *menu, int result) {
    const start_menu_result_t *r = &menu->results[result];
    switch (r->kind) {
    case START_MENU_APP:
        return START_MENU_APPS[r->index].icon;
    case START_MENU_SETTING:
        return START_MENU_SETTINGS[r->index].icon;
    case START_MENU_RECENT: {
        const char *path = menu->recent[r->index];
        int length = 0;
        while (path[length]) {
            length++;
        }
        return length > 0 && path[length - 1] == '/' ? START_MENU_ICON_FILES : START_MENU_ICON_EDITOR;
    }
    default:
        return START_MENU_ICON_APPLICATION;
    }
}

static int join(char *out, size_t capacity, const char *directory, const char *name) {
    size_t n = 0;
    for (const char *c = directory; *c; c++) {
        if (n + 1 >= capacity) {
            return -1;
        }
        out[n++] = *c;
    }
    for (const char *c = name; *c; c++) {
        if (n + 1 >= capacity) {
            return -1;
        }
        out[n++] = *c;
    }
    out[n] = '\0';
    return 0;
}

/* What to start for a result: the program's path, and the argument it is
   given - a Settings pane's name, or the file a recent entry names, which
   opens in Files when it is a folder and in the Editor when it is not. */
int start_menu_result_command(const start_menu_t *menu, int result, char *path, size_t path_capacity,
                              const char **argument) {
    if (result < 0 || result >= menu->result_count) {
        return -1;
    }
    const start_menu_result_t *r = &menu->results[result];
    switch (r->kind) {
    case START_MENU_APP:
        *argument = START_MENU_APPS[r->index].argument;
        return join(path, path_capacity, PATH_BIN_DIRECTORY, START_MENU_APPS[r->index].program);
    case START_MENU_SETTING:
        *argument = START_MENU_SETTINGS[r->index].argument;
        return join(path, path_capacity, PATH_BIN_DIRECTORY, START_MENU_SETTINGS[r->index].program);
    case START_MENU_RECENT: {
        *argument = menu->recent[r->index];
        int folder = start_menu_result_icon(menu, result) == START_MENU_ICON_FILES;
        return join(path, path_capacity, PATH_BIN_DIRECTORY, folder ? "file_manager" : "text_editor");
    }
    default:
        *argument = "";
        return join(path, path_capacity, PATH_BIN_DIRECTORY, menu->commands[r->index]);
    }
}
