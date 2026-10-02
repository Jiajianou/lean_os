#include <stdio.h>
#include <string.h>

#include "capabilities.h"
#include "desktop_applications.h"
#include "file_browser.h"
#include "file_system_utilities.h"
#include "lvgl_theme.h"
#include "os_file_system.h"
#include "os_network.h"
#include "paths.h"
#include "power_mode.h"
#include "settings_file.h"
#include "shortcuts.h"
#include "spawn_error.h"
#include "syscall_wrappers.h"
#include "time_format.h"
#include "wallpaper.h"
#include "wallpaper_picture.h"
#include "wireless.h"

#define SETTINGS_WINDOW_WIDTH  720
#define SETTINGS_WINDOW_HEIGHT 540
#define SETTINGS_SIDEBAR_WIDTH 196

#define SETTINGS_MINIMUM_DRAWN_PIXELS 20000
#define SETTINGS_SESSION_PATH PATH_ETC_DIRECTORY "session.conf"
#define SETTINGS_STORAGE_FOLDERS 12
#define SETTINGS_THUMBNAIL_WIDTH  96
#define SETTINGS_THUMBNAIL_HEIGHT 60
#define SETTINGS_SLIDER_LABEL_WIDTH 112
#define SETTINGS_THUMBNAILS (WALLPAPER_COUNT + 1)

static const uint32_t BACKGROUND_SWATCHES[] = {
    DESKTOP_PALETTE_DEFAULT_BACKGROUND,
    0x00203040u,
    0x00301A1Au,
    0x001A3020u,
    0x00302A1Au,
    0x00101018u,
};
#define BACKGROUND_SWATCH_COUNT ((int)(sizeof(BACKGROUND_SWATCHES) / sizeof(BACKGROUND_SWATCHES[0])))

static const uint32_t ACCENT_SWATCHES[] = {
    DESKTOP_PALETTE_DEFAULT_ACCENT,
    0x00E67E22u,
    0x0027AE60u,
    0x009B59B6u,
    0x00E74C3Cu,
    0x00F1C40Fu,
};
#define ACCENT_SWATCH_COUNT ((int)(sizeof(ACCENT_SWATCHES) / sizeof(ACCENT_SWATCHES[0])))

static const int16_t UTC_OFFSETS[] = {
    -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120, -60, 0,
    60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 390, 420, 480, 525, 540, 570, 600,
    630, 660, 720, 765, 780, 840,
};
#define UTC_OFFSET_COUNT ((int)(sizeof(UTC_OFFSETS) / sizeof(UTC_OFFSETS[0])))

typedef enum {
    PANE_GENERAL = 0,
    PANE_APPEARANCE,
    PANE_WALLPAPER,
    PANE_DISPLAY,
    PANE_SOUND,
    PANE_MOUSE,
    PANE_TRACKPAD,
    PANE_KEYBOARD,
    PANE_DATE_TIME,
    PANE_NETWORK,
    PANE_STORAGE,
    PANE_PRIVACY,
    PANE_STARTUP,
    PANE_COUNT,
} settings_pane_t;

typedef struct {
    const char *name;
    const char *symbol;
    uint32_t tint;
    const char *report;
} settings_pane_info_t;

static const settings_pane_info_t PANES[PANE_COUNT] = {
    {"General", LV_SYMBOL_SETTINGS, 0x007F8C8Du, "pane_general"},
    {"Appearance", LV_SYMBOL_TINT, 0x009B59B6u, "pane_appearance"},
    {"Wallpaper", LV_SYMBOL_IMAGE, 0x0027AE9Fu, "pane_wallpaper"},
    {"Display", LV_SYMBOL_EYE_OPEN, 0x003498DBu, "pane_display"},
    {"Sound", LV_SYMBOL_VOLUME_MAX, 0x00E74C3Cu, "pane_sound"},
    {"Mouse", LV_SYMBOL_GPS, 0x0016A085u, "pane_mouse"},
    {"Trackpad", LV_SYMBOL_SHUFFLE, 0x00D35400u, "pane_trackpad"},
    {"Keyboard", LV_SYMBOL_KEYBOARD, 0x00607D8Bu, "pane_keyboard"},
    {"Date & Time", LV_SYMBOL_BELL, 0x00E67E22u, "pane_date_time"},
    {"Network", LV_SYMBOL_WIFI, 0x002980B9u, "pane_network"},
    {"Storage", LV_SYMBOL_DRIVE, 0x0027AE60u, "pane_storage"},
    {"Privacy & Security", LV_SYMBOL_EYE_CLOSE, 0x00C0392Bu, "pane_privacy"},
    {"Startup", LV_SYMBOL_POWER, 0x008E44ADu, "pane_startup"},
};

static lvgl_window_t settings_window;

static window_manager_settings_request_t current;

static display_mode_t modes[DISPLAY_MAX_MODES];
static int mode_count;
static long confirm_until_ms;

/* M213: the screen as the kernel describes it, and what is waiting to be kept
   or put back. A scale and a live mode both go back by themselves after ten
   seconds; a mode for the next boot waits for a restart instead. */
typedef enum {
    PENDING_NONE = 0,
    PENDING_MODE,
    PENDING_SCALE,
} pending_display_t;

static display_status_t display_status;
static int display_status_known;
static pending_display_t pending_display;
static uint32_t pending_previous_width, pending_previous_height, pending_previous_scale;
static uint32_t pending_scale;
static display_mode_t startup_modes[DISPLAY_FIRMWARE_MODES_MAX];
static int startup_mode_count;

static int rebuild_pending;
static settings_pane_t active_pane = PANE_GENERAL;

static lv_obj_t *pane_items[PANE_COUNT];
static lv_obj_t *page;
static lv_obj_t *background_swatches[BACKGROUND_SWATCH_COUNT];
static lv_obj_t *accent_swatches[ACCENT_SWATCH_COUNT];
static lv_obj_t *wallpaper_choices[WALLPAPER_COUNT];
static lv_obj_t *picture_choice;
static lv_obj_t *choose_picture_button;
static lv_draw_buf_t *thumbnails[SETTINGS_THUMBNAILS];
static lv_obj_t *scroll_speed_slider;
static lv_obj_t *scroll_speed_label;
static lv_obj_t *trackpad_speed_slider;
static lv_obj_t *trackpad_speed_label;
static lv_obj_t *trackpad_acceleration_slider;
static lv_obj_t *trackpad_acceleration_label;
static lv_obj_t *trackpad_natural_switch;
static lv_obj_t *trackpad_scroll_slider;
static lv_obj_t *trackpad_scroll_label;
static lv_obj_t *tap_switch;
static lv_obj_t *mode_choices[DISPLAY_MAX_MODES];
static lv_obj_t *scale_choices[DISPLAY_SCALES_MAX];
static lv_obj_t *startup_choices[DISPLAY_FIRMWARE_MODES_MAX];
static lv_obj_t *restart_row;
static lv_obj_t *restart_label;
static lv_obj_t *restart_button;
static lv_obj_t *revert_button;
static lv_obj_t *confirm_row;
static lv_obj_t *keep_button;
static lv_obj_t *volume_slider;
static lv_obj_t *motion_switch;
static lv_obj_t *confirm_label;
static lv_obj_t *volume_value_label;
static lv_obj_t *clipboard_label;
static lv_obj_t *clock_label;
static lv_obj_t *date_label;
static lv_obj_t *uptime_label;
static lv_obj_t *memory_label;
static lv_obj_t *display_size_label;
static lv_obj_t *speed_slider;
static lv_obj_t *speed_value_label;
static lv_obj_t *natural_switch;
static lv_obj_t *swap_switch;
static lv_obj_t *clock_switch;
static lv_obj_t *offset_dropdown;
static lv_obj_t *restore_switch;
static lv_obj_t *wireless_label;
static lv_obj_t *storage_list;
static lv_obj_t *trash_label;

static const desktop_palette_t *palette(void) {
    return lvgl_theme_palette();
}

static lv_color_t color(uint32_t rgb) {
    return lvgl_theme_color(rgb);
}

static void apply_settings(void) {
    window_manager_set_settings(&current);
    settings_file_save(&current);
}

static void request_rebuild(void) {
    rebuild_pending = 1;
}

static lv_obj_t *plain(lv_obj_t *parent) {
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static lv_obj_t *right_value(lv_obj_t *row, const char *value) {
    lv_obj_t *label = lvgl_theme_value(row, value);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    return label;
}

static lv_obj_t *fact(lv_obj_t *card, const char *label, const char *value) {
    lv_obj_t *row = lvgl_theme_row(card, label);
    lv_obj_set_style_min_height(row, 26, LV_PART_MAIN);
    lv_obj_set_width(lv_obj_get_child(row, 0), 140);
    return right_value(row, value);
}

static lv_obj_t *spacer(lv_obj_t *row) {
    lv_obj_t *space = plain(row);
    lv_obj_set_size(space, 1, 1);
    lv_obj_set_flex_grow(space, 1);
    return space;
}

static lv_obj_t *toggle(lv_obj_t *card, const char *label, int on, lv_event_cb_t changed) {
    const desktop_palette_t *p = palette();
    lv_obj_t *row = lvgl_theme_row(card, label);
    lv_obj_set_width(lv_obj_get_child(row, 0), 1);
    lv_obj_set_flex_grow(lv_obj_get_child(row, 0), 1);
    lv_label_set_long_mode(lv_obj_get_child(row, 0), LV_LABEL_LONG_WRAP);
    lv_obj_t *control = lv_switch_create(row);
    lv_obj_set_size(control, 48, 26);
    lv_obj_set_style_bg_color(control, color(p->outline), LV_PART_MAIN);
    lv_obj_set_style_bg_color(control, color(p->accent), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(control, color(p->text), LV_PART_KNOB);
    if (on) {
        lv_obj_add_state(control, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(control, changed, LV_EVENT_VALUE_CHANGED, 0);
    return control;
}

static lv_obj_t *slider(lv_obj_t *row, int32_t minimum, int32_t maximum, int32_t value, lv_event_cb_t changed) {
    const desktop_palette_t *p = palette();
    lv_obj_t *control = lv_slider_create(row);
    lv_obj_set_flex_grow(control, 1);
    lv_obj_set_height(control, 10);
    lv_slider_set_range(control, minimum, maximum);
    lv_slider_set_value(control, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(control, color(p->outline), LV_PART_MAIN);
    lv_obj_set_style_bg_color(control, color(p->accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(control, color(p->text), LV_PART_KNOB);
    lv_obj_add_event_cb(control, changed, LV_EVENT_VALUE_CHANGED, 0);
    return control;
}

static void page_title(const char *title, const char *subtitle) {
    lv_obj_t *head = plain(page);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(head, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(head, 4, LV_PART_MAIN);
    lv_obj_t *label = lvgl_theme_value(head, title);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, LV_PART_MAIN);
    if (subtitle) {
        lv_obj_t *caption = lvgl_theme_caption(head, subtitle);
        lv_obj_set_width(caption, LV_PCT(100));
        lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
    }
}

static void wrap_caption(lv_obj_t *card, const char *message) {
    lv_obj_t *caption = lvgl_theme_caption(card, message);
    lv_obj_set_width(caption, LV_PCT(100));
    lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
}

static void format_bytes(uint64_t bytes, char *out, size_t capacity) {
    static const char *const UNITS[] = {"bytes", "KB", "MB", "GB", "TB"};
    int unit = 0;
    uint64_t whole = bytes;
    uint64_t tenths = 0;
    while (whole >= 1024 && unit < 4) {
        tenths = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        unit++;
    }
    if (unit == 0 || whole >= 10) {
        snprintf(out, capacity, "%u %s", (unsigned)whole, UNITS[unit]);
    } else {
        snprintf(out, capacity, "%u.%u %s", (unsigned)whole, (unsigned)tenths, UNITS[unit]);
    }
}

static void format_uptime(char *out, size_t capacity) {
    unsigned long seconds = (unsigned long)(sys_uptime_ms() / 1000);
    unsigned long days = seconds / 86400;
    unsigned long hours = (seconds % 86400) / 3600;
    unsigned long minutes = (seconds % 3600) / 60;
    if (days > 0) {
        snprintf(out, capacity, "%lu d %lu h %lu min", days, hours, minutes);
    } else if (hours > 0) {
        snprintf(out, capacity, "%lu h %lu min", hours, minutes);
    } else {
        snprintf(out, capacity, "%lu min %lu s", minutes, seconds % 60);
    }
}

static void format_memory(char *out, size_t capacity) {
    os_meminfo_t memory;
    if (sys_meminfo(&memory) != 0) {
        snprintf(out, capacity, "unknown");
        return;
    }
    char total[32];
    char free_text[32];
    format_bytes(memory.total_frames * memory.page_size, total, sizeof(total));
    format_bytes(memory.free_frames * memory.page_size, free_text, sizeof(free_text));
    snprintf(out, capacity, "%s, %s free", total, free_text);
}

static int read_text_file(const char *path, char *out, size_t capacity) {
    long fd = sys_open(path, OPEN_READ);
    if (fd < 0) {
        return -1;
    }
    long n = sys_read((int)fd, out, capacity - 1);
    sys_close((int)fd);
    if (n < 0) {
        return -1;
    }
    out[n] = '\0';
    return (int)n;
}

static void processor_description(char *out, size_t capacity) {
    static char cpuinfo[4096];
    int processors = 0;
    char model[96] = "";
    if (read_text_file(PATH_PROCESS_DIRECTORY "cpuinfo", cpuinfo, sizeof(cpuinfo)) > 0) {
        for (char *line = cpuinfo; *line;) {
            char *end = line;
            while (*end && *end != '\n') {
                end++;
            }
            char saved = *end;
            *end = '\0';
            if (strncmp(line, "processor", 9) == 0) {
                processors++;
            } else if (strncmp(line, "model name", 10) == 0 && !model[0]) {
                char *value = line;
                while (*value && *value != ':') {
                    value++;
                }
                if (*value == ':') {
                    value++;
                }
                while (*value == ' ' || *value == '\t') {
                    value++;
                }
                snprintf(model, sizeof(model), "%s", value);
            }
            *end = saved;
            line = *end ? end + 1 : end;
        }
    }
    if (processors == 0) {
        processors = 1;
    }
    if (model[0]) {
        snprintf(out, capacity, "%s, %d %s", model, processors, processors == 1 ? "core" : "cores");
    } else {
        snprintf(out, capacity, "x86-64, %d %s", processors, processors == 1 ? "core" : "cores");
    }
}

static void refresh_clock_labels(void) {
    char text[64];
    os_datetime_t now;
    if (sys_time(&now) > 0 && now.valid) {
        uint32_t unix_now = os_unix_time(&now);
        char clock[TIME_FORMAT_CLOCK_MAX];
        char offset[TIME_FORMAT_OFFSET_MAX];
        time_format_clock(unix_now, current.utc_offset_minutes, current.clock_24_hour != 0, clock);
        time_format_offset(current.utc_offset_minutes, offset);
        snprintf(text, sizeof(text), "%s  %s", clock, offset);
        if (clock_label) {
            lv_label_set_text(clock_label, text);
        }
        if (date_label) {
            char long_date[TIME_FORMAT_DATE_MAX];
            time_format_long_date(unix_now, current.utc_offset_minutes, long_date);
            lv_label_set_text(date_label, long_date);
        }
    } else {
        snprintf(text, sizeof(text), "the clock is not set");
        if (clock_label) {
            lv_label_set_text(clock_label, text);
        }
    }
}

static void refresh_clipboard_label(void) {
    if (!clipboard_label) {
        return;
    }
    char buffer[48];
    long length = sys_clipboard_get(buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        lv_label_set_text(clipboard_label, "(empty)");
        return;
    }
    if (length > (long)sizeof(buffer) - 1) {
        length = (long)sizeof(buffer) - 1;
    }
    buffer[length] = '\0';
    for (long i = 0; i < length; i++) {
        if (buffer[i] == '\n' || buffer[i] == '\t') {
            buffer[i] = ' ';
        }
    }
    lv_label_set_text(clipboard_label, buffer);
}

static void refresh_display_status(void) {
    display_status_known = sys_display_status(&display_status) == 0;
}

static void describe_display(char *out, size_t capacity) {
    if (!display_status_known) {
        window_manager_framebuffer_info_t framebuffer_info;
        if (sys_framebuffer_info(&framebuffer_info) == 0) {
            snprintf(out, capacity, "%u x %u", (unsigned)framebuffer_info.width, (unsigned)framebuffer_info.height);
        } else {
            snprintf(out, capacity, "unknown");
        }
        return;
    }
    if (display_status.scale_percent == 100) {
        snprintf(out, capacity, "%u x %u", (unsigned)display_status.physical_width,
                 (unsigned)display_status.physical_height);
    } else {
        snprintf(out, capacity, "%u x %u at %u%%, looks like %u x %u", (unsigned)display_status.physical_width,
                 (unsigned)display_status.physical_height, (unsigned)display_status.scale_percent,
                 (unsigned)display_status.desktop_width, (unsigned)display_status.desktop_height);
    }
}

static void refresh_display_size_label(void) {
    window_manager_framebuffer_info_t framebuffer_info;
    if (!display_size_label || sys_framebuffer_info(&framebuffer_info) != 0) {
        return;
    }
    if (display_status_known && framebuffer_info.width == display_status.desktop_width &&
        framebuffer_info.height == display_status.desktop_height) {
        return;
    }
    refresh_display_status();
    char text[80];
    describe_display(text, sizeof(text));
    lv_label_set_text(display_size_label, text);
}

static void wireless_description(char *out, size_t capacity) {
    os_wireless_status_t status;
    memset(&status, 0, sizeof(status));
    if (sys_wireless(WIRELESS_OPERATION_STATUS, &status, sizeof(status)) < 0 ||
        status.state == WIRELESS_STATE_ABSENT) {
        snprintf(out, capacity, "No wireless card in this machine");
        return;
    }
    switch (status.state) {
    case WIRELESS_STATE_CONNECTED: {
        char address[16];
        os_ip_to_string(status.ip, address);
        snprintf(out, capacity, "Connected to %s, %d dBm, %s", status.ssid, (int)status.signal_dbm, address);
        return;
    }
    case WIRELESS_STATE_RADIO_OFF:
        snprintf(out, capacity, "Radio off");
        return;
    case WIRELESS_STATE_SCANNING:
        snprintf(out, capacity, "Looking for networks");
        return;
    case WIRELESS_STATE_JOINING:
    case WIRELESS_STATE_SECURING:
    case WIRELESS_STATE_ADDRESSING:
        snprintf(out, capacity, "Joining %s", status.ssid);
        return;
    case WIRELESS_STATE_FAILED:
        snprintf(out, capacity, "The last join failed");
        return;
    default:
        snprintf(out, capacity, "Not connected");
        return;
    }
}

static settings_pane_t pane_named(const char *name);
static void build_page(void);

/* M215: another start of Settings found this one open and left the pane it
   was asked for here. */
static void take_pane_request(void) {
    char name[32];
    int length = read_text_file(DESKTOP_APPLICATION_SETTINGS_PANE_REQUEST, name, sizeof(name));
    if (length < 0) {
        return;
    }
    sys_unlink(DESKTOP_APPLICATION_SETTINGS_PANE_REQUEST);
    settings_pane_t pane = pane_named(name);
    if (pane != active_pane) {
        active_pane = pane;
        build_page();
    }
}

static void on_tick(lv_timer_t *timer) {
    (void)timer;
    take_pane_request();
    refresh_clipboard_label();
    refresh_clock_labels();
    refresh_display_size_label();
    if (uptime_label) {
        char text[48];
        format_uptime(text, sizeof(text));
        lv_label_set_text(uptime_label, text);
    }
    if (memory_label) {
        char text[64];
        format_memory(text, sizeof(text));
        lv_label_set_text(memory_label, text);
    }
    if (wireless_label) {
        char text[96];
        wireless_description(text, sizeof(text));
        lv_label_set_text(wireless_label, text);
    }
    if (confirm_until_ms != 0 && confirm_row) {
        long left_ms = confirm_until_ms - sys_uptime_ms();
        if (left_ms <= 0) {
            confirm_until_ms = 0;
            pending_display = PENDING_NONE;
            lv_obj_add_flag(confirm_row, LV_OBJ_FLAG_HIDDEN);
            request_rebuild();
        } else {
            char text[48];
            snprintf(text, sizeof(text), "Keep this %s? Going back in %ds",
                     pending_display == PENDING_SCALE ? "scale" : "size", (int)(left_ms / 1000) + 1);
            lv_label_set_text(confirm_label, text);
        }
    }
}

static void on_background_swatch(lv_event_t *event) {
    current.bg_color = BACKGROUND_SWATCHES[(int)(intptr_t)lv_event_get_user_data(event)];
    apply_settings();
    request_rebuild();
}

static void on_accent_swatch(lv_event_t *event) {
    current.accent_color = ACCENT_SWATCHES[(int)(intptr_t)lv_event_get_user_data(event)];
    apply_settings();
    request_rebuild();
}

static void select_tile(lv_obj_t *tile, int selected) {
    if (!tile) {
        return;
    }
    const desktop_palette_t *p = palette();
    lv_obj_set_style_border_color(tile, color(selected ? p->accent : p->outline), LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, selected ? 3 : 1, LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(tile, 1), color(selected ? p->accent : p->text_dim), LV_PART_MAIN);
}

static void select_wallpaper_tiles(void) {
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        select_tile(wallpaper_choices[i], (uint32_t)i == current.wallpaper);
    }
    select_tile(picture_choice, current.wallpaper == WALLPAPER_PICTURE);
}

static void on_wallpaper_choice(lv_event_t *event) {
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    current.wallpaper = (uint32_t)index;
    apply_settings();
    select_wallpaper_tiles();
    printf("[settings] wallpaper %s\n", wallpaper_name(index));
}

static void on_choose_picture(lv_event_t *event) {
    (void)event;
    os_stat_t status;
    if (sys_stat(PATH_PICTURES, &status) != 0) {
        sys_mkdir(PATH_PICTURES);
    }
    long rc = sys_spawn(PATH_BIN_DIRECTORY "file_manager", PATH_PICTURES);
    if (rc < 0) {
        window_manager_notify(WINDOW_MANAGER_NOTIFY_ERROR, "Files", spawn_error_message(rc));
    }
}

static void on_scroll_speed_changed(lv_event_t *event) {
    current.scroll_speed = (uint32_t)lv_slider_get_value((lv_obj_t *)lv_event_get_target(event));
    char text[16];
    snprintf(text, sizeof(text), "%u%%", (unsigned)current.scroll_speed);
    lv_label_set_text(scroll_speed_label, text);
    apply_settings();
}

static void on_trackpad_speed_changed(lv_event_t *event) {
    current.trackpad_speed = (uint32_t)lv_slider_get_value((lv_obj_t *)lv_event_get_target(event));
    char text[16];
    snprintf(text, sizeof(text), "%u%%", (unsigned)current.trackpad_speed);
    lv_label_set_text(trackpad_speed_label, text);
    apply_settings();
    printf("[settings] trackpad speed %u\n", (unsigned)current.trackpad_speed);
}

static void acceleration_text(uint32_t value, char *out, size_t capacity) {
    if (value == 0) {
        snprintf(out, capacity, "Off");
    } else {
        snprintf(out, capacity, "%u%%", (unsigned)value);
    }
}

static void on_trackpad_acceleration_changed(lv_event_t *event) {
    current.trackpad_acceleration = (uint32_t)lv_slider_get_value((lv_obj_t *)lv_event_get_target(event));
    char text[16];
    acceleration_text(current.trackpad_acceleration, text, sizeof(text));
    lv_label_set_text(trackpad_acceleration_label, text);
    apply_settings();
    printf("[settings] trackpad acceleration %u\n", (unsigned)current.trackpad_acceleration);
}

static void on_trackpad_natural_changed(lv_event_t *event) {
    current.trackpad_natural_scrolling =
        lv_obj_has_state((lv_obj_t *)lv_event_get_target(event), LV_STATE_CHECKED) ? 1u : 0u;
    apply_settings();
    printf("[settings] trackpad natural scrolling %u\n", (unsigned)current.trackpad_natural_scrolling);
}

static void on_trackpad_scroll_changed(lv_event_t *event) {
    current.trackpad_scroll_speed = (uint32_t)lv_slider_get_value((lv_obj_t *)lv_event_get_target(event));
    char text[16];
    snprintf(text, sizeof(text), "%u%%", (unsigned)current.trackpad_scroll_speed);
    lv_label_set_text(trackpad_scroll_label, text);
    apply_settings();
}

static void on_tap_changed(lv_event_t *event) {
    current.trackpad_tap_to_click = lv_obj_has_state((lv_obj_t *)lv_event_get_target(event), LV_STATE_CHECKED) ? 1u : 0u;
    apply_settings();
    printf("[settings] tap to click %u\n", (unsigned)current.trackpad_tap_to_click);
}

static void on_volume_changed(lv_event_t *event) {
    lv_obj_t *control = (lv_obj_t *)lv_event_get_target(event);
    current.volume = (uint32_t)lv_slider_get_value(control);
    char text[8];
    snprintf(text, sizeof(text), "%u%%", (unsigned)current.volume);
    lv_label_set_text(volume_value_label, text);
    apply_settings();
}

static void on_motion_changed(lv_event_t *event) {
    current.animations = lv_obj_has_state((lv_obj_t *)lv_event_get_target(event), LV_STATE_CHECKED) ? 1u : 0u;
    apply_settings();
}

static void on_speed_changed(lv_event_t *event) {
    lv_obj_t *control = (lv_obj_t *)lv_event_get_target(event);
    current.pointer_speed = (uint32_t)lv_slider_get_value(control);
    char text[16];
    snprintf(text, sizeof(text), "%u%%", (unsigned)current.pointer_speed);
    lv_label_set_text(speed_value_label, text);
    apply_settings();
}

static void on_natural_changed(lv_event_t *event) {
    current.natural_scrolling = lv_obj_has_state((lv_obj_t *)lv_event_get_target(event), LV_STATE_CHECKED) ? 1u : 0u;
    apply_settings();
}

static void on_swap_changed(lv_event_t *event) {
    current.swap_buttons = lv_obj_has_state((lv_obj_t *)lv_event_get_target(event), LV_STATE_CHECKED) ? 1u : 0u;
    apply_settings();
}

static void on_clock_changed(lv_event_t *event) {
    current.clock_24_hour = lv_obj_has_state((lv_obj_t *)lv_event_get_target(event), LV_STATE_CHECKED) ? 1u : 0u;
    apply_settings();
    refresh_clock_labels();
}

static void on_offset_changed(lv_event_t *event) {
    uint32_t index = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(event));
    if (index < (uint32_t)UTC_OFFSET_COUNT) {
        current.utc_offset_minutes = UTC_OFFSETS[index];
        apply_settings();
        refresh_clock_labels();
    }
}

static void on_restore_changed(lv_event_t *event) {
    current.restore_windows = lv_obj_has_state((lv_obj_t *)lv_event_get_target(event), LV_STATE_CHECKED) ? 1u : 0u;
    apply_settings();
}

static void on_clipboard_clear(lv_event_t *event) {
    (void)event;
    sys_clipboard_set("", 0);
    refresh_clipboard_label();
}

static void begin_confirmation(pending_display_t kind) {
    if (pending_display == PENDING_NONE) {
        pending_previous_width = display_status.physical_width;
        pending_previous_height = display_status.physical_height;
        pending_previous_scale = display_status.scale_requested;
    }
    pending_display = kind;
    confirm_until_ms = sys_uptime_ms() + WINDOW_MANAGER_MODE_REVERT_MS;
    if (confirm_row) {
        lv_obj_remove_flag(confirm_row, LV_OBJ_FLAG_HIDDEN);
        lvgl_window_report_geometry("keep", keep_button);
        lvgl_window_report_geometry("revert", revert_button);
    }
}

static void on_mode_choice(lv_event_t *event) {
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    begin_confirmation(PENDING_MODE);
    window_manager_set_display_mode(modes[index].width, modes[index].height);
    for (int i = 0; i < mode_count; i++) {
        lvgl_theme_choice_select(mode_choices[i], i == index);
    }
    printf("[settings] resolution %ux%u\n", (unsigned)modes[index].width, (unsigned)modes[index].height);
}

static void on_scale_choice(lv_event_t *event) {
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    uint32_t percent = display_status.scales[index];
    begin_confirmation(PENDING_SCALE);
    pending_scale = percent;
    window_manager_set_display_scale(percent);
    for (int i = 0; i < (int)display_status.scale_count; i++) {
        lvgl_theme_choice_select(scale_choices[i], i == index);
    }
    printf("[settings] scale %u\n", (unsigned)percent);
}

static void finish_confirmation(void) {
    confirm_until_ms = 0;
    pending_display = PENDING_NONE;
    if (confirm_row) {
        lv_obj_add_flag(confirm_row, LV_OBJ_FLAG_HIDDEN);
    }
    request_rebuild();
}

static void on_mode_keep(lv_event_t *event) {
    (void)event;
    window_manager_confirm_display_mode();
    if (pending_display == PENDING_SCALE) {
        settings_file_save_display_scale(pending_scale);
        printf("[settings] kept scale %u\n", (unsigned)pending_scale);
    } else {
        refresh_display_status();
        if (display_status_known) {
            settings_file_save_display(display_status.physical_width, display_status.physical_height);
        }
        printf("[settings] kept resolution\n");
    }
    finish_confirmation();
}

static void on_mode_revert(lv_event_t *event) {
    (void)event;
    if (pending_display == PENDING_SCALE) {
        window_manager_set_display_scale(pending_previous_scale);
    } else if (pending_display == PENDING_MODE) {
        window_manager_set_display_mode(pending_previous_width, pending_previous_height);
    }
    window_manager_confirm_display_mode();
    printf("[settings] put the display back\n");
    finish_confirmation();
}

static void startup_description(char *out, size_t capacity, int *differs);

static int startup_mode_is_chosen(int index) {
    const display_mode_t *mode = &startup_modes[index];
    switch (display_status.startup_selection) {
    case DISPLAY_STARTUP_LARGEST:
        return index == 0;
    case DISPLAY_STARTUP_EXACT:
        return mode->width == display_status.startup_width && mode->height == display_status.startup_height;
    case DISPLAY_STARTUP_BUILT_IN:
        return mode->width == 1024 && mode->height == 768;
    default:
        return 0;
    }
}

static void on_startup_choice(lv_event_t *event) {
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    uint32_t width = index == 0 ? 0 : startup_modes[index].width;
    uint32_t height = index == 0 ? 0 : startup_modes[index].height;
    if (sys_display_set_startup_mode(width, height) != 0) {
        window_manager_notify(WINDOW_MANAGER_NOTIFY_ERROR, "Display",
                              "The boot disk's lean_os.cfg could not be changed.");
        return;
    }
    refresh_display_status();
    for (int i = 0; i < startup_mode_count; i++) {
        lvgl_theme_choice_select(startup_choices[i], i == index);
    }
    char text[96];
    int differs = 0;
    startup_description(text, sizeof(text), &differs);
    lv_label_set_text(restart_label, text);
    if (differs) {
        lv_obj_remove_flag(restart_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(restart_button, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(restart_row, LV_OBJ_FLAG_HIDDEN);
    lvgl_window_report_geometry("restart", restart_button);
    printf("[settings] startup mode %ux%u\n", (unsigned)startup_modes[index].width,
           (unsigned)startup_modes[index].height);
}

static void on_restart(lv_event_t *event) {
    (void)event;
    window_manager_ask_power(POWER_REBOOT);
}

/* The modes the firmware offered, biggest first and each once - the first
   is what video=native picks, so it is the one called the panel's own. */
static void collect_startup_modes(void) {
    startup_mode_count = 0;
    for (uint32_t i = 0; i < display_status.firmware_mode_count; i++) {
        display_mode_t mode = display_status.firmware_modes[i];
        int seen = 0;
        for (int j = 0; j < startup_mode_count; j++) {
            seen |= startup_modes[j].width == mode.width && startup_modes[j].height == mode.height;
        }
        if (seen || mode.width == 0 || mode.height == 0) {
            continue;
        }
        int at = startup_mode_count++;
        while (at > 0 && (uint64_t)startup_modes[at - 1].width * startup_modes[at - 1].height <
                             (uint64_t)mode.width * mode.height) {
            startup_modes[at] = startup_modes[at - 1];
            at--;
        }
        startup_modes[at] = mode;
    }
}

static void on_open_wireless(lv_event_t *event) {
    (void)event;
    long rc = sys_spawn(PATH_BIN_DIRECTORY "wifi", "");
    if (rc < 0) {
        window_manager_notify(WINDOW_MANAGER_NOTIFY_ERROR, "Wi-Fi", spawn_error_message(rc));
    }
}

static void on_open_files(lv_event_t *event) {
    const char *where = (const char *)lv_event_get_user_data(event);
    long rc = sys_spawn(PATH_BIN_DIRECTORY "file_manager", where);
    if (rc < 0) {
        window_manager_notify(WINDOW_MANAGER_NOTIFY_ERROR, "Files", spawn_error_message(rc));
    }
}

static void build_storage_breakdown(void);

static void on_empty_trash(lv_event_t *event) {
    (void)event;
    int removed = file_browser_empty_trash();
    char message[48];
    snprintf(message, sizeof(message), "Removed %d %s.", removed, removed == 1 ? "item" : "items");
    if (trash_label) {
        lv_label_set_text(trash_label, message);
    }
    build_storage_breakdown();
}

static lv_obj_t *swatch_strip(lv_obj_t *card, const char *label) {
    lv_obj_t *row = lvgl_theme_row(card, label);
    lv_obj_t *strip = plain(row);
    lv_obj_set_height(strip, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(strip, 1);
    lv_obj_set_style_pad_column(strip, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(strip, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(strip, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return strip;
}

static void build_general(void) {
    page_title("General", "What this machine is and how it is doing.");

    lv_obj_t *hero = lvgl_theme_card(page, 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 16, LV_PART_MAIN);
    lv_obj_t *badge = plain(hero);
    lv_obj_set_size(badge, 64, 64);
    lv_obj_set_style_radius(badge, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(badge, color(desktop_palette_mix(palette()->accent, 0x00FFFFFFu, 20u)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(badge, color(desktop_palette_mix(palette()->accent, 0x00000000u, 30u)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(badge, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *mark = lv_label_create(badge);
    lv_label_set_text(mark, "lo");
    lv_obj_set_style_text_font(mark, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(mark, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(mark);
    lv_obj_t *words = plain(hero);
    lv_obj_set_size(words, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(words, 4, LV_PART_MAIN);
    lv_obj_t *name = lvgl_theme_value(words, "lean_os");
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, LV_PART_MAIN);
    lvgl_theme_caption(words, "Written from scratch: boot loader, kernel, drivers,");
    lvgl_theme_caption(words, "file system, window system and applications.");

    lv_obj_t *card = lvgl_theme_card(page, "THIS MACHINE");
    char text[128];
    processor_description(text, sizeof(text));
    fact(card, "Processor", text);
    format_memory(text, sizeof(text));
    memory_label = fact(card, "Memory", text);
    refresh_display_status();
    describe_display(text, sizeof(text));
    display_size_label = fact(card, "Display", text);
    os_statvfs_t disk;
    if (sys_statvfs("/", &disk) == 0) {
        char total[32];
        char free_text[32];
        format_bytes((uint64_t)disk.total_blocks * disk.block_size, total, sizeof(total));
        format_bytes((uint64_t)disk.free_blocks * disk.block_size, free_text, sizeof(free_text));
        snprintf(text, sizeof(text), "%s, %s available", total, free_text);
    } else {
        snprintf(text, sizeof(text), "unknown");
    }
    fact(card, "Startup disk", text);
    format_uptime(text, sizeof(text));
    uptime_label = fact(card, "Up for", text);

    card = lvgl_theme_card(page, "NOW");
    clock_label = fact(card, "Time", "");
    date_label = fact(card, "Date", "");
    lv_obj_t *row = lvgl_theme_row(card, "Clipboard");
    clipboard_label = lvgl_theme_value(row, "(empty)");
    lv_obj_set_flex_grow(clipboard_label, 1);
    lv_label_set_long_mode(clipboard_label, LV_LABEL_LONG_DOT);
    lv_obj_t *clear = lvgl_theme_button(row, "Clear", 0);
    lv_obj_add_event_cb(clear, on_clipboard_clear, LV_EVENT_CLICKED, 0);
    refresh_clipboard_label();
    refresh_clock_labels();
}

/* A wallpaper is chosen by what it looks like, so each choice is a picture of
   itself - drawn by the same code that draws the desktop, at thumbnail
   size. */
static lv_obj_t *wallpaper_tile(lv_obj_t *parent, int wallpaper, int slot) {
    const desktop_palette_t *p = palette();
    lv_obj_t *tile = plain(parent);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(tile, SETTINGS_THUMBNAIL_WIDTH + 8, SETTINGS_THUMBNAIL_HEIGHT + 30);
    lv_obj_set_style_radius(tile, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(tile, color(p->outline), LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tile, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_draw_buf_t *buffer = lv_draw_buf_create(SETTINGS_THUMBNAIL_WIDTH, SETTINGS_THUMBNAIL_HEIGHT,
                                               LV_COLOR_FORMAT_XRGB8888, SETTINGS_THUMBNAIL_WIDTH * 4);
    lv_obj_t *canvas = lv_canvas_create(tile);
    if (buffer) {
        thumbnails[slot] = buffer;
        graphics_context_t context = {(uint32_t *)buffer->data, SETTINGS_THUMBNAIL_WIDTH, SETTINGS_THUMBNAIL_HEIGHT};
        wallpaper_picture_t picture;
        if (wallpaper == WALLPAPER_PICTURE && wallpaper_picture_load(PATH_WALLPAPER_PICTURE, &picture) == 0) {
            wallpaper_picture_cover(&picture, &context, 0, 0, SETTINGS_THUMBNAIL_WIDTH, SETTINGS_THUMBNAIL_HEIGHT);
            wallpaper_picture_free(&picture);
        } else {
            wallpaper_fill(&context, 0, 0, SETTINGS_THUMBNAIL_WIDTH, SETTINGS_THUMBNAIL_HEIGHT, wallpaper,
                           current.bg_color);
        }
        for (uint32_t i = 0; i < SETTINGS_THUMBNAIL_WIDTH * SETTINGS_THUMBNAIL_HEIGHT; i++) {
            context.pixels[i] |= 0xFF000000u;
        }
        lv_canvas_set_draw_buf(canvas, buffer);
    }
    lv_obj_set_style_radius(canvas, 6, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(canvas, true, LV_PART_MAIN);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text(label, wallpaper_name(wallpaper));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_add_event_cb(tile, on_wallpaper_choice, LV_EVENT_CLICKED, (void *)(intptr_t)wallpaper);
    return tile;
}

static void build_appearance(void) {
    page_title("Appearance", "The colours every window derives its palette from.");
    lv_obj_t *card = lvgl_theme_card(page, "COLOURS");
    lv_obj_t *strip = swatch_strip(card, "Desktop");
    for (int i = 0; i < BACKGROUND_SWATCH_COUNT; i++) {
        background_swatches[i] = lvgl_theme_swatch(strip, BACKGROUND_SWATCHES[i]);
        lvgl_theme_swatch_select(background_swatches[i], BACKGROUND_SWATCHES[i] == current.bg_color);
        lv_obj_add_event_cb(background_swatches[i], on_background_swatch, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    strip = swatch_strip(card, "Accent");
    for (int i = 0; i < ACCENT_SWATCH_COUNT; i++) {
        accent_swatches[i] = lvgl_theme_swatch(strip, ACCENT_SWATCHES[i]);
        lvgl_theme_swatch_select(accent_swatches[i], ACCENT_SWATCHES[i] == current.accent_color);
        lv_obj_add_event_cb(accent_swatches[i], on_accent_swatch, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    wrap_caption(card, "A pair that would be hard to read is refused by the compositor and the defaults come back. "
                       "The desktop colour also tints the Flat, Gradient, Deep and Grid wallpapers.");

    card = lvgl_theme_card(page, "MOTION");
    motion_switch = toggle(card, "Animate windows and menus", current.animations != 0, on_motion_changed);
    wrap_caption(card, "Off makes windows appear, minimise and snap at once.");
}

static void build_wallpaper(void) {
    page_title("Wallpaper", "What the desktop shows behind everything.");
    lv_obj_t *card = lvgl_theme_card(page, "PICTURE");
    lv_obj_t *row = lvgl_theme_row(card, 0);
    char name[WALLPAPER_PICTURE_NAME_MAX];
    char text[WALLPAPER_PICTURE_NAME_MAX + 32];
    os_stat_t status;
    int have_picture = sys_stat(PATH_WALLPAPER_PICTURE, &status) == 0 &&
                       wallpaper_picture_read_name(PATH_WALLPAPER_PICTURE, name, sizeof(name)) == 0;
    if (have_picture) {
        snprintf(text, sizeof(text), "%s", name);
    } else {
        snprintf(text, sizeof(text), "No picture chosen yet");
    }
    lv_obj_t *picture_label = lvgl_theme_value(row, text);
    lv_obj_set_flex_grow(picture_label, 1);
    lv_label_set_long_mode(picture_label, LV_LABEL_LONG_DOT);
    choose_picture_button = lvgl_theme_button(row, "Choose a Picture...", 1);
    lv_obj_add_event_cb(choose_picture_button, on_choose_picture, LV_EVENT_CLICKED, 0);
    wrap_caption(card, "Any PNG or BMP can be the desktop: right-click it in Files and choose Set as Desktop "
                       "Picture. Paint saves what you draw into Pictures.");

    card = lvgl_theme_card(page, "STYLES");
    lv_obj_t *grid = plain(card);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(grid, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(grid, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    if (have_picture) {
        picture_choice = wallpaper_tile(grid, WALLPAPER_PICTURE, WALLPAPER_COUNT);
    }
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        wallpaper_choices[i] = wallpaper_tile(grid, i, i);
    }
    select_wallpaper_tiles();
}

static lv_obj_t *choice_grid(lv_obj_t *card) {
    lv_obj_t *grid = plain(card);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(grid, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(grid, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    return grid;
}

/* Under the screen's size, at the top of the pane, because the choice it
   asks about may be a long way down a list of thirty modes. */
static void build_confirm_row(lv_obj_t *card) {
    confirm_row = lvgl_theme_row(card, 0);
    confirm_label = lvgl_theme_value(confirm_row, "Keep this size?");
    lv_obj_set_flex_grow(confirm_label, 1);
    revert_button = lvgl_theme_button(confirm_row, "Revert", 0);
    lv_obj_add_event_cb(revert_button, on_mode_revert, LV_EVENT_CLICKED, 0);
    keep_button = lvgl_theme_button(confirm_row, "Keep", 1);
    lv_obj_add_event_cb(keep_button, on_mode_keep, LV_EVENT_CLICKED, 0);
    if (confirm_until_ms == 0) {
        lv_obj_add_flag(confirm_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void startup_description(char *out, size_t capacity, int *differs) {
    uint32_t width = display_status.startup_width;
    uint32_t height = display_status.startup_height;
    if (display_status.startup_selection == DISPLAY_STARTUP_LARGEST && startup_mode_count > 0) {
        width = startup_modes[0].width;
        height = startup_modes[0].height;
    } else if (display_status.startup_selection != DISPLAY_STARTUP_EXACT) {
        *differs = 0;
        out[0] = '\0';
        return;
    }
    *differs = width != display_status.physical_width || height != display_status.physical_height;
    if (*differs) {
        snprintf(out, capacity, "The screen will be %u x %u after a restart.", (unsigned)width, (unsigned)height);
    } else {
        snprintf(out, capacity, "The screen stays %u x %u.", (unsigned)width, (unsigned)height);
    }
}

/* Beside the confirmation and for the same reason: a mode for the next boot
   is chosen far down the list, and the restart it needs belongs where the
   screen's size is. */
static void build_restart_row(lv_obj_t *card) {
    restart_row = lvgl_theme_row(card, 0);
    restart_label = lvgl_theme_value(restart_row, "");
    lv_obj_set_flex_grow(restart_label, 1);
    lv_label_set_long_mode(restart_label, LV_LABEL_LONG_WRAP);
    restart_button = lvgl_theme_button(restart_row, "Restart Now", 1);
    lv_obj_add_event_cb(restart_button, on_restart, LV_EVENT_CLICKED, 0);
    char text[96];
    int differs = 0;
    if (mode_count == 0 && display_status_known && display_status.startup_writable) {
        collect_startup_modes();
        startup_description(text, sizeof(text), &differs);
    }
    if (differs) {
        lv_label_set_text(restart_label, text);
    } else {
        lv_obj_add_flag(restart_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_display(void) {
    refresh_display_status();
    page_title("Display", "How big everything looks, and the mode the screen runs in.");
    lv_obj_t *card = lvgl_theme_card(page, "THIS SCREEN");
    char text[96];
    describe_display(text, sizeof(text));
    display_size_label = fact(card, "Now", text);
    build_confirm_row(card);
    build_restart_row(card);

    card = lvgl_theme_card(page, "SCALE");
    if (display_status_known && display_status.scale_count > 1) {
        lv_obj_t *grid = choice_grid(card);
        for (int i = 0; i < (int)display_status.scale_count; i++) {
            uint32_t percent = display_status.scales[i];
            snprintf(text, sizeof(text), "%u%%%s\n%u x %u", (unsigned)percent,
                     percent == display_status.scale_automatic ? " (default)" : "",
                     (unsigned)(display_status.physical_width * 100u / percent),
                     (unsigned)(display_status.physical_height * 100u / percent));
            scale_choices[i] = lvgl_theme_choice(grid, text);
            lv_obj_set_height(scale_choices[i], LV_SIZE_CONTENT);
            lv_obj_set_style_pad_ver(scale_choices[i], 6, LV_PART_MAIN);
            lv_obj_set_style_text_align(scale_choices[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lvgl_theme_choice_select(scale_choices[i], percent == display_status.scale_percent);
            lv_obj_add_event_cb(scale_choices[i], on_scale_choice, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        wrap_caption(card, "A bigger scale makes text, icons and windows larger; a smaller one fits more on the "
                           "screen. The number under each is the size of the desktop it gives.");
    } else {
        wrap_caption(card, "This screen is shown one to one. A screen with more pixels can be scaled up here, "
                           "so text stays a readable size.");
    }

    card = lvgl_theme_card(page, "RESOLUTION");
    if (mode_count > 0) {
        lv_obj_t *grid = choice_grid(card);
        for (int i = 0; i < mode_count; i++) {
            snprintf(text, sizeof(text), "%ux%u", (unsigned)modes[i].width, (unsigned)modes[i].height);
            mode_choices[i] = lvgl_theme_choice(grid, text);
            lvgl_theme_choice_select(mode_choices[i], display_status_known &&
                                                          display_status.physical_width == modes[i].width &&
                                                          display_status.physical_height == modes[i].height);
            lv_obj_add_event_cb(mode_choices[i], on_mode_choice, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        wrap_caption(card, "A new size or scale goes back by itself after ten seconds unless you keep it, so one "
                           "the screen cannot show is never stuck.");
    } else if (display_status_known && display_status.startup_writable) {
        collect_startup_modes();
        lv_obj_t *grid = choice_grid(card);
        for (int i = 0; i < startup_mode_count; i++) {
            snprintf(text, sizeof(text), i == 0 ? "%ux%u (native)" : "%ux%u", (unsigned)startup_modes[i].width,
                     (unsigned)startup_modes[i].height);
            startup_choices[i] = lvgl_theme_choice(grid, text);
            lvgl_theme_choice_select(startup_choices[i], startup_mode_is_chosen(i));
            lv_obj_add_event_cb(startup_choices[i], on_startup_choice, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        wrap_caption(card, "This screen's mode is set by the firmware as the machine starts, so a new one is "
                           "written to the boot disk and used from the next restart. The native mode is the "
                           "sharpest; scale it up above rather than choosing a smaller one.");
    } else {
        wrap_caption(card, "This screen's mode is set by the firmware as the machine starts, by video= in "
                           "\\EFI\\BOOT\\lean_os.cfg on the boot disk - and this boot disk's copy cannot be "
                           "changed from here.");
    }
}

static void build_sound(void) {
    page_title("Sound", "How loud the machine is.");
    static char devices[256];
    int have_list = read_text_file(PATH_PROCESS_DIRECTORY "sound", devices, sizeof(devices)) >= 0;
    int ac97 = have_list && strstr(devices, "output\tac97") != 0;
    int hda = have_list && strstr(devices, "undriven\thda") != 0;
    lv_obj_t *card = lvgl_theme_card(page, "THIS MACHINE");
    fact(card, "Output", ac97 ? "AC'97 audio" : "None this system can drive");
    if (hda) {
        wrap_caption(card, "This machine's sound is Intel High Definition Audio, which lean_os has no driver "
                           "for yet - so there is no sound but the error beep, and only where the machine has "
                           "a beeper of its own.");
    }
    card = lvgl_theme_card(page, "OUTPUT");
    lv_obj_t *row = lvgl_theme_row(card, "Volume");
    volume_slider = slider(row, 0, 100, (int32_t)current.volume, on_volume_changed);
    char text[8];
    snprintf(text, sizeof(text), "%u%%", (unsigned)current.volume);
    volume_value_label = lvgl_theme_value(row, text);
    lv_obj_set_width(volume_value_label, 44);
    lv_obj_set_style_text_align(volume_value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    wrap_caption(card, ac97 ? "Applies to the AC'97 output and the speaker's error beep. Zero is silent."
                            : "Applies to the error beep. Zero is silent.");
    printf("[settings] sound output %s\n", ac97 ? "ac97" : hda ? "undriven hda" : "none");
}

static void build_mouse(void) {
    page_title("Mouse", "How the pointer follows your hand.");
    lv_obj_t *card = lvgl_theme_card(page, "POINTER");
    lv_obj_t *row = lvgl_theme_row(card, "Speed");
    speed_slider = slider(row, WINDOW_MANAGER_POINTER_SPEED_MINIMUM, WINDOW_MANAGER_POINTER_SPEED_MAXIMUM,
                          (int32_t)current.pointer_speed, on_speed_changed);
    char text[16];
    snprintf(text, sizeof(text), "%u%%", (unsigned)current.pointer_speed);
    speed_value_label = lvgl_theme_value(row, text);
    lv_obj_set_width(speed_value_label, 52);
    lv_obj_set_style_text_align(speed_value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    wrap_caption(card, "100% moves the pointer one pixel for each count the mouse reports.");
    swap_switch = toggle(card, "Swap the left and right buttons", current.swap_buttons != 0, on_swap_changed);

    card = lvgl_theme_card(page, "SCROLLING");
    natural_switch = toggle(card, "Natural scrolling", current.natural_scrolling != 0, on_natural_changed);
    wrap_caption(card, "Content moves the way your finger does, as on a touch screen.");
    row = lvgl_theme_row(card, "Scroll speed");
    lv_obj_set_width(lv_obj_get_child(row, 0), SETTINGS_SLIDER_LABEL_WIDTH);
    scroll_speed_slider = slider(row, WINDOW_MANAGER_SCROLL_SPEED_MINIMUM, WINDOW_MANAGER_SCROLL_SPEED_MAXIMUM,
                                 (int32_t)current.scroll_speed, on_scroll_speed_changed);
    snprintf(text, sizeof(text), "%u%%", (unsigned)current.scroll_speed);
    scroll_speed_label = lvgl_theme_value(row, text);
    lv_obj_set_width(scroll_speed_label, 52);
    lv_obj_set_style_text_align(scroll_speed_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    wrap_caption(card, "How far one click of the wheel moves what is under the pointer. These settings are the "
                       "mouse's; a trackpad has its own.");
}

static lv_obj_t *slider_row(lv_obj_t *card, const char *label, int32_t minimum, int32_t maximum, int32_t value,
                            lv_event_cb_t changed, const char *shown, lv_obj_t **value_label) {
    lv_obj_t *row = lvgl_theme_row(card, label);
    lv_obj_set_width(lv_obj_get_child(row, 0), SETTINGS_SLIDER_LABEL_WIDTH);
    lv_obj_t *control = slider(row, minimum, maximum, value, changed);
    *value_label = lvgl_theme_value(row, shown);
    lv_obj_set_width(*value_label, 52);
    lv_obj_set_style_text_align(*value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    return control;
}

/* What /proc/input says the machine has, in words. */
static void trackpad_description(char *out, size_t capacity) {
    static char devices[512];
    if (read_text_file(PATH_PROCESS_DIRECTORY "input", devices, sizeof(devices)) < 0) {
        snprintf(out, capacity, "This machine did not say what it has.");
        return;
    }
    for (char *line = devices; *line;) {
        char *end = line;
        while (*end && *end != '\n') {
            end++;
        }
        char saved = *end;
        *end = '\0';
        if (strncmp(line, "trackpad\t", 9) == 0) {
            const char *rest = line + 9;
            if (strncmp(rest, "i2c-hid ", 8) == 0) {
                int multitouch = strstr(rest, "multitouch") != 0;
                snprintf(out, capacity, "%.9s on the I2C bus, %s.", rest + 8,
                         multitouch ? "multi-touch: two fingers scroll and a tap clicks"
                                    : "in its mouse mode: it moves and clicks, but cannot scroll");
            } else {
                snprintf(out, capacity, "The PS/2 pointer, labelled a trackpad by the machine's start-up switch.");
            }
            return;
        }
        *end = saved;
        line = saved ? end + 1 : end;
    }
    snprintf(out, capacity, "No trackpad was found. These settings apply to one as soon as there is one.");
}

static void build_trackpad(void) {
    page_title("Trackpad", "How the pointer follows your finger.");
    lv_obj_t *card = lvgl_theme_card(page, "THIS MACHINE");
    char text[160];
    trackpad_description(text, sizeof(text));
    wrap_caption(card, text);

    card = lvgl_theme_card(page, "POINTER");
    snprintf(text, sizeof(text), "%u%%", (unsigned)current.trackpad_speed);
    trackpad_speed_slider = slider_row(card, "Tracking speed", WINDOW_MANAGER_POINTER_SPEED_MINIMUM,
                                       WINDOW_MANAGER_POINTER_SPEED_MAXIMUM, (int32_t)current.trackpad_speed,
                                       on_trackpad_speed_changed, text, &trackpad_speed_label);
    acceleration_text(current.trackpad_acceleration, text, sizeof(text));
    trackpad_acceleration_slider = slider_row(card, "Acceleration", 0, WINDOW_MANAGER_ACCELERATION_MAXIMUM,
                                              (int32_t)current.trackpad_acceleration,
                                              on_trackpad_acceleration_changed, text, &trackpad_acceleration_label);
    wrap_caption(card, "Acceleration makes a quick flick go further than a slow push over the same distance, so "
                       "the pointer can cross the screen and still land on a small button.");

    card = lvgl_theme_card(page, "SCROLLING");
    trackpad_natural_switch = toggle(card, "Natural scrolling", current.trackpad_natural_scrolling != 0,
                                     on_trackpad_natural_changed);
    wrap_caption(card, "On: the page moves with your fingers, like paper. Off: your fingers move the scroll "
                       "bar, the way a mouse wheel does.");
    snprintf(text, sizeof(text), "%u%%", (unsigned)current.trackpad_scroll_speed);
    trackpad_scroll_slider = slider_row(card, "Scroll speed", WINDOW_MANAGER_SCROLL_SPEED_MINIMUM,
                                        WINDOW_MANAGER_SCROLL_SPEED_MAXIMUM, (int32_t)current.trackpad_scroll_speed,
                                        on_trackpad_scroll_changed, text, &trackpad_scroll_label);

    card = lvgl_theme_card(page, "CLICKING");
    tap_switch = toggle(card, "Tap to click", current.trackpad_tap_to_click != 0, on_tap_changed);
    wrap_caption(card, "A light, quick touch is a click without pressing the pad down. Tap and touch again at once "
                       "to hold the button: slide to drag, lift to let go.");

    card = lvgl_theme_card(page, "GESTURES");
    lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);
    fact(card, "Scroll", "Two fingers, up or down");
    fact(card, "Secondary click", "Tap with two fingers, or press with two down");
    fact(card, "Middle click", "Tap with three fingers");
    fact(card, "Drag", "Press the pad and move, or tap and then slide");
}

static void build_keyboard(void) {
    page_title("Keyboard", "Shortcuts the desktop answers wherever you are.");
    const desktop_palette_t *p = palette();
    lv_obj_t *card = lvgl_theme_card(page, "SHORTCUTS");
    lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);
    for (int i = 0; i < SHORTCUT_COUNT; i++) {
        lv_obj_t *row = plain(card);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *chord = lv_label_create(row);
        lv_label_set_text(chord, SHORTCUTS[i].chord);
        lv_obj_set_width(chord, 190);
        lv_obj_set_style_text_font(chord, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(chord, color(p->text), LV_PART_MAIN);
        lv_obj_t *what = lv_label_create(row);
        lv_label_set_text(what, SHORTCUTS[i].what);
        lv_obj_set_flex_grow(what, 1);
        lv_obj_set_style_text_font(what, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(what, color(p->text_dim), LV_PART_MAIN);
    }
    card = lvgl_theme_card(page, "IN FILES");
    static const char *const FILES_KEYS[][2] = {
        {"Space", "Quick Look"},
        {"Ctrl+I", "Get Info"},
        {"Ctrl+N / Ctrl+Shift+N", "New file / new folder"},
        {"Delete or Backspace", "Move to Trash"},
        {"Ctrl+Z", "Undo the last move, rename or trash"},
        {"Ctrl+F", "Search"},
        {"Ctrl+1 / Ctrl+2", "List / icons"},
        {"Ctrl+H", "Show hidden files"},
    };
    for (int i = 0; i < (int)(sizeof(FILES_KEYS) / sizeof(FILES_KEYS[0])); i++) {
        lv_obj_t *row = plain(card);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_t *chord = lv_label_create(row);
        lv_label_set_text(chord, FILES_KEYS[i][0]);
        lv_obj_set_width(chord, 190);
        lv_obj_set_style_text_font(chord, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(chord, color(p->text), LV_PART_MAIN);
        lv_obj_t *what = lv_label_create(row);
        lv_label_set_text(what, FILES_KEYS[i][1]);
        lv_obj_set_flex_grow(what, 1);
        lv_obj_set_style_text_font(what, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(what, color(p->text_dim), LV_PART_MAIN);
    }
}

static void build_date_time(void) {
    page_title("Date & Time", "How the taskbar, Files and this panel show the time.");
    lv_obj_t *card = lvgl_theme_card(page, "NOW");
    clock_label = fact(card, "Time", "");
    date_label = fact(card, "Date", "");
    refresh_clock_labels();

    card = lvgl_theme_card(page, "FORMAT");
    clock_switch = toggle(card, "24-hour clock", current.clock_24_hour != 0, on_clock_changed);
    lv_obj_t *row = lvgl_theme_row(card, "Time zone");
    spacer(row);
    offset_dropdown = lv_dropdown_create(row);
    lv_obj_set_width(offset_dropdown, 150);
    static char options[UTC_OFFSET_COUNT * TIME_FORMAT_OFFSET_MAX];
    size_t n = 0;
    int selected = 0;
    for (int i = 0; i < UTC_OFFSET_COUNT; i++) {
        char label[TIME_FORMAT_OFFSET_MAX];
        time_format_offset(UTC_OFFSETS[i], label);
        n += (size_t)snprintf(options + n, sizeof(options) - n, "%s%s", i ? "\n" : "", label);
        if (UTC_OFFSETS[i] == current.utc_offset_minutes) {
            selected = i;
        }
    }
    lv_dropdown_set_options(offset_dropdown, options);
    lv_dropdown_set_selected(offset_dropdown, (uint32_t)selected);
    lv_obj_add_event_cb(offset_dropdown, on_offset_changed, LV_EVENT_VALUE_CHANGED, 0);
    wrap_caption(card, "The machine keeps UTC, from its own clock or from the network; the zone is how far from "
                       "UTC it is shown.");
}

static void build_network(void) {
    page_title("Network", "How this machine reaches other ones.");
    lv_obj_t *card = lvgl_theme_card(page, "WIRED");
    os_netconf_t configuration;
    char text[96];
    if (sys_netconf(&configuration) == 0 && configuration.ip != 0) {
        char a[16];
        os_ip_to_string(configuration.ip, a);
        fact(card, "Address", a);
        os_ip_to_string(configuration.mask, a);
        fact(card, "Subnet mask", a);
        os_ip_to_string(configuration.gateway, a);
        fact(card, "Router", a);
        os_ip_to_string(configuration.dns, a);
        fact(card, "DNS from DHCP", a);
        fact(card, "Configured by", configuration.leased ? "DHCP" : "a fixed address");
    } else {
        fact(card, "Status", "No address");
    }

    card = lvgl_theme_card(page, "NAME SERVERS");
    static char resolv[1024];
    int listed = 0;
    if (read_text_file(PATH_RESOLV_CONF, resolv, sizeof(resolv)) > 0) {
        for (char *line = resolv; *line;) {
            char *end = line;
            while (*end && *end != '\n') {
                end++;
            }
            char saved = *end;
            *end = '\0';
            if (strncmp(line, "nameserver", 10) == 0) {
                char *value = line + 10;
                while (*value == ' ' || *value == '\t') {
                    value++;
                }
                snprintf(text, sizeof(text), "%s", value);
                fact(card, listed == 0 ? "From resolv.conf" : "", text);
                listed++;
            }
            *end = saved;
            line = *end ? end + 1 : end;
        }
    }
    if (!listed) {
        fact(card, "From resolv.conf", "none");
    }
    wrap_caption(card, "Every name server here is asked at once alongside the one DHCP gave, and the first correct "
                       "answer wins.");

    card = lvgl_theme_card(page, "WI-FI");
    wireless_description(text, sizeof(text));
    lv_obj_t *row = lvgl_theme_row(card, "Status");
    wireless_label = right_value(row, text);
    os_wireless_status_t radio;
    memset(&radio, 0, sizeof(radio));
    if (sys_wireless(WIRELESS_OPERATION_STATUS, &radio, sizeof(radio)) >= 0 &&
        radio.state != WIRELESS_STATE_ABSENT) {
        row = lvgl_theme_row(card, 0);
        spacer(row);
        lv_obj_t *open = lvgl_theme_button(row, "Wi-Fi Networks...", 1);
        lv_obj_add_event_cb(open, on_open_wireless, LV_EVENT_CLICKED, 0);
    }
}

typedef struct {
    const char *path;
    const char *name;
    uint32_t tint;
} storage_folder_t;

static const storage_folder_t STORAGE_FOLDERS[] = {
    {PATH_BIN, "Programs (/bin)", 0x003498DBu},
    {PATH_HOME, "Home", 0x0027AE60u},
    {PKG_ROOT, "Packages", 0x00E67E22u},
    {"/usr", "/usr", 0x009B59B6u},
    {"/lib", "/lib", 0x0016A085u},
    {PATH_ETC, "Settings (/etc)", 0x00F1C40Fu},
    {PATH_ICONS, "Icons", 0x00E74C3Cu},
    {PATH_TEMPORARY, "Temporary", 0x0095A5A6u},
};

#define STORAGE_FOLDER_COUNT ((int)(sizeof(STORAGE_FOLDERS) / sizeof(STORAGE_FOLDERS[0])))

static void build_storage_breakdown(void) {
    if (!storage_list) {
        return;
    }
    lv_obj_clean(storage_list);
    os_statvfs_t disk;
    if (sys_statvfs("/", &disk) != 0 || disk.total_blocks == 0) {
        lvgl_theme_caption(storage_list, "The disk could not be measured.");
        return;
    }
    uint64_t total = (uint64_t)disk.total_blocks * disk.block_size;
    uint64_t used = (uint64_t)(disk.total_blocks - disk.free_blocks) * disk.block_size;
    uint64_t sizes[STORAGE_FOLDER_COUNT];
    uint64_t counted = 0;
    for (int i = 0; i < STORAGE_FOLDER_COUNT; i++) {
        file_system_utilities_tree_t tree;
        sizes[i] = file_system_utilities_count_tree(STORAGE_FOLDERS[i].path, &tree) == 0 ? tree.bytes : 0;
        counted += sizes[i];
    }

    lv_obj_t *bar = plain(storage_list);
    lv_obj_set_size(bar, LV_PCT(100), 18);
    lv_obj_set_style_radius(bar, 9, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(bar, true, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, color(palette()->outline), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_update_layout(storage_list);
    int32_t width = lv_obj_get_content_width(storage_list);
    for (int i = 0; i < STORAGE_FOLDER_COUNT; i++) {
        int32_t segment = (int32_t)(sizes[i] * (uint64_t)width / total);
        if (sizes[i] == 0) {
            continue;
        }
        if (segment < 3) {
            segment = 3;
        }
        lv_obj_t *piece = plain(bar);
        lv_obj_set_size(piece, segment, LV_PCT(100));
        lv_obj_set_style_bg_color(piece, color(STORAGE_FOLDERS[i].tint), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(piece, LV_OPA_COVER, LV_PART_MAIN);
    }
    uint64_t other = used > counted ? used - counted : 0;
    int32_t other_width = (int32_t)(other * (uint64_t)width / total);
    if (other_width > 0) {
        lv_obj_t *piece = plain(bar);
        lv_obj_set_size(piece, other_width, LV_PCT(100));
        lv_obj_set_style_bg_color(piece, color(0x00607080u), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(piece, LV_OPA_COVER, LV_PART_MAIN);
    }

    char text[96];
    char a[32];
    char b[32];
    format_bytes(used, a, sizeof(a));
    format_bytes(total, b, sizeof(b));
    snprintf(text, sizeof(text), "%s of %s used", a, b);
    lv_obj_t *summary = lvgl_theme_value(storage_list, text);
    lv_obj_set_style_pad_top(summary, 4, LV_PART_MAIN);
    for (int i = 0; i < STORAGE_FOLDER_COUNT; i++) {
        if (sizes[i] == 0) {
            continue;
        }
        lv_obj_t *row = plain(storage_list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
        lv_obj_t *dot = plain(row);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, 5, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, color(STORAGE_FOLDERS[i].tint), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_t *name = lvgl_theme_value(row, STORAGE_FOLDERS[i].name);
        lv_obj_set_flex_grow(name, 1);
        format_bytes(sizes[i], a, sizeof(a));
        lvgl_theme_caption(row, a);
        lv_obj_t *open = lvgl_theme_button(row, LV_SYMBOL_DIRECTORY, 0);
        lv_obj_set_size(open, 34, 26);
        lv_obj_set_style_pad_hor(open, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(open, on_open_files, LV_EVENT_CLICKED, (void *)STORAGE_FOLDERS[i].path);
    }
    snprintf(text, sizeof(text), "%u of %u files and folders left", (unsigned)disk.free_inodes,
             (unsigned)disk.total_inodes);
    lvgl_theme_caption(storage_list, text);
}

static void build_storage(void) {
    page_title("Storage", "What is on the startup disk, measured file by file.");
    lv_obj_t *card = lvgl_theme_card(page, "STARTUP DISK");
    storage_list = plain(card);
    lv_obj_set_size(storage_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(storage_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(storage_list, 8, LV_PART_MAIN);
    build_storage_breakdown();

    card = lvgl_theme_card(page, "TRASH");
    file_system_utilities_tree_t tree;
    char text[96];
    if (file_system_utilities_count_tree(FILE_BROWSER_TRASH, &tree) == 0) {
        char amount[32];
        format_bytes(tree.bytes, amount, sizeof(amount));
        uint32_t items = tree.entries;
        file_system_utilities_tree_t records;
        if (file_system_utilities_count_tree(FILE_BROWSER_TRASH_ORIGINS, &records) == 0) {
            items = items > records.entries + 1 ? items - records.entries - 1 : 0;
        }
        snprintf(text, sizeof(text), "%s in %u %s", amount, (unsigned)items, items == 1 ? "thing" : "things");
    } else {
        snprintf(text, sizeof(text), "Empty");
    }
    lv_obj_t *row = lvgl_theme_row(card, "Holding");
    trash_label = right_value(row, text);
    row = lvgl_theme_row(card, 0);
    spacer(row);
    lv_obj_t *open = lvgl_theme_button(row, "Open", 0);
    lv_obj_add_event_cb(open, on_open_files, LV_EVENT_CLICKED, (void *)FILE_BROWSER_TRASH);
    lv_obj_t *empty = lvgl_theme_button(row, "Empty Trash", 1);
    lv_obj_set_style_bg_color(empty, color(palette()->danger), LV_PART_MAIN);
    lv_obj_add_event_cb(empty, on_empty_trash, LV_EVENT_CLICKED, 0);
}

static void capability_names(uint32_t set, char *out, size_t capacity) {
    size_t n = 0;
    out[0] = '\0';
    if ((set & CAP_ALL) == CAP_ALL) {
        snprintf(out, capacity, "everything");
        return;
    }
    for (int i = 0; i < CAP_NAME_COUNT; i++) {
        if (set & CAP_NAMES[i].bit) {
            int wrote = snprintf(out + n, capacity - n, "%s%s", n ? ", " : "", CAP_NAMES[i].name);
            if (wrote < 0 || (size_t)wrote >= capacity - n) {
                return;
            }
            n += (size_t)wrote;
        }
    }
    if (n == 0) {
        snprintf(out, capacity, "nothing");
    }
}

static void build_privacy(void) {
    page_title("Privacy & Security",
               "Every program holds a set of capabilities the kernel gives it as it starts, chosen by the "
               "name it was started by. A set can only ever shrink, and a program started by another holds no "
               "more than its parent.");
    lv_obj_t *card = lvgl_theme_card(page, "THIS WINDOW");
    char text[160];
    capability_names((uint32_t)sys_getcaps(), text, sizeof(text));
    fact(card, "Settings may", text);
    capability_names(CAP_APP_DEFAULT, text, sizeof(text));
    fact(card, "Any other program", text);

    static const char *const DESKTOP_PROGRAMS[] = {
        "file_manager", "text_editor", "gui_terminal", "task_manager", "wifi", "browser",
        "gui_clock", "gui_paint", "desktop_shell", "shutdown", "os", "console",
    };
    card = lvgl_theme_card(page, "WHAT EACH PROGRAM MAY DO");
    for (int i = 0; i < (int)(sizeof(DESKTOP_PROGRAMS) / sizeof(DESKTOP_PROGRAMS[0])); i++) {
        capability_names(caps_for_program(DESKTOP_PROGRAMS[i]), text, sizeof(text));
        fact(card, DESKTOP_PROGRAMS[i], text);
    }
    wrap_caption(card, "An ordinary program cannot paint the screen, read the clipboard, list processes, open a "
                       "socket or switch the machine off. A package may ask for at most fs-write, network and audio.");

    card = lvgl_theme_card(page, "PEOPLE");
    wrap_caption(card, "This machine has one person on it, so there are no accounts and no permissions on files: "
                       "a permission model with nobody behind it would only pretend to protect anything.");
}

static void build_startup(void) {
    page_title("Startup", "What happens when the machine starts.");
    lv_obj_t *card = lvgl_theme_card(page, "WINDOWS");
    restore_switch = toggle(card, "Reopen windows that were open", current.restore_windows != 0,
                            on_restore_changed);
    wrap_caption(card, "Windows come back where they were and on the desktop they were on. An editor with unsaved "
                       "changes can still stop a shutdown either way.");

    card = lvgl_theme_card(page, "OPEN NOW, AND REMEMBERED");
    static char session[2048];
    int listed = 0;
    if (read_text_file(SETTINGS_SESSION_PATH, session, sizeof(session)) > 0) {
        for (char *line = session; *line;) {
            char *end = line;
            while (*end && *end != '\n') {
                end++;
            }
            char saved = *end;
            *end = '\0';
            char *space = line;
            while (*space && *space != ' ') {
                space++;
            }
            char program[96];
            int length = (int)(space - line);
            if (length > 0 && length < (int)sizeof(program)) {
                memcpy(program, line, (size_t)length);
                program[length] = '\0';
                lv_obj_t *row = lvgl_theme_row(card, 0);
                lvgl_theme_value(row, file_browser_basename(program));
                listed++;
            }
            *end = saved;
            line = *end ? end + 1 : end;
        }
    }
    if (!listed) {
        wrap_caption(card, "No windows are remembered yet.");
    }
}

static void report_geometry(void) {
    char name[24];
    for (int i = 0; i < PANE_COUNT; i++) {
        lvgl_window_report_geometry(PANES[i].report, pane_items[i]);
    }
    if (active_pane == PANE_WALLPAPER) {
        for (int i = 0; i < WALLPAPER_COUNT; i++) {
            snprintf(name, sizeof(name), "wallpaper%d", i);
            lvgl_window_report_geometry(name, wallpaper_choices[i]);
        }
        if (picture_choice) {
            lvgl_window_report_geometry("wallpaper_picture", picture_choice);
        }
        lvgl_window_report_geometry("choose_picture", choose_picture_button);
    } else if (active_pane == PANE_APPEARANCE) {
        for (int i = 0; i < ACCENT_SWATCH_COUNT; i++) {
            snprintf(name, sizeof(name), "accent%d", i);
            lvgl_window_report_geometry(name, accent_swatches[i]);
        }
        lvgl_window_report_geometry("motion", motion_switch);
    } else if (active_pane == PANE_DISPLAY) {
        for (int i = 0; i < mode_count; i++) {
            snprintf(name, sizeof(name), "mode%d", i);
            lvgl_window_report_geometry(name, mode_choices[i]);
        }
        for (int i = 0; i < DISPLAY_SCALES_MAX && scale_choices[i]; i++) {
            snprintf(name, sizeof(name), "scale%u", (unsigned)display_status.scales[i]);
            lvgl_window_report_geometry(name, scale_choices[i]);
        }
        for (int i = 0; i < startup_mode_count && startup_choices[i]; i++) {
            snprintf(name, sizeof(name), "startup%ux%u", (unsigned)startup_modes[i].width,
                     (unsigned)startup_modes[i].height);
            lvgl_window_report_geometry(name, startup_choices[i]);
        }
        lvgl_window_report_geometry("keep", keep_button);
    } else if (active_pane == PANE_SOUND) {
        lvgl_window_report_geometry("volume", volume_slider);
    } else if (active_pane == PANE_MOUSE) {
        lvgl_window_report_geometry("speed", speed_slider);
        lvgl_window_report_geometry("natural", natural_switch);
        lvgl_window_report_geometry("swap", swap_switch);
        lvgl_window_report_geometry("scroll_speed", scroll_speed_slider);
    } else if (active_pane == PANE_TRACKPAD) {
        lvgl_window_report_geometry("trackpad_speed", trackpad_speed_slider);
        lvgl_window_report_geometry("trackpad_acceleration", trackpad_acceleration_slider);
        lvgl_window_report_geometry("trackpad_natural", trackpad_natural_switch);
        lvgl_window_report_geometry("trackpad_scroll_speed", trackpad_scroll_slider);
        lvgl_window_report_geometry("tap_to_click", tap_switch);
    } else if (active_pane == PANE_DATE_TIME) {
        lvgl_window_report_geometry("clock24", clock_switch);
        lvgl_window_report_geometry("zone", offset_dropdown);
    } else if (active_pane == PANE_STARTUP) {
        lvgl_window_report_geometry("restore", restore_switch);
    }
    printf("[settings] showing %s\n", PANES[active_pane].name);
}

static void forget_pane_objects(void) {
    memset(background_swatches, 0, sizeof(background_swatches));
    memset(accent_swatches, 0, sizeof(accent_swatches));
    memset(wallpaper_choices, 0, sizeof(wallpaper_choices));
    for (int i = 0; i < SETTINGS_THUMBNAILS; i++) {
        if (thumbnails[i]) {
            lv_draw_buf_destroy(thumbnails[i]);
            thumbnails[i] = 0;
        }
    }
    picture_choice = choose_picture_button = 0;
    scroll_speed_slider = scroll_speed_label = 0;
    trackpad_speed_slider = trackpad_speed_label = trackpad_acceleration_slider = trackpad_acceleration_label = 0;
    trackpad_natural_switch = trackpad_scroll_slider = trackpad_scroll_label = tap_switch = 0;
    memset(mode_choices, 0, sizeof(mode_choices));
    memset(scale_choices, 0, sizeof(scale_choices));
    memset(startup_choices, 0, sizeof(startup_choices));
    restart_row = restart_label = restart_button = revert_button = 0;
    confirm_row = keep_button = volume_slider = motion_switch = confirm_label = 0;
    volume_value_label = clipboard_label = clock_label = date_label = uptime_label = memory_label = 0;
    display_size_label = speed_slider = speed_value_label = natural_switch = swap_switch = 0;
    clock_switch = offset_dropdown = restore_switch = wireless_label = storage_list = trash_label = 0;
}

static void style_pane_items(void) {
    const desktop_palette_t *p = palette();
    for (int i = 0; i < PANE_COUNT; i++) {
        int active = i == (int)active_pane;
        lv_obj_set_style_bg_color(pane_items[i], color(active ? p->accent : p->surface), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(pane_items[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(lv_obj_get_child(pane_items[i], 1), color(active ? p->accent_text : p->text),
                                    LV_PART_MAIN);
    }
}

static void build_page(void) {
    lv_obj_clean(page);
    forget_pane_objects();
    lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
    switch (active_pane) {
    case PANE_GENERAL:
        build_general();
        break;
    case PANE_APPEARANCE:
        build_appearance();
        break;
    case PANE_WALLPAPER:
        build_wallpaper();
        break;
    case PANE_DISPLAY:
        build_display();
        break;
    case PANE_SOUND:
        build_sound();
        break;
    case PANE_MOUSE:
        build_mouse();
        break;
    case PANE_TRACKPAD:
        build_trackpad();
        break;
    case PANE_KEYBOARD:
        build_keyboard();
        break;
    case PANE_DATE_TIME:
        build_date_time();
        break;
    case PANE_NETWORK:
        build_network();
        break;
    case PANE_STORAGE:
        build_storage();
        break;
    case PANE_PRIVACY:
        build_privacy();
        break;
    case PANE_STARTUP:
        build_startup();
        break;
    case PANE_COUNT:
        break;
    }
    style_pane_items();
    lv_obj_update_layout(page);
    lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
    report_geometry();
}

static void on_pane_clicked(lv_event_t *event) {
    settings_pane_t pane = (settings_pane_t)(intptr_t)lv_event_get_user_data(event);
    if (pane != active_pane) {
        active_pane = pane;
        build_page();
    }
}

static void build_user_interface(void) {
    lvgl_theme_apply(&settings_window);
    const desktop_palette_t *p = palette();
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    forget_pane_objects();

    lv_obj_t *root = plain(screen);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);

    lv_obj_t *sidebar = plain(root);
    lv_obj_add_flag(sidebar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(sidebar, SETTINGS_SIDEBAR_WIDTH, LV_PCT(100));
    lv_obj_set_style_bg_color(sidebar, color(p->surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sidebar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(sidebar, LV_BORDER_SIDE_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_width(sidebar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(sidebar, color(p->outline), LV_PART_MAIN);
    lv_obj_set_style_pad_all(sidebar, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(sidebar, 3, LV_PART_MAIN);
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < PANE_COUNT; i++) {
        lv_obj_t *item = plain(sidebar);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(item, LV_PCT(100), 34);
        lv_obj_set_style_radius(item, 7, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(item, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_column(item, 10, LV_PART_MAIN);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *badge = plain(item);
        lv_obj_set_size(badge, 24, 24);
        lv_obj_set_style_radius(badge, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(badge, color(PANES[i].tint), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_t *symbol = lv_label_create(badge);
        lv_label_set_text(symbol, PANES[i].symbol);
        lv_obj_set_style_text_font(symbol, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(symbol, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(symbol);
        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, PANES[i].name);
        lv_obj_add_event_cb(item, on_pane_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        pane_items[i] = item;
    }

    page = lvgl_theme_page(root);
    lv_obj_set_height(page, LV_PCT(100));
    lv_obj_set_width(page, 0);
    lv_obj_set_flex_grow(page, 1);
    lv_obj_set_style_pad_all(page, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, 14, LV_PART_MAIN);
    build_page();
}

static unsigned long count_drawn_pixels(void) {
    const uint32_t *pixels = settings_window.window.graphics.pixels;
    unsigned long total = (unsigned long)settings_window.window.width * settings_window.window.height;
    unsigned long drawn = 0;
    for (unsigned long i = 0; i < total; i++) {
        if ((pixels[i] & 0x00FFFFFFu) != palette()->window) {
            drawn++;
        }
    }
    return drawn;
}

static settings_pane_t pane_named(const char *name) {
    for (int i = 0; name && i < PANE_COUNT; i++) {
        const char *report = PANES[i].report + 5;
        if (strcmp(name, report) == 0) {
            return (settings_pane_t)i;
        }
    }
    return PANE_GENERAL;
}

int desktop_application_settings(int selftest) {
    if (lvgl_window_open(SETTINGS_WINDOW_WIDTH, SETTINGS_WINDOW_HEIGHT, "Settings", &settings_window) != 0) {
        printf("[m127] settings: window open failed\n");
        return 1;
    }

    settings_file_defaults(&current);
    settings_file_load(&current);
    window_manager_settings_request_t live;
    if (window_manager_query_settings(&live) == 0) {
        current = live;
    }

    long n = sys_display_modes(modes, DISPLAY_MAX_MODES);
    mode_count = n < 0 ? 0 : (int)(n > DISPLAY_MAX_MODES ? DISPLAY_MAX_MODES : n);
    active_pane = pane_named(desktop_application_argument);

    build_user_interface();
    lv_timer_create(on_tick, 500, 0);

    for (int frame = 0; frame < 8; frame++) {
        lv_timer_handler();
        lvgl_window_pump(&settings_window, 16);
    }

    unsigned long drawn = count_drawn_pixels();
    printf("[m127] settings rendered %lu pixels\n", drawn);
    if (drawn < SETTINGS_MINIMUM_DRAWN_PIXELS) {
        printf("[m127] settings render FAILED\n");
        lvgl_window_close(&settings_window);
        return 1;
    }
    if (selftest) {
        lvgl_window_close(&settings_window);
        return 0;
    }

    while (!settings_window.should_close) {
        uint32_t sleep_ms = lv_timer_handler();
        if (rebuild_pending) {
            rebuild_pending = 0;
            build_user_interface();
            sleep_ms = 0;
            continue;
        }
        if (sleep_ms > LVGL_MAX_SLEEP_MS) {
            sleep_ms = LVGL_MAX_SLEEP_MS;
        }
        lvgl_window_pump(&settings_window, (int)sleep_ms);
    }
    lvgl_window_close(&settings_window);
    return 0;
}
