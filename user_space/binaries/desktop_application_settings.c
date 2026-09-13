#include <stdio.h>
#include <string.h>

#include "desktop_applications.h"
#include "lvgl_theme.h"
#include "settings_file.h"
#include "shortcuts.h"
#include "syscall_wrappers.h"
#include "wallpaper.h"

#define SETTINGS_WINDOW_WIDTH  440
#define SETTINGS_WINDOW_HEIGHT 620

#define SETTINGS_MINIMUM_DRAWN_PIXELS 20000

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

static lvgl_window_t settings_window;

static uint32_t current_background = DESKTOP_PALETTE_DEFAULT_BACKGROUND;
static uint32_t current_accent = DESKTOP_PALETTE_DEFAULT_ACCENT;
static uint32_t current_wallpaper = WALLPAPER_GRADIENT;
static uint32_t current_animations = 1;
static uint32_t current_volume = 70;

static display_mode_t modes[DISPLAY_MAX_MODES];
static int mode_count;
static long confirm_until_ms;

static int rebuild_pending;

static lv_obj_t *background_swatches[BACKGROUND_SWATCH_COUNT];
static lv_obj_t *accent_swatches[ACCENT_SWATCH_COUNT];
static lv_obj_t *wallpaper_choices[WALLPAPER_COUNT];
static lv_obj_t *mode_choices[DISPLAY_MAX_MODES];
static lv_obj_t *confirm_row;
static lv_obj_t *keep_button;
static lv_obj_t *volume_slider;
static lv_obj_t *motion_switch;
static lv_obj_t *confirm_label;
static lv_obj_t *volume_value_label;
static lv_obj_t *clipboard_label;
static lv_obj_t *clock_label;
static lv_obj_t *display_size_label;

static void apply_theme_to_desktop(void) {
    window_manager_settings_request_t settings;
    settings.volume = current_volume;
    settings.animations = current_animations;
    settings.bg_color = current_background;
    settings.accent_color = current_accent;
    settings.wallpaper = current_wallpaper;
    window_manager_set_settings(&settings);
    settings_file_save(&settings);
}

static void build_user_interface(void);

static void request_rebuild(void) {
    rebuild_pending = 1;
}

static void on_background_swatch(lv_event_t *event) {
    int index = (int)(long)lv_event_get_user_data(event);
    current_background = BACKGROUND_SWATCHES[index];
    apply_theme_to_desktop();
    request_rebuild();
}

static void on_accent_swatch(lv_event_t *event) {
    int index = (int)(long)lv_event_get_user_data(event);
    current_accent = ACCENT_SWATCHES[index];
    apply_theme_to_desktop();
    request_rebuild();
}

static void on_wallpaper_choice(lv_event_t *event) {
    int index = (int)(long)lv_event_get_user_data(event);
    current_wallpaper = (uint32_t)index;
    apply_theme_to_desktop();
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        lvgl_theme_choice_select(wallpaper_choices[i], i == index);
    }
}

static void on_volume_changed(lv_event_t *event) {
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(event);
    current_volume = (uint32_t)lv_slider_get_value(slider);
    char text[8];
    snprintf(text, sizeof(text), "%u%%", (unsigned)current_volume);
    lv_label_set_text(volume_value_label, text);
    apply_theme_to_desktop();
}

static void on_motion_changed(lv_event_t *event) {
    lv_obj_t *toggle = (lv_obj_t *)lv_event_get_target(event);
    current_animations = lv_obj_has_state(toggle, LV_STATE_CHECKED) ? 1u : 0u;
    apply_theme_to_desktop();
}

static void on_clipboard_clear(lv_event_t *event) {
    (void)event;
    sys_clipboard_set("", 0);
    lv_label_set_text(clipboard_label, "(empty)");
}

static void on_mode_choice(lv_event_t *event) {
    int index = (int)(long)lv_event_get_user_data(event);
    window_manager_set_display_mode(modes[index].width, modes[index].height);
    confirm_until_ms = sys_uptime_ms() + WINDOW_MANAGER_MODE_REVERT_MS;
    lv_obj_remove_flag(confirm_row, LV_OBJ_FLAG_HIDDEN);
    lvgl_window_report_geometry("keep", keep_button);
}

static void on_mode_keep(lv_event_t *event) {
    (void)event;
    window_manager_confirm_display_mode();
    confirm_until_ms = 0;
    lv_obj_add_flag(confirm_row, LV_OBJ_FLAG_HIDDEN);
    window_manager_framebuffer_info_t framebuffer_info;
    if (sys_framebuffer_info(&framebuffer_info) == 0) {
        settings_file_save_display(framebuffer_info.width, framebuffer_info.height);
    }
}

static void refresh_clipboard_label(void) {
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
    lv_label_set_text(clipboard_label, buffer);
}

static void refresh_clock_label(void) {
    char text[48];
    os_datetime_t now;
    if (sys_time(&now) > 0 && now.valid) {
        snprintf(text, sizeof(text), "%04u-%02u-%02u  %02u:%02u UTC",
                 (unsigned)now.year, (unsigned)now.month, (unsigned)now.day,
                 (unsigned)now.hour, (unsigned)now.minute);
    } else {
        snprintf(text, sizeof(text), "up %u s", (unsigned)(sys_uptime_ms() / 1000));
    }
    lv_label_set_text(clock_label, text);
}

static void refresh_display_size_label(void) {
    window_manager_framebuffer_info_t framebuffer_info;
    char text[32];
    if (sys_framebuffer_info(&framebuffer_info) != 0) {
        return;
    }
    snprintf(text, sizeof(text), "%u x %u", (unsigned)framebuffer_info.width,
             (unsigned)framebuffer_info.height);
    lv_label_set_text(display_size_label, text);
}

static void on_tick(lv_timer_t *timer) {
    (void)timer;
    refresh_clipboard_label();
    refresh_clock_label();
    refresh_display_size_label();
    if (confirm_until_ms != 0) {
        long left_ms = confirm_until_ms - sys_uptime_ms();
        if (left_ms <= 0) {
            confirm_until_ms = 0;
            lv_obj_add_flag(confirm_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            char text[40];
            snprintf(text, sizeof(text), "Keep this size? %ds", (int)(left_ms / 1000) + 1);
            lv_label_set_text(confirm_label, text);
        }
    }
}

static lv_obj_t *swatch_strip(lv_obj_t *card, const char *label) {
    lv_obj_t *row = lvgl_theme_row(card, label);
    lv_obj_t *strip = lv_obj_create(row);
    lv_obj_set_height(strip, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(strip, 1);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(strip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(strip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(strip, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(strip, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    return strip;
}

static void build_appearance_card(lv_obj_t *page) {
    lv_obj_t *card = lvgl_theme_card(page, "APPEARANCE");

    lv_obj_t *strip = swatch_strip(card, "Desktop");
    for (int i = 0; i < BACKGROUND_SWATCH_COUNT; i++) {
        background_swatches[i] = lvgl_theme_swatch(strip, BACKGROUND_SWATCHES[i]);
        lvgl_theme_swatch_select(background_swatches[i], BACKGROUND_SWATCHES[i] == current_background);
        lv_obj_add_event_cb(background_swatches[i], on_background_swatch, LV_EVENT_CLICKED,
                            (void *)(long)i);
    }

    strip = swatch_strip(card, "Accent");
    for (int i = 0; i < ACCENT_SWATCH_COUNT; i++) {
        accent_swatches[i] = lvgl_theme_swatch(strip, ACCENT_SWATCHES[i]);
        lvgl_theme_swatch_select(accent_swatches[i], ACCENT_SWATCHES[i] == current_accent);
        lv_obj_add_event_cb(accent_swatches[i], on_accent_swatch, LV_EVENT_CLICKED, (void *)(long)i);
    }

    strip = swatch_strip(card, "Wallpaper");
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        wallpaper_choices[i] = lvgl_theme_choice(strip, wallpaper_name(i));
        lvgl_theme_choice_select(wallpaper_choices[i], (uint32_t)i == current_wallpaper);
        lv_obj_add_event_cb(wallpaper_choices[i], on_wallpaper_choice, LV_EVENT_CLICKED,
                            (void *)(long)i);
    }
}

static void build_display_card(lv_obj_t *page) {
    lv_obj_t *card = lvgl_theme_card(page, "DISPLAY");

    window_manager_framebuffer_info_t framebuffer_info;
    char text[32];
    if (sys_framebuffer_info(&framebuffer_info) == 0) {
        snprintf(text, sizeof(text), "%u x %u", (unsigned)framebuffer_info.width,
                 (unsigned)framebuffer_info.height);
    } else {
        snprintf(text, sizeof(text), "unknown");
    }
    lv_obj_t *row = lvgl_theme_row(card, "Size");
    display_size_label = lvgl_theme_value(row, text);
    lv_obj_set_flex_grow(display_size_label, 1);
    lv_obj_set_style_text_align(display_size_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    if (mode_count == 0) {
        lvgl_theme_caption(card, "This display cannot be resized after boot.");
        confirm_row = lv_obj_create(card);
        lv_obj_add_flag(confirm_row, LV_OBJ_FLAG_HIDDEN);
        confirm_label = lvgl_theme_caption(confirm_row, "");
        return;
    }

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(grid, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(grid, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    window_manager_framebuffer_info_t current;
    int have_current = sys_framebuffer_info(&current) == 0;
    for (int i = 0; i < mode_count; i++) {
        snprintf(text, sizeof(text), "%ux%u", (unsigned)modes[i].width, (unsigned)modes[i].height);
        mode_choices[i] = lvgl_theme_choice(grid, text);
        lvgl_theme_choice_select(mode_choices[i],
                                 have_current && current.width == modes[i].width &&
                                     current.height == modes[i].height);
        lv_obj_add_event_cb(mode_choices[i], on_mode_choice, LV_EVENT_CLICKED, (void *)(long)i);
    }
    confirm_row = lvgl_theme_row(card, NULL);
    confirm_label = lvgl_theme_value(confirm_row, "Keep this size?");
    lv_obj_set_flex_grow(confirm_label, 1);
    lv_obj_t *keep = lvgl_theme_button(confirm_row, "Keep", 1);
    lv_obj_add_event_cb(keep, on_mode_keep, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(confirm_row, LV_OBJ_FLAG_HIDDEN);
    keep_button = keep;
}

static void build_sound_card(lv_obj_t *page) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    lv_obj_t *card = lvgl_theme_card(page, "SOUND & MOTION");

    lv_obj_t *row = lvgl_theme_row(card, "Volume");
    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, 10);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, (int32_t)current_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lvgl_theme_color(palette->outline), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lvgl_theme_color(palette->accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lvgl_theme_color(palette->text), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_volume_changed, LV_EVENT_VALUE_CHANGED, NULL);

    char text[8];
    snprintf(text, sizeof(text), "%u%%", (unsigned)current_volume);
    volume_value_label = lvgl_theme_value(row, text);
    lv_obj_set_width(volume_value_label, 40);
    lv_obj_set_style_text_align(volume_value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    row = lvgl_theme_row(card, "Motion");
    lv_obj_t *spacer = lv_obj_create(row);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(spacer, 0, LV_PART_MAIN);
    lv_obj_t *toggle = lv_switch_create(row);
    lv_obj_set_size(toggle, 48, 26);
    lv_obj_set_style_bg_color(toggle, lvgl_theme_color(palette->outline), LV_PART_MAIN);
    lv_obj_set_style_bg_color(toggle, lvgl_theme_color(palette->accent),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(toggle, lvgl_theme_color(palette->text), LV_PART_KNOB);
    if (current_animations) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(toggle, on_motion_changed, LV_EVENT_VALUE_CHANGED, NULL);

    volume_slider = slider;
    motion_switch = toggle;
}

static void build_system_card(lv_obj_t *page) {
    lv_obj_t *card = lvgl_theme_card(page, "SYSTEM");

    lv_obj_t *row = lvgl_theme_row(card, "Clock");
    clock_label = lvgl_theme_value(row, "");
    lv_obj_set_flex_grow(clock_label, 1);
    lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    refresh_clock_label();

    row = lvgl_theme_row(card, "Clipboard");
    clipboard_label = lvgl_theme_value(row, "(empty)");
    lv_obj_set_flex_grow(clipboard_label, 1);
    lv_label_set_long_mode(clipboard_label, LV_LABEL_LONG_DOT);
    lv_obj_t *clear = lvgl_theme_button(row, "Clear", 0);
    lv_obj_add_event_cb(clear, on_clipboard_clear, LV_EVENT_CLICKED, NULL);
    refresh_clipboard_label();
}

static void build_shortcut_card(lv_obj_t *page) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    lv_obj_t *card = lvgl_theme_card(page, "SHORTCUTS");
    lv_obj_set_style_pad_row(card, 4, LV_PART_MAIN);

    for (int i = 0; i < SHORTCUT_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *chord = lv_label_create(row);
        lv_label_set_text(chord, SHORTCUTS[i].chord);
        lv_obj_set_width(chord, 160);
        lv_obj_set_style_text_font(chord, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(chord, lvgl_theme_color(palette->text), LV_PART_MAIN);

        lv_obj_t *what = lv_label_create(row);
        lv_label_set_text(what, SHORTCUTS[i].what);
        lv_obj_set_flex_grow(what, 1);
        lv_obj_set_style_text_font(what, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(what, lvgl_theme_color(palette->text_dim), LV_PART_MAIN);
    }
}

static void report_geometry(void) {
    char name[24];
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        snprintf(name, sizeof(name), "wallpaper%d", i);
        lvgl_window_report_geometry(name, wallpaper_choices[i]);
    }
    for (int i = 0; i < ACCENT_SWATCH_COUNT; i++) {
        snprintf(name, sizeof(name), "accent%d", i);
        lvgl_window_report_geometry(name, accent_swatches[i]);
    }
    for (int i = 0; i < mode_count; i++) {
        snprintf(name, sizeof(name), "mode%d", i);
        lvgl_window_report_geometry(name, mode_choices[i]);
    }
    lvgl_window_report_geometry("keep", keep_button);
    lvgl_window_report_geometry("volume", volume_slider);
    lvgl_window_report_geometry("motion", motion_switch);
}

static void build_user_interface(void) {
    lvgl_theme_apply(&settings_window);
    lv_obj_clean(lv_screen_active());

    lv_obj_t *page = lvgl_theme_page(lv_screen_active());
    build_appearance_card(page);
    build_display_card(page);
    build_sound_card(page);
    build_system_card(page);
    build_shortcut_card(page);
    if (confirm_until_ms != 0 && confirm_row) {
        lv_obj_remove_flag(confirm_row, LV_OBJ_FLAG_HIDDEN);
    }
    report_geometry();
}

static unsigned long count_drawn_pixels(void) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    const uint32_t *pixels = settings_window.window.graphics.pixels;
    unsigned long total = (unsigned long)settings_window.window.width * settings_window.window.height;
    unsigned long drawn = 0;
    for (unsigned long i = 0; i < total; i++) {
        if ((pixels[i] & 0x00FFFFFFu) != palette->window) {
            drawn++;
        }
    }
    return drawn;
}

int desktop_application_settings(int selftest) {
    if (lvgl_window_open(SETTINGS_WINDOW_WIDTH, SETTINGS_WINDOW_HEIGHT, "Settings",
                         &settings_window) != 0) {
        printf("[m127] settings: window open failed\n");
        return 1;
    }

    window_manager_settings_request_t settings;
    if (window_manager_query_settings(&settings) == 0) {
        current_background = settings.bg_color;
        current_accent = settings.accent_color;
        current_wallpaper = settings.wallpaper;
        current_animations = settings.animations;
        current_volume = settings.volume;
    }

    long n = sys_display_modes(modes, DISPLAY_MAX_MODES);
    mode_count = n < 0 ? 0 : (int)(n > DISPLAY_MAX_MODES ? DISPLAY_MAX_MODES : n);

    build_user_interface();
    lv_timer_create(on_tick, 500, NULL);

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
