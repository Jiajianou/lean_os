/* user_space/bin/console.c
 *
 * M70: the kernel log, on the screen, live.
 *
 * The milestone this closes was opened by M65, which wrote "a rule nobody
 * can see is a rule nobody can check" about capabilities and then logged
 * every denial to a serial port. It was true of far more than
 * capabilities: for seventy milestones every driver message, every spawn
 * failure, every filesystem complaint and the panic handler's last words
 * all went to COM1 and to a framebuffer console the compositor paints
 * over three seconds into boot. On any machine without a serial cable
 * attached - which is every real machine - this OS could not tell you
 * anything about itself.
 *
 * This program is the third destination. It is the only one on the
 * machine holding CAP_SYSLOG (system_api/include/caps.h), because the log
 * describes what every other process is doing and that is authority
 * rather than a fact about the hardware.
 *
 * Deliberately simple, and each simplification is a decision:
 *
 *  - it FOLLOWS rather than pages. The question this answers is "what
 *    just happened", and a viewer that started at the top of a 64 KiB
 *    ring would answer "what happened during boot" instead. Scrollback is
 *    the ring's job and the ring already has it; a cursor is all this
 *    needs to walk backwards later, and nothing has asked yet.
 *  - it re-reads from a cursor, so it never re-prints a line and never
 *    misses one. If the cursor the kernel hands back jumps, this fell
 *    behind the ring and says so, rather than showing a seamless stream
 *    with a hole in it.
 *  - it wraps long lines rather than truncating. A driver message with an
 *    address on the end is exactly the line whose end matters.
 */
#include <string.h>

#include "gfx.h"
#include "syscall_wrappers.h"
#include "uifont.h"
#include "wmclient.h"

#define WIN_W 640
#define WIN_H 400

#define BG_COLOR    0x00101014u
#define TEXT_COLOR  0x00C8D0C8u
#define DIM_COLOR   0x00708070u
#define HEADER_BG   0x00202830u
#define HEADER_TXT  0x0090A0C0u

#define HEADER_H 20
#define LINE_H   (UI_FONT_SMALL_HEIGHT + 2)
#define TEXT_X   6
#define ROWS     ((WIN_H - HEADER_H - 4) / LINE_H)
#define COLS     ((WIN_W - TEXT_X * 2) / 6) /* small font is ~6px wide */
#define MAX_COL  200

/* A ring of rendered lines. Bounded by construction: this holds what fits
 * on screen plus a little, not the whole log - the kernel's ring is the
 * log, and duplicating it here would be a second copy to keep in step. */
static char lines[ROWS][MAX_COL + 1];
static int line_len[ROWS];
static int line_count;   /* how many of `lines` are in use */
static int line_head;    /* oldest line's index */

static void push_line(const char *src, int len) {
    if (len > MAX_COL) {
        len = MAX_COL;
    }
    int slot = (line_head + line_count) % ROWS;
    if (line_count == ROWS) {
        slot = line_head;
        line_head = (line_head + 1) % ROWS;
    } else {
        line_count++;
    }
    for (int i = 0; i < len; i++) {
        lines[slot][i] = src[i];
    }
    lines[slot][len] = '\0';
    line_len[slot] = len;
}

/* Accumulates bytes into lines, breaking on '\n' and wrapping at COLS. */
static char pending[MAX_COL + 1];
static int pending_len;

static void feed(const char *buf, long n) {
    for (long i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            push_line(pending, pending_len);
            pending_len = 0;
            continue;
        }
        if (c == '\r' || c == '\t') {
            c = ' ';
        }
        if (c < 0x20 || c > 0x7E) {
            continue; /* the log is ASCII; anything else is a driver being odd */
        }
        pending[pending_len++] = c;
        if (pending_len >= COLS || pending_len >= MAX_COL) {
            push_line(pending, pending_len);
            pending_len = 0;
        }
    }
}

/* No strstr in this project's str.h, and one substring test does not
 * justify adding it to a library every program links. */
static int contains(const char *hay, const char *needle) {
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) {
            j++;
        }
        if (!needle[j]) {
            return 1;
        }
    }
    return 0;
}

static void redraw(wm_window_t *win, int fell_behind) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, HEADER_H, HEADER_BG);
    gfx_draw_text_font(&win->gfx, TEXT_X, 3,
                        fell_behind ? "kernel log  (following - some output was dropped)"
                                    : "kernel log  (following)",
                        HEADER_TXT, &ui_font_small, 0);

    int y = HEADER_H + 2;
    for (int i = 0; i < line_count; i++) {
        int slot = (line_head + i) % ROWS;
        /* A denial is the line most worth finding on this screen, so it
         * is the one line that is not the same colour as the rest. */
        uint32_t colour = TEXT_COLOR;
        if (contains(lines[slot], "refused") || contains(lines[slot], "PANIC")) {
            colour = 0x00E09090u;
        } else if (line_len[slot] == 0) {
            colour = DIM_COLOR;
        }
        gfx_draw_text_font(&win->gfx, TEXT_X, y, lines[slot], colour, &ui_font_small, 0);
        y += LINE_H;
    }
}

int main(void) {
    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Console", &win) != 0) {
        return 1;
    }

    /* Start at the beginning of whatever the ring still holds, so the
     * window opens with the boot log already in it rather than empty and
     * waiting for something to go wrong. */
    uint64_t cursor = 0;
    uint64_t expected = 0;
    int fell_behind = 0;
    int dirty = 1;

    static char buf[2048];
    for (;;) {
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                dirty = 1;
            }
        }

        uint64_t next = cursor;
        long n = sys_klog(cursor, buf, sizeof(buf), &next);
        if (n < 0) {
            /* No CAP_SYSLOG. Say so on the window rather than showing an
             * empty one - a viewer that is silently unable to read is the
             * confusing failure M65's own denial logging exists to stop. */
            gfx_fill_rect(&win.gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);
            gfx_draw_text_font(&win.gfx, TEXT_X, HEADER_H,
                                "refused: this program does not hold the 'syslog' capability",
                                0x00E09090u, &ui_font_small, 0);
            return 1;
        }
        if (n > 0) {
            if (expected != 0 && cursor > expected) {
                fell_behind = 1; /* the kernel advanced us past bytes we never saw */
            }
            feed(buf, n);
            cursor = next;
            expected = next;
            dirty = 1;
        }

        if (dirty) {
            redraw(&win, fell_behind);
            dirty = 0;
            wm_present(&win);
        }
        /* M117: the kernel log has no descriptor to wait on, so this is
         * a poll - at 10 Hz rather than as fast as the scheduler would
         * hand the CPU back, which is what sys_yield here amounted to. */
        wm_wait_ms(&win, NULL, 0, 100);
    }
}
