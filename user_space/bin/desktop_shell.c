/* user_space/bin/desktop_shell.c
 *
 * M22: the piece that makes "desktop environment running custom apps"
 * genuinely true rather than aspirational (this milestone's own
 * framing) - a real user-space client, connected as a chrome-less panel
 * (system_api/include/wm.h's wm_create_request_t.panel, compositor.c's
 * M22 addition), docked to the bottom of the screen.
 *
 * Two rows of clickable slots in that one panel:
 *   - launcher (left, growing right): one slot per file SYS_listfiles
 *     reports on disk - clicking spawns it (SYS_spawn). No filtering by
 *     file type: this project doesn't have one, and a coreutil like
 *     `ls` is harmless to spawn from here (it runs, writes to a stdout
 *     nothing is reading, and exits) rather than something the launcher
 *     needs to guard against.
 *   - running windows (right, growing left): one slot per window
 *     wm_query_windows reports (skipping this panel itself via
 *     wm_window_info_t.is_panel) - clicking an unfocused/minimized one
 *     focuses it, clicking the already-focused one minimizes it. The
 *     "click to focus/minimize" milestone wording, read literally as one
 *     click doing both jobs depending on current state.
 */
#include "font8x16.h" /* FONT_WIDTH - sizing the clock's fixed-width text area */
#include "syscall_wrappers.h"
#include "wmclient.h"

#define PANEL_HEIGHT      32
#define SLOT_W            64
#define SLOT_H            24
#define SLOT_MARGIN       4
#define LABEL_MAX         7 /* truncated to fit SLOT_W at 8px/char with margin */
#define MAX_LAUNCHER_SLOTS 16
#define MAX_RUNNING_SLOTS  WM_MAX_ROUTABLE_WINDOWS
#define REFRESH_INTERVAL_MS 300

/* System-tray-style clock pinned to the panel's right edge, "MM:SS" of
 * sys_uptime_ms (this project has no RTC/wall-clock source - see
 * gui_clock.c's own uptime-based display for the same reason) - the
 * one always-present, unmistakably-a-taskbar element, the way a real
 * desktop's taskbar clock is. CLOCK_PAD separates the divider line from
 * the text on one side and the text from the panel's own right edge on
 * the other. */
#define CLOCK_TEXT_W (5 * FONT_WIDTH)
#define CLOCK_PAD     8
#define CLOCK_AREA_W (CLOCK_PAD * 2 + CLOCK_TEXT_W)

#define PANEL_BG        0x00181828u
#define PANEL_BORDER_COLOR 0x00445566u /* 1px top edge - the panel's only visual separation from the desktop above it otherwise */
#define SEPARATOR_COLOR    0x00445566u
#define SLOT_BORDER_COLOR  0x00445566u /* every slot is outlined so it reads as a button, not a flat color swatch */
#define LAUNCHER_SLOT_BG 0x00334455u
#define RUNNING_SLOT_BG  0x00335522u
#define RUNNING_SLOT_FOCUS_BG 0x0055AA33u
#define RUNNING_SLOT_FOCUS_BORDER 0x0099EE55u
#define RUNNING_SLOT_MIN_BG   0x00553322u
#define LABEL_COLOR     0x00FFFFFFu

/* Duplicated rather than pulled from kernel/fs/leanfs.h (kernel-only,
 * this process can't include it) - just needs to be big enough for any
 * name SYS_listfiles ever hands back. */
#define LEANFS_MAX_NAME_LOCAL 28

typedef struct {
    int32_t x, w;
    char name[LEANFS_MAX_NAME_LOCAL];
} launcher_slot_t;

static char list_buf[2048];
static launcher_slot_t launcher_slots[MAX_LAUNCHER_SLOTS];
static int launcher_count;

typedef struct {
    int32_t x, w;
    int32_t window_id;
    uint8_t focused;
} running_slot_t;

static running_slot_t running_slots[MAX_RUNNING_SLOTS];
static int running_count;

static void parse_file_list(long n) {
    launcher_count = 0;
    int start = 0;
    for (long i = 0; i <= n && launcher_count < MAX_LAUNCHER_SLOTS; i++) {
        if (i == n || list_buf[i] == '\n') {
            int len = (int)i - start;
            if (len > 0) {
                launcher_slot_t *slot = &launcher_slots[launcher_count];
                int copy_len = len < LEANFS_MAX_NAME_LOCAL - 1 ? len : LEANFS_MAX_NAME_LOCAL - 1;
                for (int j = 0; j < copy_len; j++) {
                    slot->name[j] = list_buf[start + j];
                }
                slot->name[copy_len] = '\0';
                slot->w = SLOT_W;
                launcher_count++;
            }
            start = (int)i + 1;
        }
    }
    int32_t x = SLOT_MARGIN;
    for (int i = 0; i < launcher_count; i++) {
        launcher_slots[i].x = x;
        x += launcher_slots[i].w + SLOT_MARGIN;
    }
}

static void draw_label(gfx_ctx_t *gfx, int32_t x, int32_t y, const char *name) {
    char label[LABEL_MAX + 1];
    int i = 0;
    for (; i < LABEL_MAX && name[i]; i++) {
        label[i] = name[i];
    }
    label[i] = '\0';
    gfx_draw_text(gfx, x, y, label, LABEL_COLOR);
}

static void refresh_running_slots(wm_window_t *self) {
    wm_query_response_t q;
    if (wm_query_windows(&q) != 0) {
        running_count = 0;
        return;
    }
    running_count = 0;
    for (int32_t i = 0; i < q.count && running_count < MAX_RUNNING_SLOTS; i++) {
        const wm_window_info_t *info = &q.windows[i];
        if (info->is_panel || info->is_desktop || info->window_id == self->window_id) {
            continue;
        }
        running_slot_t *slot = &running_slots[running_count];
        slot->window_id = info->window_id;
        slot->focused = info->focused;
        slot->w = SLOT_W;
        running_count++;
    }
    int32_t x = self->width - SLOT_MARGIN - CLOCK_AREA_W;
    for (int i = running_count - 1; i >= 0; i--) {
        x -= running_slots[i].w;
        running_slots[i].x = x;
        x -= SLOT_MARGIN;
    }
}

/* How many launcher_slots entries actually fit before colliding with the
 * running-window row's own left edge (running_slots[0].x - see
 * refresh_running_slots's positioning loop: index 0 ends up leftmost
 * regardless of running_count) - the file list this project ships has
 * only ever grown (M15's own note on leanfs headroom, this project's
 * general direction), so a fixed slot count was always going to run into
 * this on a fixed-width panel eventually. Any launcher_slots entry past
 * this point is simply not drawn or clickable rather than overlapping the
 * taskbar - an honest "ran out of room" rather than visual corruption. */
static int visible_launcher_count(const wm_window_t *self) {
    int32_t boundary = running_count > 0 ? running_slots[0].x - SLOT_MARGIN : (int32_t)self->width - CLOCK_AREA_W;
    int n = 0;
    while (n < launcher_count && launcher_slots[n].x + launcher_slots[n].w <= boundary) {
        n++;
    }
    return n;
}

/* "MM:SS" of uptime, zero-padded - no itoa in this project's str.h
 * (gui_clock.c's format_uint hit the same gap for its own free-form
 * "uptime: Ns" text), but a fixed two-digit field is simpler to write
 * directly than to generalize for. Minutes wrap at 100 purely so the
 * field never grows past its reserved CLOCK_TEXT_W. */
static void format_clock(long now_ms, char *out) {
    long total_s = now_ms / 1000;
    long mins = (total_s / 60) % 100;
    long secs = total_s % 60;
    out[0] = (char)('0' + (mins / 10) % 10);
    out[1] = (char)('0' + mins % 10);
    out[2] = ':';
    out[3] = (char)('0' + secs / 10);
    out[4] = (char)('0' + secs % 10);
    out[5] = '\0';
}

static void redraw(wm_window_t *self) {
    gfx_fill_rect(&self->gfx, 0, 0, (int32_t)self->width, (int32_t)self->height, PANEL_BG);
    /* Top edge is otherwise the only thing telling this panel apart from
     * the desktop it's docked to - one line makes it read as a distinct
     * bar rather than the desktop background bleeding into it. */
    gfx_draw_line(&self->gfx, 0, 0, (int32_t)self->width - 1, 0, PANEL_BORDER_COLOR);

    int visible = visible_launcher_count(self);
    for (int i = 0; i < visible; i++) {
        const launcher_slot_t *slot = &launcher_slots[i];
        gfx_fill_rect(&self->gfx, slot->x, SLOT_MARGIN, slot->w, SLOT_H, LAUNCHER_SLOT_BG);
        gfx_draw_rect(&self->gfx, slot->x, SLOT_MARGIN, slot->w, SLOT_H, SLOT_BORDER_COLOR);
        draw_label(&self->gfx, slot->x + 2, SLOT_MARGIN + 4, slot->name);
    }

    if (running_count > 0) {
        int32_t sep_x = running_slots[0].x - SLOT_MARGIN / 2 - 1;
        gfx_draw_line(&self->gfx, sep_x, SLOT_MARGIN, sep_x, SLOT_MARGIN + SLOT_H, SEPARATOR_COLOR);
    }

    for (int i = 0; i < running_count; i++) {
        const running_slot_t *slot = &running_slots[i];
        uint32_t bg = slot->focused ? RUNNING_SLOT_FOCUS_BG : RUNNING_SLOT_BG;
        uint32_t border = slot->focused ? RUNNING_SLOT_FOCUS_BORDER : SLOT_BORDER_COLOR;
        gfx_fill_rect(&self->gfx, slot->x, SLOT_MARGIN, slot->w, SLOT_H, bg);
        gfx_draw_rect(&self->gfx, slot->x, SLOT_MARGIN, slot->w, SLOT_H, border);
        char num[4];
        num[0] = '#';
        num[1] = (char)('0' + (slot->window_id % 10));
        num[2] = '\0';
        gfx_draw_text(&self->gfx, slot->x + 2, SLOT_MARGIN + 4, num, LABEL_COLOR);
    }

    int32_t clock_x = (int32_t)self->width - CLOCK_AREA_W;
    gfx_draw_line(&self->gfx, clock_x, SLOT_MARGIN, clock_x, SLOT_MARGIN + SLOT_H, SEPARATOR_COLOR);
    char clock_text[6];
    format_clock(sys_uptime_ms(), clock_text);
    gfx_draw_text(&self->gfx, clock_x + CLOCK_PAD, SLOT_MARGIN + 4, clock_text, LABEL_COLOR);
}

static void handle_click(wm_window_t *self, int32_t x, int32_t y) {
    int visible = visible_launcher_count(self);
    for (int i = 0; i < visible; i++) {
        const launcher_slot_t *slot = &launcher_slots[i];
        if (x >= slot->x && x < slot->x + slot->w && y >= SLOT_MARGIN && y < SLOT_MARGIN + SLOT_H) {
            sys_spawn(slot->name, "");
            return;
        }
    }
    for (int i = 0; i < running_count; i++) {
        const running_slot_t *slot = &running_slots[i];
        if (x >= slot->x && x < slot->x + slot->w && y >= SLOT_MARGIN && y < SLOT_MARGIN + SLOT_H) {
            if (slot->focused) {
                wm_send_action(slot->window_id, WM_ACTION_TOGGLE_MINIMIZE);
            } else {
                wm_send_action(slot->window_id, WM_ACTION_FOCUS);
            }
            (void)self;
            return;
        }
    }
}

int main(void) {
    wm_window_t win;
    if (wm_connect_panel(PANEL_HEIGHT, &win) != 0) {
        sys_exit(1);
    }

    long n = sys_listfiles(list_buf, sizeof(list_buf));
    if (n < 0) {
        n = 0;
    }
    parse_file_list(n);

    refresh_running_slots(&win);
    redraw(&win);

    long next_refresh = 0;
    for (;;) {
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                handle_click(&win, ev.x, ev.y);
                refresh_running_slots(&win);
                redraw(&win);
            }
        }

        long now = sys_uptime_ms();
        if (now >= next_refresh) {
            refresh_running_slots(&win);
            redraw(&win);
            next_refresh = now + REFRESH_INTERVAL_MS;
        }
    }
}
