#include "syscall_wrappers.h"
#include "wmclient.h"

#define WIN_W 200
#define WIN_H 90
#define BG_COLOR   0x00122438u
#define TEXT_COLOR 0x00FFFFFFu
#define REDRAW_INTERVAL_MS 250

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

int main(void) {
    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Clock", &win) != 0) {
        sys_exit(1);
    }

    gfx_fill_rect(&win.gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);
    gfx_draw_text(&win.gfx, 10, 10, "CLOCK", TEXT_COLOR);

    long next_redraw = 0;
    for (;;) {
        wm_event_t ev;
        int expose = 0;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                expose = 1;
            }
        }

        long now = sys_uptime_ms();
        if (now < next_redraw && !expose) {
            wm_wait_ms(&win, NULL, 0, (int)(next_redraw - now));
            continue;
        }
        next_redraw = now + REDRAW_INTERVAL_MS;

        gfx_fill_rect(&win.gfx, 10, 34, WIN_W - 20, UI_FONT_UI_HEIGHT, BG_COLOR);

        char line[32];
        int li = 0;
        static const char label[] = "uptime: ";
        for (int i = 0; label[i]; i++) {
            line[li++] = label[i];
        }
        char num[10];
        int nlen = format_uint((uint32_t)(now / 1000), num);
        for (int i = 0; i < nlen; i++) {
            line[li++] = num[i];
        }
        line[li++] = 's';
        line[li] = '\0';

        gfx_draw_text(&win.gfx, 10, 34, line, TEXT_COLOR);
        wm_present(&win);
    }
}
