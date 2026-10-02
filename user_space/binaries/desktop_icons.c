#include "icon.h"
#include "icons.h"
#include "children.h"
#include "malloc.h"
#include "paths.h"
#define LABEL_H UI_FONT_UI_HEIGHT
#include "string_utilities.h"
#include "spawn_error.h"
#include "syscall_wrappers.h"
#include "wallpaper.h"
#include "wallpaper_picture.h"
#include "window_manager_client.h"

#include <stdio.h>

#define LABEL_COLOR        0x00FFFFFFu
#define LABEL_SHADOW_COLOR 0x00000000u
#define LABEL_SHADOW_ALPHA 150u
#define SELECTION_COLOR    0x00FFFFFFu
#define SELECTION_ALPHA    46u
#define SELECTION_EDGE_ALPHA 72u
#define SELECTION_RADIUS   10
#define SELECTION_PAD      6

#define ICON_SIZE    ICON_LARGE_SIZE
#define ICON_MARGIN  32
#define ICON_CELL_W  90
#define ICON_CELL_H  90
#define PANEL_MARGIN 40
#define DOUBLE_CLICK_MS 500

#define CONTEXT_MENU_W        216
#define CONTEXT_MENU_ITEM_H   28
#define CONTEXT_MENU_PAD      6
#define CONTEXT_MENU_SEPARATOR_H 9
#define CONTEXT_MENU_RADIUS   10
#define CONTEXT_MENU_BG       0x00222A36u
#define CONTEXT_MENU_BORDER   0x00FFFFFFu
#define CONTEXT_MENU_BORDER_ALPHA 34u
#define CONTEXT_MENU_TEXT     0x00F2F4F8u
#define CONTEXT_MENU_SHADOW_ALPHA 22u

typedef struct {
    const char *label;
    const char *program;
    const char *argument;
    int separator_before;
} context_menu_item_t;

/* M211. The desktop's own menu is where a desktop's settings are reached from
   on every other system - the wallpaper first, because the desktop is what
   was clicked. Each Settings entry opens Settings on its own pane. */
static const context_menu_item_t CONTEXT_MENU[] = {
    {"Change Wallpaper...", PATH_BIN_DIRECTORY "settings", "wallpaper", 0},
    {"Display Settings...", PATH_BIN_DIRECTORY "settings", "display", 0},
    {"Trackpad & Mouse...", PATH_BIN_DIRECTORY "settings", "trackpad", 0},
    {"New Terminal", PATH_BIN_DIRECTORY "gui_terminal", "", 1},
    {"Open Files", PATH_BIN_DIRECTORY "file_manager", "", 0},
    {"Open Settings", PATH_BIN_DIRECTORY "settings", "", 0},
};
#define CONTEXT_MENU_COUNT ((int)(sizeof(CONTEXT_MENU) / sizeof(CONTEXT_MENU[0])))

static int context_menu_open;
static int context_menu_hover = -1;
static int32_t context_menu_x, context_menu_y;

#define DEFAULT_BG_COLOR 0x001A1A2Eu
#define DEFAULT_ACCENT   0x004C99E6u
#define THEME_POLL_MS 500

static uint32_t theme_bg = DEFAULT_BG_COLOR;
static uint32_t theme_accent = DEFAULT_ACCENT;
static int theme_wallpaper = WALLPAPER_GRADIENT;
static os_stat_t picture_stamp;
static int picture_present;

static int same_stamp(const os_stat_t *a, const os_stat_t *b) {
    return a->size == b->size && a->mtime == b->mtime && a->inode == b->inode;
}

static int refresh_theme(void) {
    window_manager_settings_request_t settings;
    if (window_manager_query_settings(&settings) != 0) {
        return 0;
    }
    int changed = settings.bg_color != theme_bg || (int)settings.wallpaper != theme_wallpaper ||
                  settings.accent_color != theme_accent;
    theme_bg = settings.bg_color;
    theme_accent = settings.accent_color;
    theme_wallpaper = (int)settings.wallpaper;
    if (theme_wallpaper == WALLPAPER_PICTURE) {
        os_stat_t now;
        int present = sys_stat(PATH_WALLPAPER_PICTURE, &now) == 0;
        if (present != picture_present || (present && !same_stamp(&now, &picture_stamp))) {
            picture_present = present;
            picture_stamp = now;
            changed = 1;
        }
    }
    return changed;
}

static int32_t context_menu_height(void) {
    int32_t h = 2 * CONTEXT_MENU_PAD;
    for (int i = 0; i < CONTEXT_MENU_COUNT; i++) {
        h += CONTEXT_MENU_ITEM_H + (CONTEXT_MENU[i].separator_before ? CONTEXT_MENU_SEPARATOR_H : 0);
    }
    return h;
}

static int32_t context_menu_item_top(int index) {
    int32_t y = context_menu_y + CONTEXT_MENU_PAD;
    for (int i = 0; i < index; i++) {
        y += CONTEXT_MENU_ITEM_H + (CONTEXT_MENU[i + 1].separator_before ? CONTEXT_MENU_SEPARATOR_H : 0);
    }
    return y;
}

static int context_menu_hit(int32_t x, int32_t y) {
    if (x < context_menu_x + CONTEXT_MENU_PAD || x >= context_menu_x + CONTEXT_MENU_W - CONTEXT_MENU_PAD) {
        return -1;
    }
    for (int i = 0; i < CONTEXT_MENU_COUNT; i++) {
        int32_t top = context_menu_item_top(i);
        if (y >= top && y < top + CONTEXT_MENU_ITEM_H) {
            return i;
        }
    }
    return -1;
}

static void draw_context_menu(graphics_context_t *target) {
    int32_t h = context_menu_height();
    for (int32_t k = 4; k >= 1; k--) {
        graphics_fill_rounded(target, context_menu_x - k + 2, context_menu_y - k + 6, CONTEXT_MENU_W + 2 * k - 4,
                              h + 2 * k - 4, CONTEXT_MENU_RADIUS + k, 0, CONTEXT_MENU_SHADOW_ALPHA);
    }
    graphics_fill_rounded(target, context_menu_x, context_menu_y, CONTEXT_MENU_W, h, CONTEXT_MENU_RADIUS,
                          CONTEXT_MENU_BG, 255);
    graphics_stroke_rounded(target, context_menu_x, context_menu_y, CONTEXT_MENU_W, h, CONTEXT_MENU_RADIUS,
                            CONTEXT_MENU_BORDER, CONTEXT_MENU_BORDER_ALPHA);
    for (int i = 0; i < CONTEXT_MENU_COUNT; i++) {
        int32_t top = context_menu_item_top(i);
        if (CONTEXT_MENU[i].separator_before) {
            graphics_fill_rect(target, context_menu_x + 12, top - CONTEXT_MENU_SEPARATOR_H / 2 - 1,
                               CONTEXT_MENU_W - 24, 1, 0x003A4454u);
        }
        if (i == context_menu_hover) {
            graphics_fill_rounded(target, context_menu_x + CONTEXT_MENU_PAD, top,
                                  CONTEXT_MENU_W - 2 * CONTEXT_MENU_PAD, CONTEXT_MENU_ITEM_H, 6, theme_accent, 255);
        }
        int32_t text_y = top + (CONTEXT_MENU_ITEM_H - LABEL_H) / 2;
        graphics_draw_text(target, context_menu_x + 16, text_y, CONTEXT_MENU[i].label, CONTEXT_MENU_TEXT);
    }
}

static void launch_with(const char *label, const char *program, const char *arg) {
    long rc = sys_spawn(program, arg ? arg : "");
    if (rc < 0) {
        window_manager_notify(WINDOW_MANAGER_NOTIFY_ERROR, label, spawn_error_message(rc));
    }
    child_track(rc);
}

#define ICON_IMAGE_SCALE 1

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
    {"Browser",  PATH_BIN_DIRECTORY "browser",      "", ICON_BROWSER},
};
#define ICON_COUNT ((int)(sizeof(ICONS) / sizeof(ICONS[0])))

static int32_t icon_x[ICON_COUNT], icon_y[ICON_COUNT];

#define ICON_FILE_MAX (ICON_HEADER_BYTES + 4 * ICON_LARGE_SIZE * ICON_LARGE_SIZE)
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
        long n = -1;
        if (sys_stat(path, &st) == 0) {
            n = sys_readfile(path, icon_file_data[i], ICON_FILE_MAX);
        }
        int usable = (n >= ICON_HEADER_BYTES && n <= ICON_FILE_MAX &&
                      icon_valid(icon_file_data[i]) &&
                      icon_is_truecolor(icon_file_data[i]) &&
                      icon_bytes(icon_file_data[i]) <= (int)n);
        if (!usable) {
            sys_writefile(path, ICONS[i].image, (size_t)icon_bytes(ICONS[i].image));
            continue;
        }
        icon_image[i] = icon_file_data[i];
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

static void redraw_icon(graphics_context_t *target, int i, int pressed) {
    int32_t label_w = graphics_text_width(graphics_ui_font(), ICONS[i].label);
    int32_t label_x = icon_x[i] + ICON_SIZE / 2 - label_w / 2;
    int32_t label_y = icon_y[i] + ICON_SIZE + 4;

    if (pressed) {
        int32_t left = icon_x[i] - SELECTION_PAD;
        int32_t right = label_x + label_w + SELECTION_PAD;
        if (icon_x[i] + ICON_SIZE + SELECTION_PAD > right) {
            right = icon_x[i] + ICON_SIZE + SELECTION_PAD;
        }
        if (label_x - SELECTION_PAD < left) {
            left = label_x - SELECTION_PAD;
        }
        int32_t top = icon_y[i] - SELECTION_PAD / 2;
        int32_t bottom = label_y + LABEL_H + SELECTION_PAD / 2;
        graphics_fill_rounded(target, left, top, right - left, bottom - top,
                              SELECTION_RADIUS, SELECTION_COLOR, SELECTION_ALPHA);
        graphics_stroke_rounded(target, left, top, right - left, bottom - top,
                                SELECTION_RADIUS, SELECTION_COLOR, SELECTION_EDGE_ALPHA);
    }

    icon_draw(target, icon_x[i], icon_y[i], icon_image[i], ICON_IMAGE_SCALE);

    graphics_draw_text_shadowed(target, label_x, label_y, ICONS[i].label,
                                LABEL_COLOR, LABEL_SHADOW_COLOR, LABEL_SHADOW_ALPHA);
}

/* M200. The redraw painted the wallpaper over the whole window and then put
   the icons back, in the pixels the compositor composites from - so a
   composite that landed in between showed a desktop with no icons on it for
   a frame. Nothing noticed while the compositor drew twenty frames a second;
   at sixty, the input suite caught the icon column blinking out when an
   application opened. The desktop is drawn off to one side and copied over
   finished, which the fast memcpy makes about a millisecond. */
static uint32_t *offscreen;
static size_t offscreen_pixels;

/* M211. The wallpaper is painted once into a buffer of its own and copied
   under the icons on every redraw: the painted styles cost a few operations
   a pixel and a picture costs a file read and a scale, and neither should be
   paid when an icon is merely pressed. */
static uint32_t *wallpaper_cache;
static size_t wallpaper_cache_pixels;
static int wallpaper_cache_valid;

static void paint_wallpaper(graphics_context_t *target) {
    const char *shown = wallpaper_name(theme_wallpaper);
    int painted = 0;
    if (theme_wallpaper == WALLPAPER_PICTURE && picture_present) {
        wallpaper_picture_t picture;
        if (wallpaper_picture_load(PATH_WALLPAPER_PICTURE, &picture) == 0) {
            wallpaper_picture_cover(&picture, target, 0, 0, target->width, target->height);
            printf("[desktop] wallpaper picture %s, %ux%u\n", picture.name, (unsigned)picture.width,
                   (unsigned)picture.height);
            wallpaper_picture_free(&picture);
            painted = 1;
        } else {
            shown = "Gradient, because the picture could not be read";
        }
    }
    if (!painted) {
        wallpaper_fill(target, 0, 0, target->width, target->height, theme_wallpaper, theme_bg);
        printf("[desktop] wallpaper %s\n", shown);
    }
}

static void redraw(window_manager_window_t *self, int pressed_icon) {
    size_t pixels = (size_t)self->width * (size_t)self->height;
    if (pixels != offscreen_pixels) {
        free(offscreen);
        offscreen = (uint32_t *)malloc(pixels * sizeof(uint32_t));
        offscreen_pixels = offscreen ? pixels : 0;
    }
    if (pixels != wallpaper_cache_pixels) {
        free(wallpaper_cache);
        wallpaper_cache = (uint32_t *)malloc(pixels * sizeof(uint32_t));
        wallpaper_cache_pixels = wallpaper_cache ? pixels : 0;
        wallpaper_cache_valid = 0;
    }
    graphics_context_t back = {offscreen, (int32_t)self->width, (int32_t)self->height};
    graphics_context_t *target = offscreen ? &back : &self->graphics;
    if (wallpaper_cache) {
        if (!wallpaper_cache_valid) {
            graphics_context_t cache = {wallpaper_cache, (int32_t)self->width, (int32_t)self->height};
            paint_wallpaper(&cache);
            wallpaper_cache_valid = 1;
        }
        memcpy(target->pixels, wallpaper_cache, pixels * sizeof(uint32_t));
    } else {
        paint_wallpaper(target);
    }
    for (int i = 0; i < ICON_COUNT; i++) {
        redraw_icon(target, i, i == pressed_icon);
    }
    if (context_menu_open) {
        draw_context_menu(target);
    }
    if (offscreen) {
        memcpy(self->graphics.pixels, offscreen, pixels * sizeof(uint32_t));
    }
}

static void open_context_menu(const window_manager_window_t *win, int32_t x, int32_t y) {
    context_menu_open = 1;
    context_menu_hover = -1;
    context_menu_x = x;
    context_menu_y = y;
    int32_t max_x = (int32_t)win->width - CONTEXT_MENU_W - 4;
    int32_t max_y = (int32_t)win->height - context_menu_height() - PANEL_MARGIN - 4;
    if (context_menu_x > max_x) {
        context_menu_x = max_x;
    }
    if (context_menu_y > max_y) {
        context_menu_y = max_y;
    }
    if (context_menu_x < 0) {
        context_menu_x = 0;
    }
    if (context_menu_y < 0) {
        context_menu_y = 0;
    }
    printf("[geometry] desktop_menu %d %d %d %d\n", (int)context_menu_x, (int)context_menu_y, CONTEXT_MENU_W,
           (int)context_menu_height());
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
                if (ev.type == WINDOW_MANAGER_EVENT_DISPLAY_CHANGED) {
                    wallpaper_cache_valid = 0;
                }
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
                open_context_menu(&win, ev.x, ev.y);
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_MOVE && context_menu_open) {
                int hover = context_menu_hit(ev.x, ev.y);
                if (hover != context_menu_hover) {
                    context_menu_hover = hover;
                    changed = 1;
                }
            } else if (ev.type == WINDOW_MANAGER_EVENT_KEY && context_menu_open && ev.ch == 27) {
                context_menu_open = 0;
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && (ev.buttons & 1) && context_menu_open) {
                int idx = context_menu_hit(ev.x, ev.y);
                context_menu_open = 0;
                if (idx >= 0) {
                    launch_with(CONTEXT_MENU[idx].label, CONTEXT_MENU[idx].program, CONTEXT_MENU[idx].argument);
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
                wallpaper_cache_valid = 0;
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
