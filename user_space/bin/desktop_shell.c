/* user_space/bin/desktop_shell.c
 *
 * The taskbar: a chrome-less panel client (system_api/include/wm.h's
 * wm_create_request_t.panel, compositor.c's M22 addition), docked to the
 * bottom of the screen. One row of clickable slots, one per *running*
 * window (system_api/include/wm.h's WM_QUERY_PIPE query protocol via
 * wm_query_windows), labeled with the app's own title
 * (wm_create_request_t.title, threaded through to wm_window_info_t.title)
 * rather than a bare window id - skipping this panel itself and any other
 * panel/desktop-background client via wm_window_info_t.is_panel/
 * is_desktop. Clicking an unfocused/minimized slot focuses it; clicking
 * the already-focused one minimizes it - one click doing both jobs
 * depending on current state.
 *
 * No launcher row: this used to also list every file on disk
 * (SYS_listfiles) as a spawnable slot, which meant every coreutil this
 * project ships (hello, cat, ls, ...) showed up as taskbar clutter with
 * no relation to "what's currently running". Launching programs is
 * desktop_icons.c's job (double-click on the desktop background); this
 * file's only job now is to honestly reflect what's actually open, the
 * same way a real desktop's taskbar app list works. An empty desktop
 * means an empty taskbar - just the clock.
 */
#include "font8x16.h" /* FONT_WIDTH - sizing the clock's fixed-width text area */
#include "syscall_wrappers.h"
#include "wmclient.h"

#define PANEL_HEIGHT      32
#define SLOT_W            96
#define SLOT_H            24
#define SLOT_MARGIN       4
#define LABEL_MAX         11 /* (SLOT_W - 2px left pad - 2px right pad) / 8px per glyph, rounded down */
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

#define PANEL_BG            0x00181828u
#define PANEL_BORDER_COLOR  0x00445566u /* 1px top edge - the panel's only visual separation from the desktop above it otherwise */
#define PANEL_BEVEL_COLOR   0x00223349u /* faint 1px highlight just under the top edge - a cheap two-tone "lit from above" bevel, the only depth cue available without alpha blending */
#define SEPARATOR_COLOR     0x00445566u
#define SLOT_BORDER_COLOR   0x00445566u /* every slot is outlined so it reads as a button, not a flat color swatch */
#define RUNNING_SLOT_BG            0x00263447u
#define RUNNING_SLOT_FOCUS_BG      0x002E4A63u
#define RUNNING_SLOT_FOCUS_BORDER  0x004C99E6u /* same blue as compositor.c's TITLEBAR_FOCUS_COLOR - the focused window's titlebar and its taskbar slot read as the same "this one" accent */
#define RUNNING_SLOT_MIN_BG        0x00352A20u /* dim, warm - visually distinct from both normal and focused so a minimized app doesn't look like it just quietly vanished */
#define LABEL_COLOR         0x00FFFFFFu

typedef struct {
    int32_t x, w;
    int32_t window_id;
    uint8_t focused;
    uint8_t minimized;
    char name[LABEL_MAX + 1];
} running_slot_t;

static running_slot_t running_slots[MAX_RUNNING_SLOTS];
static int running_count;

/* Bounded copy of a window's title into a slot label, falling back to a
 * generic name for the (currently theoretical - every GUI client sets a
 * title) case of a client that connects without one, so a slot never
 * renders as blank. */
static void copy_label(char *dst, const char *src) {
    int i = 0;
    if (src && src[0]) {
        for (; i < LABEL_MAX && src[i]; i++) {
            dst[i] = src[i];
        }
    } else {
        static const char fallback[] = "App";
        for (; fallback[i]; i++) {
            dst[i] = fallback[i];
        }
    }
    dst[i] = '\0';
}

/* Lays out one slot per non-panel/non-desktop window left to right from
 * the panel's left edge, stopping (rather than overlapping) once the next
 * slot would collide with the clock's own reserved area - an honest "ran
 * out of room" past MAX_RUNNING_SLOTS or a narrow display, same as this
 * file's old launcher row did for the same reason. */
static void refresh_running_slots(wm_window_t *self) {
    wm_query_response_t q;
    if (wm_query_windows(&q) != 0) {
        running_count = 0;
        return;
    }
    int32_t boundary = (int32_t)self->width - CLOCK_AREA_W - SLOT_MARGIN;
    int32_t x = SLOT_MARGIN;
    running_count = 0;
    for (int32_t i = 0; i < q.count && running_count < MAX_RUNNING_SLOTS; i++) {
        const wm_window_info_t *info = &q.windows[i];
        if (info->is_panel || info->is_desktop || info->window_id == self->window_id) {
            continue;
        }
        if (x + SLOT_W > boundary) {
            break;
        }
        running_slot_t *slot = &running_slots[running_count];
        slot->x = x;
        slot->w = SLOT_W;
        slot->window_id = info->window_id;
        slot->focused = info->focused;
        slot->minimized = info->minimized;
        copy_label(slot->name, info->title);
        x += SLOT_W + SLOT_MARGIN;
        running_count++;
    }
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
     * bar rather than the desktop background bleeding into it. The
     * second, fainter line right under it is a one-pixel bevel highlight
     * - together they read as a lit top edge instead of a flat outline. */
    gfx_draw_line(&self->gfx, 0, 0, (int32_t)self->width - 1, 0, PANEL_BORDER_COLOR);
    gfx_draw_line(&self->gfx, 0, 1, (int32_t)self->width - 1, 1, PANEL_BEVEL_COLOR);

    for (int i = 0; i < running_count; i++) {
        const running_slot_t *slot = &running_slots[i];
        uint32_t bg = slot->minimized ? RUNNING_SLOT_MIN_BG : (slot->focused ? RUNNING_SLOT_FOCUS_BG : RUNNING_SLOT_BG);
        uint32_t border = slot->focused ? RUNNING_SLOT_FOCUS_BORDER : SLOT_BORDER_COLOR;
        gfx_fill_rect(&self->gfx, slot->x, SLOT_MARGIN, slot->w, SLOT_H, bg);
        gfx_draw_rect(&self->gfx, slot->x, SLOT_MARGIN, slot->w, SLOT_H, border);
        gfx_draw_text(&self->gfx, slot->x + 2, SLOT_MARGIN + 4, slot->name, LABEL_COLOR);
    }

    int32_t clock_x = (int32_t)self->width - CLOCK_AREA_W;
    gfx_draw_line(&self->gfx, clock_x, SLOT_MARGIN, clock_x, SLOT_MARGIN + SLOT_H, SEPARATOR_COLOR);
    char clock_text[6];
    format_clock(sys_uptime_ms(), clock_text);
    gfx_draw_text(&self->gfx, clock_x + CLOCK_PAD, SLOT_MARGIN + 4, clock_text, LABEL_COLOR);
}

static void handle_click(wm_window_t *self, int32_t x, int32_t y) {
    (void)self;
    for (int i = 0; i < running_count; i++) {
        const running_slot_t *slot = &running_slots[i];
        if (x >= slot->x && x < slot->x + slot->w && y >= SLOT_MARGIN && y < SLOT_MARGIN + SLOT_H) {
            if (slot->focused) {
                wm_send_action(slot->window_id, WM_ACTION_TOGGLE_MINIMIZE);
            } else {
                wm_send_action(slot->window_id, WM_ACTION_FOCUS);
            }
            return;
        }
    }
}

int main(void) {
    wm_window_t win;
    if (wm_connect_panel(PANEL_HEIGHT, &win) != 0) {
        sys_exit(1);
    }

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
