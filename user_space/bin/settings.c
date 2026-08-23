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
 */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "str.h"
#include "syscall_wrappers.h"
#include "wmclient.h"

#define WIN_W 320
#define WIN_H 280

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

#define CLEAR_BTN_X 220
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

static void redraw(wm_window_t *win, int clear_hover) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);

    gfx_draw_text(&win->gfx, 10, 10, "System", LABEL_COLOR);
    gfx_draw_line(&win->gfx, 10, 26, WIN_W - 10, 26, BORDER_COLOR);

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
        gfx_draw_text(&win->gfx, 10, 36, line, TEXT_COLOR);
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
        gfx_draw_text(&win->gfx, 10, 52, line, TEXT_COLOR);
    }

    gfx_draw_text(&win->gfx, 10, 76, "Clipboard", LABEL_COLOR);
    gfx_draw_line(&win->gfx, 10, 92, WIN_W - 10, 92, BORDER_COLOR);

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
    gfx_draw_text(&win->gfx, 10, 102, clip_buf, TEXT_COLOR);

    gfx_draw_button(&win->gfx, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H,
                     clear_hover ? BTN_HOVER : BTN_COLOR, BORDER_COLOR, "Clear", TEXT_COLOR);

    gfx_draw_text(&win->gfx, 10, 130, "Desktop color", LABEL_COLOR);
    gfx_draw_line(&win->gfx, 10, 146, WIN_W - 10, 146, BORDER_COLOR);
    for (int i = 0; i < BG_SWATCH_COUNT; i++) {
        int32_t x = 10 + i * (SWATCH_SIZE + SWATCH_GAP);
        gfx_fill_rect(&win->gfx, x, BG_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE, BG_SWATCHES[i]);
        gfx_draw_rect(&win->gfx, x, BG_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE,
                      BG_SWATCHES[i] == current_bg ? TEXT_COLOR : BORDER_COLOR);
    }

    gfx_draw_text(&win->gfx, 10, 190, "Accent color", LABEL_COLOR);
    gfx_draw_line(&win->gfx, 10, 206, WIN_W - 10, 206, BORDER_COLOR);
    for (int i = 0; i < ACCENT_SWATCH_COUNT; i++) {
        int32_t x = 10 + i * (SWATCH_SIZE + SWATCH_GAP);
        gfx_fill_rect(&win->gfx, x, ACCENT_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE, ACCENT_SWATCHES[i]);
        gfx_draw_rect(&win->gfx, x, ACCENT_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE,
                      ACCENT_SWATCHES[i] == current_accent ? TEXT_COLOR : BORDER_COLOR);
    }
}

int main(void) {
    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Settings", &win) != 0) {
        sys_exit(1);
    }

    int clear_hover = 0;
    long next_redraw = 0;
    redraw(&win, 0);

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
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                if (gfx_point_in_rect(ev.x, ev.y, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H)) {
                    sys_clipboard_set("", 0);
                    changed = 1;
                }
                for (int i = 0; i < BG_SWATCH_COUNT; i++) {
                    int32_t x = 10 + i * (SWATCH_SIZE + SWATCH_GAP);
                    if (gfx_point_in_rect(ev.x, ev.y, x, BG_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE)) {
                        current_bg = BG_SWATCHES[i];
                        wm_set_theme(current_bg, current_accent);
                        changed = 1;
                        break;
                    }
                }
                for (int i = 0; i < ACCENT_SWATCH_COUNT; i++) {
                    int32_t x = 10 + i * (SWATCH_SIZE + SWATCH_GAP);
                    if (gfx_point_in_rect(ev.x, ev.y, x, ACCENT_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE)) {
                        current_accent = ACCENT_SWATCHES[i];
                        wm_set_theme(current_bg, current_accent);
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
            redraw(&win, clear_hover);
        }
    }
}
