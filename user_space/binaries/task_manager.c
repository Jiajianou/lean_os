#define LIST_FONT   ui_font_small
#define LIST_FONT_H UI_FONT_SMALL_HEIGHT
#include "signal.h"
#include "string_utilities.h"
#include "syscall_wrappers.h"
#include "window_manager_client.h"

#define WIN_W 420
#define WIN_H 360
#define ROW_H (LIST_FONT_H + 4)
#define HEADER_H 22
#define COLS_H   18
#define FOOTER_H 34
#define LIST_Y   (HEADER_H + COLS_H)
#define SCROLLBAR_W 8
#define LIST_W (WIN_W - SCROLLBAR_W)
#define LIST_H (WIN_H - LIST_Y - FOOTER_H)
#define ROWS_VISIBLE (LIST_H / ROW_H)

#define BG_COLOR        0x001C1C24u
#define HEADER_COLOR    0x00303850u
#define COLS_COLOR      0x00242C3Cu
#define TEXT_COLOR      0x00D8D8D8u
#define DIM_TEXT_COLOR  0x008090A8u
#define SELECT_COLOR    0x004C6699u
#define LABEL_COLOR     0x0090A0C0u
#define SCROLLBAR_TRACK 0x00141820u
#define SCROLLBAR_THUMB 0x00506080u
#define BTN_BG          0x00303C52u
#define BTN_BORDER      0x00506070u
#define BTN_TEXT        0x00FFFFFFu
#define KILL_BTN_BG     0x00663038u
#define STATUS_OK       0x0090C0A0u
#define STATUS_ERR      0x00E08878u

#define COL_PID   6
#define COL_NAME  56
#define COL_STATE 232
#define COL_PPID  308
#define COL_RES   356

#define MAX_ENTRIES TASK_INFO_MAX

static const char *const PROTECTED[] = {"init", "kernel", "cpu-idle"};
#define PROTECTED_COUNT ((int)(sizeof(PROTECTED) / sizeof(PROTECTED[0])))

#define REFRESH_MS 1000
#define DOUBLE_CLICK_MS 500

#define BTN_W 96
#define BTN_H 22
#define BTN_Y (WIN_H - BTN_H - 6)
#define KILL_BTN_X (WIN_W - BTN_W - 8)
#define END_BTN_X  (KILL_BTN_X - BTN_W - 8)

static task_info_t tasks[MAX_ENTRIES];
static int task_count;
static int selected = -1;
static int scroll_top;
static const char *status_text = "";
static uint32_t status_color = STATUS_OK;
static int pressed_btn = -1;

static int is_protected(const char *name) {
    for (int i = 0; i < PROTECTED_COUNT; i++) {
        if (strcmp(name, PROTECTED[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *state_name(int32_t state) {
    if (state == TASK_INFO_TERMINATED) {
        return "exited";
    }
    return state == TASK_INFO_RUNNING ? "running" : "ready";
}

static void format_int(int32_t v, char *out) {
    if (v < 0) {
        out[0] = '-';
        format_int(-v, out + 1);
        return;
    }
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    for (int i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = '\0';
}

static void format_resources(const task_info_t *t, char *out) {
    char a[12], b[12];
    format_int(t->open_fds, a);
    format_int(t->shm_segments, b);
    int n = 0;
    for (int i = 0; a[i]; i++) {
        out[n++] = a[i];
    }
    out[n++] = '/';
    for (int i = 0; b[i]; i++) {
        out[n++] = b[i];
    }
    out[n] = '\0';
}

static void refresh_tasks(void) {
    long n = sys_taskinfo(tasks, MAX_ENTRIES);
    task_count = n > 0 ? (int)n : 0;
    if (selected >= task_count) {
        selected = task_count - 1;
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
    if (scroll_top < 0) {
        scroll_top = 0;
    }
}

static void signal_selected(int sig) {
    if (selected < 0 || selected >= task_count) {
        status_text = "Nothing selected.";
        status_color = STATUS_ERR;
        return;
    }
    const task_info_t *t = &tasks[selected];
    if (t->state == TASK_INFO_TERMINATED) {
        status_text = "That process has already exited.";
        status_color = STATUS_ERR;
        return;
    }
    if (is_protected(t->name)) {
        status_text = "That process is part of the desktop.";
        status_color = STATUS_ERR;
        return;
    }
    if (sys_kill(t->pid, sig) != 0) {
        status_text = "The kernel refused that signal.";
        status_color = STATUS_ERR;
        return;
    }
    status_text = sig == SIGKILL ? "Force Quit sent." : "End Task sent.";
    status_color = STATUS_OK;
    refresh_tasks();
}

static void draw_row(wm_window_t *win, int i, int32_t y) {
    const task_info_t *t = &tasks[i];
    int dead = (t->state == TASK_INFO_TERMINATED);
    uint32_t fg = dead ? DIM_TEXT_COLOR : TEXT_COLOR;
    if (i == selected) {
        gfx_fill_rect(&win->gfx, 0, y, LIST_W, ROW_H, SELECT_COLOR);
        fg = TEXT_COLOR;
    }
    char buf[16];
    format_int(t->pid, buf);
    gfx_draw_text_font(&win->gfx, COL_PID, y + 2, buf, fg, &LIST_FONT, 0);
    gfx_draw_text_font(&win->gfx, COL_NAME, y + 2, t->name[0] ? t->name : "?", fg, &LIST_FONT, 0);
    gfx_draw_text_font(&win->gfx, COL_STATE, y + 2, state_name(t->state), fg, &LIST_FONT, 0);
    format_int(t->parent_pid, buf);
    gfx_draw_text_font(&win->gfx, COL_PPID, y + 2, buf, fg, &LIST_FONT, 0);
    format_resources(t, buf);
    gfx_draw_text_font(&win->gfx, COL_RES, y + 2, buf, fg, &LIST_FONT, 0);
}

static void redraw(wm_window_t *win) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, HEADER_H, HEADER_COLOR);
    gfx_draw_text(&win->gfx, 6, 3, "Processes", LABEL_COLOR);

    gfx_fill_rect(&win->gfx, 0, HEADER_H, WIN_W, COLS_H, COLS_COLOR);
    gfx_draw_text_font(&win->gfx, COL_PID, HEADER_H + 1, "PID", LABEL_COLOR, &LIST_FONT, 0);
    gfx_draw_text_font(&win->gfx, COL_NAME, HEADER_H + 1, "Name", LABEL_COLOR, &LIST_FONT, 0);
    gfx_draw_text_font(&win->gfx, COL_STATE, HEADER_H + 1, "State", LABEL_COLOR, &LIST_FONT, 0);
    gfx_draw_text_font(&win->gfx, COL_PPID, HEADER_H + 1, "PPID", LABEL_COLOR, &LIST_FONT, 0);
    gfx_draw_text_font(&win->gfx, COL_RES, HEADER_H + 1, "fd/sh", LABEL_COLOR, &LIST_FONT, 0);

    for (int row = 0; row < ROWS_VISIBLE; row++) {
        int i = scroll_top + row;
        if (i >= task_count) {
            break;
        }
        draw_row(win, i, LIST_Y + row * ROW_H);
    }

    gfx_draw_scrollbar(&win->gfx, LIST_W, LIST_Y, SCROLLBAR_W, LIST_H,
                        task_count, ROWS_VISIBLE, scroll_top,
                        SCROLLBAR_TRACK, SCROLLBAR_THUMB);

    gfx_draw_text(&win->gfx, 8, BTN_Y - (int32_t)gfx_ui_font()->height - 2, status_text, status_color);
    gfx_draw_button_state(&win->gfx, END_BTN_X, BTN_Y, BTN_W, BTN_H, BTN_BG, BTN_BORDER,
                           "End Task", BTN_TEXT, pressed_btn == 0);
    gfx_draw_button_state(&win->gfx, KILL_BTN_X, BTN_Y, BTN_W, BTN_H, KILL_BTN_BG, BTN_BORDER,
                           "Force Quit", BTN_TEXT, pressed_btn == 1);
}

int main(void) {
    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Tasks", &win) != 0) {
        sys_exit(1);
    }

    refresh_tasks();
    redraw(&win);

    wm_present(&win);
    long next_refresh = sys_uptime_ms() + REFRESH_MS;
    long last_click_ms = -1;
    int last_click_row = -1;

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                changed = 1;
            } else if (ev.type == WM_EVENT_KEY) {
                if (ev.ch == KBD_KEY_UP && selected > 0) {
                    selected--;
                    clamp_scroll();
                    changed = 1;
                } else if (ev.ch == KBD_KEY_DOWN && selected + 1 < task_count) {
                    selected++;
                    clamp_scroll();
                    changed = 1;
                } else if (ev.ch == '\n' || ev.ch == '\r') {
                    signal_selected(SIGTERM);
                    changed = 1;
                }
            } else if (ev.type == WM_EVENT_MOUSE_WHEEL) {
                int max_top = task_count - ROWS_VISIBLE;
                if (max_top < 0) {
                    max_top = 0;
                }
                int want = scroll_top + ev.wheel;
                if (want < 0) {
                    want = 0;
                }
                if (want > max_top) {
                    want = max_top;
                }
                if (want != scroll_top) {
                    scroll_top = want;
                    changed = 1;
                }
            } else if (ev.type == WM_EVENT_MOUSE_MOVE) {
                if (pressed_btn >= 0) {
                    int32_t bx = pressed_btn == 0 ? END_BTN_X : KILL_BTN_X;
                    if (!((ev.buttons & 1) && gfx_point_in_rect(ev.x, ev.y, bx, BTN_Y, BTN_W, BTN_H))) {
                        pressed_btn = -1;
                        changed = 1;
                    }
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && !(ev.buttons & 1)) {
                if (pressed_btn >= 0) {
                    pressed_btn = -1;
                    changed = 1;
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                if (gfx_point_in_rect(ev.x, ev.y, END_BTN_X, BTN_Y, BTN_W, BTN_H)) {
                    pressed_btn = 0;
                    signal_selected(SIGTERM);
                    changed = 1;
                } else if (gfx_point_in_rect(ev.x, ev.y, KILL_BTN_X, BTN_Y, BTN_W, BTN_H)) {
                    pressed_btn = 1;
                    signal_selected(SIGKILL);
                    changed = 1;
                } else if (ev.y >= LIST_Y && ev.y < LIST_Y + LIST_H && ev.x < LIST_W) {
                    int row = scroll_top + (ev.y - LIST_Y) / ROW_H;
                    if (row < task_count) {
                        selected = row;
                        changed = 1;
                        long now = (long)ev.time_ms;
                        if (last_click_row == row && last_click_ms >= 0 &&
                            now - last_click_ms <= DOUBLE_CLICK_MS) {
                            signal_selected(SIGTERM);
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
            refresh_tasks();
            next_refresh = now + REFRESH_MS;
            changed = 1;
        }
        if (changed) {
            redraw(&win);
            wm_present(&win);
        }
        wm_wait_ms(&win, NULL, 0, (int)(next_refresh - now));
    }
}
