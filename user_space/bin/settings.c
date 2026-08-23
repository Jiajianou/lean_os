/* user_space/bin/settings.c
 *
 * M33: the missing "user-facing configuration surface" - previously zero.
 * Two real, live controls rather than a static info panel that just
 * echoes read-only state back: a desktop background color picker (wm_
 * set_bg_color, wmclient.h - system_api/include/wm.h's new
 * WM_SETTINGS_PIPE, the compositor's first global, non-per-window
 * setting) and a clipboard viewer/clear button (M32's SYS_clipboard_*).
 * Display resolution and uptime are shown too, read-only, as a
 * lightweight "system info" strip - genuinely live (SYS_fb_info/
 * SYS_uptime_ms), not hardcoded.
 */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "str.h"
#include "syscall_wrappers.h"
#include "wmclient.h"

#define WIN_W 320
#define WIN_H 220

#define BG_COLOR      0x00202430u
#define TEXT_COLOR    0x00E0E0E0u
#define LABEL_COLOR   0x0090A0B0u
#define BORDER_COLOR  0x00404860u
#define BTN_COLOR     0x00445566u
#define BTN_HOVER     0x00607088u

#define SWATCH_SIZE 28
#define SWATCH_GAP  8
#define SWATCH_Y    150

static const uint32_t SWATCHES[] = {
    0x001A1A2Eu, /* the original default */
    0x00203040u,
    0x00301A1Au,
    0x001A3020u,
    0x00302A1Au,
    0x00101018u,
};
#define SWATCH_COUNT ((int)(sizeof(SWATCHES) / sizeof(SWATCHES[0])))

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

static int point_in_rect(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h) {
    return px >= x && px < x + w && py >= y && py < y + h;
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

    gfx_fill_rect(&win->gfx, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H, clear_hover ? BTN_HOVER : BTN_COLOR);
    gfx_draw_text(&win->gfx, CLEAR_BTN_X + 12, CLEAR_BTN_Y + 2, "Clear", TEXT_COLOR);

    gfx_draw_text(&win->gfx, 10, 130, "Desktop color", LABEL_COLOR);
    gfx_draw_line(&win->gfx, 10, 146, WIN_W - 10, 146, BORDER_COLOR);
    for (int i = 0; i < SWATCH_COUNT; i++) {
        int32_t x = 10 + i * (SWATCH_SIZE + SWATCH_GAP);
        gfx_fill_rect(&win->gfx, x, SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE, SWATCHES[i]);
        gfx_draw_rect(&win->gfx, x, SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE, BORDER_COLOR);
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
                int hover = point_in_rect(ev.x, ev.y, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H);
                if (hover != clear_hover) {
                    clear_hover = hover;
                    changed = 1;
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                if (point_in_rect(ev.x, ev.y, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H)) {
                    sys_clipboard_set("", 0);
                    changed = 1;
                }
                for (int i = 0; i < SWATCH_COUNT; i++) {
                    int32_t x = 10 + i * (SWATCH_SIZE + SWATCH_GAP);
                    if (point_in_rect(ev.x, ev.y, x, SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE)) {
                        wm_set_bg_color(SWATCHES[i]);
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
