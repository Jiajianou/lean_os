/* user_space/bin/desktop_shell.c
 *
 * The taskbar: a chrome-less panel client (system_api/include/wm.h's
 * wm_create_request_t.panel, compositor.c's M22 addition), docked to the
 * bottom of the screen and laid out left to right the way a Windows
 * taskbar is - a Start button, then one button per *running* window, then
 * a system tray with the clock at the far right.
 *
 * M42 put this back to being the system bar. M41 had split it in two (a
 * macOS-style menu bar along the top holding the clock and the focused
 * app's menus, this reduced to just the running-app list); direct design
 * feedback settled the question the other way - this project is a
 * deliberate Windows/macOS hybrid, and the bar belongs at the bottom with
 * each app's own menus drawn inside its own window. So menu_bar.c is
 * gone, format_clock came back here with the clock, and the Start button
 * is the new piece: it sends WM_ACTION_TOGGLE_LAUNCHER, the one action in
 * the protocol that acts on the compositor rather than on a window, which
 * opens the launcher overlay (M43).
 *
 * The running-window list itself is unchanged, and deliberately so: it is
 * labeled rectangles carrying each app's own title (wm_create_request_t.
 * title, threaded through to wm_window_info_t.title via the WM_QUERY_PIPE
 * snapshot), skipping this panel and the desktop background via
 * wm_window_info_t.is_panel/is_desktop. Clicking an unfocused/minimized
 * button focuses it; clicking the already-focused one minimizes it - one
 * click doing both jobs depending on current state, same as Windows.
 *
 * Hover highlighting is new here and needed one compositor change to be
 * possible at all (M42): a panel now receives WM_EVENT_MOUSE_MOVE while
 * the cursor is over it even though it never holds focus, and a click on
 * it no longer takes focus away from the app you were using.
 *
 * No launcher *row*: this used to also list every file on disk
 * (SYS_listfiles) as a spawnable slot, which meant every coreutil this
 * project ships (hello, cat, ls, ...) showed up as taskbar clutter with
 * no relation to "what's currently running". That job is the Start
 * button's now (and desktop_icons.c's double-click before it); this row
 * only ever reflects what's actually open. An empty desktop means an
 * empty running-app list.
 */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "syscall_wrappers.h"
#include "wmclient.h"

#define PANEL_HEIGHT      32
#define BTN_H             24
#define BTN_Y             4  /* (PANEL_HEIGHT - BTN_H) / 2 */
#define EDGE_PAD          4

/* Start button: a 2x2 tile glyph (this project has no icon-image format -
 * see desktop_icons.c's own note) followed by the word "Start". */
#define START_X       EDGE_PAD
#define START_W       72
#define START_GLYPH_X (START_X + 8)
#define START_TILE    5
#define START_TILE_GAP 2
#define START_TEXT_X  (START_X + 24)

#define SLOT_W            96
#define SLOT_H            BTN_H
#define SLOT_GAP          4
#define SLOTS_X           (START_X + START_W + 8)
#define LABEL_PAD         6  /* M44: was 2 - rounded corners need the label held further off the edge, and 6 lines it up with the Start button's own glyph inset */
#define LABEL_MAX         10 /* (SLOT_W - LABEL_PAD either side) / 8px per glyph, rounded down */
#define MAX_RUNNING_SLOTS  WM_MAX_ROUTABLE_WINDOWS

/* System tray, right-aligned: a separator, a couple of status indicators,
 * then the clock. The indicators are static by design - there is nothing
 * in this OS that changes state in a way a tray icon would report (no
 * volume, no battery, and the NIC is either present at boot or isn't), so
 * drawing them as live would be drawing a lie. They are the tray's shape,
 * and what a real status indicator would slot into. */
#define CLOCK_CHARS   5 /* "MM:SS" */
#define CLOCK_TEXT_W  (CLOCK_CHARS * FONT_WIDTH)
#define TRAY_PAD      10
#define TRAY_ICON     8
#define TRAY_ICON_GAP 6
#define TRAY_ICONS    2
#define TRAY_W (TRAY_PAD + TRAY_ICONS * TRAY_ICON + (TRAY_ICONS - 1) * TRAY_ICON_GAP + \
                TRAY_PAD + CLOCK_TEXT_W + TRAY_PAD)

#define REFRESH_INTERVAL_MS 300
#define PRESS_FLASH_MS      150 /* how long the Start button stays lit after a click - see start_pressed_until_ms */

#define PANEL_BG            0x00181828u
#define PANEL_BORDER_COLOR  0x00445566u /* 1px top edge - the panel's only visual separation from the desktop above it otherwise */
#define PANEL_BEVEL_COLOR   0x00223349u /* faint 1px highlight just under the top edge - a cheap two-tone "lit from above" bevel, the only depth cue available without alpha blending */
#define SLOT_BORDER_COLOR   0x00445566u /* every slot is outlined so it reads as a button, not a flat color swatch */
#define RUNNING_SLOT_BG            0x00263447u
#define RUNNING_SLOT_HOVER_BG      0x00365070u
#define RUNNING_SLOT_FOCUS_BG      0x002E4A63u
#define RUNNING_SLOT_FOCUS_BORDER  0x004C99E6u /* same blue as compositor.c's TITLEBAR_FOCUS_COLOR - the focused window's titlebar and its taskbar slot read as the same "this one" accent */
#define RUNNING_SLOT_MIN_BG        0x00352A20u /* dim, warm - visually distinct from both normal and focused so a minimized app doesn't look like it just quietly vanished */
#define LABEL_COLOR         0x00FFFFFFu

#define START_BG        0x00243447u
#define START_HOVER_BG  0x00365070u
#define START_PRESS_BG  0x004C99E6u
#define START_GLYPH_FG  0x004C99E6u
#define START_PRESS_GLYPH_FG 0x00FFFFFFu
#define TRAY_SEP_COLOR  0x00303C4Eu
#define TRAY_ICON_COLOR 0x006C8098u
#define CLOCK_FG        0x00C8D4E4u

/* What the cursor is currently over. Ordinary running-app slots are their
 * own index; the two negative values are "nothing" and "the Start
 * button", which keeps hover state a single int instead of a flag plus an
 * index that could disagree with each other. */
#define HOVER_NONE  (-1)
#define HOVER_START (-2)

typedef struct {
    int32_t x, w;
    int32_t window_id;
    uint8_t focused;
    uint8_t minimized;
    char name[LABEL_MAX + 1];
} running_slot_t;

static running_slot_t running_slots[MAX_RUNNING_SLOTS];
static int running_count;
static int hovered = HOVER_NONE;
static long start_pressed_until_ms;

/* "MM:SS" of uptime - this project has no RTC/wall-clock source (see
 * gui_clock.c for the same note). Minutes wrap at 100 so the field never
 * outgrows CLOCK_TEXT_W. Lived here until M41 moved it to the top menu
 * bar and came back with the clock when M42 deleted that bar - it has
 * never existed in two places at once. */
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
 * just past the Start button, stopping (rather than overlapping) once the
 * next slot would collide with the tray - an honest "ran out of room"
 * past MAX_RUNNING_SLOTS or on a narrow display, same as this file's old
 * launcher row did for the same reason. */
static void refresh_running_slots(wm_window_t *self) {
    wm_query_response_t q;
    if (wm_query_windows(&q) != 0) {
        running_count = 0;
        return;
    }
    int32_t boundary = (int32_t)self->width - TRAY_W;
    int32_t x = SLOTS_X;
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
        x += SLOT_W + SLOT_GAP;
        running_count++;
    }
    /* A slot that scrolled out from under the cursor must not stay lit. */
    if (hovered >= running_count) {
        hovered = HOVER_NONE;
    }
}

static void draw_start_button(wm_window_t *self, int pressed) {
    uint32_t bg = pressed ? START_PRESS_BG
                          : (hovered == HOVER_START ? START_HOVER_BG : START_BG);
    uint32_t glyph = pressed ? START_PRESS_GLYPH_FG : START_GLYPH_FG;
    gfx_fill_rect_rounded(&self->gfx, START_X, BTN_Y, START_W, BTN_H, bg);
    gfx_draw_rect_rounded(&self->gfx, START_X, BTN_Y, START_W, BTN_H, SLOT_BORDER_COLOR);

    int32_t gy = BTN_Y + (BTN_H - (2 * START_TILE + START_TILE_GAP)) / 2;
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 2; col++) {
            gfx_fill_rect(&self->gfx,
                          START_GLYPH_X + col * (START_TILE + START_TILE_GAP),
                          gy + row * (START_TILE + START_TILE_GAP),
                          START_TILE, START_TILE, glyph);
        }
    }
    gfx_draw_text(&self->gfx, START_TEXT_X, BTN_Y + (BTN_H - FONT_HEIGHT) / 2, "Start", LABEL_COLOR);
}

static void draw_tray(wm_window_t *self) {
    int32_t tray_x = (int32_t)self->width - TRAY_W;
    gfx_draw_line(&self->gfx, tray_x, BTN_Y + 2, tray_x, BTN_Y + BTN_H - 3, TRAY_SEP_COLOR);

    int32_t icon_y = (PANEL_HEIGHT - TRAY_ICON) / 2;
    for (int i = 0; i < TRAY_ICONS; i++) {
        gfx_draw_rect(&self->gfx, tray_x + TRAY_PAD + i * (TRAY_ICON + TRAY_ICON_GAP), icon_y,
                      TRAY_ICON, TRAY_ICON, TRAY_ICON_COLOR);
    }

    char clock_text[CLOCK_CHARS + 1];
    format_clock(sys_uptime_ms(), clock_text);
    gfx_draw_text(&self->gfx, (int32_t)self->width - TRAY_PAD - CLOCK_TEXT_W,
                  (PANEL_HEIGHT - FONT_HEIGHT) / 2, clock_text, CLOCK_FG);
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

    draw_start_button(self, sys_uptime_ms() < start_pressed_until_ms);

    for (int i = 0; i < running_count; i++) {
        const running_slot_t *slot = &running_slots[i];
        uint32_t bg = slot->minimized ? RUNNING_SLOT_MIN_BG
                                      : (slot->focused ? RUNNING_SLOT_FOCUS_BG
                                                       : (i == hovered ? RUNNING_SLOT_HOVER_BG : RUNNING_SLOT_BG));
        uint32_t border = slot->focused ? RUNNING_SLOT_FOCUS_BORDER : SLOT_BORDER_COLOR;
        gfx_fill_rect_rounded(&self->gfx, slot->x, BTN_Y, slot->w, SLOT_H, bg);
        gfx_draw_rect_rounded(&self->gfx, slot->x, BTN_Y, slot->w, SLOT_H, border);
        gfx_draw_text(&self->gfx, slot->x + LABEL_PAD, BTN_Y + 4, slot->name, LABEL_COLOR);
    }

    draw_tray(self);
}

/* Which button (if any) is at (x, y) - HOVER_START, a running-slot index,
 * or HOVER_NONE. One function so the hover highlight and the click
 * handler can't disagree about where a button's edges are. */
static int button_at(int32_t x, int32_t y) {
    if (y < BTN_Y || y >= BTN_Y + BTN_H) {
        return HOVER_NONE;
    }
    if (gfx_point_in_rect(x, y, START_X, BTN_Y, START_W, BTN_H)) {
        return HOVER_START;
    }
    for (int i = 0; i < running_count; i++) {
        if (gfx_point_in_rect(x, y, running_slots[i].x, BTN_Y, running_slots[i].w, SLOT_H)) {
            return i;
        }
    }
    return HOVER_NONE;
}

static void handle_click(int32_t x, int32_t y) {
    int hit = button_at(x, y);
    if (hit == HOVER_START) {
        start_pressed_until_ms = sys_uptime_ms() + PRESS_FLASH_MS;
        wm_toggle_launcher();
        return;
    }
    if (hit >= 0) {
        const running_slot_t *slot = &running_slots[hit];
        wm_send_action(slot->window_id, slot->focused ? WM_ACTION_TOGGLE_MINIMIZE : WM_ACTION_FOCUS);
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
        int changed = 0;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                handle_click(ev.x, ev.y);
                refresh_running_slots(&win);
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_MOVE) {
                /* M42: a panel gets these even unfocused, and gets one
                 * final event as the cursor leaves it - see compositor.c's
                 * routing block. Redrawing only when the answer actually
                 * changes keeps a moving cursor from repainting the whole
                 * bar on every single event. */
                int now_over = button_at(ev.x, ev.y);
                if (now_over != hovered) {
                    hovered = now_over;
                    changed = 1;
                }
            }
        }

        long now = sys_uptime_ms();
        if (now >= next_refresh) {
            refresh_running_slots(&win);
            next_refresh = now + REFRESH_INTERVAL_MS;
            changed = 1;
        }
        /* The Start button's press flash expires on a timer, with no
         * event to announce it - so the tick that crosses that deadline
         * has to repaint even if nothing else changed. */
        static int was_pressed;
        int pressed = now < start_pressed_until_ms;
        if (pressed != was_pressed) {
            was_pressed = pressed;
            changed = 1;
        }
        if (changed) {
            redraw(&win);
        }
    }
}
