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

#define PANEL_BG        0x00181828u
#define LAUNCHER_SLOT_BG 0x00334455u
#define RUNNING_SLOT_BG  0x00335522u
#define RUNNING_SLOT_FOCUS_BG 0x0055AA33u
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
        if (info->is_panel || info->window_id == self->window_id) {
            continue;
        }
        running_slot_t *slot = &running_slots[running_count];
        slot->window_id = info->window_id;
        slot->focused = info->focused;
        slot->w = SLOT_W;
        running_count++;
    }
    int32_t x = self->width - SLOT_MARGIN;
    for (int i = running_count - 1; i >= 0; i--) {
        x -= running_slots[i].w;
        running_slots[i].x = x;
        x -= SLOT_MARGIN;
    }
}

static void redraw(wm_window_t *self) {
    gfx_fill_rect(&self->gfx, 0, 0, (int32_t)self->width, (int32_t)self->height, PANEL_BG);

    for (int i = 0; i < launcher_count; i++) {
        const launcher_slot_t *slot = &launcher_slots[i];
        gfx_fill_rect(&self->gfx, slot->x, SLOT_MARGIN, slot->w, SLOT_H, LAUNCHER_SLOT_BG);
        draw_label(&self->gfx, slot->x + 2, SLOT_MARGIN + 4, slot->name);
    }

    for (int i = 0; i < running_count; i++) {
        const running_slot_t *slot = &running_slots[i];
        uint32_t bg = slot->focused ? RUNNING_SLOT_FOCUS_BG : RUNNING_SLOT_BG;
        gfx_fill_rect(&self->gfx, slot->x, SLOT_MARGIN, slot->w, SLOT_H, bg);
        char num[4];
        num[0] = '#';
        num[1] = (char)('0' + (slot->window_id % 10));
        num[2] = '\0';
        gfx_draw_text(&self->gfx, slot->x + 2, SLOT_MARGIN + 4, num, LABEL_COLOR);
    }
}

static void handle_click(wm_window_t *self, int32_t x, int32_t y) {
    for (int i = 0; i < launcher_count; i++) {
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
