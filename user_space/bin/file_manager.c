/* user_space/bin/file_manager.c
 *
 * M33: the missing "browse the filesystem" GUI piece - until now the
 * only way was `ls` inside a terminal. leanfs (kernel/fs/leanfs.h) is
 * flat (no directories), so this is genuinely the entire namespace in
 * one list, not a tree - SYS_listfiles already returns exactly that.
 *
 * Selection works two ways, both ending at the same place
 * (open_selected): Up/Down arrow keys move the selection and Enter opens
 * it, or a mouse double-click (same DOUBLE_CLICK_MS pattern desktop_
 * icons.c already uses) on a row does the same thing. "Open" always
 * means "load it in text_editor.c" (M33's other new app) - this project
 * has no per-file type metadata (leanfs inodes don't record one) to make
 * a smarter guess from, and every file this OS ships or a user is likely
 * to create here is plain text anyway.
 *
 * Refreshes its listing every REFRESH_MS so a file saved from a
 * concurrently open text_editor.c shows up without needing to relaunch.
 */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "str.h"
#include "syscall_wrappers.h"
#include "wmclient.h"

#define WIN_W 280
#define WIN_H 360
#define ROW_H (FONT_HEIGHT + 4)
#define HEADER_H 24
#define SCROLLBAR_W 8 /* M37: reserved strip along the right edge - see redraw()'s gfx_draw_scrollbar call */
#define LIST_W (WIN_W - SCROLLBAR_W)
#define ROWS_VISIBLE ((WIN_H - HEADER_H) / ROW_H)

#define BG_COLOR       0x001C1C24u
#define HEADER_COLOR   0x00303850u
#define TEXT_COLOR     0x00D8D8D8u
#define SELECT_COLOR   0x004C6699u
#define LABEL_COLOR    0x0090A0C0u
#define SCROLLBAR_TRACK 0x00141820u
#define SCROLLBAR_THUMB 0x00506080u

#define MAX_FILES    48
#define MAX_NAME_LEN 32 /* leanfs's real cap (LEANFS_MAX_NAME, kernel/fs/leanfs.h) is 27 + a NUL - this just needs to be at least that, kept as its own constant since that header isn't visible to user_space builds */
#define LIST_BUF_SIZE 2048
#define DOUBLE_CLICK_MS 500

static char names[MAX_FILES][MAX_NAME_LEN];
static int file_count;
static int selected = -1;
static int scroll_top;

static void refresh_list(void) {
    static char buf[LIST_BUF_SIZE];
    long n = sys_listfiles(buf, sizeof(buf));
    file_count = 0;
    if (n <= 0) {
        return;
    }
    if (n > (long)sizeof(buf)) {
        n = (long)sizeof(buf);
    }
    int col = 0;
    for (long i = 0; i < n && file_count < MAX_FILES; i++) {
        char c = buf[i];
        if (c == '\n') {
            names[file_count][col] = '\0';
            file_count++;
            col = 0;
        } else if (col < MAX_NAME_LEN - 1) {
            names[file_count][col++] = c;
        }
    }
    if (selected >= file_count) {
        selected = file_count - 1;
    }
}

static void clamp_scroll(void) {
    if (selected < 0) {
        return;
    }
    if (selected < scroll_top) {
        scroll_top = selected;
    }
    if (selected >= scroll_top + ROWS_VISIBLE) {
        scroll_top = selected - ROWS_VISIBLE + 1;
    }
}

static void open_selected(void) {
    if (selected < 0 || selected >= file_count) {
        return;
    }
    sys_spawn("text_editor", names[selected]);
}

static void redraw(wm_window_t *win) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);
    gfx_fill_rect(&win->gfx, 0, 0, LIST_W, HEADER_H, HEADER_COLOR);
    gfx_draw_text(&win->gfx, 6, 4, "Files", LABEL_COLOR);

    for (int row = 0; row < ROWS_VISIBLE; row++) {
        int i = scroll_top + row;
        if (i >= file_count) {
            break;
        }
        int32_t y = HEADER_H + row * ROW_H;
        if (i == selected) {
            gfx_fill_rect(&win->gfx, 0, y, LIST_W, ROW_H, SELECT_COLOR);
        }
        gfx_draw_text(&win->gfx, 6, y + 2, names[i], TEXT_COLOR);
    }

    /* M37: the on-screen position/extent indicator this list previously
     * had none of - it already scrolled (Up/Down, or clicking a row near
     * an edge), there was just no visual cue there was more above/below. */
    gfx_draw_scrollbar(&win->gfx, LIST_W, HEADER_H, SCROLLBAR_W, WIN_H - HEADER_H,
                        file_count, ROWS_VISIBLE, scroll_top,
                        SCROLLBAR_TRACK, SCROLLBAR_THUMB);
}

int main(void) {
    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Files", &win) != 0) {
        sys_exit(1);
    }

    refresh_list();
    redraw(&win);

    long last_click_ms = -1;
    int last_click_row = -1;
    long next_refresh = sys_uptime_ms() + 1000;
    static const long REFRESH_MS = 1000;

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_KEY) {
                if (ev.ch == KBD_KEY_UP && selected > 0) {
                    selected--;
                    clamp_scroll();
                    changed = 1;
                } else if (ev.ch == KBD_KEY_DOWN && selected + 1 < file_count) {
                    selected++;
                    clamp_scroll();
                    changed = 1;
                } else if (ev.ch == '\n' || ev.ch == '\r') {
                    open_selected();
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                if (ev.y >= HEADER_H) {
                    int row = scroll_top + (ev.y - HEADER_H) / ROW_H;
                    if (row < file_count) {
                        selected = row;
                        changed = 1;
                        long now = sys_uptime_ms();
                        if (last_click_row == row && last_click_ms >= 0 && now - last_click_ms <= DOUBLE_CLICK_MS) {
                            open_selected();
                            last_click_row = -1;
                            last_click_ms = -1;
                        } else {
                            last_click_row = row;
                            last_click_ms = now;
                        }
                    }
                }
            }
        }

        long now = sys_uptime_ms();
        if (now >= next_refresh) {
            refresh_list();
            next_refresh = now + REFRESH_MS;
            changed = 1;
        }

        if (changed) {
            redraw(&win);
        }
    }
}
