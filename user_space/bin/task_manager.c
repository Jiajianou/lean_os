/* user_space/bin/task_manager.c
 *
 * M45: the thing this OS had no version of - somewhere to *see* what is
 * running, and a way to stop it. Everything underneath already existed
 * (SYS_kill since M14, M29's crash-reclaim path, M42's fix for
 * signalling a task blocked inside a syscall); what was missing was
 * entirely above the kernel.
 *
 * One SYS_taskinfo call (system_api/include/proc.h) per refresh gives a
 * whole snapshot of the task table - pid, name, state, parent, plus the
 * open-fd and shm-segment counts that make a resource leak visible from
 * user space for the first time. Refreshed on a timer exactly the way
 * file_manager.c already refreshes its listing, and driven with the same
 * interaction file_manager.c uses (Up/Down + Enter, or click to select)
 * rather than inventing a third one.
 *
 * End Task sends SIGTERM, Force Quit sends SIGKILL. The distinction
 * matters here for the same reason it does in the window manager
 * (WM_ACTION_CLOSE vs WM_ACTION_KILL): a GUI client that opted into
 * M36's confirm-close is allowed to ignore a polite request forever.
 *
 * The four processes that *are* the desktop are listed but refused, and
 * say so rather than silently ignoring the click. That guard lives here,
 * not in sys_kill: the kernel stays exactly as permissive as it has
 * always been, because a real rule there needs a permission model this
 * project doesn't have - and inventing one for four hardcoded names
 * would be the first fake permission check in the project.
 */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "signal.h"   /* system_api/include/signal.h - SIGTERM/SIGKILL */
#include "str.h"
#include "syscall_wrappers.h"
#include "wmclient.h"

#define WIN_W 420
#define WIN_H 360
#define ROW_H (FONT_HEIGHT + 4)
#define HEADER_H 22
#define COLS_H   18 /* the column-label strip under the header */
#define FOOTER_H 34 /* the two action buttons plus the status line above them */
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

/* Column x offsets. Fixed rather than measured: every field here has a
 * known maximum width (a pid is at most a few digits at MAX_TASKS = 64,
 * a state is one of three fixed words, and a name is bounded by
 * TASK_INFO_NAME_MAX), so laying them out by hand is honest and a
 * measure-then-place pass would be pretending otherwise. */
#define COL_PID   6
#define COL_NAME  56
#define COL_STATE 232
#define COL_PPID  308
#define COL_RES   356

/* MAX_TASKS in kernel/sched/sched.h - not visible to a user_space build,
 * so this is the same "keep our own constant, at least as large" the
 * file manager already does for leanfs's name length. SYS_taskinfo
 * simply fills fewer entries if the kernel's table is smaller, and stops
 * at this cap if it is ever larger. */
#define MAX_ENTRIES 64

/* The processes that *are* the desktop. Killing any of these does not
 * "close an app", it takes the screen away - so this list is refused
 * with a reason rather than obeyed. Matched by name because that is what
 * SYS_taskinfo reports and what a person reading the list sees; a pid
 * would be no safer (they are not fixed) and much less obvious here. */
static const char *const PROTECTED[] = {"init", "compositor", "desktop_shell", "desktop_icons"};
#define PROTECTED_COUNT ((int)(sizeof(PROTECTED) / sizeof(PROTECTED[0])))

#define REFRESH_MS 1000
#define DOUBLE_CLICK_MS 500

/* The two action buttons, laid out from the window's right edge. */
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

/* Small unsigned decimal into a caller-supplied buffer (>= 12 bytes) -
 * this project's str.h has no itoa, and every number on this screen is a
 * pid, a count or an exit code, all comfortably small. */
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

/* "fds/shm" as one short field - two numbers that are only ever read
 * together (is anything leaking?) and would otherwise cost two columns
 * to say the same thing. */
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

/* Both buttons funnel through here - the only difference between End
 * Task and Force Quit is which signal, which is exactly the distinction
 * worth having and not worth duplicating a function over. */
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
    /* A terminated task is dimmed rather than dropped: its slot is never
     * recycled (the scheduler never reuses an id), and seeing what just
     * exited - and with what code - is most of the value of watching this
     * list while something dies. */
    int dead = (t->state == TASK_INFO_TERMINATED);
    uint32_t fg = dead ? DIM_TEXT_COLOR : TEXT_COLOR;
    if (i == selected) {
        gfx_fill_rect(&win->gfx, 0, y, LIST_W, ROW_H, SELECT_COLOR);
        fg = TEXT_COLOR;
    }
    char buf[16];
    format_int(t->pid, buf);
    gfx_draw_text(&win->gfx, COL_PID, y + 2, buf, fg);
    gfx_draw_text(&win->gfx, COL_NAME, y + 2, t->name[0] ? t->name : "?", fg);
    gfx_draw_text(&win->gfx, COL_STATE, y + 2, state_name(t->state), fg);
    format_int(t->parent_pid, buf);
    gfx_draw_text(&win->gfx, COL_PPID, y + 2, buf, fg);
    format_resources(t, buf);
    gfx_draw_text(&win->gfx, COL_RES, y + 2, buf, fg);
}

static void redraw(wm_window_t *win) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, HEADER_H, HEADER_COLOR);
    gfx_draw_text(&win->gfx, 6, 3, "Processes", LABEL_COLOR);

    gfx_fill_rect(&win->gfx, 0, HEADER_H, WIN_W, COLS_H, COLS_COLOR);
    gfx_draw_text(&win->gfx, COL_PID, HEADER_H + 1, "PID", LABEL_COLOR);
    gfx_draw_text(&win->gfx, COL_NAME, HEADER_H + 1, "Name", LABEL_COLOR);
    gfx_draw_text(&win->gfx, COL_STATE, HEADER_H + 1, "State", LABEL_COLOR);
    gfx_draw_text(&win->gfx, COL_PPID, HEADER_H + 1, "PPID", LABEL_COLOR);
    gfx_draw_text(&win->gfx, COL_RES, HEADER_H + 1, "fd/sh", LABEL_COLOR);

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

    /* The status line sits directly above the buttons that write to it -
     * an in-window message about the window you are looking at, which is
     * the right place for it. */
    gfx_draw_text(&win->gfx, 8, BTN_Y - FONT_HEIGHT - 2, status_text, status_color);
    gfx_draw_button(&win->gfx, END_BTN_X, BTN_Y, BTN_W, BTN_H, BTN_BG, BTN_BORDER, "End Task", BTN_TEXT);
    gfx_draw_button(&win->gfx, KILL_BTN_X, BTN_Y, BTN_W, BTN_H, KILL_BTN_BG, BTN_BORDER, "Force Quit", BTN_TEXT);
}

int main(void) {
    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Tasks", &win) != 0) {
        sys_exit(1);
    }

    refresh_tasks();
    redraw(&win);

    long next_refresh = sys_uptime_ms() + REFRESH_MS;
    long last_click_ms = -1;
    int last_click_row = -1;

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_KEY) {
                if (ev.ch == KBD_KEY_UP && selected > 0) {
                    selected--;
                    clamp_scroll();
                    changed = 1;
                } else if (ev.ch == KBD_KEY_DOWN && selected + 1 < task_count) {
                    selected++;
                    clamp_scroll();
                    changed = 1;
                } else if (ev.ch == '\n' || ev.ch == '\r') {
                    /* Enter is the polite verb, matching the leftmost
                     * button - Force Quit stays a deliberate click. */
                    signal_selected(SIGTERM);
                    changed = 1;
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                if (gfx_point_in_rect(ev.x, ev.y, END_BTN_X, BTN_Y, BTN_W, BTN_H)) {
                    signal_selected(SIGTERM);
                    changed = 1;
                } else if (gfx_point_in_rect(ev.x, ev.y, KILL_BTN_X, BTN_Y, BTN_W, BTN_H)) {
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
        }
    }
}
