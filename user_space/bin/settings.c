/* user_space/bin/settings.c
 *
 * M33: the missing "user-facing configuration surface" - previously zero.
 * Two real, live controls rather than a static info panel that just
 * echoes read-only state back: a desktop background color picker (wm_
 * set_theme, wmclient.h - system_api/include/wm.h's new
 * WM_SETTINGS_PIPE, the compositor's first global, non-per-window
 * setting) and a clipboard viewer/clear button (M32's SYS_clipboard_*).
 * Display resolution and uptime are shown too, read-only, as a
 * lightweight "system info" strip - genuinely live (SYS_fb_info/
 * SYS_uptime_ms), not hardcoded.
 *
 * M38 adds a second swatch row: the focused-window titlebar accent color
 * (compositor.c's second global setting, previously a compile-time
 * TITLEBAR_FOCUS_COLOR constant). Both colors are always sent together
 * (wm_set_theme takes both) - this process tracks its own current choice
 * of each in current_bg/current_accent rather than only ever sending
 * whichever one control just changed, so clicking one swatch row never
 * resets the other back to its default.
 *
 * M44 adds a third control, the wallpaper style (user_space/lib/
 * wallpaper.h), and one correctness fix that matters more than it looks:
 * this window now *queries* the compositor for the current settings at
 * startup (wm_query_settings) instead of assuming the three defaults. It
 * had been assuming since M33, which meant closing Settings and
 * reopening it showed the wrong swatch as selected, and - worse - the
 * next click would send those wrong assumed values back for the two
 * controls you hadn't touched.
 */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "shortcuts.h" /* system_api/include/shortcuts.h - M49: the Shortcuts pane is generated from the same table the compositor dispatches from */
#include "str.h"
#include "settings_file.h" /* M47: this window is where the three settings are chosen, so it is also where they get written down */
#include "syscall_wrappers.h"
#include "wallpaper.h"
#include "wmclient.h"

#define WIN_W 320
/* M49: 340 -> 520, for the Shortcuts pane along the bottom. Still well
 * inside what the compositor will place without clamping (a window is
 * kept clear of the taskbar since M45), and taller is the right answer
 * rather than a second tab: this list is reference material you read
 * once, not a control you return to. */
#define WIN_H 520

#define BG_COLOR      0x00202430u
#define TEXT_COLOR    0x00E0E0E0u
#define LABEL_COLOR   0x0090A0B0u
#define BORDER_COLOR  0x00404860u
#define BTN_COLOR     0x00445566u
#define BTN_HOVER     0x00607088u

#define SWATCH_SIZE 28
#define SWATCH_GAP  8
#define BG_SWATCH_Y     150
#define ACCENT_SWATCH_Y 210

/* M44: the wallpaper picker - one labeled button per style. Four of them
 * across WIN_W with the same 10px margin every other row here uses. */
#define WALL_BTN_Y  270
#define WALL_BTN_W  68 /* four of these plus their gaps land exactly inside the GFX_PAD margins */
#define WALL_BTN_H  22
#define WALL_BTN_GAP 6
#define WALL_BTN_X(i) (GFX_PAD + (i) * (WALL_BTN_W + WALL_BTN_GAP))

/* M49: the Shortcuts pane. Rows come straight out of SHORTCUTS[] - the
 * same table compositor.c's handle_keyboard dispatches from - so a chord
 * cannot exist without being listed here, and nothing can be listed here
 * that isn't wired up. That is the whole point: a shortcuts list is only
 * worth having if it is true. */
#define SHORTCUT_LABEL_Y 304
#define SHORTCUT_ROW_Y   326
#define SHORTCUT_ROW_H   18
#define SHORTCUT_DESC_X  (GFX_PAD + 124) /* clears the longest chord ("Ctrl+Shift+Esc", 14 glyphs) */

#define DEFAULT_BG_COLOR     0x001A1A2Eu /* mirrors compositor.c's own compile-time default - see this file's header comment */
#define DEFAULT_ACCENT_COLOR 0x004C99E6u

static const uint32_t BG_SWATCHES[] = {
    DEFAULT_BG_COLOR, /* the original default */
    0x00203040u,
    0x00301A1Au,
    0x001A3020u,
    0x00302A1Au,
    0x00101018u,
};
#define BG_SWATCH_COUNT ((int)(sizeof(BG_SWATCHES) / sizeof(BG_SWATCHES[0])))

static const uint32_t ACCENT_SWATCHES[] = {
    DEFAULT_ACCENT_COLOR, /* the original default */
    0x00E67E22u, /* orange */
    0x0027AE60u, /* green */
    0x009B59B6u, /* purple */
    0x00E74C3Cu, /* red */
    0x00F1C40Fu, /* yellow */
};
#define ACCENT_SWATCH_COUNT ((int)(sizeof(ACCENT_SWATCHES) / sizeof(ACCENT_SWATCHES[0])))

static uint32_t current_bg = DEFAULT_BG_COLOR;
static uint32_t current_accent = DEFAULT_ACCENT_COLOR;
static uint32_t current_wallpaper = WALLPAPER_GRADIENT; /* mirrors compositor.c's own default */

#define CLEAR_BTN_X (WIN_W - GFX_PAD - CLEAR_BTN_W) /* M44: right-aligned to the same inset every other row uses, rather than a hand-placed 220 */
#define CLEAR_BTN_Y 100
#define CLEAR_BTN_W 80
#define CLEAR_BTN_H 20

static int format_uint(uint32_t v, char *buf) {
    char tmp[10];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    int len = 0;
    for (int i = n - 1; i >= 0; i--) {
        buf[len++] = tmp[i];
    }
    buf[len] = '\0';
    return len;
}

/* M47: every control that changes a setting goes through here rather than
 * calling wm_set_theme directly - the live compositor and the file on
 * disk have to move together, and three call sites each remembering to do
 * both is three chances not to. */
static void apply_theme(void) {
    wm_settings_request_t settings;
    settings.bg_color = current_bg;
    settings.accent_color = current_accent;
    settings.wallpaper = current_wallpaper;
    wm_set_theme(settings.bg_color, settings.accent_color, settings.wallpaper);
    settings_file_save(&settings);
}

static void redraw(wm_window_t *win, int clear_hover, int clear_pressed) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);

    gfx_draw_text(&win->gfx, GFX_PAD, 10, "System", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, 26, WIN_W - GFX_PAD, 26, BORDER_COLOR);

    wm_fb_info_t fb_info;
    char line[64];
    if (sys_fb_info(&fb_info) == 0) {
        int i = 0;
        static const char label[] = "Display: ";
        for (int j = 0; label[j]; j++) {
            line[i++] = label[j];
        }
        i += format_uint(fb_info.width, line + i);
        line[i++] = 'x';
        i += format_uint(fb_info.height, line + i);
        line[i] = '\0';
        gfx_draw_text(&win->gfx, GFX_PAD, 36, line, TEXT_COLOR);
    }

    {
        int i = 0;
        static const char label[] = "Uptime: ";
        for (int j = 0; label[j]; j++) {
            line[i++] = label[j];
        }
        i += format_uint((uint32_t)(sys_uptime_ms() / 1000), line + i);
        line[i++] = 's';
        line[i] = '\0';
        gfx_draw_text(&win->gfx, GFX_PAD, 52, line, TEXT_COLOR);
    }

    gfx_draw_text(&win->gfx, GFX_PAD, 76, "Clipboard", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, 92, WIN_W - GFX_PAD, 92, BORDER_COLOR);

    char clip_buf[40];
    long clip_len = sys_clipboard_get(clip_buf, sizeof(clip_buf) - 1);
    if (clip_len <= 0) {
        static const char empty[] = "(empty)";
        memcpy(clip_buf, empty, sizeof(empty));
    } else {
        if (clip_len > (long)sizeof(clip_buf) - 1) {
            clip_len = (long)sizeof(clip_buf) - 1;
        }
        clip_buf[clip_len] = '\0';
    }
    gfx_draw_text(&win->gfx, GFX_PAD, 102, clip_buf, TEXT_COLOR);

    /* M46: hover *and* press, rather than a button that only ever reacted
     * once the click had already been acted on. */
    gfx_draw_button_state(&win->gfx, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H,
                           clear_hover ? BTN_HOVER : BTN_COLOR, BORDER_COLOR, "Clear", TEXT_COLOR,
                           clear_pressed);

    gfx_draw_text(&win->gfx, GFX_PAD, 130, "Desktop color", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, 146, WIN_W - GFX_PAD, 146, BORDER_COLOR);
    for (int i = 0; i < BG_SWATCH_COUNT; i++) {
        int32_t x = GFX_PAD + i * (SWATCH_SIZE + SWATCH_GAP);
        gfx_fill_rect_rounded(&win->gfx, x, BG_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE, BG_SWATCHES[i]);
        gfx_draw_rect_rounded(&win->gfx, x, BG_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE,
                              BG_SWATCHES[i] == current_bg ? TEXT_COLOR : BORDER_COLOR);
    }

    gfx_draw_text(&win->gfx, GFX_PAD, 190, "Accent color", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, 206, WIN_W - GFX_PAD, 206, BORDER_COLOR);
    for (int i = 0; i < ACCENT_SWATCH_COUNT; i++) {
        int32_t x = GFX_PAD + i * (SWATCH_SIZE + SWATCH_GAP);
        gfx_fill_rect_rounded(&win->gfx, x, ACCENT_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE, ACCENT_SWATCHES[i]);
        gfx_draw_rect_rounded(&win->gfx, x, ACCENT_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE,
                              ACCENT_SWATCHES[i] == current_accent ? TEXT_COLOR : BORDER_COLOR);
    }

    /* M44: the wallpaper row. Each button shows the style's own ramp
     * behind its name rather than a flat button color - a two-word label
     * ("Deep", "Grid") says much less about what you are picking than
     * eight pixels of the actual thing does, and wallpaper_fill draws a
     * 70x22 preview exactly the way it draws a 1024x768 desktop. */
    gfx_draw_text(&win->gfx, GFX_PAD, 250, "Wallpaper", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, 266, WIN_W - GFX_PAD, 266, BORDER_COLOR);
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        int32_t x = WALL_BTN_X(i);
        wallpaper_fill(&win->gfx, x, WALL_BTN_Y, WALL_BTN_W, WALL_BTN_H, i, current_bg);
        gfx_draw_rect_rounded(&win->gfx, x, WALL_BTN_Y, WALL_BTN_W, WALL_BTN_H,
                              (uint32_t)i == current_wallpaper ? TEXT_COLOR : BORDER_COLOR);
        const char *name = wallpaper_name(i);
        int32_t label_w = (int32_t)strlen(name) * FONT_WIDTH;
        gfx_draw_text(&win->gfx, x + (WALL_BTN_W - label_w) / 2,
                      WALL_BTN_Y + (WALL_BTN_H - FONT_HEIGHT) / 2, name, TEXT_COLOR);
    }

    gfx_draw_text(&win->gfx, GFX_PAD, SHORTCUT_LABEL_Y, "Shortcuts", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, SHORTCUT_LABEL_Y + 16, WIN_W - GFX_PAD, SHORTCUT_LABEL_Y + 16, BORDER_COLOR);
    for (int i = 0; i < SHORTCUT_COUNT; i++) {
        int32_t y = SHORTCUT_ROW_Y + i * SHORTCUT_ROW_H;
        gfx_draw_text(&win->gfx, GFX_PAD, y, SHORTCUTS[i].chord, TEXT_COLOR);
        gfx_draw_text(&win->gfx, SHORTCUT_DESC_X, y, SHORTCUTS[i].what, LABEL_COLOR);
    }
}

int main(void) {
    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Settings", &win) != 0) {
        sys_exit(1);
    }

    /* M44: open showing what is actually set, not what the defaults are -
     * see this file's header comment on what assuming cost. */
    {
        wm_settings_request_t settings;
        if (wm_query_settings(&settings) == 0) {
            current_bg = settings.bg_color;
            current_accent = settings.accent_color;
            current_wallpaper = settings.wallpaper;
        }
    }

    int clear_hover = 0;
    int clear_pressed = 0;
    long next_redraw = 0;
    redraw(&win, 0, 0);

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_MOUSE_MOVE) {
                int hover = gfx_point_in_rect(ev.x, ev.y, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H);
                if (hover != clear_hover) {
                    clear_hover = hover;
                    changed = 1;
                }
                /* M46: the button un-presses if the pointer leaves it
                 * while held, the way a real one does. */
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
                if (gfx_point_in_rect(ev.x, ev.y, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H)) {
                    clear_pressed = 1;
                    sys_clipboard_set("", 0);
                    changed = 1;
                }
                for (int i = 0; i < BG_SWATCH_COUNT; i++) {
                    int32_t x = GFX_PAD + i * (SWATCH_SIZE + SWATCH_GAP);
                    if (gfx_point_in_rect(ev.x, ev.y, x, BG_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE)) {
                        current_bg = BG_SWATCHES[i];
                        apply_theme();
                        changed = 1;
                        break;
                    }
                }
                for (int i = 0; i < ACCENT_SWATCH_COUNT; i++) {
                    int32_t x = GFX_PAD + i * (SWATCH_SIZE + SWATCH_GAP);
                    if (gfx_point_in_rect(ev.x, ev.y, x, ACCENT_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE)) {
                        current_accent = ACCENT_SWATCHES[i];
                        apply_theme();
                        changed = 1;
                        break;
                    }
                }
                for (int i = 0; i < WALLPAPER_COUNT; i++) {
                    if (gfx_point_in_rect(ev.x, ev.y, WALL_BTN_X(i), WALL_BTN_Y, WALL_BTN_W, WALL_BTN_H)) {
                        current_wallpaper = (uint32_t)i;
                        apply_theme();
                        changed = 1;
                        break;
                    }
                }
            }
        }

        long now = sys_uptime_ms();
        if (now >= next_redraw) {
            next_redraw = now + 500; /* uptime display is the only thing that changes with no input at all */
            changed = 1;
        }

        if (changed) {
            redraw(&win, clear_hover, clear_pressed);
        }
    }
}
