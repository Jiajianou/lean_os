#include <stdio.h>
#include <string.h>

#include "capabilities.h"
#include "desktop_applications.h"
#include "file_browser.h"
#include "file_system_utilities.h"
#include "input.h"
#include "lvgl_theme.h"
#include "os_file_system.h"
#include "paths.h"
#include "recent.h"
#include "settings_file.h"
#include "spawn_error.h"
#include "syscall_wrappers.h"
#include "time_format.h"
#include "wallpaper.h"

#define FILES_WINDOW_WIDTH  760
#define FILES_WINDOW_HEIGHT 480
#define FILES_SIDEBAR_WIDTH 172
#define FILES_TOOLBAR_HEIGHT 48
#define FILES_HEADER_HEIGHT 26
#define FILES_PAGE_ROWS 12
#define FILES_ROW_HEIGHT 26
#define FILES_PATH_BAR_HEIGHT 26
#define FILES_STATUS_HEIGHT 24
#define FILES_TILE_WIDTH 100
#define FILES_TILE_HEIGHT 92
#define FILES_ICON_COLUMN 20
#define FILES_DATE_COLUMN 136
#define FILES_SIZE_COLUMN 64
#define FILES_KIND_COLUMN 120
#define FILES_MENU_WIDTH 220
#define FILES_MENU_ITEM_HEIGHT 26
#define FILES_MENU_SEPARATOR 9

#define FILES_MAX_ENTRIES 1024
#define FILES_MAX_RESULTS 400
#define FILES_LOCATION_MAX 512
#define FILES_HISTORY_MAX 32
#define FILES_REFRESH_MS 2000
#define FILES_SEARCH_DELAY_MS 300
#define FILES_SEARCH_BUDGET 20000
#define FILES_DOUBLE_CLICK_MS 500
#define FILES_DRAG_THRESHOLD 8
#define FILES_TYPE_SELECT_MS 1000
#define FILES_STATUS_MS 5000
#define FILES_SPACE_MS 5000
#define FILES_PREVIEW_TEXT_BYTES 12288
#define FILES_PREVIEW_BYTE_COUNT 384
#define FILES_PREVIEW_WIDTH 580
#define FILES_PREVIEW_HEIGHT 400
#define FILES_CLIPBOARD_MAX 8192
#define FILES_OPEN_AT_ONCE 8
#define FILES_MINIMUM_DRAWN_PIXELS 20000

#define UNDO_DEPTH 8
#define UNDO_ITEMS 32
#define UNDO_PATH  256

typedef enum {
    FILES_MODE_FOLDER = 0,
    FILES_MODE_RECENT,
    FILES_MODE_SEARCH,
} files_mode_t;

typedef struct {
    files_mode_t mode;
    char path[FILES_LOCATION_MAX];
} files_location_t;

typedef struct {
    const char *name;
    const char *symbol;
    const char *path;
    files_mode_t mode;
    const char *report;
} files_place_t;

static const files_place_t PLACES[] = {
    {"Home", LV_SYMBOL_HOME, PATH_HOME, FILES_MODE_FOLDER, "place_home"},
    {"Recent", LV_SYMBOL_LOOP, "", FILES_MODE_RECENT, "place_recent"},
    {"Applications", LV_SYMBOL_LIST, PATH_BIN, FILES_MODE_FOLDER, "place_applications"},
    {"Temporary", LV_SYMBOL_DIRECTORY, PATH_TEMPORARY, FILES_MODE_FOLDER, "place_temporary"},
    {"Computer", LV_SYMBOL_DRIVE, "/", FILES_MODE_FOLDER, "place_computer"},
    {"Packages", LV_SYMBOL_DOWNLOAD, PKG_ROOT, FILES_MODE_FOLDER, "place_packages"},
    {"Trash", LV_SYMBOL_TRASH, FILE_BROWSER_TRASH, FILES_MODE_FOLDER, "place_trash"},
};

#define PLACE_COUNT ((int)(sizeof(PLACES) / sizeof(PLACES[0])))
#define PLACE_FIRST_LOCATION 4

typedef enum {
    ACTION_OPEN = 0,
    ACTION_OPEN_IN_EDITOR,
    ACTION_OPEN_TERMINAL,
    ACTION_QUICK_LOOK,
    ACTION_INFO,
    ACTION_RENAME,
    ACTION_DUPLICATE,
    ACTION_COPY,
    ACTION_CUT,
    ACTION_PASTE,
    ACTION_TRASH,
    ACTION_DELETE,
    ACTION_PUT_BACK,
    ACTION_EMPTY_TRASH,
    ACTION_NEW_FOLDER,
    ACTION_NEW_FILE,
    ACTION_NEW_WINDOW,
    ACTION_SHOW_ENCLOSING,
    ACTION_VIEW_LIST,
    ACTION_VIEW_ICONS,
    ACTION_TOGGLE_HIDDEN,
    ACTION_SELECT_ALL,
    ACTION_UNDO,
    ACTION_GO_TO,
    ACTION_SET_DESKTOP_PICTURE,
    ACTION_SEPARATOR,
} files_action_t;

static const char *const ACTION_REPORT[] = {
    "menu_open", "menu_open_in_editor", "menu_open_terminal", "menu_quick_look", "menu_info",
    "menu_rename", "menu_duplicate", "menu_copy", "menu_cut", "menu_paste", "menu_trash",
    "menu_delete", "menu_put_back", "menu_empty_trash", "menu_new_folder", "menu_new_file",
    "menu_new_window", "menu_show_enclosing", "menu_view_list", "menu_view_icons",
    "menu_toggle_hidden", "menu_select_all", "menu_undo", "menu_go_to", "menu_set_desktop_picture", "",
};

typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_MENU,
    OVERLAY_NAME,
    OVERLAY_CONFIRM,
    OVERLAY_INFO,
    OVERLAY_PREVIEW,
} files_overlay_t;

typedef enum {
    NAME_NEW_FOLDER = 0,
    NAME_NEW_FILE,
    NAME_RENAME,
    NAME_GO_TO,
} files_name_purpose_t;

typedef enum {
    CONFIRM_DELETE = 0,
    CONFIRM_EMPTY_TRASH,
    CONFIRM_DELETE_INSTEAD,
} files_confirm_purpose_t;

typedef enum {
    UNDO_MOVE = 1,
    UNDO_TRASH,
    UNDO_CREATE,
} files_undo_kind_t;

typedef struct {
    files_undo_kind_t kind;
    int count;
    char from[UNDO_ITEMS][UNDO_PATH];
    char to[UNDO_ITEMS][UNDO_PATH];
} files_undo_t;

static lvgl_window_t files_window;
static uint32_t own_capabilities;

static files_location_t here;
static files_location_t back_history[FILES_HISTORY_MAX];
static int back_count;
static files_location_t forward_history[FILES_HISTORY_MAX];
static int forward_count;

static file_browser_entry_t entry_buffers[2][FILES_MAX_ENTRIES];
static file_browser_entry_t *entries = entry_buffers[0];
static int entry_count;
static char result_paths[FILES_MAX_RESULTS][FILES_LOCATION_MAX];
static int order[FILES_MAX_ENTRIES];
static uint8_t selected[FILES_MAX_ENTRIES];
static lv_obj_t *item_objects[FILES_MAX_ENTRIES];
static int cursor = -1;
static int anchor = -1;
static uint32_t listing_signature;
static int listing_missing;

static file_browser_sort_t sort_key = FILE_BROWSER_SORT_NAME;
static int sort_descending;
static int view_icons;
static int show_hidden;

static int clock_24_hour = 1;
static int32_t utc_offset_minutes;

static char search_query[64];
static char search_scope[FILES_LOCATION_MAX];
static char search_origin[FILES_LOCATION_MAX];
static long search_due_ms;
static long search_visited;
static int search_truncated;

static long last_press_ms = -1;
static int last_press_position = -1;
static int press_position = -1;
static int32_t press_x;
static int32_t press_y;
static int dragging;
static uint8_t previous_buttons;

static char type_select[32];
static int type_select_length;
static long type_select_ms;

static char status_message[160];
static int status_is_error;
static long status_expires_ms;
static long space_next_ms;
static os_statvfs_t space_cached;
static int space_known;
static long refresh_next_ms;

static files_undo_t undo_stack[UNDO_DEPTH];
static int undo_count;

static char clipboard_mine[FILES_CLIPBOARD_MAX];
static int clipboard_is_cut;

static lv_obj_t *sidebar_items[PLACE_COUNT];
static lv_obj_t *back_button;
static lv_obj_t *forward_button;
static lv_obj_t *title_label;
static lv_obj_t *list_button;
static lv_obj_t *icons_button;
static lv_obj_t *search_field;
static lv_obj_t *header_row;
static lv_obj_t *header_labels[FILE_BROWSER_SORT_COUNT];
static lv_obj_t *trash_bar;
static lv_obj_t *search_bar;
static lv_obj_t *scope_here_button;
static lv_obj_t *scope_computer_button;
static lv_obj_t *content;
static lv_obj_t *empty_label;
static lv_obj_t *path_bar;
static lv_obj_t *status_label;

static files_overlay_t overlay_kind;
static lv_obj_t *overlay;
static lv_obj_t *overlay_field;
static files_name_purpose_t name_purpose;
static files_confirm_purpose_t confirm_purpose;
static lv_obj_t *preview_body;
static int typing_in_search;

static void show_location(const files_location_t *location, int remember);
static void rebuild_items(void);
static void refresh_listing(int force);
static void update_chrome(void);
static void update_status(void);
static void close_overlay(void);
static void run_action(files_action_t action);
static void open_preview(void);

static const desktop_palette_t *palette(void) {
    return lvgl_theme_palette();
}

static lv_color_t color(uint32_t rgb) {
    return lvgl_theme_color(rgb);
}

static void say(const char *message, int is_error) {
    snprintf(status_message, sizeof(status_message), "%s", message);
    status_is_error = is_error;
    status_expires_ms = sys_uptime_ms() + FILES_STATUS_MS;
    update_status();
}

static void say_error(int error) {
    say(file_browser_error_message(error), 1);
}

static int in_trash_view(void) {
    return here.mode == FILES_MODE_FOLDER && strcmp(here.path, FILE_BROWSER_TRASH) == 0;
}

static int virtual_view(void) {
    return here.mode != FILES_MODE_FOLDER;
}

static int entry_path(int index, char *out, size_t capacity) {
    if (index < 0 || index >= entry_count) {
        return -1;
    }
    if (virtual_view()) {
        if (index >= FILES_MAX_RESULTS || strlen(result_paths[index]) + 1 > capacity) {
            return -1;
        }
        memcpy(out, result_paths[index], strlen(result_paths[index]) + 1);
        return 0;
    }
    return file_browser_join(here.path, entries[index].name, out, capacity) == FILE_BROWSER_OK ? 0 : -1;
}

static int selection_count(void) {
    int count = 0;
    for (int i = 0; i < entry_count; i++) {
        count += selected[i] != 0;
    }
    return count;
}

static int first_selected(void) {
    for (int position = 0; position < entry_count; position++) {
        if (selected[order[position]]) {
            return order[position];
        }
    }
    return -1;
}

static const char *kind_symbol(int kind) {
    switch (kind) {
    case FILE_BROWSER_KIND_FOLDER:
        return LV_SYMBOL_DIRECTORY;
    case FILE_BROWSER_KIND_PROGRAM:
        return LV_SYMBOL_PLAY;
    case FILE_BROWSER_KIND_SOURCE:
        return LV_SYMBOL_EDIT;
    case FILE_BROWSER_KIND_IMAGE:
        return LV_SYMBOL_IMAGE;
    case FILE_BROWSER_KIND_ARCHIVE:
        return LV_SYMBOL_SAVE;
    case FILE_BROWSER_KIND_FONT:
        return LV_SYMBOL_KEYBOARD;
    case FILE_BROWSER_KIND_LINK:
        return LV_SYMBOL_SHUFFLE;
    default:
        return LV_SYMBOL_FILE;
    }
}

static uint32_t kind_color(int kind) {
    switch (kind) {
    case FILE_BROWSER_KIND_FOLDER:
        return palette()->accent;
    case FILE_BROWSER_KIND_PROGRAM:
        return 0x0027AE60u;
    case FILE_BROWSER_KIND_SOURCE:
        return 0x00E67E22u;
    case FILE_BROWSER_KIND_IMAGE:
        return 0x009B59B6u;
    case FILE_BROWSER_KIND_ARCHIVE:
        return 0x00B8964Au;
    case FILE_BROWSER_KIND_FONT:
        return 0x003498DBu;
    case FILE_BROWSER_KIND_DOCUMENT:
        return 0x00E74C3Cu;
    case FILE_BROWSER_KIND_LINK:
        return 0x0016A085u;
    case FILE_BROWSER_KIND_TEXT:
        return 0x008A94A8u;
    default:
        return 0x00707A88u;
    }
}

static uint32_t now_unix(void) {
    os_datetime_t now;
    if (sys_time(&now) > 0 && now.valid) {
        return os_unix_time(&now);
    }
    return 0;
}

static void load_clock_settings(void) {
    window_manager_settings_request_t settings;
    settings_file_defaults(&settings);
    settings_file_load(&settings);
    clock_24_hour = settings.clock_24_hour != 0;
    utc_offset_minutes = settings.utc_offset_minutes;
}

static void format_size(uint32_t bytes, char *out, size_t capacity) {
    if (bytes < 1000) {
        snprintf(out, capacity, "%u bytes", (unsigned)bytes);
        return;
    }
    char cell[FILE_SYSTEM_UTILITIES_SIZE_MAX];
    file_system_utilities_format_size(bytes, cell);
    size_t n = strlen(cell);
    char unit = cell[n - 1];
    cell[n - 1] = '\0';
    snprintf(out, capacity, "%s %cB", cell, unit);
}

static void format_large_size(uint64_t bytes, char *out, size_t capacity) {
    static const char *const UNITS[] = {"bytes", "KB", "MB", "GB", "TB"};
    int unit = 0;
    uint64_t whole = bytes;
    uint64_t tenths = 0;
    while (whole >= 1024 && unit < 4) {
        tenths = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        unit++;
    }
    if (unit == 0) {
        snprintf(out, capacity, "%u bytes", (unsigned)whole);
    } else if (whole < 10) {
        snprintf(out, capacity, "%u.%u %s", (unsigned)whole, (unsigned)tenths, UNITS[unit]);
    } else {
        snprintf(out, capacity, "%u %s", (unsigned)whole, UNITS[unit]);
    }
}

static lv_obj_t *plain(lv_obj_t *parent) {
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS |
                                   LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return object;
}

static lv_obj_t *text(lv_obj_t *parent, const char *value, uint32_t rgb, const lv_font_t *font) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, value);
    lv_obj_set_style_text_color(label, color(rgb), LV_PART_MAIN);
    if (font) {
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }
    return label;
}

static lv_obj_t *tool_button(lv_obj_t *parent, const char *symbol, int width) {
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 30);
    lv_obj_set_style_radius(button, LVGL_THEME_CONTROL_R, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, color(palette()->outline), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, color(palette()->surface_raised), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, color(desktop_palette_mix(palette()->surface_raised, 0x00FFFFFFu, 12u)),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, color(palette()->text), LV_PART_MAIN);
    lv_obj_set_style_text_color(button, color(palette()->text_dim), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);
    return button;
}

static void set_enabled(lv_obj_t *object, int enabled) {
    if (enabled) {
        lv_obj_remove_state(object, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(object, LV_STATE_DISABLED);
    }
}

static void folder_title(const char *path, char *out, size_t capacity) {
    for (int i = 0; i < PLACE_COUNT; i++) {
        if (PLACES[i].mode == FILES_MODE_FOLDER && strcmp(PLACES[i].path, path) == 0) {
            snprintf(out, capacity, "%s", PLACES[i].name);
            return;
        }
    }
    snprintf(out, capacity, "%s", file_browser_basename(path));
}

static uint32_t entry_signature(const file_browser_entry_t *list, int count) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < count; i++) {
        for (const char *c = list[i].name; *c; c++) {
            hash = (hash ^ (uint8_t)*c) * 16777619u;
        }
        hash = (hash ^ list[i].size) * 16777619u;
        hash = (hash ^ list[i].mtime) * 16777619u;
        hash = (hash ^ list[i].is_directory) * 16777619u;
    }
    return (hash ^ (uint32_t)count) * 16777619u;
}

static const file_browser_entry_t *previous_entry(const file_browser_entry_t *previous,
                                                  int previous_count, const char *name) {
    for (int i = 0; i < previous_count; i++) {
        if (strcmp(previous[i].name, name) == 0) {
            return &previous[i];
        }
    }
    return 0;
}

static int load_folder(file_browser_entry_t *into, int *missing) {
    int flags = show_hidden ? FILE_BROWSER_LIST_HIDDEN : 0;
    int count = file_browser_list(here.path, flags, into, FILES_MAX_ENTRIES);
    *missing = count < 0;
    if (count < 0) {
        return 0;
    }
    if (in_trash_view()) {
        int kept = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(into[i].name, FILE_BROWSER_ORIGINS_NAME) != 0) {
                into[kept++] = into[i];
            }
        }
        count = kept;
    }
    char full[PATH_MAX_LENGTH];
    for (int i = 0; i < count; i++) {
        if (!file_browser_needs_sniff(&into[i])) {
            continue;
        }
        const file_browser_entry_t *old = previous_entry(entries, entry_count, into[i].name);
        if (old && old->size == into[i].size && old->mtime == into[i].mtime && !virtual_view()) {
            into[i].kind = old->kind;
            into[i].kind_name = old->kind_name;
        } else if (file_browser_join(here.path, into[i].name, full, sizeof(full)) == FILE_BROWSER_OK) {
            file_browser_sniff_entry(full, &into[i]);
        }
    }
    return count;
}

static void fill_entry_from_path(file_browser_entry_t *entry, const char *path) {
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->name, sizeof(entry->name), "%s", file_browser_basename(path));
    os_stat_t status;
    os_stat_t link_status;
    if (sys_stat(path, &status) == 0) {
        entry->size = status.size;
        entry->mtime = status.mtime;
        entry->is_directory = status.is_directory;
    }
    if (sys_lstat(path, &link_status) == 0) {
        entry->is_link = link_status.is_link;
    }
    file_browser_sniff_entry(path, entry);
}

static int load_recent(file_browser_entry_t *into) {
    static char recent[RECENT_MAX][PATH_MAX_LENGTH];
    int got = recent_load(recent, RECENT_MAX);
    int count = 0;
    for (int i = 0; i < got && count < FILES_MAX_RESULTS; i++) {
        if (strlen(recent[i]) >= FILES_LOCATION_MAX || !file_browser_exists(recent[i])) {
            continue;
        }
        fill_entry_from_path(&into[count], recent[i]);
        memcpy(result_paths[count], recent[i], strlen(recent[i]) + 1);
        count++;
    }
    return count;
}

typedef struct {
    file_browser_entry_t *into;
    int count;
} search_collect_t;

static int collect_result(void *context, const char *path, const os_stat_t *status) {
    (void)status;
    search_collect_t *collect = (search_collect_t *)context;
    if (strlen(path) >= FILES_LOCATION_MAX) {
        return 0;
    }
    if (file_browser_is_inside(path, FILE_BROWSER_TRASH)) {
        return 0;
    }
    fill_entry_from_path(&collect->into[collect->count], path);
    memcpy(result_paths[collect->count], path, strlen(path) + 1);
    collect->count++;
    return collect->count >= FILES_MAX_RESULTS;
}

static int load_search(file_browser_entry_t *into) {
    search_collect_t collect = {into, 0};
    long visited = file_browser_search(search_scope, search_query, show_hidden, FILES_SEARCH_BUDGET,
                                       collect_result, &collect);
    search_truncated = visited < 0 || collect.count >= FILES_MAX_RESULTS;
    search_visited = visited < 0 ? -visited : visited;
    printf("[files] search \"%s\" in %s: %d results\n", search_query, search_scope, collect.count);
    return collect.count;
}

static void remember_selection(char names[][FILE_BROWSER_NAME_MAX], int *count, int capacity) {
    *count = 0;
    for (int i = 0; i < entry_count && *count < capacity; i++) {
        if (selected[i]) {
            memcpy(names[(*count)++], entries[i].name, FILE_BROWSER_NAME_MAX);
        }
    }
}

static void refresh_listing(int force) {
    static char kept[64][FILE_BROWSER_NAME_MAX];
    int kept_count = 0;
    char cursor_name[FILE_BROWSER_NAME_MAX] = "";
    if (!force) {
        remember_selection(kept, &kept_count, 64);
        if (cursor >= 0 && cursor < entry_count) {
            memcpy(cursor_name, entries[order[cursor]].name, FILE_BROWSER_NAME_MAX);
        }
    }

    file_browser_entry_t *fresh = entries == entry_buffers[0] ? entry_buffers[1] : entry_buffers[0];
    int count = 0;
    int missing = 0;
    if (here.mode == FILES_MODE_FOLDER) {
        count = load_folder(fresh, &missing);
    } else if (here.mode == FILES_MODE_RECENT) {
        count = load_recent(fresh);
    } else {
        count = load_search(fresh);
    }
    uint32_t signature = entry_signature(fresh, count);
    if (!force && signature == listing_signature && missing == listing_missing) {
        return;
    }
    entries = fresh;
    entry_count = count;
    listing_signature = signature;
    listing_missing = missing;
    memset(selected, 0, sizeof(selected));
    file_browser_sort(entries, order, entry_count, sort_key, sort_descending);
    cursor = -1;
    anchor = -1;
    for (int k = 0; k < kept_count; k++) {
        for (int i = 0; i < entry_count; i++) {
            if (strcmp(entries[i].name, kept[k]) == 0) {
                selected[i] = 1;
            }
        }
    }
    for (int position = 0; cursor_name[0] && position < entry_count; position++) {
        if (strcmp(entries[order[position]].name, cursor_name) == 0) {
            cursor = position;
            anchor = position;
        }
    }
    rebuild_items();
    if (here.mode != FILES_MODE_SEARCH) {
        printf("[files] showing %s %d items\n",
               here.mode == FILES_MODE_RECENT ? "recent" : here.path, entry_count);
    }
}

static void style_item(int position) {
    lv_obj_t *item = item_objects[position];
    if (!item) {
        return;
    }
    int index = order[position];
    int is_selected = selected[index] != 0;
    const desktop_palette_t *p = palette();
    uint32_t background = is_selected ? p->accent
                                      : (!view_icons && (position % 2) ? desktop_palette_mix(p->window, p->text, 4u)
                                                                       : p->window);
    lv_obj_set_style_bg_color(item, color(background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(item, (is_selected || (!view_icons && position % 2)) ? LV_OPA_COVER : LV_OPA_TRANSP,
                            LV_PART_MAIN);
    uint32_t count = lv_obj_get_child_count(item);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
        if (!lv_obj_check_type(child, &lv_label_class)) {
            continue;
        }
        int is_icon = (view_icons == 0 && i == 0);
        int is_name = i == 1;
        uint32_t ink = is_selected ? p->accent_text
                       : is_icon ? kind_color(entries[index].kind)
                       : is_name ? p->text
                                 : p->text_dim;
        lv_obj_set_style_text_color(child, color(ink), LV_PART_MAIN);
    }
}

static void style_all_items(void) {
    for (int position = 0; position < entry_count; position++) {
        style_item(position);
    }
}

static void select_only(int position) {
    memset(selected, 0, sizeof(selected));
    if (position >= 0 && position < entry_count) {
        selected[order[position]] = 1;
    }
    cursor = position;
    anchor = position;
    style_all_items();
    update_status();
}

static void select_range(int from, int to) {
    memset(selected, 0, sizeof(selected));
    int low = from < to ? from : to;
    int high = from < to ? to : from;
    for (int position = low; position <= high && position < entry_count; position++) {
        if (position >= 0) {
            selected[order[position]] = 1;
        }
    }
    cursor = to;
    style_all_items();
    update_status();
}

static void reveal(int position) {
    if (position >= 0 && position < entry_count && item_objects[position]) {
        lv_obj_scroll_to_view(item_objects[position], LV_ANIM_OFF);
    }
}

static void select_named_path(const char *path) {
    for (int position = 0; position < entry_count; position++) {
        char full[PATH_MAX_LENGTH];
        if (entry_path(order[position], full, sizeof(full)) == 0 && strcmp(full, path) == 0) {
            select_only(position);
            reveal(position);
            return;
        }
    }
}

static void on_item_pressed(lv_event_t *event);

static void add_list_columns(lv_obj_t *row, const file_browser_entry_t *entry, int index) {
    char cell[TIME_FORMAT_DATE_MAX];
    lv_obj_t *icon = text(row, kind_symbol(entry->kind), kind_color(entry->kind), &lv_font_montserrat_14);
    lv_obj_set_width(icon, FILES_ICON_COLUMN);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *name = text(row, entry->name, palette()->text, 0);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

    time_format_date(entry->mtime, now_unix(), utc_offset_minutes, clock_24_hour, cell);
    lv_obj_t *date = text(row, cell, palette()->text_dim, &lv_font_montserrat_12);
    lv_obj_set_width(date, FILES_DATE_COLUMN);
    lv_label_set_long_mode(date, LV_LABEL_LONG_DOT);

    if (entry->is_directory) {
        snprintf(cell, sizeof(cell), "--");
    } else {
        format_size(entry->size, cell, sizeof(cell));
    }
    lv_obj_t *size = text(row, cell, palette()->text_dim, &lv_font_montserrat_12);
    lv_obj_set_width(size, FILES_SIZE_COLUMN);
    lv_obj_set_style_text_align(size, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    const char *kind_text = entry->kind_name ? entry->kind_name : "Document";
    char location[FILES_LOCATION_MAX];
    if (virtual_view() && index < FILES_MAX_RESULTS &&
        file_browser_parent(result_paths[index], location, sizeof(location)) == FILE_BROWSER_OK) {
        kind_text = location;
    }
    lv_obj_t *kind = text(row, kind_text, palette()->text_dim, &lv_font_montserrat_12);
    lv_obj_set_width(kind, FILES_KIND_COLUMN);
    lv_label_set_long_mode(kind, LV_LABEL_LONG_DOT);
}

static lv_obj_t *make_row(int position) {
    int index = order[position];
    lv_obj_t *row = plain(content);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_PCT(100), FILES_ROW_HEIGHT);
    lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    add_list_columns(row, &entries[index], index);
    lv_obj_set_user_data(row, (void *)(intptr_t)position);
    lv_obj_add_event_cb(row, on_item_pressed, LV_EVENT_PRESSED, 0);
    return row;
}

static lv_obj_t *make_tile(int position) {
    int index = order[position];
    const file_browser_entry_t *entry = &entries[index];
    lv_obj_t *tile = plain(content);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(tile, FILES_TILE_WIDTH, FILES_TILE_HEIGHT);
    lv_obj_set_style_radius(tile, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(tile, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tile, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *badge = plain(tile);
    lv_obj_set_size(badge, 46, 46);
    lv_obj_set_style_radius(badge, 11, LV_PART_MAIN);
    uint32_t base = kind_color(entry->kind);
    lv_obj_set_style_bg_color(badge, color(desktop_palette_mix(base, 0x00FFFFFFu, 18u)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(badge, color(desktop_palette_mix(base, 0x00000000u, 22u)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(badge, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *symbol = text(badge, kind_symbol(entry->kind), 0x00FFFFFFu, &lv_font_montserrat_24);
    lv_obj_center(symbol);

    lv_obj_t *name = text(tile, entry->name, palette()->text, &lv_font_montserrat_12);
    lv_obj_set_size(name, FILES_TILE_WIDTH - 8, 30);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_set_user_data(tile, (void *)(intptr_t)position);
    lv_obj_add_event_cb(tile, on_item_pressed, LV_EVENT_PRESSED, 0);
    return tile;
}

static void report_items(void) {
    char name[24];
    for (int position = 0; position < 4 && position < entry_count; position++) {
        snprintf(name, sizeof(name), "item%d", position);
        lvgl_window_report_geometry(name, item_objects[position]);
    }
    lvgl_window_report_geometry("content", content);
}

static void rebuild_items(void) {
    lv_obj_clean(content);
    memset(item_objects, 0, sizeof(item_objects));
    if (view_icons) {
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(content, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_column(content, 6, LV_PART_MAIN);
    } else {
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(content, 0, LV_PART_MAIN);
    }
    for (int position = 0; position < entry_count; position++) {
        item_objects[position] = view_icons ? make_tile(position) : make_row(position);
    }
    empty_label = 0;
    if (entry_count == 0) {
        const char *message = in_trash_view() ? "The Trash is empty."
                              : listing_missing ? "This folder is not there."
                              : here.mode == FILES_MODE_SEARCH ? (search_query[0] ? "No results." : "Type to search.")
                              : here.mode == FILES_MODE_RECENT ? "Nothing opened recently."
                                                               : "This folder is empty.";
        empty_label = text(content, message, palette()->text_dim, &lv_font_montserrat_16);
        lv_obj_set_width(empty_label, LV_PCT(100));
        lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_pad_top(empty_label, 80, LV_PART_MAIN);
    }
    style_all_items();
    update_chrome();
    update_status();
    if (cursor >= 0) {
        reveal(cursor);
    }
    report_items();
}

static void style_place(int i, int active) {
    const desktop_palette_t *p = palette();
    lv_obj_t *item = sidebar_items[i];
    lv_obj_set_style_bg_color(item, color(active ? desktop_palette_mix(p->surface, p->accent, 35u) : p->surface),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(item, active ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
}

static void on_crumb_clicked(lv_event_t *event);

static void update_path_bar(void) {
    lv_obj_clean(path_bar);
    const desktop_palette_t *p = palette();
    if (here.mode != FILES_MODE_FOLDER) {
        char line[96];
        if (here.mode == FILES_MODE_RECENT) {
            snprintf(line, sizeof(line), "Recent documents");
        } else {
            snprintf(line, sizeof(line), "Searching %s", search_scope);
        }
        text(path_bar, line, p->text_dim, &lv_font_montserrat_12);
        return;
    }
    const char *starts[FILES_HISTORY_MAX];
    int lengths[FILES_HISTORY_MAX];
    int count = 0;
    starts[count] = here.path;
    lengths[count++] = 1;
    for (const char *s = here.path + 1; *s && count < FILES_HISTORY_MAX;) {
        const char *end = s;
        while (*end && *end != '/') {
            end++;
        }
        if (end > s) {
            starts[count] = here.path;
            lengths[count++] = (int)(end - here.path);
        }
        s = *end ? end + 1 : end;
    }
    int first = count > 6 ? count - 6 : 0;
    for (int i = first; i < count; i++) {
        char prefix[FILES_LOCATION_MAX];
        memcpy(prefix, starts[i], (size_t)lengths[i]);
        prefix[lengths[i]] = '\0';
        if (i > first) {
            text(path_bar, LV_SYMBOL_RIGHT, p->text_dim, &lv_font_montserrat_12);
        }
        char crumb[FILE_BROWSER_NAME_MAX];
        if (i == 0) {
            snprintf(crumb, sizeof(crumb), "%s Computer", LV_SYMBOL_DRIVE);
        } else {
            folder_title(prefix, crumb, sizeof(crumb));
        }
        lv_obj_t *label = text(path_bar, crumb, i == count - 1 ? p->text : p->text_dim, &lv_font_montserrat_12);
        lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_hor(label, 4, LV_PART_MAIN);
        lv_obj_set_style_radius(label, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(label, color(p->surface_raised), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_user_data(label, (void *)(intptr_t)lengths[i]);
        lv_obj_add_event_cb(label, on_crumb_clicked, LV_EVENT_CLICKED, 0);
    }
}

static void update_chrome(void) {
    char title[FILE_BROWSER_NAME_MAX + 16];
    if (here.mode == FILES_MODE_RECENT) {
        snprintf(title, sizeof(title), "Recent");
    } else if (here.mode == FILES_MODE_SEARCH) {
        snprintf(title, sizeof(title), "Searching \"%s\"", search_query);
    } else {
        folder_title(here.path, title, sizeof(title));
    }
    lv_label_set_text(title_label, title);
    for (int i = 0; i < PLACE_COUNT; i++) {
        int active = PLACES[i].mode == here.mode &&
                     (here.mode != FILES_MODE_FOLDER || strcmp(PLACES[i].path, here.path) == 0);
        style_place(i, active);
    }
    set_enabled(back_button, back_count > 0);
    set_enabled(forward_button, forward_count > 0);
    lvgl_theme_choice_select(list_button, !view_icons);
    lvgl_theme_choice_select(icons_button, view_icons);
    if (view_icons) {
        lv_obj_add_flag(header_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(header_row, LV_OBJ_FLAG_HIDDEN);
    }
    static const char *const HEADINGS[FILE_BROWSER_SORT_COUNT] = {"Name", "Date Modified", "Size", "Kind"};
    for (int k = 0; k < FILE_BROWSER_SORT_COUNT; k++) {
        char heading[40];
        const char *label = HEADINGS[k];
        if (k == FILE_BROWSER_SORT_KIND && virtual_view()) {
            label = "Where";
        }
        if (k == (int)sort_key) {
            snprintf(heading, sizeof(heading), "%s %s", label, sort_descending ? LV_SYMBOL_DOWN : LV_SYMBOL_UP);
        } else {
            snprintf(heading, sizeof(heading), "%s", label);
        }
        lv_label_set_text(header_labels[k], heading);
        lv_obj_set_style_text_color(header_labels[k],
                                    color(k == (int)sort_key ? palette()->text : palette()->text_dim), LV_PART_MAIN);
    }
    if (in_trash_view()) {
        lv_obj_remove_flag(trash_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(trash_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (here.mode == FILES_MODE_SEARCH) {
        lv_obj_remove_flag(search_bar, LV_OBJ_FLAG_HIDDEN);
        lvgl_theme_choice_select(scope_computer_button, strcmp(search_scope, "/") == 0);
        lvgl_theme_choice_select(scope_here_button, strcmp(search_scope, "/") != 0);
        char scope[FILE_BROWSER_NAME_MAX + 4];
        char named[FILE_BROWSER_NAME_MAX];
        folder_title(search_origin, named, sizeof(named));
        snprintf(scope, sizeof(scope), "\"%s\"", named);
        lv_label_set_text(lv_obj_get_child(scope_here_button, 0), scope);
    } else {
        lv_obj_add_flag(search_bar, LV_OBJ_FLAG_HIDDEN);
    }
    update_path_bar();
}

static void update_status(void) {
    if (!status_label) {
        return;
    }
    long now = sys_uptime_ms();
    if (status_expires_ms && now < status_expires_ms) {
        lv_label_set_text(status_label, status_message);
        lv_obj_set_style_text_color(status_label, color(status_is_error ? palette()->danger : palette()->text),
                                    LV_PART_MAIN);
        return;
    }
    status_expires_ms = 0;
    char line[200];
    int n = 0;
    int chosen = selection_count();
    if (chosen > 0) {
        n = snprintf(line, sizeof(line), "%d of %d selected", chosen, entry_count);
    } else if (here.mode == FILES_MODE_SEARCH) {
        n = snprintf(line, sizeof(line), "%d %s", entry_count, entry_count == 1 ? "result" : "results");
    } else {
        n = snprintf(line, sizeof(line), "%d %s", entry_count, entry_count == 1 ? "item" : "items");
    }
    if (here.mode == FILES_MODE_SEARCH && search_truncated) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " (stopped after %ld entries)", search_visited);
    }
    if (here.mode == FILES_MODE_FOLDER) {
        if (space_next_ms == 0 || now >= space_next_ms) {
            space_known = sys_statvfs(here.path, &space_cached) == 0;
            space_next_ms = now + FILES_SPACE_MS;
        }
        if (space_known) {
            char free_text[32];
            format_large_size((uint64_t)space_cached.free_blocks * space_cached.block_size, free_text,
                              sizeof(free_text));
            snprintf(line + n, sizeof(line) - (size_t)n, ", %s available", free_text);
        }
    }
    lv_label_set_text(status_label, line);
    lv_obj_set_style_text_color(status_label, color(palette()->text_dim), LV_PART_MAIN);
}

static void remember_location(files_location_t *stack, int *count, const files_location_t *location) {
    if (*count == FILES_HISTORY_MAX) {
        memmove(stack, stack + 1, sizeof(stack[0]) * (FILES_HISTORY_MAX - 1));
        (*count)--;
    }
    stack[(*count)++] = *location;
}

static void show_location(const files_location_t *location, int remember) {
    if (remember && (location->mode != here.mode || strcmp(location->path, here.path) != 0 ||
                     location->mode == FILES_MODE_SEARCH)) {
        if (here.mode != FILES_MODE_SEARCH) {
            remember_location(back_history, &back_count, &here);
        }
        forward_count = 0;
    }
    here = *location;
    entry_count = 0;
    cursor = -1;
    anchor = -1;
    space_next_ms = 0;
    status_expires_ms = 0;
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
    refresh_listing(1);
}

static void go_folder(const char *path) {
    files_location_t location;
    location.mode = FILES_MODE_FOLDER;
    if (strlen(path) >= sizeof(location.path)) {
        say_error(FILE_BROWSER_ERROR_TOO_LONG);
        return;
    }
    memcpy(location.path, path, strlen(path) + 1);
    if (here.mode == FILES_MODE_SEARCH) {
        lv_textarea_set_text(search_field, "");
        search_query[0] = '\0';
    }
    show_location(&location, 1);
}

static void go_recent(void) {
    files_location_t location;
    location.mode = FILES_MODE_RECENT;
    location.path[0] = '\0';
    show_location(&location, 1);
}

static void go_back(void) {
    if (back_count == 0) {
        return;
    }
    if (here.mode != FILES_MODE_SEARCH) {
        remember_location(forward_history, &forward_count, &here);
    }
    files_location_t target = back_history[--back_count];
    if (here.mode == FILES_MODE_SEARCH) {
        lv_textarea_set_text(search_field, "");
        search_query[0] = '\0';
    }
    show_location(&target, 0);
}

static void go_forward(void) {
    if (forward_count == 0) {
        return;
    }
    remember_location(back_history, &back_count, &here);
    files_location_t target = forward_history[--forward_count];
    show_location(&target, 0);
}

static void go_enclosing(void) {
    if (here.mode != FILES_MODE_FOLDER) {
        go_back();
        return;
    }
    if (strcmp(here.path, "/") == 0) {
        return;
    }
    char parent[FILES_LOCATION_MAX];
    char previous[FILES_LOCATION_MAX];
    memcpy(previous, here.path, sizeof(previous));
    file_browser_parent(here.path, parent, sizeof(parent));
    go_folder(parent);
    select_named_path(previous);
}

static void push_undo(const files_undo_t *record) {
    if (record->count == 0) {
        return;
    }
    if (undo_count == UNDO_DEPTH) {
        memmove(undo_stack, undo_stack + 1, sizeof(undo_stack[0]) * (UNDO_DEPTH - 1));
        undo_count--;
    }
    undo_stack[undo_count++] = *record;
}

static void undo_note(files_undo_t *record, const char *from, const char *to) {
    if (record->count < 0) {
        return;
    }
    if (record->count >= UNDO_ITEMS || strlen(from) >= UNDO_PATH || strlen(to) >= UNDO_PATH) {
        record->count = -1;
        return;
    }
    memcpy(record->from[record->count], from, strlen(from) + 1);
    memcpy(record->to[record->count], to, strlen(to) + 1);
    record->count++;
}

static void finish_undo(files_undo_t *record) {
    if (record->count > 0) {
        push_undo(record);
    }
}

static void undo_last(void) {
    if (undo_count == 0) {
        say("Nothing to undo.", 0);
        return;
    }
    files_undo_t *record = &undo_stack[--undo_count];
    int failed = 0;
    for (int i = record->count - 1; i >= 0; i--) {
        int rc = FILE_BROWSER_OK;
        if (record->kind == UNDO_MOVE) {
            rc = file_browser_exists(record->from[i]) ? FILE_BROWSER_ERROR_EXISTS
                 : sys_rename(record->to[i], record->from[i]) == 0 ? FILE_BROWSER_OK
                                                                   : FILE_BROWSER_ERROR_RENAME;
        } else if (record->kind == UNDO_TRASH) {
            rc = file_browser_put_back(record->to[i], 0, 0);
        } else {
            rc = file_browser_trash(record->to[i], 0, 0);
        }
        failed |= rc != FILE_BROWSER_OK;
    }
    say(failed ? "Could not undo all of that." : "Undone.", failed);
    refresh_listing(0);
}

static int collect_selected_paths(char paths[][FILES_LOCATION_MAX], int capacity) {
    int count = 0;
    for (int position = 0; position < entry_count && count < capacity; position++) {
        int index = order[position];
        if (selected[index] && entry_path(index, paths[count], FILES_LOCATION_MAX) == 0) {
            count++;
        }
    }
    return count;
}

static int program_fits(const char *path, char *missing, size_t capacity) {
    uint32_t wanted = caps_for_program(path);
    uint32_t lacking = wanted & ~own_capabilities;
    missing[0] = '\0';
    if (!lacking) {
        return 1;
    }
    size_t n = 0;
    for (int i = 0; i < CAP_NAME_COUNT; i++) {
        if (lacking & CAP_NAMES[i].bit) {
            int wrote = snprintf(missing + n, capacity - n, "%s%s", n ? ", " : "", CAP_NAMES[i].name);
            if (wrote < 0 || (size_t)wrote >= capacity - n) {
                break;
            }
            n += (size_t)wrote;
        }
    }
    return 0;
}

static void open_in_editor(const char *path) {
    long rc = sys_spawn(PATH_BIN_DIRECTORY "text_editor", path);
    if (rc < 0) {
        say(spawn_error_message(rc), 1);
        return;
    }
    recent_add(path);
}

static void open_index(int index, int *opened_folder) {
    char path[FILES_LOCATION_MAX];
    if (entry_path(index, path, sizeof(path)) != 0) {
        return;
    }
    const file_browser_entry_t *entry = &entries[index];
    if (entry->is_directory) {
        if (!*opened_folder) {
            *opened_folder = 1;
            go_folder(path);
        }
        return;
    }
    if (file_browser_is_inside(path, FILE_BROWSER_TRASH)) {
        say("Put it back from the Trash before opening it.", 1);
        return;
    }
    if (entry->kind == FILE_BROWSER_KIND_PROGRAM) {
        char missing[96];
        if (!program_fits(path, missing, sizeof(missing))) {
            char message[160];
            snprintf(message, sizeof(message), "%s needs %s - open it from the launcher.", entry->name, missing);
            say(message, 1);
            return;
        }
        long rc = sys_spawn(path, "");
        if (rc < 0) {
            say(spawn_error_message(rc), 1);
        } else {
            say("Opened.", 0);
        }
        return;
    }
    if (entry->kind == FILE_BROWSER_KIND_IMAGE || entry->kind == FILE_BROWSER_KIND_ARCHIVE ||
        entry->kind == FILE_BROWSER_KIND_FONT || entry->kind == FILE_BROWSER_KIND_DATA) {
        open_preview();
        return;
    }
    open_in_editor(path);
}

static void open_selection(void) {
    int opened_folder = 0;
    int opened = 0;
    for (int position = 0; position < entry_count && opened < FILES_OPEN_AT_ONCE; position++) {
        int index = order[position];
        if (selected[index]) {
            open_index(index, &opened_folder);
            opened++;
            if (opened_folder) {
                return;
            }
        }
    }
}

static void open_terminal_here(void) {
    char where[FILES_LOCATION_MAX];
    int index = first_selected();
    if (index >= 0 && entries[index].is_directory && entry_path(index, where, sizeof(where)) == 0) {
    } else if (here.mode == FILES_MODE_FOLDER) {
        memcpy(where, here.path, strlen(here.path) + 1);
    } else if (index >= 0 && entry_path(index, where, sizeof(where)) == 0) {
        char parent[FILES_LOCATION_MAX];
        file_browser_parent(where, parent, sizeof(parent));
        memcpy(where, parent, strlen(parent) + 1);
    } else {
        memcpy(where, PATH_HOME, sizeof(PATH_HOME));
    }
    char before[FILES_LOCATION_MAX];
    if (sys_getcwd(before, sizeof(before)) < 0) {
        memcpy(before, PATH_HOME, sizeof(PATH_HOME));
    }
    if (sys_chdir(where) != 0) {
        say("Could not go there.", 1);
        return;
    }
    long rc = sys_spawn(PATH_BIN_DIRECTORY "gui_terminal", "");
    sys_chdir(before);
    if (rc < 0) {
        say(spawn_error_message(rc), 1);
    }
}

static void new_window(void) {
    const char *where = here.mode == FILES_MODE_FOLDER ? here.path : PATH_HOME;
    long rc = sys_spawn(PATH_BIN_DIRECTORY "file_manager", where);
    if (rc < 0) {
        say(spawn_error_message(rc), 1);
    }
}

static int writable_here(void) {
    if (here.mode != FILES_MODE_FOLDER) {
        say("Choose a folder first - this is a list, not a folder.", 1);
        return 0;
    }
    if (in_trash_view()) {
        say("Nothing can be made inside the Trash.", 1);
        return 0;
    }
    return 1;
}

static void create_named(const char *wanted, int folder) {
    char name[FILE_BROWSER_NAME_MAX];
    const char *base = wanted[0] ? wanted : (folder ? "untitled folder" : "untitled.txt");
    if (!file_system_utilities_name_ok(base)) {
        say_error(FILE_BROWSER_ERROR_NAME);
        return;
    }
    if (wanted[0]) {
        snprintf(name, sizeof(name), "%s", base);
    } else if (file_browser_available_name(here.path, base, name, sizeof(name)) != FILE_BROWSER_OK) {
        say_error(FILE_BROWSER_ERROR_EXISTS);
        return;
    }
    char full[PATH_MAX_LENGTH];
    if (file_browser_join(here.path, name, full, sizeof(full)) != FILE_BROWSER_OK) {
        say_error(FILE_BROWSER_ERROR_TOO_LONG);
        return;
    }
    if (file_browser_exists(full)) {
        say_error(FILE_BROWSER_ERROR_EXISTS);
        return;
    }
    int ok;
    if (folder) {
        ok = sys_mkdir(full) == 0;
    } else {
        long fd = sys_open(full, OPEN_WRITE | OPEN_CREATE | OPEN_EXCL);
        ok = fd >= 0;
        if (ok) {
            sys_close((int)fd);
        }
    }
    if (!ok) {
        say_error(FILE_BROWSER_ERROR_WRITE);
        return;
    }
    files_undo_t record = {UNDO_CREATE, 0, {{0}}, {{0}}};
    undo_note(&record, full, full);
    finish_undo(&record);
    printf("[files] created %s\n", full);
    say(folder ? "Folder created." : "File created.", 0);
    space_next_ms = 0;
    refresh_listing(0);
    select_named_path(full);
}

static void rename_selected(const char *wanted) {
    int index = first_selected();
    char from[FILES_LOCATION_MAX];
    if (index < 0 || entry_path(index, from, sizeof(from)) != 0) {
        return;
    }
    if (strcmp(wanted, file_browser_basename(from)) == 0) {
        return;
    }
    if (!file_system_utilities_name_ok(wanted)) {
        say_error(FILE_BROWSER_ERROR_NAME);
        return;
    }
    char parent[FILES_LOCATION_MAX];
    char to[PATH_MAX_LENGTH];
    if (file_browser_parent(from, parent, sizeof(parent)) != FILE_BROWSER_OK ||
        file_browser_join(parent, wanted, to, sizeof(to)) != FILE_BROWSER_OK) {
        say_error(FILE_BROWSER_ERROR_TOO_LONG);
        return;
    }
    if (file_browser_exists(to)) {
        say_error(FILE_BROWSER_ERROR_EXISTS);
        return;
    }
    if (sys_rename(from, to) != 0) {
        say_error(FILE_BROWSER_ERROR_RENAME);
        return;
    }
    files_undo_t record = {UNDO_MOVE, 0, {{0}}, {{0}}};
    undo_note(&record, from, to);
    finish_undo(&record);
    say("Renamed.", 0);
    refresh_listing(0);
    select_named_path(to);
}

static void duplicate_selection(void) {
    static char paths[UNDO_ITEMS][FILES_LOCATION_MAX];
    int count = collect_selected_paths(paths, UNDO_ITEMS);
    if (count == 0) {
        say("Select something first.", 1);
        return;
    }
    files_undo_t record = {UNDO_CREATE, 0, {{0}}, {{0}}};
    int error = FILE_BROWSER_OK;
    char made[PATH_MAX_LENGTH];
    for (int i = 0; i < count; i++) {
        char parent[FILES_LOCATION_MAX];
        file_browser_parent(paths[i], parent, sizeof(parent));
        if (file_browser_is_inside(paths[i], FILE_BROWSER_TRASH)) {
            error = FILE_BROWSER_ERROR_IN_TRASH;
            continue;
        }
        int rc = file_browser_copy_into(paths[i], parent, made, sizeof(made));
        if (rc == FILE_BROWSER_OK) {
            undo_note(&record, made, made);
        } else {
            error = rc;
        }
    }
    finish_undo(&record);
    space_next_ms = 0;
    if (error != FILE_BROWSER_OK) {
        say_error(error);
    } else {
        say(count == 1 ? "Duplicated." : "Duplicated them.", 0);
    }
    refresh_listing(0);
    if (count == 1 && error == FILE_BROWSER_OK) {
        select_named_path(made);
    }
}

static void open_confirm(files_confirm_purpose_t purpose);

static void trash_selection(void) {
    static char paths[UNDO_ITEMS][FILES_LOCATION_MAX];
    int count = collect_selected_paths(paths, UNDO_ITEMS);
    if (count == 0) {
        say("Select something first.", 1);
        return;
    }
    if (in_trash_view()) {
        run_action(ACTION_DELETE);
        return;
    }
    files_undo_t record = {UNDO_TRASH, 0, {{0}}, {{0}}};
    int error = FILE_BROWSER_OK;
    int moved = 0;
    for (int i = 0; i < count; i++) {
        char trashed[PATH_MAX_LENGTH];
        int rc = file_browser_trash(paths[i], trashed, sizeof(trashed));
        if (rc == FILE_BROWSER_OK) {
            undo_note(&record, paths[i], trashed);
            moved++;
            printf("[files] trashed %s\n", paths[i]);
        } else {
            error = rc;
        }
    }
    finish_undo(&record);
    if (error == FILE_BROWSER_ERROR_RENAME && moved == 0) {
        open_confirm(CONFIRM_DELETE_INSTEAD);
        return;
    }
    if (error != FILE_BROWSER_OK) {
        say_error(error);
    } else {
        say(moved == 1 ? "Moved to the Trash. Ctrl+Z puts it back." : "Moved them to the Trash. Ctrl+Z puts them back.", 0);
    }
    refresh_listing(0);
}

static void delete_selection_now(void) {
    static char paths[FILES_MAX_RESULTS][FILES_LOCATION_MAX];
    int count = collect_selected_paths(paths, FILES_MAX_RESULTS);
    int error = FILE_BROWSER_OK;
    for (int i = 0; i < count; i++) {
        int rc = file_browser_delete(paths[i]);
        if (rc != FILE_BROWSER_OK) {
            error = rc;
        } else {
            printf("[files] deleted %s\n", paths[i]);
        }
    }
    space_next_ms = 0;
    if (error != FILE_BROWSER_OK) {
        say_error(error);
    } else {
        say("Deleted.", 0);
    }
    refresh_listing(0);
}

static void put_back_selection(void) {
    static char paths[FILES_MAX_RESULTS][FILES_LOCATION_MAX];
    int count = collect_selected_paths(paths, FILES_MAX_RESULTS);
    int error = FILE_BROWSER_OK;
    for (int i = 0; i < count; i++) {
        char restored[PATH_MAX_LENGTH];
        int rc = file_browser_put_back(paths[i], restored, sizeof(restored));
        if (rc == FILE_BROWSER_OK) {
            printf("[files] put back %s\n", restored);
        } else {
            error = rc;
        }
    }
    if (error != FILE_BROWSER_OK) {
        say_error(error);
    } else if (count > 0) {
        say("Put back.", 0);
    }
    refresh_listing(0);
}

static void copy_selection(int cut) {
    static char paths[FILES_MAX_RESULTS][FILES_LOCATION_MAX];
    int count = collect_selected_paths(paths, FILES_MAX_RESULTS);
    if (count == 0) {
        say("Select something first.", 1);
        return;
    }
    size_t n = 0;
    for (int i = 0; i < count; i++) {
        size_t length = strlen(paths[i]);
        if (n + length + 2 > sizeof(clipboard_mine)) {
            break;
        }
        memcpy(clipboard_mine + n, paths[i], length);
        n += length;
        clipboard_mine[n++] = '\n';
    }
    clipboard_mine[n] = '\0';
    clipboard_is_cut = cut;
    sys_clipboard_set(clipboard_mine, n);
    char message[64];
    snprintf(message, sizeof(message), "%s %d %s.", cut ? "Cut" : "Copied", count, count == 1 ? "item" : "items");
    say(message, 0);
}

static void paste_here(void) {
    if (!writable_here()) {
        return;
    }
    static char board[FILES_CLIPBOARD_MAX];
    long length = sys_clipboard_get(board, sizeof(board) - 1);
    if (length <= 0) {
        memcpy(board, clipboard_mine, strlen(clipboard_mine) + 1);
        length = (long)strlen(board);
    }
    if (length <= 0) {
        say("There is nothing to paste.", 1);
        return;
    }
    if (length > (long)sizeof(board) - 1) {
        length = (long)sizeof(board) - 1;
    }
    board[length] = '\0';
    int moving = clipboard_is_cut && strcmp(board, clipboard_mine) == 0;
    files_undo_t record = {moving ? UNDO_MOVE : UNDO_CREATE, 0, {{0}}, {{0}}};
    int done = 0;
    int error = FILE_BROWSER_OK;
    char *line = board;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') {
            end++;
        }
        int last = *end == '\0';
        *end = '\0';
        if (line[0] == '/' && file_browser_exists(line)) {
            char made[PATH_MAX_LENGTH];
            int rc = moving ? file_browser_move_into(line, here.path, made, sizeof(made))
                            : file_browser_copy_into(line, here.path, made, sizeof(made));
            if (rc == FILE_BROWSER_OK) {
                undo_note(&record, moving ? line : made, made);
                done++;
            } else {
                error = rc;
            }
        } else if (line[0]) {
            error = FILE_BROWSER_ERROR_NOT_FOUND;
        }
        if (last) {
            break;
        }
        line = end + 1;
    }
    finish_undo(&record);
    if (moving && done > 0) {
        clipboard_is_cut = 0;
        clipboard_mine[0] = '\0';
    }
    space_next_ms = 0;
    if (done == 0) {
        say(error == FILE_BROWSER_OK ? "The clipboard holds no files." : file_browser_error_message(error), 1);
    } else if (error != FILE_BROWSER_OK) {
        say_error(error);
    } else {
        say(moving ? "Moved." : "Pasted.", 0);
    }
    refresh_listing(0);
}

static void move_selection_into(const char *directory, int copy) {
    static char paths[UNDO_ITEMS][FILES_LOCATION_MAX];
    int count = collect_selected_paths(paths, UNDO_ITEMS);
    files_undo_t record = {copy ? UNDO_CREATE : UNDO_MOVE, 0, {{0}}, {{0}}};
    int error = FILE_BROWSER_OK;
    int done = 0;
    for (int i = 0; i < count; i++) {
        char made[PATH_MAX_LENGTH];
        int rc;
        if (!copy && strcmp(directory, FILE_BROWSER_TRASH) == 0) {
            rc = file_browser_trash(paths[i], made, sizeof(made));
            if (rc == FILE_BROWSER_OK) {
                record.kind = UNDO_TRASH;
            }
        } else {
            rc = copy ? file_browser_copy_into(paths[i], directory, made, sizeof(made))
                      : file_browser_move_into(paths[i], directory, made, sizeof(made));
        }
        if (rc == FILE_BROWSER_OK) {
            undo_note(&record, copy ? made : paths[i], made);
            done++;
        } else if (rc != FILE_BROWSER_ERROR_SAME) {
            error = rc;
        }
    }
    finish_undo(&record);
    if (error != FILE_BROWSER_OK) {
        say_error(error);
    } else if (done > 0) {
        say(copy ? "Copied." : "Moved.", 0);
    }
    refresh_listing(0);
}

static void show_enclosing(void) {
    int index = first_selected();
    char path[FILES_LOCATION_MAX];
    if (index < 0 || entry_path(index, path, sizeof(path)) != 0) {
        return;
    }
    char parent[FILES_LOCATION_MAX];
    file_browser_parent(path, parent, sizeof(parent));
    lv_textarea_set_text(search_field, "");
    search_query[0] = '\0';
    go_folder(parent);
    select_named_path(path);
}

static void on_backdrop_clicked(lv_event_t *event) {
    if (lv_event_get_target(event) == lv_event_get_current_target(event)) {
        close_overlay();
    }
}

static void stop_typing(void);

static void close_overlay(void) {
    if (overlay) {
        lv_obj_delete(overlay);
    }
    overlay = 0;
    overlay_field = 0;
    preview_body = 0;
    overlay_kind = OVERLAY_NONE;
    stop_typing();
}

static lv_obj_t *open_backdrop(files_overlay_t kind, int dim) {
    close_overlay();
    overlay_kind = kind;
    overlay = plain(lv_layer_top());
    lv_obj_set_size(overlay, FILES_WINDOW_WIDTH, FILES_WINDOW_HEIGHT);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, dim ? LV_OPA_40 : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(overlay, on_backdrop_clicked, LV_EVENT_CLICKED, 0);
    return overlay;
}

static lv_obj_t *panel(lv_obj_t *parent, int32_t width, int32_t height) {
    const desktop_palette_t *p = palette();
    lv_obj_t *box = plain(parent);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_bg_color(box, color(p->surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, color(p->outline), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(box, LVGL_THEME_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(box, 24, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(box, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, LVGL_THEME_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    return box;
}

static void on_menu_item(lv_event_t *event) {
    files_action_t action = (files_action_t)(intptr_t)lv_event_get_user_data(event);
    close_overlay();
    run_action(action);
}

typedef struct {
    files_action_t action;
    const char *label;
    const char *chord;
    int enabled;
} files_menu_entry_t;

static void open_menu(int32_t x, int32_t y, const files_menu_entry_t *items, int count) {
    const desktop_palette_t *p = palette();
    lv_obj_t *backdrop = open_backdrop(OVERLAY_MENU, 0);
    int32_t height = 2 * 6;
    for (int i = 0; i < count; i++) {
        height += items[i].action == ACTION_SEPARATOR ? FILES_MENU_SEPARATOR : FILES_MENU_ITEM_HEIGHT;
    }
    lv_obj_t *menu = plain(backdrop);
    lv_obj_add_flag(menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(menu, FILES_MENU_WIDTH, height);
    lv_obj_set_style_bg_color(menu, color(p->surface_raised), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(menu, color(p->outline), LV_PART_MAIN);
    lv_obj_set_style_border_width(menu, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(menu, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(menu, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(menu, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_pad_all(menu, 5, LV_PART_MAIN);
    lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_COLUMN);
    if (x + FILES_MENU_WIDTH > FILES_WINDOW_WIDTH - 4) {
        x = FILES_WINDOW_WIDTH - 4 - FILES_MENU_WIDTH;
    }
    if (y + height > FILES_WINDOW_HEIGHT - 4) {
        y = FILES_WINDOW_HEIGHT - 4 - height;
    }
    lv_obj_set_pos(menu, x < 0 ? 0 : x, y < 0 ? 0 : y);
    for (int i = 0; i < count; i++) {
        if (items[i].action == ACTION_SEPARATOR) {
            lv_obj_t *line = plain(menu);
            lv_obj_set_size(line, LV_PCT(100), FILES_MENU_SEPARATOR);
            lv_obj_t *rule = plain(line);
            lv_obj_set_size(rule, LV_PCT(100), 1);
            lv_obj_center(rule);
            lv_obj_set_style_bg_color(rule, color(p->outline), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);
            continue;
        }
        lv_obj_t *row = plain(menu);
        lv_obj_set_size(row, LV_PCT(100), FILES_MENU_ITEM_HEIGHT);
        lv_obj_set_style_radius(row, 5, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 10, LV_PART_MAIN);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *label = text(row, items[i].label, items[i].enabled ? p->text : p->text_dim, 0);
        (void)label;
        if (items[i].chord) {
            text(row, items[i].chord, p->text_dim, &lv_font_montserrat_12);
        }
        if (items[i].enabled) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(row, color(p->accent), LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_add_event_cb(row, on_menu_item, LV_EVENT_CLICKED, (void *)(intptr_t)items[i].action);
        }
        lvgl_window_report_geometry(ACTION_REPORT[items[i].action], row);
    }
    lvgl_window_report_geometry("menu", menu);
}

static void open_item_menu(int32_t x, int32_t y) {
    int chosen = selection_count();
    int index = first_selected();
    int in_trash = 0;
    char path[FILES_LOCATION_MAX];
    if (index >= 0 && entry_path(index, path, sizeof(path)) == 0) {
        in_trash = file_browser_is_inside(path, FILE_BROWSER_TRASH);
    }
    int folder = index >= 0 && entries[index].is_directory;
    files_menu_entry_t items[20];
    int n = 0;
    items[n++] = (files_menu_entry_t){ACTION_OPEN, "Open", "Enter", !in_trash};
    if (!folder) {
        items[n++] = (files_menu_entry_t){ACTION_OPEN_IN_EDITOR, "Open in Text Editor", 0, !in_trash && chosen == 1};
    } else {
        items[n++] = (files_menu_entry_t){ACTION_OPEN_TERMINAL, "Open in Terminal", 0, !in_trash};
    }
    items[n++] = (files_menu_entry_t){ACTION_QUICK_LOOK, "Quick Look", "Space", 1};
    items[n++] = (files_menu_entry_t){ACTION_INFO, "Get Info", "Ctrl+I", 1};
    if (!folder && !in_trash && index >= 0 && entries[index].kind == FILE_BROWSER_KIND_IMAGE) {
        items[n++] = (files_menu_entry_t){ACTION_SET_DESKTOP_PICTURE, "Set as Desktop Picture", 0, chosen == 1};
    }
    items[n++] = (files_menu_entry_t){ACTION_SEPARATOR, 0, 0, 0};
    if (in_trash) {
        items[n++] = (files_menu_entry_t){ACTION_PUT_BACK, "Put Back", 0, 1};
        items[n++] = (files_menu_entry_t){ACTION_DELETE, "Delete Immediately...", "Del", 1};
    } else {
        items[n++] = (files_menu_entry_t){ACTION_RENAME, "Rename...", "F2", chosen == 1};
        items[n++] = (files_menu_entry_t){ACTION_DUPLICATE, "Duplicate", "Ctrl+D", 1};
        items[n++] = (files_menu_entry_t){ACTION_COPY, "Copy", "Ctrl+C", 1};
        items[n++] = (files_menu_entry_t){ACTION_CUT, "Cut", "Ctrl+X", 1};
        items[n++] = (files_menu_entry_t){ACTION_SEPARATOR, 0, 0, 0};
        items[n++] = (files_menu_entry_t){ACTION_TRASH, "Move to Trash", "Del", 1};
    }
    if (virtual_view()) {
        items[n++] = (files_menu_entry_t){ACTION_SEPARATOR, 0, 0, 0};
        items[n++] = (files_menu_entry_t){ACTION_SHOW_ENCLOSING, "Show in Enclosing Folder", 0, chosen == 1};
    }
    open_menu(x, y, items, n);
}

static void open_background_menu(int32_t x, int32_t y) {
    int folder = here.mode == FILES_MODE_FOLDER && !in_trash_view();
    files_menu_entry_t items[20];
    int n = 0;
    if (in_trash_view()) {
        items[n++] = (files_menu_entry_t){ACTION_EMPTY_TRASH, "Empty Trash...", 0, entry_count > 0};
        items[n++] = (files_menu_entry_t){ACTION_SEPARATOR, 0, 0, 0};
    }
    items[n++] = (files_menu_entry_t){ACTION_NEW_FOLDER, "New Folder", "Ctrl+Shift+N", folder};
    items[n++] = (files_menu_entry_t){ACTION_NEW_FILE, "New File", "Ctrl+N", folder};
    items[n++] = (files_menu_entry_t){ACTION_PASTE, "Paste", "Ctrl+V", folder};
    items[n++] = (files_menu_entry_t){ACTION_UNDO, "Undo", "Ctrl+Z", undo_count > 0};
    items[n++] = (files_menu_entry_t){ACTION_SEPARATOR, 0, 0, 0};
    items[n++] = (files_menu_entry_t){view_icons ? ACTION_VIEW_LIST : ACTION_VIEW_ICONS,
                                      view_icons ? "View as List" : "View as Icons",
                                      view_icons ? "Ctrl+1" : "Ctrl+2", 1};
    items[n++] = (files_menu_entry_t){ACTION_TOGGLE_HIDDEN, show_hidden ? "Hide Hidden Files" : "Show Hidden Files",
                                      "Ctrl+H", 1};
    items[n++] = (files_menu_entry_t){ACTION_SELECT_ALL, "Select All", "Ctrl+A", entry_count > 0};
    items[n++] = (files_menu_entry_t){ACTION_SEPARATOR, 0, 0, 0};
    items[n++] = (files_menu_entry_t){ACTION_OPEN_TERMINAL, "Open in Terminal", 0, here.mode == FILES_MODE_FOLDER};
    items[n++] = (files_menu_entry_t){ACTION_GO_TO, "Go to Folder...", "Ctrl+L", 1};
    items[n++] = (files_menu_entry_t){ACTION_NEW_WINDOW, "New Window", 0, 1};
    open_menu(x, y, items, n);
}

static void dialog_buttons(lv_obj_t *box, const char *confirm, int destructive, lv_event_cb_t on_confirm);

static void on_dialog_cancel(lv_event_t *event) {
    (void)event;
    close_overlay();
}

static void confirm_name(void) {
    if (overlay_kind != OVERLAY_NAME || !overlay_field) {
        return;
    }
    char wanted[FILES_LOCATION_MAX];
    snprintf(wanted, sizeof(wanted), "%s", lv_textarea_get_text(overlay_field));
    files_name_purpose_t purpose = name_purpose;
    close_overlay();
    if (purpose == NAME_NEW_FOLDER || purpose == NAME_NEW_FILE) {
        create_named(wanted, purpose == NAME_NEW_FOLDER);
    } else if (purpose == NAME_RENAME) {
        if (wanted[0]) {
            rename_selected(wanted);
        }
    } else if (wanted[0]) {
        char target[FILES_LOCATION_MAX];
        if (wanted[0] == '/') {
            snprintf(target, sizeof(target), "%s", wanted);
        } else if (file_browser_join(here.mode == FILES_MODE_FOLDER ? here.path : PATH_HOME, wanted, target,
                                     sizeof(target)) != FILE_BROWSER_OK) {
            say_error(FILE_BROWSER_ERROR_TOO_LONG);
            return;
        }
        os_stat_t status;
        if (sys_stat(target, &status) != 0 || !status.is_directory) {
            say("There is no folder there.", 1);
            return;
        }
        go_folder(target);
    }
}

static void on_name_confirm(lv_event_t *event) {
    (void)event;
    confirm_name();
}

static void open_name_dialog(files_name_purpose_t purpose) {
    if ((purpose == NAME_NEW_FOLDER || purpose == NAME_NEW_FILE) && !writable_here()) {
        return;
    }
    int index = first_selected();
    if (purpose == NAME_RENAME) {
        char path[FILES_LOCATION_MAX];
        if (index < 0 || entry_path(index, path, sizeof(path)) != 0) {
            say("Select something to rename.", 1);
            return;
        }
        if (file_browser_is_inside(path, FILE_BROWSER_TRASH)) {
            say("Put it back before renaming it.", 1);
            return;
        }
    }
    static const char *const TITLES[] = {"New Folder", "New File", "Rename", "Go to Folder"};
    static const char *const HINTS[] = {
        "Name the folder, or press Enter for \"untitled folder\".",
        "Name the file, or press Enter for \"untitled.txt\".",
        "A new name for it.",
        "A path, like /home or /bin.",
    };
    lv_obj_t *backdrop = open_backdrop(OVERLAY_NAME, 1);
    name_purpose = purpose;
    lv_obj_t *box = panel(backdrop, 360, LV_SIZE_CONTENT);
    lv_obj_center(box);
    text(box, TITLES[purpose], palette()->text, &lv_font_montserrat_16);
    text(box, HINTS[purpose], palette()->text_dim, &lv_font_montserrat_12);
    overlay_field = lv_textarea_create(box);
    lv_textarea_set_one_line(overlay_field, true);
    lv_obj_set_width(overlay_field, LV_PCT(100));
    lv_textarea_set_max_length(overlay_field, purpose == NAME_GO_TO ? FILES_LOCATION_MAX - 1 : FILE_BROWSER_NAME_MAX - 1);
    if (purpose == NAME_NEW_FOLDER) {
        lv_textarea_set_placeholder_text(overlay_field, "untitled folder");
    } else if (purpose == NAME_NEW_FILE) {
        lv_textarea_set_placeholder_text(overlay_field, "untitled.txt");
    } else if (purpose == NAME_RENAME) {
        lv_textarea_set_text(overlay_field, entries[index].name);
    } else {
        lv_textarea_set_text(overlay_field, here.mode == FILES_MODE_FOLDER ? here.path : PATH_HOME);
    }
    lv_group_add_obj(files_window.group, overlay_field);
    lv_group_focus_obj(overlay_field);
    lv_obj_add_state(overlay_field, LV_STATE_FOCUSED);
    dialog_buttons(box, purpose == NAME_RENAME ? "Rename" : purpose == NAME_GO_TO ? "Go" : "Create", 0,
                   on_name_confirm);
    lvgl_window_report_geometry("dialog_field", overlay_field);
}

static void confirm_destructive(void) {
    files_confirm_purpose_t purpose = confirm_purpose;
    close_overlay();
    if (purpose == CONFIRM_EMPTY_TRASH) {
        int removed = file_browser_empty_trash();
        char message[64];
        snprintf(message, sizeof(message), "Emptied the Trash (%d %s).", removed, removed == 1 ? "item" : "items");
        printf("[files] emptied the trash of %d items\n", removed);
        space_next_ms = 0;
        say(message, 0);
        refresh_listing(0);
    } else {
        delete_selection_now();
    }
}

static void on_confirm(lv_event_t *event) {
    (void)event;
    confirm_destructive();
}

static void dialog_buttons(lv_obj_t *box, const char *confirm, int destructive, lv_event_cb_t on_confirm_cb) {
    lv_obj_t *row = plain(box);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_t *cancel = lvgl_theme_button(row, "Cancel", 0);
    lv_obj_add_event_cb(cancel, on_dialog_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_t *ok = lvgl_theme_button(row, confirm, 1);
    if (destructive) {
        lv_obj_set_style_bg_color(ok, color(palette()->danger), LV_PART_MAIN);
        lv_obj_set_style_text_color(ok, lv_color_white(), LV_PART_MAIN);
    }
    lv_obj_add_event_cb(ok, on_confirm_cb, LV_EVENT_CLICKED, 0);
    lvgl_window_report_geometry("dialog_ok", ok);
    lvgl_window_report_geometry("dialog_cancel", cancel);
}

static void open_confirm(files_confirm_purpose_t purpose) {
    int chosen = selection_count();
    if (purpose != CONFIRM_EMPTY_TRASH && chosen == 0) {
        say("Select something first.", 1);
        return;
    }
    char headline[160];
    const char *detail = "This cannot be undone.";
    if (purpose == CONFIRM_EMPTY_TRASH) {
        snprintf(headline, sizeof(headline), "Empty the Trash of %d %s?", entry_count,
                 entry_count == 1 ? "item" : "items");
    } else if (chosen == 1) {
        int index = first_selected();
        char path[FILES_LOCATION_MAX];
        entry_path(index, path, sizeof(path));
        file_system_utilities_tree_t tree;
        if (entries[index].is_directory && file_system_utilities_count_tree(path, &tree) == 0 && tree.entries > 0) {
            snprintf(headline, sizeof(headline), "Delete \"%s\" and the %u %s inside it?", entries[index].name,
                     (unsigned)tree.entries, tree.entries == 1 ? "item" : "items");
        } else {
            snprintf(headline, sizeof(headline), "Delete \"%s\" immediately?", entries[index].name);
        }
        if (purpose == CONFIRM_DELETE_INSTEAD) {
            detail = "It cannot go to the Trash from here, so it would be gone for good.";
        }
    } else {
        snprintf(headline, sizeof(headline), "Delete these %d items immediately?", chosen);
    }
    lv_obj_t *backdrop = open_backdrop(OVERLAY_CONFIRM, 1);
    confirm_purpose = purpose;
    lv_obj_t *box = panel(backdrop, 380, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_t *head = text(box, headline, palette()->text, &lv_font_montserrat_16);
    lv_obj_set_width(head, LV_PCT(100));
    lv_label_set_long_mode(head, LV_LABEL_LONG_WRAP);
    text(box, detail, palette()->text_dim, &lv_font_montserrat_12);
    dialog_buttons(box, purpose == CONFIRM_EMPTY_TRASH ? "Empty Trash" : "Delete", 1, on_confirm);
}

static void info_row(lv_obj_t *box, const char *label, const char *value) {
    lv_obj_t *row = plain(box);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
    lv_obj_t *left = text(row, label, palette()->text_dim, &lv_font_montserrat_12);
    lv_obj_set_width(left, 86);
    lv_obj_set_style_text_align(left, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_t *right = text(row, value, palette()->text, &lv_font_montserrat_12);
    lv_obj_set_flex_grow(right, 1);
    lv_label_set_long_mode(right, LV_LABEL_LONG_WRAP);
}

static void on_close_clicked(lv_event_t *event) {
    (void)event;
    close_overlay();
}

static void open_info(void) {
    int chosen = selection_count();
    char path[FILES_LOCATION_MAX];
    int index = first_selected();
    int whole_folder = chosen == 0 && here.mode == FILES_MODE_FOLDER;
    if (chosen == 0 && !whole_folder) {
        say("Select something first.", 1);
        return;
    }
    lv_obj_t *backdrop = open_backdrop(OVERLAY_INFO, 1);
    lv_obj_t *box = panel(backdrop, 380, LV_SIZE_CONTENT);
    lv_obj_center(box);
    char value[FILES_LOCATION_MAX + 64];
    char cell[64];
    if (chosen > 1) {
        uint64_t total = 0;
        int folders = 0;
        for (int i = 0; i < entry_count; i++) {
            if (selected[i]) {
                total += entries[i].size;
                folders += entries[i].is_directory;
            }
        }
        snprintf(value, sizeof(value), "%d items", chosen);
        text(box, value, palette()->text, &lv_font_montserrat_20);
        snprintf(value, sizeof(value), "%d folders, %d files", folders, chosen - folders);
        info_row(box, "Contains", value);
        format_large_size(total, cell, sizeof(cell));
        snprintf(value, sizeof(value), "%s in the files themselves", cell);
        info_row(box, "Size", value);
    } else {
        if (whole_folder) {
            memcpy(path, here.path, strlen(here.path) + 1);
        } else {
            entry_path(index, path, sizeof(path));
        }
        os_stat_t status;
        os_stat_t link_status;
        int have = sys_stat(path, &status) == 0;
        int have_link = sys_lstat(path, &link_status) == 0;
        file_browser_entry_t entry;
        fill_entry_from_path(&entry, path);
        lv_obj_t *head = plain(box);
        lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(head, 12, LV_PART_MAIN);
        text(head, kind_symbol(entry.kind), kind_color(entry.kind), &lv_font_montserrat_28);
        char title[FILE_BROWSER_NAME_MAX];
        folder_title(path, title, sizeof(title));
        lv_obj_t *name = text(head, whole_folder ? title : entry.name, palette()->text, &lv_font_montserrat_20);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        info_row(box, "Kind", entry.kind_name ? entry.kind_name : "Document");
        if (have && status.is_directory) {
            file_system_utilities_tree_t tree;
            if (file_system_utilities_count_tree(path, &tree) == 0) {
                char exact[FILE_SYSTEM_UTILITIES_EXACT_MAX];
                file_system_utilities_format_exact(tree.bytes, exact);
                format_large_size(tree.bytes, cell, sizeof(cell));
                snprintf(value, sizeof(value), "%u %s, %s (%s bytes)%s", (unsigned)tree.entries,
                         tree.entries == 1 ? "item" : "items", cell, exact, tree.deep ? ", deeper than counted" : "");
            } else {
                snprintf(value, sizeof(value), "could not be counted");
            }
            info_row(box, "Contains", value);
        } else if (have) {
            char exact[FILE_SYSTEM_UTILITIES_EXACT_MAX];
            file_system_utilities_format_exact(status.size, exact);
            format_size(status.size, cell, sizeof(cell));
            if (status.size < 1000) {
                snprintf(value, sizeof(value), "%s", cell);
            } else {
                snprintf(value, sizeof(value), "%s (%s bytes)", cell, exact);
            }
            info_row(box, "Size", value);
        }
        char parent[FILES_LOCATION_MAX];
        file_browser_parent(path, parent, sizeof(parent));
        info_row(box, "Where", whole_folder ? path : parent);
        if (have) {
            char when[TIME_FORMAT_DATE_MAX];
            char long_date[TIME_FORMAT_DATE_MAX];
            char clock[TIME_FORMAT_CLOCK_MAX];
            time_format_long_date(status.mtime, utc_offset_minutes, long_date);
            time_format_clock(status.mtime, utc_offset_minutes, clock_24_hour, clock);
            snprintf(when, sizeof(when), "%s", long_date);
            snprintf(value, sizeof(value), "%s at %s", when, clock);
            info_row(box, "Modified", value);
            snprintf(value, sizeof(value), "%u", (unsigned)status.inode);
            info_row(box, "Inode", value);
        }
        if (have_link && link_status.is_link) {
            char target[FILES_LOCATION_MAX];
            long n = sys_readlink(path, target, sizeof(target) - 1);
            if (n >= 0) {
                target[n] = '\0';
                info_row(box, "Points to", target);
            }
        }
        char origin[FILES_LOCATION_MAX];
        if (file_browser_trash_origin(path, origin, sizeof(origin)) == FILE_BROWSER_OK) {
            info_row(box, "Came from", origin);
        }
        if (entry.kind == FILE_BROWSER_KIND_PROGRAM) {
            uint32_t wanted = caps_for_program(path);
            int n = 0;
            for (int i = 0; i < CAP_NAME_COUNT && n < (int)sizeof(value) - 32; i++) {
                if (wanted & CAP_NAMES[i].bit) {
                    n += snprintf(value + n, sizeof(value) - (size_t)n, "%s%s", n ? ", " : "", CAP_NAMES[i].name);
                }
            }
            info_row(box, "May", n ? value : "nothing beyond running");
        }
    }
    lv_obj_t *row = plain(box);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *done = lvgl_theme_button(row, "Done", 1);
    lv_obj_add_event_cb(done, on_close_clicked, LV_EVENT_CLICKED, 0);
    lvgl_window_report_geometry("info", box);
}

static void fill_bytes_preview(lv_obj_t *body, const unsigned char *bytes, long count) {
    static char dump[FILES_PREVIEW_BYTE_COUNT / 16 * 80 + 64];
    size_t n = 0;
    for (long line = 0; line < count; line += 16) {
        n += (size_t)snprintf(dump + n, sizeof(dump) - n, "%06lx  ", line);
        for (long i = 0; i < 16; i++) {
            if (line + i < count) {
                n += (size_t)snprintf(dump + n, sizeof(dump) - n, "%02x ", bytes[line + i]);
            } else {
                n += (size_t)snprintf(dump + n, sizeof(dump) - n, "   ");
            }
        }
        dump[n++] = ' ';
        for (long i = 0; i < 16 && line + i < count; i++) {
            unsigned char c = bytes[line + i];
            dump[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        dump[n++] = '\n';
        dump[n] = '\0';
    }
    lv_obj_t *label = text(body, dump, palette()->text, &lv_font_unscii_16);
    lv_obj_set_style_text_font(label, &lv_font_unscii_16, LV_PART_MAIN);
}

static void fill_preview(void) {
    if (!preview_body) {
        return;
    }
    lv_obj_t *box = lv_obj_get_parent(preview_body);
    lv_obj_clean(preview_body);
    lv_obj_t *heading = lv_obj_get_child(box, 0);
    int index = first_selected();
    char path[FILES_LOCATION_MAX];
    if (index < 0 || entry_path(index, path, sizeof(path)) != 0) {
        close_overlay();
        return;
    }
    const file_browser_entry_t *entry = &entries[index];
    char subtitle[FILE_BROWSER_NAME_MAX + 64];
    char size_text[32];
    format_size(entry->size, size_text, sizeof(size_text));
    snprintf(subtitle, sizeof(subtitle), "%s  -  %s%s%s", entry->name, entry->kind_name ? entry->kind_name : "",
             entry->is_directory ? "" : ", ", entry->is_directory ? "" : size_text);
    lv_label_set_text(lv_obj_get_child(heading, 1), subtitle);
    lv_label_set_text(lv_obj_get_child(heading, 0), kind_symbol(entry->kind));
    lv_obj_set_style_text_color(lv_obj_get_child(heading, 0), color(kind_color(entry->kind)), LV_PART_MAIN);

    if (entry->is_directory) {
        file_system_utilities_tree_t tree;
        char line[96];
        if (file_system_utilities_count_tree(path, &tree) == 0) {
            char total[32];
            format_large_size(tree.bytes, total, sizeof(total));
            snprintf(line, sizeof(line), "A folder of %u %s, %s in all.", (unsigned)tree.entries,
                     tree.entries == 1 ? "item" : "items", total);
        } else {
            snprintf(line, sizeof(line), "A folder that could not be read.");
        }
        lv_obj_t *icon = text(preview_body, LV_SYMBOL_DIRECTORY, palette()->accent, &lv_font_montserrat_28);
        lv_obj_set_style_transform_scale(icon, 512, LV_PART_MAIN);
        text(preview_body, line, palette()->text, &lv_font_montserrat_16);
        lv_obj_set_flex_align(preview_body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        return;
    }
    lv_obj_set_flex_align(preview_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    static unsigned char bytes[FILES_PREVIEW_TEXT_BYTES + 1];
    long fd = sys_open(path, OPEN_READ);
    long count = fd >= 0 ? sys_read((int)fd, bytes, FILES_PREVIEW_TEXT_BYTES) : -1;
    if (fd >= 0) {
        sys_close((int)fd);
    }
    if (count < 0) {
        text(preview_body, "This cannot be read.", palette()->danger, 0);
        return;
    }
    if (count == 0) {
        text(preview_body, "This file is empty.", palette()->text_dim, &lv_font_montserrat_16);
        return;
    }
    file_browser_preview_t kind = file_browser_preview_kind(bytes, (size_t)count);
    if (kind == FILE_BROWSER_PREVIEW_IMAGE) {
        char source[FILES_LOCATION_MAX + 4];
        snprintf(source, sizeof(source), "A:%s", path);
        lv_image_header_t header;
        if (lv_image_decoder_get_info(source, &header) == LV_RESULT_OK && header.w > 0 && header.h > 0) {
            lv_obj_set_flex_align(preview_body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            int32_t room_w = FILES_PREVIEW_WIDTH - 2 * LVGL_THEME_PAD - 8;
            int32_t room_h = FILES_PREVIEW_HEIGHT - 120;
            uint32_t scale = 256;
            if ((int32_t)header.w > room_w || (int32_t)header.h > room_h) {
                uint32_t by_width = (uint32_t)room_w * 256u / header.w;
                uint32_t by_height = (uint32_t)room_h * 256u / header.h;
                scale = by_width < by_height ? by_width : by_height;
                if (scale < 8) {
                    scale = 8;
                }
            }
            lv_obj_t *holder = plain(preview_body);
            lv_obj_set_size(holder, (int32_t)(header.w * scale / 256u), (int32_t)(header.h * scale / 256u));
            lv_obj_t *image = lv_image_create(holder);
            lv_image_set_src(image, source);
            lv_image_set_pivot(image, 0, 0);
            lv_image_set_scale(image, scale);
            char dimensions[48];
            snprintf(dimensions, sizeof(dimensions), "%u x %u pixels", (unsigned)header.w, (unsigned)header.h);
            text(preview_body, dimensions, palette()->text_dim, &lv_font_montserrat_12);
            return;
        }
        kind = FILE_BROWSER_PREVIEW_BYTES;
    }
    if (kind == FILE_BROWSER_PREVIEW_TEXT) {
        bytes[count] = '\0';
        lv_obj_t *label = text(preview_body, (const char *)bytes, palette()->text, &lv_font_montserrat_14);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        if (count == FILES_PREVIEW_TEXT_BYTES) {
            text(preview_body, "... the rest is in the file.", palette()->text_dim, &lv_font_montserrat_12);
        }
        return;
    }
    fill_bytes_preview(preview_body, bytes, count < FILES_PREVIEW_BYTE_COUNT ? count : FILES_PREVIEW_BYTE_COUNT);
}

static void open_preview(void) {
    if (selection_count() == 0) {
        say("Select something to look at.", 1);
        return;
    }
    lv_obj_t *backdrop = open_backdrop(OVERLAY_PREVIEW, 1);
    lv_obj_t *box = panel(backdrop, FILES_PREVIEW_WIDTH, FILES_PREVIEW_HEIGHT);
    lv_obj_center(box);
    lv_obj_t *heading = plain(box);
    lv_obj_set_size(heading, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(heading, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(heading, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(heading, 10, LV_PART_MAIN);
    text(heading, LV_SYMBOL_FILE, palette()->text, &lv_font_montserrat_20);
    lv_obj_t *title = text(heading, "", palette()->text, &lv_font_montserrat_14);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t *close = tool_button(heading, LV_SYMBOL_CLOSE, 30);
    lv_obj_add_event_cb(close, on_close_clicked, LV_EVENT_CLICKED, 0);

    preview_body = plain(box);
    lv_obj_add_flag(preview_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(preview_body, LV_PCT(100));
    lv_obj_set_flex_grow(preview_body, 1);
    lv_obj_set_style_bg_color(preview_body, color(palette()->window), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview_body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(preview_body, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(preview_body, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(preview_body, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(preview_body, LV_FLEX_FLOW_COLUMN);
    fill_preview();
    lvgl_window_report_geometry("preview", box);
    printf("[files] quick look %s\n", entries[first_selected()].name);
}

static void set_view(int icons) {
    if (view_icons == icons) {
        return;
    }
    view_icons = icons;
    rebuild_items();
}

static void select_all(void) {
    for (int i = 0; i < entry_count; i++) {
        selected[i] = 1;
    }
    cursor = entry_count - 1;
    style_all_items();
    update_status();
}

static void set_desktop_picture(void) {
    char path[FILES_LOCATION_MAX];
    if (entry_path(first_selected(), path, sizeof(path)) != 0) {
        return;
    }
    say("Making the desktop picture...", 0);
    lv_refr_now(NULL);
    char message[160];
    if (desktop_application_make_desktop_picture(path, PATH_WALLPAPER_PICTURE, message, sizeof(message)) != 0) {
        say(message, 1);
        return;
    }
    if (desktop_application_use_wallpaper(WALLPAPER_PICTURE) != 0) {
        say("The picture is ready, but the desktop could not be told.", 1);
        return;
    }
    printf("[files] desktop picture %s\n", path);
    say(message, 0);
}

static void run_action(files_action_t action) {
    switch (action) {
    case ACTION_SET_DESKTOP_PICTURE:
        set_desktop_picture();
        break;
    case ACTION_OPEN:
        open_selection();
        break;
    case ACTION_OPEN_IN_EDITOR: {
        char path[FILES_LOCATION_MAX];
        if (entry_path(first_selected(), path, sizeof(path)) == 0) {
            open_in_editor(path);
        }
        break;
    }
    case ACTION_OPEN_TERMINAL:
        open_terminal_here();
        break;
    case ACTION_QUICK_LOOK:
        open_preview();
        break;
    case ACTION_INFO:
        open_info();
        break;
    case ACTION_RENAME:
        open_name_dialog(NAME_RENAME);
        break;
    case ACTION_DUPLICATE:
        duplicate_selection();
        break;
    case ACTION_COPY:
        copy_selection(0);
        break;
    case ACTION_CUT:
        copy_selection(1);
        break;
    case ACTION_PASTE:
        paste_here();
        break;
    case ACTION_TRASH:
        trash_selection();
        break;
    case ACTION_DELETE:
        open_confirm(CONFIRM_DELETE);
        break;
    case ACTION_PUT_BACK:
        put_back_selection();
        break;
    case ACTION_EMPTY_TRASH:
        open_confirm(CONFIRM_EMPTY_TRASH);
        break;
    case ACTION_NEW_FOLDER:
        open_name_dialog(NAME_NEW_FOLDER);
        break;
    case ACTION_NEW_FILE:
        open_name_dialog(NAME_NEW_FILE);
        break;
    case ACTION_NEW_WINDOW:
        new_window();
        break;
    case ACTION_SHOW_ENCLOSING:
        show_enclosing();
        break;
    case ACTION_VIEW_LIST:
        set_view(0);
        break;
    case ACTION_VIEW_ICONS:
        set_view(1);
        break;
    case ACTION_TOGGLE_HIDDEN:
        show_hidden = !show_hidden;
        say(show_hidden ? "Showing hidden files." : "Hiding hidden files.", 0);
        refresh_listing(1);
        break;
    case ACTION_SELECT_ALL:
        select_all();
        break;
    case ACTION_UNDO:
        undo_last();
        break;
    case ACTION_GO_TO:
        open_name_dialog(NAME_GO_TO);
        break;
    case ACTION_SEPARATOR:
        break;
    }
}

static int item_position_of(lv_obj_t *object) {
    for (; object; object = lv_obj_get_parent(object)) {
        if (lv_obj_get_parent(object) == content) {
            for (int position = 0; position < entry_count; position++) {
                if (item_objects[position] == object) {
                    return position;
                }
            }
            return -1;
        }
    }
    return -1;
}

static int is_inside(lv_obj_t *object, lv_obj_t *ancestor) {
    for (; object; object = lv_obj_get_parent(object)) {
        if (object == ancestor) {
            return 1;
        }
    }
    return 0;
}

static void stop_typing(void) {
    typing_in_search = 0;
    lv_obj_remove_state(search_field, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
}

static void on_item_pressed(lv_event_t *event) {
    lv_obj_t *target = (lv_obj_t *)lv_event_get_current_target(event);
    int position = (int)(intptr_t)lv_obj_get_user_data(target);
    if (position < 0 || position >= entry_count) {
        return;
    }
    stop_typing();
    long mods = sys_keyboard_modifiers();
    long now = sys_uptime_ms();
    int index = order[position];
    if (mods & KEYBOARD_MOD_SHIFT && anchor >= 0) {
        select_range(anchor, position);
    } else if (mods & KEYBOARD_MOD_CTRL) {
        selected[index] = !selected[index];
        cursor = position;
        anchor = position;
        style_item(position);
        update_status();
    } else if (!selected[index] || selection_count() == 1) {
        select_only(position);
    } else {
        cursor = position;
        anchor = position;
    }
    press_position = position;
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    press_x = point.x;
    press_y = point.y;
    dragging = 0;
    if (last_press_position == position && last_press_ms >= 0 && now - last_press_ms <= FILES_DOUBLE_CLICK_MS &&
        !(mods & (KEYBOARD_MOD_SHIFT | KEYBOARD_MOD_CTRL))) {
        last_press_position = -1;
        last_press_ms = -1;
        press_position = -1;
        lv_indev_wait_release(lv_indev_active());
        select_only(position);
        open_selection();
        return;
    }
    last_press_position = position;
    last_press_ms = now;
    lv_indev_wait_release(lv_indev_active());
}

static void on_content_pressed(lv_event_t *event) {
    if (lv_event_get_target(event) != content) {
        return;
    }
    stop_typing();
    if (!(sys_keyboard_modifiers() & (KEYBOARD_MOD_CTRL | KEYBOARD_MOD_SHIFT))) {
        select_only(-1);
    }
    press_position = -1;
}

static void on_place_clicked(lv_event_t *event) {
    int i = (int)(intptr_t)lv_event_get_user_data(event);
    stop_typing();
    if (PLACES[i].mode == FILES_MODE_RECENT) {
        go_recent();
    } else {
        go_folder(PLACES[i].path);
    }
}

static void on_back(lv_event_t *event) {
    (void)event;
    go_back();
}

static void on_forward(lv_event_t *event) {
    (void)event;
    go_forward();
}

static void on_view_list(lv_event_t *event) {
    (void)event;
    set_view(0);
}

static void on_view_icons(lv_event_t *event) {
    (void)event;
    set_view(1);
}

static void on_header_clicked(lv_event_t *event) {
    file_browser_sort_t key = (file_browser_sort_t)(intptr_t)lv_event_get_user_data(event);
    if (key == sort_key) {
        sort_descending = !sort_descending;
    } else {
        sort_key = key;
        sort_descending = key == FILE_BROWSER_SORT_DATE || key == FILE_BROWSER_SORT_SIZE;
    }
    file_browser_sort(entries, order, entry_count, sort_key, sort_descending);
    cursor = -1;
    anchor = -1;
    rebuild_items();
}

static void on_crumb_clicked(lv_event_t *event) {
    lv_obj_t *target = (lv_obj_t *)lv_event_get_current_target(event);
    if (here.mode != FILES_MODE_FOLDER) {
        return;
    }
    int length = (int)(intptr_t)lv_obj_get_user_data(target);
    if (length <= 0 || length >= (int)sizeof(here.path)) {
        return;
    }
    char prefix[FILES_LOCATION_MAX];
    memcpy(prefix, here.path, (size_t)length);
    prefix[length] = '\0';
    if (strcmp(prefix, here.path) != 0) {
        go_folder(prefix);
    }
}

static void begin_search(void) {
    const char *typed = lv_textarea_get_text(search_field);
    snprintf(search_query, sizeof(search_query), "%s", typed);
    if (!search_query[0]) {
        if (here.mode == FILES_MODE_SEARCH) {
            go_back();
        }
        return;
    }
    if (here.mode != FILES_MODE_SEARCH) {
        snprintf(search_origin, sizeof(search_origin), "%s", here.mode == FILES_MODE_FOLDER ? here.path : PATH_HOME);
        snprintf(search_scope, sizeof(search_scope), "%s", search_origin);
    }
    files_location_t location;
    location.mode = FILES_MODE_SEARCH;
    snprintf(location.path, sizeof(location.path), "%s", search_scope);
    show_location(&location, here.mode != FILES_MODE_SEARCH);
}

static void on_search_changed(lv_event_t *event) {
    (void)event;
    search_due_ms = sys_uptime_ms() + FILES_SEARCH_DELAY_MS;
}

static void on_search_focused(lv_event_t *event) {
    (void)event;
    typing_in_search = 1;
}

static void on_scope(lv_event_t *event) {
    int computer = (int)(intptr_t)lv_event_get_user_data(event);
    snprintf(search_scope, sizeof(search_scope), "%s", computer ? "/" : search_origin);
    search_due_ms = sys_uptime_ms();
}

static void on_empty_trash(lv_event_t *event) {
    (void)event;
    open_confirm(CONFIRM_EMPTY_TRASH);
}

static void build_sidebar(lv_obj_t *root) {
    const desktop_palette_t *p = palette();
    lv_obj_t *sidebar = plain(root);
    lv_obj_set_size(sidebar, FILES_SIDEBAR_WIDTH, LV_PCT(100));
    lv_obj_set_style_bg_color(sidebar, color(p->surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sidebar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(sidebar, LV_BORDER_SIDE_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_width(sidebar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(sidebar, color(p->outline), LV_PART_MAIN);
    lv_obj_set_style_pad_all(sidebar, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(sidebar, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < PLACE_COUNT; i++) {
        if (i == 0 || i == PLACE_FIRST_LOCATION) {
            lv_obj_t *heading = text(sidebar, i == 0 ? "FAVORITES" : "LOCATIONS", p->text_dim, &lv_font_montserrat_12);
            lv_obj_set_style_text_letter_space(heading, 1, LV_PART_MAIN);
            lv_obj_set_style_pad_top(heading, i == 0 ? 2 : 14, LV_PART_MAIN);
            lv_obj_set_style_pad_bottom(heading, 4, LV_PART_MAIN);
        }
        lv_obj_t *item = plain(sidebar);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(item, LV_PCT(100), 30);
        lv_obj_set_style_radius(item, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(item, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_column(item, 10, LV_PART_MAIN);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(item, color(p->surface_raised), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_t *icon = text(item, PLACES[i].symbol, p->accent, 0);
        lv_obj_set_width(icon, 18);
        text(item, PLACES[i].name, p->text, 0);
        lv_obj_add_event_cb(item, on_place_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        sidebar_items[i] = item;
    }
}

static void build_toolbar(lv_obj_t *main_column) {
    const desktop_palette_t *p = palette();
    lv_obj_t *toolbar = plain(main_column);
    lv_obj_set_size(toolbar, LV_PCT(100), FILES_TOOLBAR_HEIGHT);
    lv_obj_set_style_pad_hor(toolbar, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(toolbar, 6, LV_PART_MAIN);
    lv_obj_set_style_border_side(toolbar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(toolbar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(toolbar, color(p->outline), LV_PART_MAIN);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    back_button = tool_button(toolbar, LV_SYMBOL_LEFT, 32);
    lv_obj_add_event_cb(back_button, on_back, LV_EVENT_CLICKED, 0);
    forward_button = tool_button(toolbar, LV_SYMBOL_RIGHT, 32);
    lv_obj_add_event_cb(forward_button, on_forward, LV_EVENT_CLICKED, 0);

    title_label = text(toolbar, "", p->text, &lv_font_montserrat_16);
    lv_obj_set_flex_grow(title_label, 1);
    lv_obj_set_style_pad_left(title_label, 8, LV_PART_MAIN);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);

    list_button = lvgl_theme_choice(toolbar, LV_SYMBOL_LIST);
    lv_obj_remove_flag(list_button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(list_button, on_view_list, LV_EVENT_CLICKED, 0);
    icons_button = lvgl_theme_choice(toolbar, LV_SYMBOL_IMAGE);
    lv_obj_remove_flag(icons_button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(icons_button, on_view_icons, LV_EVENT_CLICKED, 0);

    search_field = lv_textarea_create(toolbar);
    lv_textarea_set_one_line(search_field, true);
    lv_textarea_set_placeholder_text(search_field, "Search");
    lv_textarea_set_max_length(search_field, sizeof(search_query) - 1);
    lv_obj_set_size(search_field, 180, 32);
    lv_obj_set_style_radius(search_field, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(search_field, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(search_field, color(p->window), LV_PART_MAIN);
    lv_obj_set_style_border_color(search_field, color(p->outline), LV_PART_MAIN);
    lv_obj_set_style_border_color(search_field, color(p->accent), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_event_cb(search_field, on_search_changed, LV_EVENT_VALUE_CHANGED, 0);
    lv_obj_add_event_cb(search_field, on_search_focused, LV_EVENT_CLICKED, 0);
    lv_group_add_obj(files_window.group, search_field);
}

static lv_obj_t *notice_bar(lv_obj_t *main_column, const char *message) {
    const desktop_palette_t *p = palette();
    lv_obj_t *bar = plain(main_column);
    lv_obj_set_size(bar, LV_PCT(100), 38);
    lv_obj_set_style_bg_color(bar, color(p->surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_column(bar, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *label = text(bar, message, p->text_dim, &lv_font_montserrat_12);
    lv_obj_set_flex_grow(label, 1);
    return bar;
}

static void build_main(lv_obj_t *root) {
    const desktop_palette_t *p = palette();
    lv_obj_t *main_column = plain(root);
    lv_obj_set_height(main_column, LV_PCT(100));
    lv_obj_set_flex_grow(main_column, 1);
    lv_obj_set_flex_flow(main_column, LV_FLEX_FLOW_COLUMN);

    build_toolbar(main_column);

    trash_bar = notice_bar(main_column, "Things here are kept until the Trash is emptied.");
    lv_obj_t *empty = lvgl_theme_button(trash_bar, "Empty", 0);
    lv_obj_set_height(empty, 26);
    lv_obj_add_event_cb(empty, on_empty_trash, LV_EVENT_CLICKED, 0);

    search_bar = notice_bar(main_column, "Search:");
    lv_obj_set_flex_grow(lv_obj_get_child(search_bar, 0), 0);
    scope_computer_button = lvgl_theme_choice(search_bar, "This Computer");
    lv_obj_set_height(scope_computer_button, 26);
    lv_obj_add_event_cb(scope_computer_button, on_scope, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    scope_here_button = lvgl_theme_choice(search_bar, "here");
    lv_obj_set_height(scope_here_button, 26);
    lv_obj_add_event_cb(scope_here_button, on_scope, LV_EVENT_CLICKED, (void *)(intptr_t)0);

    header_row = plain(main_column);
    lv_obj_set_size(header_row, LV_PCT(100), FILES_HEADER_HEIGHT);
    lv_obj_set_style_pad_left(header_row, 10 + 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(header_row, 10 + 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(header_row, 8, LV_PART_MAIN);
    lv_obj_set_style_border_side(header_row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(header_row, color(p->outline), LV_PART_MAIN);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *spacer = plain(header_row);
    lv_obj_set_size(spacer, FILES_ICON_COLUMN, 1);
    static const int32_t WIDTHS[FILE_BROWSER_SORT_COUNT] = {0, FILES_DATE_COLUMN, FILES_SIZE_COLUMN, FILES_KIND_COLUMN};
    for (int k = 0; k < FILE_BROWSER_SORT_COUNT; k++) {
        header_labels[k] = text(header_row, "", p->text_dim, &lv_font_montserrat_12);
        lv_obj_add_flag(header_labels[k], LV_OBJ_FLAG_CLICKABLE);
        if (WIDTHS[k]) {
            lv_obj_set_width(header_labels[k], WIDTHS[k]);
        } else {
            lv_obj_set_flex_grow(header_labels[k], 1);
        }
        if (k == FILE_BROWSER_SORT_SIZE) {
            lv_obj_set_style_text_align(header_labels[k], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        }
        lv_obj_add_event_cb(header_labels[k], on_header_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    }

    content = plain(main_column);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_pad_all(content, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_top(content, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(content, color(p->outline), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(content, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(content, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(content, 3, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(content, on_content_pressed, LV_EVENT_PRESSED, 0);

    path_bar = plain(main_column);
    lv_obj_set_size(path_bar, LV_PCT(100), FILES_PATH_BAR_HEIGHT);
    lv_obj_set_style_pad_hor(path_bar, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(path_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_border_side(path_bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(path_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(path_bar, color(p->outline), LV_PART_MAIN);
    lv_obj_set_flex_flow(path_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(path_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *status_bar = plain(main_column);
    lv_obj_set_size(status_bar, LV_PCT(100), FILES_STATUS_HEIGHT);
    lv_obj_set_style_pad_hor(status_bar, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(status_bar, color(p->surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    status_label = text(status_bar, "", p->text_dim, &lv_font_montserrat_12);
}

static void build_user_interface(void) {
    lvgl_theme_apply(&files_window);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_t *root = plain(screen);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, color(palette()->window), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    build_sidebar(root);
    build_main(root);
    for (int i = 0; i < PLACE_COUNT; i++) {
        lvgl_window_report_geometry(PLACES[i].report, sidebar_items[i]);
    }
    lvgl_window_report_geometry("search", search_field);
    lvgl_window_report_geometry("back", back_button);
    lvgl_window_report_geometry("forward", forward_button);
    lvgl_window_report_geometry("view_list", list_button);
    lvgl_window_report_geometry("view_icons", icons_button);
}

static void move_cursor(int to, int extend) {
    if (entry_count == 0) {
        return;
    }
    if (to < 0) {
        to = 0;
    }
    if (to >= entry_count) {
        to = entry_count - 1;
    }
    if (extend && anchor >= 0) {
        select_range(anchor, to);
    } else {
        select_only(to);
    }
    reveal(to);
    if (overlay_kind == OVERLAY_PREVIEW) {
        fill_preview();
    }
}

static int icon_columns(void) {
    int32_t width = lv_obj_get_content_width(content);
    int columns = (int)((width + 6) / (FILES_TILE_WIDTH + 6));
    return columns < 1 ? 1 : columns;
}

static void type_to_select(char ch) {
    long now = sys_uptime_ms();
    if (now - type_select_ms > FILES_TYPE_SELECT_MS) {
        type_select_length = 0;
    }
    type_select_ms = now;
    if (type_select_length < (int)sizeof(type_select) - 1) {
        type_select[type_select_length++] = ch;
        type_select[type_select_length] = '\0';
    }
    for (int position = 0; position < entry_count; position++) {
        const char *name = entries[order[position]].name;
        int matches = 1;
        for (int i = 0; i < type_select_length; i++) {
            char a = name[i];
            char b = type_select[i];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                matches = 0;
                break;
            }
        }
        if (matches) {
            move_cursor(position, 0);
            return;
        }
    }
}

static int handle_chord(char ch, uint32_t mods) {
    char key = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    int shift = (mods & KEYBOARD_MOD_SHIFT) != 0;
    switch (key) {
    case 'n':
        run_action(shift ? ACTION_NEW_FOLDER : ACTION_NEW_FILE);
        return 1;
    case 'o':
        run_action(ACTION_OPEN);
        return 1;
    case 'i':
        run_action(ACTION_INFO);
        return 1;
    case 'd':
        run_action(ACTION_DUPLICATE);
        return 1;
    case 'c':
        if (shift) {
            go_folder("/");
        } else {
            run_action(ACTION_COPY);
        }
        return 1;
    case 'x':
        run_action(ACTION_CUT);
        return 1;
    case 'v':
        run_action(ACTION_PASTE);
        return 1;
    case 'a':
        if (shift) {
            go_folder(PATH_BIN);
        } else {
            run_action(ACTION_SELECT_ALL);
        }
        return 1;
    case 'z':
        run_action(ACTION_UNDO);
        return 1;
    case 'f':
        typing_in_search = 1;
        lv_group_focus_obj(search_field);
        lv_obj_add_state(search_field, LV_STATE_FOCUSED);
        return 1;
    case 'h':
        if (shift) {
            go_folder(PATH_HOME);
        } else {
            run_action(ACTION_TOGGLE_HIDDEN);
        }
        return 1;
    case 'l':
    case 'g':
        run_action(ACTION_GO_TO);
        return 1;
    case 'y':
        run_action(ACTION_QUICK_LOOK);
        return 1;
    case 'w':
        lvgl_window_request_close(&files_window);
        return 1;
    case '1':
        set_view(0);
        return 1;
    case '2':
        set_view(1);
        return 1;
    case '[':
        go_back();
        return 1;
    case ']':
        go_forward();
        return 1;
    case '\b':
    case 0x7F:
        if (shift && in_trash_view()) {
            run_action(ACTION_EMPTY_TRASH);
        } else {
            run_action(ACTION_TRASH);
        }
        return 1;
    default:
        break;
    }
    if (ch == (char)KEYBOARD_KEY_UP) {
        go_enclosing();
        return 1;
    }
    if (ch == (char)KEYBOARD_KEY_DOWN) {
        run_action(ACTION_OPEN);
        return 1;
    }
    return 1;
}

static int files_key(lvgl_window_t *window, uint32_t ch, uint32_t mods) {
    (void)window;
    char key = (char)ch;
    if (overlay_kind == OVERLAY_NAME) {
        if (key == '\r' || key == '\n') {
            confirm_name();
            return 1;
        }
        if (key == 27) {
            close_overlay();
            return 1;
        }
        return (mods & KEYBOARD_MOD_CTRL) != 0;
    }
    if (overlay_kind == OVERLAY_CONFIRM) {
        if (key == '\r' || key == '\n') {
            confirm_destructive();
        } else if (key == 27) {
            close_overlay();
        }
        return 1;
    }
    if (overlay_kind == OVERLAY_PREVIEW) {
        if (key == ' ' || key == 27 || ((mods & KEYBOARD_MOD_CTRL) && (key == 'y' || key == 'Y'))) {
            close_overlay();
        } else if (key == (char)KEYBOARD_KEY_UP || key == (char)KEYBOARD_KEY_LEFT) {
            move_cursor(cursor - (view_icons && key == (char)KEYBOARD_KEY_UP ? icon_columns() : 1), 0);
        } else if (key == (char)KEYBOARD_KEY_DOWN || key == (char)KEYBOARD_KEY_RIGHT) {
            move_cursor(cursor + (view_icons && key == (char)KEYBOARD_KEY_DOWN ? icon_columns() : 1), 0);
        }
        return 1;
    }
    if (overlay_kind != OVERLAY_NONE) {
        if (key == 27 || key == '\r' || key == '\n' || ((mods & KEYBOARD_MOD_CTRL) && (key == 'i' || key == 'I'))) {
            close_overlay();
        }
        return 1;
    }
    if (typing_in_search) {
        if (key == 27) {
            lv_textarea_set_text(search_field, "");
            stop_typing();
            return 1;
        }
        if (key == '\r' || key == '\n' || key == (char)KEYBOARD_KEY_DOWN) {
            search_due_ms = sys_uptime_ms();
            stop_typing();
            if (entry_count > 0) {
                move_cursor(0, 0);
            }
            return 1;
        }
        if ((mods & KEYBOARD_MOD_CTRL) && (key == 'f' || key == 'F')) {
            return 1;
        }
        if (mods & KEYBOARD_MOD_CTRL) {
            return 1;
        }
        return 0;
    }
    if ((mods & KEYBOARD_MOD_CTRL) && !(mods & KEYBOARD_MOD_ALT)) {
        return handle_chord(key, mods);
    }
    if ((mods & KEYBOARD_MOD_ALT) && key == (char)KEYBOARD_KEY_LEFT) {
        go_back();
        return 1;
    }
    if ((mods & KEYBOARD_MOD_ALT) && key == (char)KEYBOARD_KEY_RIGHT) {
        go_forward();
        return 1;
    }
    if ((mods & KEYBOARD_MOD_ALT) && key == (char)KEYBOARD_KEY_UP) {
        go_enclosing();
        return 1;
    }
    int extend = (mods & KEYBOARD_MOD_SHIFT) != 0;
    int step = view_icons ? icon_columns() : 1;
    switch (key) {
    case (char)KEYBOARD_KEY_UP:
        move_cursor(cursor < 0 ? 0 : cursor - step, extend);
        return 1;
    case (char)KEYBOARD_KEY_DOWN:
        move_cursor(cursor < 0 ? 0 : cursor + step, extend);
        return 1;
    case (char)KEYBOARD_KEY_LEFT:
        if (view_icons) {
            move_cursor(cursor < 0 ? 0 : cursor - 1, extend);
        } else {
            go_enclosing();
        }
        return 1;
    case (char)KEYBOARD_KEY_RIGHT:
        if (view_icons) {
            move_cursor(cursor < 0 ? 0 : cursor + 1, extend);
        } else {
            run_action(ACTION_OPEN);
        }
        return 1;
    case '\r':
    case '\n':
        run_action(ACTION_OPEN);
        return 1;
    case ' ':
        run_action(ACTION_QUICK_LOOK);
        return 1;
    case '\b':
    case 0x7F:
    case (char)KEYBOARD_KEY_DELETE:
        run_action(ACTION_TRASH);
        return 1;
    case (char)KEYBOARD_KEY_HOME:
        move_cursor(0, extend);
        return 1;
    case (char)KEYBOARD_KEY_END:
        move_cursor(entry_count - 1, extend);
        return 1;
    case (char)KEYBOARD_KEY_PAGE_UP:
        move_cursor(cursor < 0 ? 0 : cursor - FILES_PAGE_ROWS * step, extend);
        return 1;
    case (char)KEYBOARD_KEY_PAGE_DOWN:
        move_cursor(cursor < 0 ? 0 : cursor + FILES_PAGE_ROWS * step, extend);
        return 1;
    case 27:
        if (here.mode == FILES_MODE_SEARCH) {
            lv_textarea_set_text(search_field, "");
            go_back();
        } else {
            select_only(-1);
        }
        return 1;
    default:
        break;
    }
    if (ch == (uint32_t)KEYBOARD_KEY_FUNCTION(2)) {
        run_action(ACTION_RENAME);
        return 1;
    }
    if (key > ' ' && key < 0x7F) {
        type_to_select(key);
    }
    return 1;
}

static void start_drag(void) {
    if (press_position < 0 || press_position >= entry_count) {
        return;
    }
    char path[FILES_LOCATION_MAX];
    if (entry_path(order[press_position], path, sizeof(path)) != 0) {
        return;
    }
    dragging = 1;
    lvgl_window_release_pointer(&files_window);
    window_manager_drag_begin(path);
    printf("[files] dragging %s\n", path);
}

static void drop_at(int32_t x, int32_t y) {
    char payload[WINDOW_MANAGER_DRAG_PAYLOAD_MAX];
    int have_payload = window_manager_drag_payload(payload, sizeof(payload)) == 0 && payload[0];
    int copy = (sys_keyboard_modifiers() & KEYBOARD_MOD_CTRL) != 0;
    char destination[FILES_LOCATION_MAX];
    destination[0] = '\0';
    lv_obj_t *target = lvgl_window_object_at(x, y);
    for (int i = 0; i < PLACE_COUNT; i++) {
        if (is_inside(target, sidebar_items[i]) && PLACES[i].mode == FILES_MODE_FOLDER) {
            snprintf(destination, sizeof(destination), "%s", PLACES[i].path);
        }
    }
    int position = item_position_of(target);
    if (!destination[0] && position >= 0 && entries[order[position]].is_directory) {
        entry_path(order[position], destination, sizeof(destination));
    }
    if (!destination[0] && here.mode == FILES_MODE_FOLDER && is_inside(target, content)) {
        snprintf(destination, sizeof(destination), "%s", here.path);
    }
    int was_dragging = dragging;
    dragging = 0;
    press_position = -1;
    if (!destination[0]) {
        return;
    }
    if (was_dragging) {
        if (position >= 0 && selected[order[position]]) {
            return;
        }
        move_selection_into(destination, copy);
        return;
    }
    if (have_payload && payload[0] == '/' && file_browser_exists(payload)) {
        char made[PATH_MAX_LENGTH];
        int rc = copy ? file_browser_copy_into(payload, destination, made, sizeof(made))
                      : file_browser_move_into(payload, destination, made, sizeof(made));
        if (rc == FILE_BROWSER_OK) {
            say(copy ? "Copied." : "Moved.", 0);
        } else if (rc != FILE_BROWSER_ERROR_SAME) {
            say_error(rc);
        }
        refresh_listing(0);
    }
}

static int files_event(lvgl_window_t *window, const window_manager_event_t *event) {
    (void)window;
    if (event->type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON) {
        int right_down = (event->buttons & 2) && !(previous_buttons & 2);
        previous_buttons = event->buttons;
        if (!(event->buttons & 1)) {
            press_position = -1;
            dragging = 0;
        }
        if (right_down && overlay_kind == OVERLAY_NONE) {
            lv_obj_t *target = lvgl_window_object_at(event->x, event->y);
            int position = item_position_of(target);
            if (position >= 0) {
                stop_typing();
                if (!selected[order[position]]) {
                    select_only(position);
                }
                open_item_menu(event->x, event->y);
            } else if (is_inside(target, content)) {
                stop_typing();
                select_only(-1);
                open_background_menu(event->x, event->y);
            }
            return 1;
        }
        if (right_down) {
            close_overlay();
            return 1;
        }
        return 0;
    }
    if (event->type == WINDOW_MANAGER_EVENT_MOUSE_MOVE) {
        if (!(event->buttons & 1)) {
            dragging = 0;
        }
        if ((event->buttons & 1) && press_position >= 0 && !dragging && overlay_kind == OVERLAY_NONE) {
            int32_t dx = event->x - press_x;
            int32_t dy = event->y - press_y;
            if (dx < 0) {
                dx = -dx;
            }
            if (dy < 0) {
                dy = -dy;
            }
            if (dx + dy >= FILES_DRAG_THRESHOLD) {
                start_drag();
                return 1;
            }
        }
        return 0;
    }
    if (event->type == WINDOW_MANAGER_EVENT_DROP) {
        drop_at(event->x, event->y);
        return 1;
    }
    return 0;
}

static void on_tick(lv_timer_t *timer) {
    (void)timer;
    long now = sys_uptime_ms();
    if (search_due_ms && now >= search_due_ms) {
        search_due_ms = 0;
        begin_search();
    }
    if (now >= refresh_next_ms) {
        refresh_next_ms = now + FILES_REFRESH_MS;
        load_clock_settings();
        if (here.mode != FILES_MODE_SEARCH && overlay_kind == OVERLAY_NONE && !dragging) {
            refresh_listing(0);
        }
    }
    if (status_expires_ms && now >= status_expires_ms) {
        update_status();
    }
}

static unsigned long count_drawn_pixels(void) {
    const uint32_t *pixels = files_window.window.graphics.pixels;
    unsigned long total = (unsigned long)files_window.window.width * files_window.window.height;
    unsigned long drawn = 0;
    for (unsigned long i = 0; i < total; i++) {
        if ((pixels[i] & 0x00FFFFFFu) != palette()->window) {
            drawn++;
        }
    }
    return drawn;
}

int desktop_application_files(int selftest) {
    if (lvgl_window_open(FILES_WINDOW_WIDTH, FILES_WINDOW_HEIGHT, "Files", &files_window) != 0) {
        printf("[m210] files: window open failed\n");
        return 1;
    }
    lv_group_set_default(0);
    own_capabilities = (uint32_t)sys_getcaps();
    lvgl_window_set_key_handler(&files_window, files_key);
    lvgl_window_set_event_handler(&files_window, files_event);
    load_clock_settings();

    const char *start = desktop_application_argument && desktop_application_argument[0] == '/'
                            ? desktop_application_argument
                            : PATH_HOME;
    here.mode = FILES_MODE_FOLDER;
    snprintf(here.path, sizeof(here.path), "%s", start);
    build_user_interface();
    show_location(&here, 0);
    stop_typing();
    refresh_next_ms = sys_uptime_ms() + FILES_REFRESH_MS;
    lv_timer_create(on_tick, 100, 0);

    for (int frame = 0; frame < 8; frame++) {
        lv_timer_handler();
        lvgl_window_pump(&files_window, 16);
    }
    unsigned long drawn = count_drawn_pixels();
    printf("[m210] files rendered %lu pixels with %d items in %s\n", drawn, entry_count, here.path);
    if (drawn < FILES_MINIMUM_DRAWN_PIXELS) {
        printf("[m210] files render FAILED\n");
        lvgl_window_close(&files_window);
        return 1;
    }
    if (selftest) {
        lvgl_window_close(&files_window);
        return 0;
    }
    lvgl_window_run(&files_window);
    lvgl_window_close(&files_window);
    return 0;
}
