/* user_space/bin/gui_clock.c
 *
 * M21 demo GUI app #1: a window that updates itself with no input at
 * all, driven by sys_uptime_ms() instead - proves the compositor's
 * periodic (not just on-input) redraw actually reaches a client whose
 * content changes purely on a timer, and exercises user_space/lib/gfx.h
 * text rendering (font8x16.c) end to end through wmclient.h.
 */
#include "syscall_wrappers.h"
#include "wmclient.h"

#define WIN_W 200
#define WIN_H 90
#define BG_COLOR   0x00122438u
#define TEXT_COLOR 0x00FFFFFFu
#define REDRAW_INTERVAL_MS 250

/* No itoa in this project's tiny str.h (M11's hello.c hit the exact
 * same gap and formatted its pid by hand) - writes decimal digits into
 * buf and returns how many. */
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

    /* Static caption, drawn once: kernel_main's M21 self-test checks
     * specific pixels of this exact text (deterministic, unlike the
     * live uptime line below) to prove gfx_draw_text actually landed
     * glyph pixels, not just that it compiled. */
    gfx_fill_rect(&win.gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);
    gfx_draw_text(&win.gfx, 10, 10, "CLOCK", TEXT_COLOR);

    long next_redraw = 0;
    for (;;) {
        wm_event_t ev;
        int expose = 0;
        while (wm_poll_event(&win, &ev)) {
            /* This app doesn't act on input - just drain the pipe so it
             * never fills while the compositor keeps routing events to
             * whichever window is focused, including this one. */
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                expose = 1; /* M55 - see WM_EVENT_EXPOSE */
            }
        }

        long now = sys_uptime_ms();
        if (now < next_redraw && !expose) {
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
    }
}
