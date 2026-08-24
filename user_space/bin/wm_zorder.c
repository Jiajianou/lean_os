/* user_space/bin/wm_zorder.c
 *
 * M51: a GUI client that exists only so the [m51] boot self-test can ask
 * "which window got that click?" and read the answer off the screen -
 * the same self-test-only role wm_demo.c has held since M20 and
 * wm_stubborn.c since M45.
 *
 * Two things make it different from every other client this project
 * ships, and both are needed to state the z-order bug as a test:
 *
 *  - it fills its whole window with one flat color, passed on argv, so
 *    reading a single pixel in the region where two of these overlap says
 *    which of the two is in front. Paint order used to be connection
 *    order, so that pixel could never change; with a real z-order,
 *    clicking the window behind must change it.
 *
 *  - it draws a row of click "ticks" in its own bottom-right corner, one
 *    lit per left-button press it has been routed. The bottom-right
 *    corner is the one part of the *lower* of two cascaded windows that
 *    the upper one never covers, so both windows' tick rows stay readable
 *    whichever is in front - which is what lets the test assert that one
 *    window received a click *and that the other did not*.
 *
 * Presses only, not releases: the compositor sends a WM_EVENT_MOUSE_BUTTON
 * on both edges, and "two ticks per click" would just be a number to
 * remember rather than one to reason about.
 */
#include "syscall_wrappers.h"
#include "wmclient.h"

/* Big enough that two of these, cascaded 40px apart by the compositor's
 * own placement, overlap over a large, easily-probed area. */
#define WIN_W 300
#define WIN_H 200

/* One 10x10 cell per press, laid out right-to-left from the window's own
 * bottom-right corner. TICK_MAX is what the [m51] test needs plus room to
 * notice an extra one arriving. */
#define TICK_MAX    4
#define TICK_SIZE   10
#define TICK_GAP    4
#define TICK_INSET  2
#define TICK_COLOR  0x00F0E000u /* saturated yellow - nothing else this desktop draws is near it, same reasoning as wm_stubborn's FILL_COLOR */

static int32_t tick_x(int i) {
    return WIN_W - TICK_INSET - TICK_SIZE - (int32_t)i * (TICK_SIZE + TICK_GAP);
}

/* This project's "argv" is one NUL-terminated string, not an argc/argv
 * array (see kernel/proc/proc.c's own note) - so the two things this
 * client needs are a title and a fill color, space-separated:
 * "zA 00A02020". Both are optional and defaulted, so it is still
 * runnable by hand from the shell. */
int main(const char *arg) {
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
            p++; /* skip a title longer than WM_TITLE_MAX rather than reading its tail as a color */
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

    for (;;) {
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            /* Presses only - see the header comment. A press is a button
             * event whose button state has the left bit set; the matching
             * release arrives with it clear. */
            if (ev.type == WM_EVENT_EXPOSE) {
                /* M55: a replacement compositor handed this client a
                 * blank buffer. Repainting the fill *and* every tick
                 * already earned is what makes this program usable as a
                 * "did the window really come back with its own pixels"
                 * probe rather than just a "is something there" one. */
                gfx_fill_rect(&win.gfx, 0, 0, (int32_t)win.width, (int32_t)win.height, fill);
                for (int t = 0; t < ticks; t++) {
                    gfx_fill_rect(&win.gfx, tick_x(t), WIN_H - TICK_INSET - TICK_SIZE,
                                   TICK_SIZE, TICK_SIZE, TICK_COLOR);
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1) && ticks < TICK_MAX) {
                gfx_fill_rect(&win.gfx, tick_x(ticks), WIN_H - TICK_INSET - TICK_SIZE,
                               TICK_SIZE, TICK_SIZE, TICK_COLOR);
                ticks++;
            }
        }
        sys_yield();
    }
}
