#include <string.h>

#include "graphics.h"
#include "syscall_wrappers.h"
#include "user_interface_font.h"
#include "window_manager_client.h"

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
#define COLS     ((WIN_W - TEXT_X * 2) / 6)
#define MAX_COL  200

static char lines[ROWS][MAX_COL + 1];
static int line_length[ROWS];
static int line_count;
static int line_head;

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
    line_length[slot] = len;
}

static char pending[MAX_COL + 1];
static int pending_length;

static void feed(const char *buf, long n) {
    for (long i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            push_line(pending, pending_length);
            pending_length = 0;
            continue;
        }
        if (c == '\r' || c == '\t') {
            c = ' ';
        }
        if (c < 0x20 || c > 0x7E) {
            continue;
        }
        pending[pending_length++] = c;
        if (pending_length >= COLS || pending_length >= MAX_COL) {
            push_line(pending, pending_length);
            pending_length = 0;
        }
    }
}

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

static void redraw(window_manager_window_t *win, int fell_behind) {
    graphics_fill_rect(&win->graphics, 0, 0, WIN_W, WIN_H, BG_COLOR);
    graphics_fill_rect(&win->graphics, 0, 0, WIN_W, HEADER_H, HEADER_BG);
    graphics_draw_text_font(&win->graphics, TEXT_X, 3,
                        fell_behind ? "kernel log  (following - some output was dropped)"
                                    : "kernel log  (following)",
                        HEADER_TXT, &ui_font_small, 0);

    int y = HEADER_H + 2;
    for (int i = 0; i < line_count; i++) {
        int slot = (line_head + i) % ROWS;
        uint32_t colour = TEXT_COLOR;
        if (contains(lines[slot], "refused") || contains(lines[slot], "PANIC")) {
            colour = 0x00E09090u;
        } else if (line_length[slot] == 0) {
            colour = DIM_COLOR;
        }
        graphics_draw_text_font(&win->graphics, TEXT_X, y, lines[slot], colour, &ui_font_small, 0);
        y += LINE_H;
    }
}

int main(void) {
    window_manager_window_t win;
    if (window_manager_connect(WIN_W, WIN_H, "Console", &win) != 0) {
        return 1;
    }

    uint64_t cursor = 0;
    uint64_t expected = 0;
    int fell_behind = 0;
    int dirty = 1;

    static char buf[2048];
    for (;;) {
        wm_event_t ev;
        while (window_manager_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                dirty = 1;
            }
        }

        uint64_t next = cursor;
        long n = sys_klog(cursor, buf, sizeof(buf), &next);
        if (n < 0) {
            graphics_fill_rect(&win.graphics, 0, 0, WIN_W, WIN_H, BG_COLOR);
            graphics_draw_text_font(&win.graphics, TEXT_X, HEADER_H,
                                "refused: this program does not hold the 'syslog' capability",
                                0x00E09090u, &ui_font_small, 0);
            return 1;
        }
        if (n > 0) {
            if (expected != 0 && cursor > expected) {
                fell_behind = 1;
            }
            feed(buf, n);
            cursor = next;
            expected = next;
            dirty = 1;
        }

        if (dirty) {
            redraw(&win, fell_behind);
            dirty = 0;
            window_manager_present(&win);
        }
        window_manager_wait_ms(&win, NULL, 0, 100);
    }
}
