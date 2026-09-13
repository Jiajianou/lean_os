#include "shortcuts.h"
#include "string_utilities.h"
#include "settings_file.h"
#include "syscall_wrappers.h"
#include "wallpaper.h"
#include "window_manager_client.h"

#define WIN_W 320
#define WIN_H 680

#define BG_COLOR      0x00202430u
#define TEXT_COLOR    0x00E0E0E0u
#define LABEL_COLOR   0x0090A0B0u
#define MARK_COLOR     0x00FFFFFFu
#define BORDER_COLOR  0x00404860u
#define BTN_COLOR     0x00445566u
#define BTN_HOVER     0x00607088u

#define SWATCH_SIZE 28
#define SWATCH_GAP  8
#define BG_SWATCH_Y     150
#define ACCENT_SWATCH_Y 204

#define WALL_BTN_Y  258
#define WALL_BTN_W  68
#define WALL_BTN_H  22
#define WALL_BTN_GAP 6
#define WALL_BTN_X(i) (GFX_PAD + (i) * (WALL_BTN_W + WALL_BTN_GAP))

#define VOL_STEPS   5
#define VOL_BTN_W   22
#define VOL_BTN_H   20
#define VOL_BTN_GAP 4
#define VOL_X(i)    (GFX_PAD + 58 + (i) * (VOL_BTN_W + VOL_BTN_GAP))
#define VOL_Y       288
#define VOL_LABEL_Y 290

#define MOTION_LABEL_Y 290
#define MOTION_BTN_W   50
#define MOTION_BTN_H   20
#define MOTION_BTN_X   (WIN_W - GFX_PAD - MOTION_BTN_W)

#define MODE_LABEL_Y  316
#define MODE_BTN_Y    340
#define MODE_BTN_W    92
#define MODE_BTN_H    20
#define MODE_BTN_GAP  6
#define MODE_COLS     3
#define MODE_BTN_X(i) (GFX_PAD + ((i) % MODE_COLS) * (MODE_BTN_W + MODE_BTN_GAP))
#define MODE_ROW_Y(i) (MODE_BTN_Y + ((i) / MODE_COLS) * (MODE_BTN_H + 4))
#define MODE_ROWS     3
#define CONFIRM_Y     (MODE_BTN_Y + MODE_ROWS * (MODE_BTN_H + 4) + 6)
#define CONFIRM_H     20
#define KEEP_BTN_W    64
#define KEEP_BTN_X    (WIN_W - GFX_PAD - KEEP_BTN_W)

#define SHORTCUT_LABEL_Y 452
#define SHORTCUT_ROW_Y   474
#define SHORTCUT_ROW_H   15
#define SHORTCUT_FONT    ui_font_small
#define SHORTCUT_DESC_X  (GFX_PAD + 124)

#define DEFAULT_BG_COLOR     0x001A1A2Eu
#define DEFAULT_ACCENT_COLOR 0x004C99E6u

static const uint32_t BG_SWATCHES[] = {
    DEFAULT_BG_COLOR,
    0x00203040u,
    0x00301A1Au,
    0x001A3020u,
    0x00302A1Au,
    0x00101018u,
};
#define BG_SWATCH_COUNT ((int)(sizeof(BG_SWATCHES) / sizeof(BG_SWATCHES[0])))

static const uint32_t ACCENT_SWATCHES[] = {
    DEFAULT_ACCENT_COLOR,
    0x00E67E22u,
    0x0027AE60u,
    0x009B59B6u,
    0x00E74C3Cu,
    0x00F1C40Fu,
};
#define ACCENT_SWATCH_COUNT ((int)(sizeof(ACCENT_SWATCHES) / sizeof(ACCENT_SWATCHES[0])))

static display_mode_t modes[DISPLAY_MAX_MODES];
static int mode_count;

static long confirm_until_ms;

static uint32_t current_bg = DEFAULT_BG_COLOR;
static uint32_t current_accent = DEFAULT_ACCENT_COLOR;
static uint32_t current_wallpaper = WALLPAPER_GRADIENT;
static uint32_t current_animations = 1;
static uint32_t current_volume = 70;

static int vol_step_percent(int i) {
    return i == 0 ? 0 : 100 * i / (VOL_STEPS - 1);
}

#define CLEAR_BTN_X (WIN_W - GFX_PAD - CLEAR_BTN_W)
#define CLEAR_BTN_Y 100
#define CLEAR_BTN_W 80
#define CLEAR_BTN_H 20

static int format_uint(uint32_t v, char *buf) {
    char temporary[10];
    int n = 0;
    if (v == 0) {
        temporary[n++] = '0';
    }
    while (v > 0) {
        temporary[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    int len = 0;
    for (int i = n - 1; i >= 0; i--) {
        buf[len++] = temporary[i];
    }
    buf[len] = '\0';
    return len;
}

static void apply_theme(void) {
    wm_settings_request_t settings;
    settings.volume = current_volume;
    settings.animations = current_animations;
    settings.bg_color = current_bg;
    settings.accent_color = current_accent;
    settings.wallpaper = current_wallpaper;
    window_manager_set_settings(&settings);
    settings_file_save(&settings);
}

static void draw_selected_mark(window_manager_window_t *win, int32_t x, int32_t y, int selected) {
    if (!selected) {
        return;
    }
    const ui_font_t *f = graphics_ui_font();
    int32_t gw = graphics_char_advance(f, UI_G_CHECK);
    graphics_draw_char_font(&win->graphics, x + (SWATCH_SIZE - gw) / 2,
                       y + (SWATCH_SIZE - (int32_t)f->height) / 2,
                       UI_G_CHECK, MARK_COLOR, f, 1);
}

static void redraw(window_manager_window_t *win, int clear_hover, int clear_pressed) {
    graphics_fill_rect(&win->graphics, 0, 0, WIN_W, WIN_H, BG_COLOR);

    graphics_draw_text(&win->graphics, GFX_PAD, 10, "System", LABEL_COLOR);
    graphics_draw_line(&win->graphics, GFX_PAD, 26, WIN_W - GFX_PAD, 26, BORDER_COLOR);

    wm_fb_info_t framebuffer_info;
    uint32_t framebuffer_w = 0, framebuffer_h = 0;
    char line[64];
    if (sys_framebuffer_info(&framebuffer_info) == 0) {
        framebuffer_w = framebuffer_info.width;
        framebuffer_h = framebuffer_info.height;
        int i = 0;
        static const char label[] = "Display: ";
        for (int j = 0; label[j]; j++) {
            line[i++] = label[j];
        }
        i += format_uint(framebuffer_info.width, line + i);
        line[i++] = 'x';
        i += format_uint(framebuffer_info.height, line + i);
        line[i] = '\0';
        graphics_draw_text(&win->graphics, GFX_PAD, 36, line, TEXT_COLOR);
    }

    {
        os_datetime_t t;
        int i = 0;
        if (sys_time(&t) > 0 && t.valid) {
            i += format_uint(t.year, line + i);
            line[i++] = '-';
            line[i++] = (char)('0' + t.month / 10);
            line[i++] = (char)('0' + t.month % 10);
            line[i++] = '-';
            line[i++] = (char)('0' + t.day / 10);
            line[i++] = (char)('0' + t.day % 10);
            line[i++] = ' ';
            line[i++] = (char)('0' + t.hour / 10);
            line[i++] = (char)('0' + t.hour % 10);
            line[i++] = ':';
            line[i++] = (char)('0' + t.minute / 10);
            line[i++] = (char)('0' + t.minute % 10);
            line[i++] = ' ';
            line[i++] = 'U';
            line[i++] = 'T';
            line[i++] = 'C';
        } else {
            static const char label[] = "Uptime: ";
            for (int j = 0; label[j]; j++) {
                line[i++] = label[j];
            }
            i += format_uint((uint32_t)(sys_uptime_ms() / 1000), line + i);
            line[i++] = 's';
        }
        line[i] = '\0';
        graphics_draw_text(&win->graphics, GFX_PAD, 52, line, TEXT_COLOR);
    }

    graphics_draw_text(&win->graphics, GFX_PAD, 76, "Clipboard", LABEL_COLOR);
    graphics_draw_line(&win->graphics, GFX_PAD, 92, WIN_W - GFX_PAD, 92, BORDER_COLOR);

    char clip_buffer[40];
    long clip_length = sys_clipboard_get(clip_buffer, sizeof(clip_buffer) - 1);
    if (clip_length <= 0) {
        static const char empty[] = "(empty)";
        memcpy(clip_buffer, empty, sizeof(empty));
    } else {
        if (clip_length > (long)sizeof(clip_buffer) - 1) {
            clip_length = (long)sizeof(clip_buffer) - 1;
        }
        clip_buffer[clip_length] = '\0';
    }
    graphics_draw_text(&win->graphics, GFX_PAD, 102, clip_buffer, TEXT_COLOR);

    graphics_draw_button_state(&win->graphics, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H,
                           clear_hover ? BTN_HOVER : BTN_COLOR, BORDER_COLOR, "Clear", TEXT_COLOR,
                           clear_pressed);

    graphics_draw_text(&win->graphics, GFX_PAD, 130, "Desktop color", LABEL_COLOR);
    graphics_draw_line(&win->graphics, GFX_PAD, 146, WIN_W - GFX_PAD, 146, BORDER_COLOR);
    for (int i = 0; i < BG_SWATCH_COUNT; i++) {
        int32_t x = GFX_PAD + i * (SWATCH_SIZE + SWATCH_GAP);
        graphics_fill_rect_rounded(&win->graphics, x, BG_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE, BG_SWATCHES[i]);
        graphics_draw_rect_rounded(&win->graphics, x, BG_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE,
                              BG_SWATCHES[i] == current_bg ? TEXT_COLOR : BORDER_COLOR);
        draw_selected_mark(win, x, BG_SWATCH_Y, BG_SWATCHES[i] == current_bg);
    }

    graphics_draw_text(&win->graphics, GFX_PAD, 184, "Accent color", LABEL_COLOR);
    graphics_draw_line(&win->graphics, GFX_PAD, 200, WIN_W - GFX_PAD, 200, BORDER_COLOR);
    for (int i = 0; i < ACCENT_SWATCH_COUNT; i++) {
        int32_t x = GFX_PAD + i * (SWATCH_SIZE + SWATCH_GAP);
        graphics_fill_rect_rounded(&win->graphics, x, ACCENT_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE, ACCENT_SWATCHES[i]);
        graphics_draw_rect_rounded(&win->graphics, x, ACCENT_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE,
                              ACCENT_SWATCHES[i] == current_accent ? TEXT_COLOR : BORDER_COLOR);
        draw_selected_mark(win, x, ACCENT_SWATCH_Y, ACCENT_SWATCHES[i] == current_accent);
    }

    graphics_draw_text(&win->graphics, GFX_PAD, 238, "Wallpaper", LABEL_COLOR);
    graphics_draw_line(&win->graphics, GFX_PAD, 254, WIN_W - GFX_PAD, 254, BORDER_COLOR);
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        int32_t x = WALL_BTN_X(i);
        wallpaper_fill(&win->graphics, x, WALL_BTN_Y, WALL_BTN_W, WALL_BTN_H, i, current_bg);
        graphics_draw_rect_rounded(&win->graphics, x, WALL_BTN_Y, WALL_BTN_W, WALL_BTN_H,
                              (uint32_t)i == current_wallpaper ? TEXT_COLOR : BORDER_COLOR);
        const char *name = wallpaper_name(i);
        int32_t label_w = graphics_text_width(graphics_ui_font(), name);
        graphics_draw_text(&win->graphics, x + (WALL_BTN_W - label_w) / 2,
                      WALL_BTN_Y + (WALL_BTN_H - (int32_t)graphics_ui_font()->height) / 2, name, TEXT_COLOR);
    }

    graphics_draw_text(&win->graphics, GFX_PAD, MODE_LABEL_Y, "Resolution", LABEL_COLOR);
    graphics_draw_line(&win->graphics, GFX_PAD, MODE_LABEL_Y + 16, WIN_W - GFX_PAD, MODE_LABEL_Y + 16, BORDER_COLOR);
    if (mode_count == 0) {
        graphics_draw_text(&win->graphics, GFX_PAD, MODE_BTN_Y,
                       "This display cannot be", LABEL_COLOR);
        graphics_draw_text(&win->graphics, GFX_PAD, MODE_BTN_Y + 16,
                       "resized after boot.", LABEL_COLOR);
    } else {
        for (int i = 0; i < mode_count; i++) {
            char label[16];
            int n = format_uint(modes[i].width, label);
            label[n++] = 'x';
            n += format_uint(modes[i].height, label + n);
            label[n] = '\0';
            int current = (framebuffer_w == modes[i].width && framebuffer_h == modes[i].height);
            graphics_draw_button(&win->graphics, MODE_BTN_X(i), MODE_ROW_Y(i), MODE_BTN_W, MODE_BTN_H,
                             current ? BTN_HOVER : BTN_COLOR,
                             current ? TEXT_COLOR : BORDER_COLOR,
                             label, TEXT_COLOR);
        }
        if (confirm_until_ms != 0) {
            long left_ms = confirm_until_ms - sys_uptime_ms();
            if (left_ms < 0) {
                left_ms = 0;
            }
            char line[40];
            int i = 0;
            static const char ask[] = "Keep this size? ";
            for (int j = 0; ask[j]; j++) {
                line[i++] = ask[j];
            }
            i += format_uint((uint32_t)(left_ms / 1000) + 1, line + i);
            line[i++] = 's';
            line[i] = '\0';
            graphics_draw_text(&win->graphics, GFX_PAD, CONFIRM_Y + 2, line, TEXT_COLOR);
            graphics_draw_button(&win->graphics, KEEP_BTN_X, CONFIRM_Y, KEEP_BTN_W, CONFIRM_H,
                             BTN_COLOR, BORDER_COLOR, "Keep", TEXT_COLOR);
        }
    }

    graphics_draw_text(&win->graphics, GFX_PAD, VOL_LABEL_Y, "Volume", LABEL_COLOR);
    for (int i = 0; i < VOL_STEPS; i++) {
        int filled = (int)current_volume >= vol_step_percent(i) && current_volume > 0;
        if (i == 0) {
            filled = current_volume == 0;
        }
        graphics_fill_rect_rounded(&win->graphics, VOL_X(i), VOL_Y, VOL_BTN_W, VOL_BTN_H,
                               filled ? BTN_HOVER : BTN_COLOR);
        graphics_draw_rect_rounded(&win->graphics, VOL_X(i), VOL_Y, VOL_BTN_W, VOL_BTN_H, BORDER_COLOR);
        if (i == 0) {
            graphics_draw_char_font(&win->graphics, VOL_X(i) + 7, VOL_Y + 2, UI_G_CLOSE,
                               TEXT_COLOR, graphics_ui_font(), 0);
        }
    }

    graphics_draw_text(&win->graphics, MOTION_BTN_X - 52, MOTION_LABEL_Y, "Motion", LABEL_COLOR);
    graphics_draw_button(&win->graphics, MOTION_BTN_X, MOTION_LABEL_Y - 2, MOTION_BTN_W, MOTION_BTN_H,
                     current_animations ? BTN_HOVER : BTN_COLOR, BORDER_COLOR,
                     current_animations ? "On" : "Off", TEXT_COLOR);

    graphics_draw_text(&win->graphics, GFX_PAD, SHORTCUT_LABEL_Y, "Shortcuts", LABEL_COLOR);
    graphics_draw_line(&win->graphics, GFX_PAD, SHORTCUT_LABEL_Y + 16, WIN_W - GFX_PAD, SHORTCUT_LABEL_Y + 16, BORDER_COLOR);
    for (int i = 0; i < SHORTCUT_COUNT; i++) {
        int32_t y = SHORTCUT_ROW_Y + i * SHORTCUT_ROW_H;
        graphics_draw_text_font(&win->graphics, GFX_PAD, y, SHORTCUTS[i].chord, TEXT_COLOR, &SHORTCUT_FONT, 0);
        graphics_draw_text_font(&win->graphics, SHORTCUT_DESC_X, y, SHORTCUTS[i].what, LABEL_COLOR, &SHORTCUT_FONT, 0);
    }
}

int main(void) {
    window_manager_window_t win;
    if (window_manager_connect(WIN_W, WIN_H, "Settings", &win) != 0) {
        sys_exit(1);
    }

    {
        wm_settings_request_t settings;
        if (window_manager_query_settings(&settings) == 0) {
            current_bg = settings.bg_color;
            current_accent = settings.accent_color;
            current_wallpaper = settings.wallpaper;
            current_animations = settings.animations;
            current_volume = settings.volume;
        }
    }

    {
        long n = sys_display_modes(modes, DISPLAY_MAX_MODES);
        mode_count = n < 0 ? 0 : (int)(n > DISPLAY_MAX_MODES ? DISPLAY_MAX_MODES : n);
    }

    int clear_hover = 0;
    int clear_pressed = 0;
    long next_redraw = 0;
    redraw(&win, 0, 0);

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (window_manager_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_MOVE) {
                int hover = graphics_point_in_rect(ev.x, ev.y, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H);
                if (hover != clear_hover) {
                    clear_hover = hover;
                    changed = 1;
                }
                if (clear_pressed && !((ev.buttons & 1) && hover)) {
                    clear_pressed = 0;
                    changed = 1;
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && !(ev.buttons & 1)) {
                if (clear_pressed) {
                    clear_pressed = 0;
                    changed = 1;
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                if (graphics_point_in_rect(ev.x, ev.y, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H)) {
                    clear_pressed = 1;
                    sys_clipboard_set("", 0);
                    changed = 1;
                }
                for (int i = 0; i < BG_SWATCH_COUNT; i++) {
                    int32_t x = GFX_PAD + i * (SWATCH_SIZE + SWATCH_GAP);
                    if (graphics_point_in_rect(ev.x, ev.y, x, BG_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE)) {
                        current_bg = BG_SWATCHES[i];
                        apply_theme();
                        changed = 1;
                        break;
                    }
                }
                for (int i = 0; i < ACCENT_SWATCH_COUNT; i++) {
                    int32_t x = GFX_PAD + i * (SWATCH_SIZE + SWATCH_GAP);
                    if (graphics_point_in_rect(ev.x, ev.y, x, ACCENT_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE)) {
                        current_accent = ACCENT_SWATCHES[i];
                        apply_theme();
                        changed = 1;
                        break;
                    }
                }
                for (int i = 0; i < WALLPAPER_COUNT; i++) {
                    if (graphics_point_in_rect(ev.x, ev.y, WALL_BTN_X(i), WALL_BTN_Y, WALL_BTN_W, WALL_BTN_H)) {
                        current_wallpaper = (uint32_t)i;
                        apply_theme();
                        changed = 1;
                        break;
                    }
                }
                for (int i = 0; i < VOL_STEPS; i++) {
                    if (graphics_point_in_rect(ev.x, ev.y, VOL_X(i), VOL_Y, VOL_BTN_W, VOL_BTN_H)) {
                        current_volume = (uint32_t)vol_step_percent(i);
                        apply_theme();
                        changed = 1;
                        break;
                    }
                }
                if (graphics_point_in_rect(ev.x, ev.y, MOTION_BTN_X, MOTION_LABEL_Y - 2,
                                       MOTION_BTN_W, MOTION_BTN_H)) {
                    current_animations = !current_animations;
                    apply_theme();
                    changed = 1;
                }
                if (confirm_until_ms != 0 &&
                    graphics_point_in_rect(ev.x, ev.y, KEEP_BTN_X, CONFIRM_Y, KEEP_BTN_W, CONFIRM_H)) {
                    window_manager_confirm_display_mode();
                    confirm_until_ms = 0;
                    wm_fb_info_t now_framebuffer;
                    if (sys_framebuffer_info(&now_framebuffer) == 0) {
                        settings_file_save_display(now_framebuffer.width, now_framebuffer.height);
                    }
                    changed = 1;
                } else {
                    for (int i = 0; i < mode_count; i++) {
                        if (graphics_point_in_rect(ev.x, ev.y, MODE_BTN_X(i), MODE_ROW_Y(i), MODE_BTN_W, MODE_BTN_H)) {
                            window_manager_set_display_mode(modes[i].width, modes[i].height);
                            confirm_until_ms = sys_uptime_ms() + WM_MODE_REVERT_MS;
                            changed = 1;
                            break;
                        }
                    }
                }
            }
        }

        long now = sys_uptime_ms();
        if (confirm_until_ms != 0 && now >= confirm_until_ms) {
            confirm_until_ms = 0;
            changed = 1;
        }
        if (now >= next_redraw) {
            next_redraw = now + 500;
            changed = 1;
        }

        if (changed) {
            redraw(&win, clear_hover, clear_pressed);
            window_manager_present(&win);
        }
        window_manager_wait_ms(&win, NULL, 0, confirm_until_ms != 0 ? 50 : (int)(next_redraw - now));
    }
}
