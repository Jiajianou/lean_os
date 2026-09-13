#include "syscall_wrappers.h"
#include "wmclient.h"

#define WIN_W 300
#define WIN_H 200

#define TICK_MAX    4
#define TICK_SIZE   10
#define TICK_GAP    4
#define TICK_INSET  2
#define TICK_COLOR  0x00F0E000u

static int32_t tick_x(int i) {
    return WIN_W - TICK_INSET - TICK_SIZE - (int32_t)i * (TICK_SIZE + TICK_GAP);
}

int main(int argc, char **argv) {
    const char *arg = argc > 1 ? argv[1] : "";
    char title[WM_TITLE_MAX] = "ZOrder";
    uint32_t fill = 0x00206040u;

    const char *p = arg;
    while (p && *p == ' ') {
        p++;
    }
    if (p && *p) {
        int i = 0;
        for (; *p && *p != ' ' && i < WM_TITLE_MAX - 1; p++, i++) {
            title[i] = *p;
        }
        title[i] = '\0';
        while (*p && *p != ' ') {
            p++;
        }
        while (*p == ' ') {
            p++;
        }
        uint32_t parsed = 0;
        int digits = 0;
        for (; *p; p++) {
            int v;
            if (*p >= '0' && *p <= '9') {
                v = *p - '0';
            } else if (*p >= 'a' && *p <= 'f') {
                v = *p - 'a' + 10;
            } else if (*p >= 'A' && *p <= 'F') {
                v = *p - 'A' + 10;
            } else {
                break;
            }
            parsed = (parsed << 4) | (uint32_t)v;
            digits++;
        }
        if (digits > 0) {
            fill = parsed;
        }
    }

    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, title, &win) != 0) {
        sys_exit(1);
    }

    int ticks = 0;
    gfx_fill_rect(&win.gfx, 0, 0, (int32_t)win.width, (int32_t)win.height, fill);
    wm_present(&win);

    for (;;) {
        wm_event_t ev;
        int drew = 0;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                drew = 1;
                gfx_fill_rect(&win.gfx, 0, 0, (int32_t)win.width, (int32_t)win.height, fill);
                for (int t = 0; t < ticks; t++) {
                    gfx_fill_rect(&win.gfx, tick_x(t), WIN_H - TICK_INSET - TICK_SIZE,
                                   TICK_SIZE, TICK_SIZE, TICK_COLOR);
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1) && ticks < TICK_MAX) {
                drew = 1;
                gfx_fill_rect(&win.gfx, tick_x(ticks), WIN_H - TICK_INSET - TICK_SIZE,
                               TICK_SIZE, TICK_SIZE, TICK_COLOR);
                ticks++;
            }
        }
        if (drew) {
            wm_present(&win);
        }
        wm_wait_ms(&win, NULL, 0, -1);
    }
}
