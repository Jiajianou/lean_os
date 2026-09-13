#include "icon.h"
#include "icons.h"
#include "children.h"
#include "paths.h"
#define LABEL_H UI_FONT_UI_HEIGHT
#include "string_utilities.h"
#include "spawn_error.h"
#include "syscall_wrappers.h"
#include "wallpaper.h"
#include "window_manager_client.h"

#define ICON_BOX_COLOR 0x004C99E6u
#define ICON_HOVER_COLOR 0x006CB9FFu
#define LABEL_COLOR    0x00FFFFFFu

#define ICON_SIZE    48
#define ICON_MARGIN  32
#define ICON_CELL_W  90
#define ICON_CELL_H  90
#define PANEL_MARGIN 40
#define DOUBLE_CLICK_MS 500

#define CONTEXT_MENU_ITEM_W 120
#define CONTEXT_MENU_ITEM_H (LABEL_H + 4)
#define CONTEXT_MENU_BG     0x00243040u
#define CONTEXT_MENU_HOVER  0x003A5A80u
#define CONTEXT_MENU_BORDER 0x00506070u
#define CONTEXT_MENU_TEXT   0x00FFFFFFu

static const char *const CONTEXT_MENU_ITEMS[] = {"Terminal", "Settings"};
static const char *const CONTEXT_MENU_PROGRAMS[] = {PATH_BIN_DIRECTORY "gui_terminal", PATH_BIN_DIRECTORY "settings"};
#define CONTEXT_MENU_COUNT ((int)(sizeof(CONTEXT_MENU_ITEMS) / sizeof(CONTEXT_MENU_ITEMS[0])))

static int context_menu_open;
static int32_t context_menu_x, context_menu_y;

#define DEFAULT_BG_COLOR 0x001A1A2Eu
#define THEME_POLL_MS 500

static uint32_t theme_bg = DEFAULT_BG_COLOR;
static int theme_wallpaper = WALLPAPER_GRADIENT;

static int refresh_theme(void) {
    window_manager_settings_request_t settings;
    if (window_manager_query_settings(&settings) != 0) {
        return 0;
    }
    if (settings.bg_color == theme_bg && (int)settings.wallpaper == theme_wallpaper) {
        return 0;
    }
    theme_bg = settings.bg_color;
    theme_wallpaper = (int)settings.wallpaper;
    return 1;
}

static void launch_with(const char *label, const char *program, const char *arg) {
    long rc = sys_spawn(program, arg ? arg : "");
    if (rc < 0) {
        window_manager_notify(WINDOW_MANAGER_NOTIFY_ERROR, label, spawn_error_message(rc));
    }
    child_track(rc);
}

static void launch(const char *label, const char *program) {
    launch_with(label, program, "");
}

#define ICON_IMAGE_SCALE 2

typedef struct {
    const char *label;
    const char *program;
    const char *arg;
    const uint8_t *image;
} icon_def_t;

static const icon_def_t ICONS[] = {
    {"Terminal", PATH_BIN_DIRECTORY "gui_terminal", "", ICON_TERMINAL},
    {"Editor",   PATH_BIN_DIRECTORY "text_editor",  "", ICON_EDITOR},
    {"Files",    PATH_BIN_DIRECTORY "file_manager", "", ICON_FILES},
    {"Settings", PATH_BIN_DIRECTORY "settings",     "", ICON_SETTINGS},
    {"Clock",    PATH_BIN_DIRECTORY "gui_clock",    "", ICON_CLOCK},
    {"Paint",    PATH_BIN_DIRECTORY "gui_paint",    "", ICON_PAINT},
    {"Tasks",    PATH_BIN_DIRECTORY "task_manager", "", ICON_TASKS},
    {"README",   PATH_BIN_DIRECTORY "text_editor",  PATH_HOME_DIRECTORY "readme.txt", ICON_EDITOR},
    {"Browser",  PATH_BIN_DIRECTORY "netsurf",      "", ICON_BROWSER},
};
#define ICON_COUNT ((int)(sizeof(ICONS) / sizeof(ICONS[0])))

static int32_t icon_x[ICON_COUNT], icon_y[ICON_COUNT];

#define ICON_FILE_MAX 512
static uint8_t icon_file_data[ICON_COUNT][ICON_FILE_MAX];
static const uint8_t *icon_image[ICON_COUNT];

static int icon_path_for(int i, char *out) {
    int n = 0;
    for (const char *s = PATH_ICONS_DIRECTORY; *s; s++) {
        out[n++] = *s;
    }
    for (const char *s = ICONS[i].label; *s; s++) {
        if (n >= PATH_MAX_LENGTH - 5) {
            return -1;
        }
        out[n++] = *s;
    }
    static const char ext[] = ".icn";
    for (int k = 0; ext[k]; k++) {
        out[n++] = ext[k];
    }
    out[n] = '\0';
    return 0;
}

static void load_icons(void) {
    os_stat_t st;
    if (sys_stat(PATH_ICONS, &st) != 0) {
        sys_mkdir(PATH_ICONS);
    }
    for (int i = 0; i < ICON_COUNT; i++) {
        icon_image[i] = ICONS[i].image;
        char path[PATH_MAX_LENGTH];
        if (icon_path_for(i, path) != 0) {
            continue;
        }
        if (sys_stat(path, &st) != 0) {
            sys_writefile(path, ICONS[i].image, (size_t)icon_bytes(ICONS[i].image));
        }
        long n = sys_readfile(path, icon_file_data[i], ICON_FILE_MAX);
        if (n >= ICON_HEADER_BYTES && n <= ICON_FILE_MAX &&
            icon_valid(icon_file_data[i]) &&
            icon_bytes(icon_file_data[i]) <= (int)n) {
            icon_image[i] = icon_file_data[i];
        }
    }
}

static void layout_icons(int32_t win_h) {
    int32_t x = ICON_MARGIN, y = ICON_MARGIN;
    int32_t max_y = win_h - PANEL_MARGIN - ICON_SIZE - LABEL_H - 4;
    for (int i = 0; i < ICON_COUNT; i++) {
        if (y > max_y && y > ICON_MARGIN) {
            y = ICON_MARGIN;
            x += ICON_CELL_W;
        }
        icon_x[i] = x;
        icon_y[i] = y;
        y += ICON_CELL_H;
    }
}

static int point_in_icon(int i, int32_t x, int32_t y) {
    return graphics_point_in_rect(x, y, icon_x[i], icon_y[i] - LABEL_H - 4,
                              ICON_SIZE, ICON_SIZE + 2 * (LABEL_H + 4));
}

static void redraw_icon(window_manager_window_t *self, int i, int pressed) {
    uint32_t box_color = pressed ? ICON_HOVER_COLOR : ICON_BOX_COLOR;
    graphics_fill_rect_rounded(&self->graphics, icon_x[i], icon_y[i], ICON_SIZE, ICON_SIZE, box_color);

    {
        const uint8_t *blob = icon_image[i];
        int32_t drawn = 24 * ICON_IMAGE_SCALE;
        icon_draw(&self->graphics, icon_x[i] + (ICON_SIZE - drawn) / 2,
                   icon_y[i] + (ICON_SIZE - drawn) / 2, blob, ICON_IMAGE_SCALE);
    }

    int32_t label_w = graphics_text_width(graphics_ui_font(), ICONS[i].label);
    int32_t label_x = icon_x[i] + ICON_SIZE / 2 - label_w / 2;
    graphics_draw_text(&self->graphics, label_x, icon_y[i] + ICON_SIZE + 4, ICONS[i].label, LABEL_COLOR);
}

static void redraw(window_manager_window_t *self, int pressed_icon) {
    wallpaper_fill(&self->graphics, 0, 0, (int32_t)self->width, (int32_t)self->height,
                    theme_wallpaper, theme_bg);
    for (int i = 0; i < ICON_COUNT; i++) {
        redraw_icon(self, i, i == pressed_icon);
    }
    if (context_menu_open) {
        graphics_draw_menu(&self->graphics, context_menu_x, context_menu_y, CONTEXT_MENU_ITEM_W, CONTEXT_MENU_ITEM_H,
                      CONTEXT_MENU_ITEMS, CONTEXT_MENU_COUNT, -1,
                      CONTEXT_MENU_BG, CONTEXT_MENU_HOVER, CONTEXT_MENU_BORDER, CONTEXT_MENU_TEXT);
    }
}

int main(void) {
    load_icons();

    window_manager_window_t win;
    if (window_manager_connect_desktop(&win) != 0) {
        const char message[] = "desktop_icons: no window from the compositor - exiting so init restarts the session\n";
        sys_write(1, message, sizeof(message) - 1);
        sys_exit(1);
    }

    layout_icons((int32_t)win.height);
    refresh_theme();

    long next_theme_poll = 0;
    int last_click_icon = -1;
    long last_click_ms = -1;
    long pressed_until_ms = 0;
    int pressed_icon = -1;

    redraw(&win, -1);
    window_manager_present(&win);

    for (;;) {
        child_reap();
        window_manager_event_t ev;
        int changed = 0;
        while (window_manager_poll_event(&win, &ev)) {
            if (ev.type == WINDOW_MANAGER_EVENT_EXPOSE || ev.type == WINDOW_MANAGER_EVENT_DISPLAY_CHANGED) {
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_DROP) {
                char dropped[WINDOW_MANAGER_DRAG_PAYLOAD_MAX];
                if (window_manager_drag_payload(dropped, sizeof(dropped)) == 0 && dropped[0]) {
                    long rc = sys_spawn(PATH_BIN_DIRECTORY "text_editor", dropped);
                    if (rc < 0) {
                        window_manager_notify(WINDOW_MANAGER_NOTIFY_ERROR, dropped, spawn_error_message(rc));
                    }
                    child_track(rc);
                }
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && (ev.buttons & 2)) {
                context_menu_open = 1;
                context_menu_x = ev.x;
                context_menu_y = ev.y;
                int32_t max_x = (int32_t)win.width - CONTEXT_MENU_ITEM_W;
                int32_t max_y = (int32_t)win.height - CONTEXT_MENU_ITEM_H * CONTEXT_MENU_COUNT;
                if (context_menu_x > max_x) {
                    context_menu_x = max_x;
                }
                if (context_menu_y > max_y) {
                    context_menu_y = max_y;
                }
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && (ev.buttons & 1) && context_menu_open) {
                int idx = graphics_menu_hit_test(ev.x, ev.y, context_menu_x, context_menu_y,
                                             CONTEXT_MENU_ITEM_W, CONTEXT_MENU_ITEM_H, CONTEXT_MENU_COUNT);
                context_menu_open = 0;
                if (idx >= 0) {
                    launch(CONTEXT_MENU_ITEMS[idx], CONTEXT_MENU_PROGRAMS[idx]);
                }
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                for (int i = 0; i < ICON_COUNT; i++) {
                    if (!point_in_icon(i, ev.x, ev.y)) {
                        continue;
                    }
                    long now = (long)ev.time_ms;
                    pressed_until_ms = sys_uptime_ms() + 150;
                    pressed_icon = i;
                    if (last_click_icon == i && last_click_ms >= 0 && now - last_click_ms <= DOUBLE_CLICK_MS) {
                        launch_with(ICONS[i].label, ICONS[i].program, ICONS[i].arg);
                        last_click_icon = -1;
                        last_click_ms = -1;
                    } else {
                        last_click_icon = i;
                        last_click_ms = now;
                    }
                    changed = 1;
                    break;
                }
            }
        }

        long now = sys_uptime_ms();
        if (now >= next_theme_poll) {
            next_theme_poll = now + THEME_POLL_MS;
            if (refresh_theme()) {
                changed = 1;
            }
        }
        int pressed = now < pressed_until_ms ? pressed_icon : -1;
        static int was_pressed_icon = -1;
        if (changed || pressed != was_pressed_icon) {
            redraw(&win, pressed);
            was_pressed_icon = pressed;
            window_manager_present(&win);
        }
        window_manager_wait_ms(&win, NULL, 0, pressed >= 0 ? 50 : (int)(next_theme_poll - now));
    }
}
