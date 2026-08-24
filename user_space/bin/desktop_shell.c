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

/* M45: the bar can now raise a context menu *out* of itself. A panel is
 * clipped to its own buffer and 32px tall, and a three-item menu is
 * ~72px, so this window allocates PANEL_OVERHANG_MAX extra rows above
 * the docked strip and asks the compositor to composite and click-route
 * as many of them as the menu currently needs
 * (WM_ACTION_SET_PANEL_OVERHANG / wm_set_panel_overhang). M41 built that
 * mechanism for its top-bar dropdowns and M42 deleted it with the bar;
 * this brings it back rather than inventing a second one.
 *
 * The buffer's layout is: rows [0, PANEL_OVERHANG_MAX) are the overhang,
 * rows [PANEL_OVERHANG_MAX, PANEL_OVERHANG_MAX + PANEL_HEIGHT) are the
 * bar itself. `bar_gfx` below is a gfx_ctx_t over just that second
 * region, which is why not one coordinate in the bar's own drawing had
 * to move for any of this. */
#define PANEL_OVERHANG_MAX 96
#define PANEL_BUF_H       (PANEL_OVERHANG_MAX + PANEL_HEIGHT)

/* The right-click menu on a running-app button. Its three verbs are the
 * same ones (and in the same order) the compositor draws for a
 * right-click on the window's own titlebar - they act on another
 * client's window, which is exactly the "the app defines the menu, the
 * system only draws it" split this milestone kept. Every row ends at
 * wm_send_action, so all three ways to close a window are literally one
 * code path. */
#define CTX_W        124
#define CTX_ITEM_H   22
#define CTX_COUNT    3
#define CTX_BG       0x00243040u
#define CTX_HOVER_BG 0x003A5A80u
#define CTX_BORDER   0x00506070u
#define CTX_TEXT     0x00FFFFFFu
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
#define RUNNING_SLOT_FRONT_BORDER  0x002E5C86u /* M51: half-lit accent - the window on top when nothing holds focus, see running_slot_t.frontmost */
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
    /* M51: this is the window at the top of the compositor's z-order -
     * which is almost always also the focused one, since focusing raises.
     * The case worth drawing is the one where it isn't: minimizing the
     * focused window leaves nothing focused at all, and the bar can still
     * say which of the remaining windows is in front. Read from
     * wm_window_info_t.z_index rather than from the order the query
     * returned, deliberately: the buttons stay in window_id order for the
     * whole life of a window so they can be found by muscle memory, which
     * is the Windows behavior and the reason the query does not simply
     * hand them back in z-order. */
    uint8_t frontmost;
    uint8_t minimized;
    char name[LABEL_MAX + 1];
} running_slot_t;

static running_slot_t running_slots[MAX_RUNNING_SLOTS];
static int running_count;
static int hovered = HOVER_NONE;
static long start_pressed_until_ms;

/* M45: the context menu's state. `ctx_slot` is an index into
 * running_slots (not a window id) so the menu re-reads that slot's live
 * focused/minimized state on every redraw - the label on its first row
 * follows it. -1 = closed. `ctx_x`/`ctx_y` are in the *bar's* coordinate
 * space (y negative: the menu is above the bar's own top edge), the same
 * space every event the compositor routes here arrives in. */
static int ctx_slot = -1;
static int32_t ctx_x, ctx_y;
static int ctx_hover = -1;
static int32_t ctx_overhang; /* what the compositor was last told to raise - only re-sent when it changes */

/* A gfx_ctx_t over just the docked strip of this window's buffer. Every
 * function that draws the bar takes this, so the bar's own coordinates
 * are unchanged by the overhang rows sitting above it. */
static gfx_ctx_t bar_gfx;

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
        slot->frontmost = 0; /* filled in below - it is a property of the whole set, not of one row */
        slot->minimized = info->minimized;
        copy_label(slot->name, info->title);
        x += SLOT_W + SLOT_GAP;
        running_count++;
    }
    /* M51: whichever listed window sits highest in the z-order, ignoring
     * minimized ones (a minimized window is not in front of anything -
     * it isn't on screen at all). One pass over what was just laid out,
     * so the marker follows the same slots the loop above kept. */
    {
        int front = -1;
        int32_t best_z = -1;
        for (int32_t i = 0, k = 0; i < q.count && k < running_count; i++) {
            const wm_window_info_t *info = &q.windows[i];
            if (info->is_panel || info->is_desktop || info->window_id == self->window_id) {
                continue;
            }
            if (running_slots[k].window_id == info->window_id && !info->minimized &&
                info->z_index > best_z) {
                best_z = info->z_index;
                front = k;
            }
            k++;
        }
        if (front >= 0) {
            running_slots[front].frontmost = 1;
        }
    }
    /* A slot that scrolled out from under the cursor must not stay lit. */
    if (hovered >= running_count) {
        hovered = HOVER_NONE;
    }
}

static void draw_start_button(int pressed) {
    uint32_t bg = pressed ? START_PRESS_BG
                          : (hovered == HOVER_START ? START_HOVER_BG : START_BG);
    uint32_t glyph = pressed ? START_PRESS_GLYPH_FG : START_GLYPH_FG;
    gfx_fill_rect_rounded(&bar_gfx, START_X, BTN_Y, START_W, BTN_H, bg);
    gfx_draw_rect_rounded(&bar_gfx, START_X, BTN_Y, START_W, BTN_H, SLOT_BORDER_COLOR);

    int32_t gy = BTN_Y + (BTN_H - (2 * START_TILE + START_TILE_GAP)) / 2;
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 2; col++) {
            gfx_fill_rect(&bar_gfx,
                          START_GLYPH_X + col * (START_TILE + START_TILE_GAP),
                          gy + row * (START_TILE + START_TILE_GAP),
                          START_TILE, START_TILE, glyph);
        }
    }
    gfx_draw_text(&bar_gfx, START_TEXT_X, BTN_Y + (BTN_H - FONT_HEIGHT) / 2, "Start", LABEL_COLOR);
}

static void draw_tray(wm_window_t *self) {
    int32_t tray_x = (int32_t)self->width - TRAY_W;
    gfx_draw_line(&bar_gfx, tray_x, BTN_Y + 2, tray_x, BTN_Y + BTN_H - 3, TRAY_SEP_COLOR);

    int32_t icon_y = (PANEL_HEIGHT - TRAY_ICON) / 2;
    for (int i = 0; i < TRAY_ICONS; i++) {
        gfx_draw_rect(&bar_gfx, tray_x + TRAY_PAD + i * (TRAY_ICON + TRAY_ICON_GAP), icon_y,
                      TRAY_ICON, TRAY_ICON, TRAY_ICON_COLOR);
    }

    char clock_text[CLOCK_CHARS + 1];
    format_clock(sys_uptime_ms(), clock_text);
    gfx_draw_text(&bar_gfx, (int32_t)self->width - TRAY_PAD - CLOCK_TEXT_W,
                  (PANEL_HEIGHT - FONT_HEIGHT) / 2, clock_text, CLOCK_FG);
}

/* M45: how tall the raised region has to be for the menu as currently
 * positioned. The menu's top edge is at ctx_y (negative - above the
 * bar), so this is simply how far above the bar's own top edge it
 * reaches. Sent to the compositor only when it changes; the compositor
 * clamps it to the buffer it actually allocated, so an over-tall answer
 * degrades to "as much as exists" rather than reading past the buffer. */
static int32_t ctx_needed_overhang(void) {
    if (ctx_slot < 0) {
        return 0;
    }
    return ctx_y < 0 ? -ctx_y : 0;
}

/* The first row's verb follows the target window's live state - the same
 * choice the compositor's titlebar version of this menu makes from the
 * same field, which is why both read it rather than hardcoding a label. */
static const char *ctx_label(int i) {
    if (i == 0) {
        return running_slots[ctx_slot].minimized ? "Restore" : "Minimize";
    }
    return i == 1 ? "Close" : "Force Quit";
}

/* Drawn into the *window's* full buffer, not bar_gfx: the whole point is
 * that it is above the bar. Buffer y is PANEL_OVERHANG_MAX + the menu's
 * own (negative) bar-space y. */
static void draw_ctx_menu(wm_window_t *self) {
    int32_t by = PANEL_OVERHANG_MAX + ctx_y;
    int32_t h = CTX_ITEM_H * CTX_COUNT;
    gfx_fill_rect_rounded(&self->gfx, ctx_x, by, CTX_W, h, CTX_BG);
    gfx_draw_rect_rounded(&self->gfx, ctx_x, by, CTX_W, h, CTX_BORDER);
    for (int i = 0; i < CTX_COUNT; i++) {
        int32_t ry = by + i * CTX_ITEM_H;
        if (i == ctx_hover) {
            gfx_fill_rect_rounded(&self->gfx, ctx_x + 2, ry + 1, CTX_W - 4, CTX_ITEM_H - 2, CTX_HOVER_BG);
        }
        gfx_draw_text(&self->gfx, ctx_x + 8, ry + (CTX_ITEM_H - FONT_HEIGHT) / 2, ctx_label(i), CTX_TEXT);
    }
}

/* Which menu row is at (x, y) in bar space, or -1 if the point is
 * outside the menu. */
static int ctx_row_at(int32_t x, int32_t y) {
    if (ctx_slot < 0 ||
        !gfx_point_in_rect(x, y, ctx_x, ctx_y, CTX_W, CTX_ITEM_H * CTX_COUNT)) {
        return -1;
    }
    return (y - ctx_y) / CTX_ITEM_H;
}

static void redraw(wm_window_t *self) {
    gfx_fill_rect(&bar_gfx, 0, 0, (int32_t)self->width, PANEL_HEIGHT, PANEL_BG);
    /* Top edge is otherwise the only thing telling this panel apart from
     * the desktop it's docked to - one line makes it read as a distinct
     * bar rather than the desktop background bleeding into it. The
     * second, fainter line right under it is a one-pixel bevel highlight
     * - together they read as a lit top edge instead of a flat outline. */
    gfx_draw_line(&bar_gfx, 0, 0, (int32_t)self->width - 1, 0, PANEL_BORDER_COLOR);
    gfx_draw_line(&bar_gfx, 0, 1, (int32_t)self->width - 1, 1, PANEL_BEVEL_COLOR);

    draw_start_button(sys_uptime_ms() < start_pressed_until_ms);

    for (int i = 0; i < running_count; i++) {
        const running_slot_t *slot = &running_slots[i];
        uint32_t bg = slot->minimized ? RUNNING_SLOT_MIN_BG
                                      : (slot->focused ? RUNNING_SLOT_FOCUS_BG
                                                       : (i == hovered ? RUNNING_SLOT_HOVER_BG : RUNNING_SLOT_BG));
        uint32_t border = slot->focused ? RUNNING_SLOT_FOCUS_BORDER
                                        : (slot->frontmost ? RUNNING_SLOT_FRONT_BORDER : SLOT_BORDER_COLOR);
        gfx_fill_rect_rounded(&bar_gfx, slot->x, BTN_Y, slot->w, SLOT_H, bg);
        gfx_draw_rect_rounded(&bar_gfx, slot->x, BTN_Y, slot->w, SLOT_H, border);
        gfx_draw_text(&bar_gfx, slot->x + LABEL_PAD, BTN_Y + 4, slot->name, LABEL_COLOR);
    }

    draw_tray(self);

    /* M45: the overhang rows are cleared on every redraw and repainted
     * only if the menu is up. The compositor stops compositing them the
     * moment the overhang goes back to 0, so this is belt-and-braces -
     * but a stale menu surviving in a buffer that gets raised again is
     * exactly the kind of thing that would show up once, confusingly. */
    gfx_fill_rect(&self->gfx, 0, 0, (int32_t)self->width, PANEL_OVERHANG_MAX, PANEL_BG);
    if (ctx_slot >= 0) {
        draw_ctx_menu(self);
    }
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

/* Raises the menu above the slot that was right-clicked, clamped so it
 * cannot be drawn off the bar's own left/right edges or ask for more
 * overhang than exists. */
static void ctx_open_on(int slot, int32_t x, int32_t win_w) {
    ctx_slot = slot;
    ctx_hover = -1;
    ctx_x = x;
    if (ctx_x > win_w - CTX_W) {
        ctx_x = win_w - CTX_W;
    }
    if (ctx_x < 0) {
        ctx_x = 0;
    }
    ctx_y = -(CTX_ITEM_H * CTX_COUNT);
}

static void ctx_close(void) {
    ctx_slot = -1;
    ctx_hover = -1;
}

/* Both menu rows that end a window drive wm_send_action, exactly as the
 * bar's own left-click already does - Close is the polite verb
 * (WM_ACTION_CLOSE, which honors an app's confirm_close opt-in) and
 * Force Quit is the one that always works (WM_ACTION_KILL, which
 * deliberately does not). */
static void ctx_activate(int row) {
    if (ctx_slot < 0 || ctx_slot >= running_count) {
        ctx_close();
        return;
    }
    int32_t window_id = running_slots[ctx_slot].window_id;
    ctx_close();
    if (row == 0) {
        wm_send_action(window_id, WM_ACTION_TOGGLE_MINIMIZE);
    } else if (row == 1) {
        wm_send_action(window_id, WM_ACTION_CLOSE);
    } else if (row == 2) {
        wm_send_action(window_id, WM_ACTION_KILL);
    }
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
    /* M45: the window is PANEL_BUF_H tall, but only PANEL_HEIGHT of it
     * docks - see PANEL_OVERHANG_MAX. */
    if (wm_connect_panel(PANEL_BUF_H, PANEL_HEIGHT, &win) != 0) {
        sys_exit(1);
    }
    /* The bar's own drawing surface: the bottom PANEL_HEIGHT rows of the
     * buffer, which is where the compositor docks them. */
    bar_gfx.pixels = win.gfx.pixels + (int32_t)win.width * PANEL_OVERHANG_MAX;
    bar_gfx.width = (int32_t)win.width;
    bar_gfx.height = PANEL_HEIGHT;

    refresh_running_slots(&win);
    redraw(&win);

    long next_refresh = 0;
    for (;;) {
        wm_event_t ev;
        int changed = 0;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE) {
                changed = 1; /* M55 - see WM_EVENT_EXPOSE */
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 2)) {
                /* M45: right-click a running-app button -> its context
                 * menu. Anywhere else on the bar just dismisses one that
                 * is already up; there is nothing a right-click on the
                 * Start button or the tray would sensibly offer. */
                int hit = button_at(ev.x, ev.y);
                if (hit >= 0) {
                    ctx_open_on(hit, running_slots[hit].x, (int32_t)win.width);
                } else {
                    ctx_close();
                }
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1) && ctx_slot >= 0) {
                /* An open menu owns the next left-click outright - it
                 * either picks a row or dismisses, and never also reaches
                 * the taskbar button underneath it. Same rule every other
                 * menu in this project follows. */
                int row = ctx_row_at(ev.x, ev.y);
                if (row >= 0) {
                    ctx_activate(row);
                } else {
                    ctx_close();
                }
                refresh_running_slots(&win);
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                handle_click(ev.x, ev.y);
                refresh_running_slots(&win);
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_MOVE) {
                /* M42: a panel gets these even unfocused, and gets one
                 * final event as the cursor leaves it - see compositor.c's
                 * routing block. Redrawing only when the answer actually
                 * changes keeps a moving cursor from repainting the whole
                 * bar on every single event. */
                if (ctx_slot >= 0) {
                    int row = ctx_row_at(ev.x, ev.y);
                    if (row != ctx_hover) {
                        ctx_hover = row;
                        changed = 1;
                    }
                }
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
        /* M45: tell the compositor how far out of the bar to draw, after
         * the pixels are already there - raising the overhang first would
         * composite one frame of whatever the buffer last held. Only on a
         * change: this is a pipe write, and it runs every loop. */
        int32_t want = ctx_needed_overhang();
        if (want != ctx_overhang) {
            ctx_overhang = want;
            wm_set_panel_overhang(win.window_id, want);
        }
    }
}
