/* user_space/bin/desktop_icons.c
 *
 * The literal "desktop" in "desktop environment": a chrome-less, full-
 * screen background window (wmclient.h's wm_connect_desktop -
 * wm_create_request_t.desktop) drawn underneath every ordinary window and
 * panel (compositor.c's M22-mirroring addition), holding one clickable
 * icon. Double-clicking it (two left-button presses on the same icon
 * within DOUBLE_CLICK_MS of each other - there's no drag/select/rename,
 * so "double-click to launch" is the entire interaction this needs) spawns
 * user_space/bin/gui_terminal.c, the same way desktop_shell.c's launcher
 * slots spawn a program on a single click - a desktop icon is just a
 * different (and, for exactly one program, more familiar) way to reach
 * the same sys_spawn call.
 *
 * M32: ICONS[] below is the whole "multiple icons" story - a real
 * top-to-bottom, wrap-to-a-new-column grid (layout_icons) laid out once
 * at connect time from the window's own actual height (never hardcoded),
 * leaving PANEL_MARGIN clear at the bottom so an icon in the last row
 * never ends up drawn underneath desktop_shell.c's panel (which is
 * always on top regardless of connection order - compositor.c's own
 * z-order rule - so a covered icon would be genuinely unreachable, not
 * just visually crowded). Everything else (double-click detection,
 * press/hover redraw) is the same per-icon logic the single hardcoded
 * icon already had, just indexed now instead of hardcoded to one.
 */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "str.h" /* strlen - ICONS[].label is data-driven now, not a compile-time sizeof() */
#include "syscall_wrappers.h"
#include "wmclient.h"

#define BG_COLOR       0x00203040u
#define ICON_BOX_COLOR 0x004C99E6u
#define ICON_HOVER_COLOR 0x006CB9FFu
#define ICON_GLYPH_COLOR 0x000A1420u
#define LABEL_COLOR    0x00FFFFFFu

#define ICON_SIZE    48
#define ICON_MARGIN  32  /* top/left/right inset, and gap between grid cells beyond the icon+label's own footprint */
#define ICON_CELL_W  90
#define ICON_CELL_H  90
#define PANEL_MARGIN 40  /* clears desktop_shell.c's PANEL_HEIGHT(32) with room to spare - see this file's header comment on why an icon must never end up under it */
#define DOUBLE_CLICK_MS 500

typedef struct {
    const char *label;
    const char *program;
    const char *glyph; /* 2-3 chars drawn inside the icon box - this project has no separate icon-image format, see redraw_icon */
} icon_def_t;

static const icon_def_t ICONS[] = {
    {"Terminal", "gui_terminal", ">_"},
    {"Editor",   "text_editor",  "Ed"},
    {"Files",    "file_manager", "[]"},
    {"Settings", "settings",     "**"},
    {"Clock",    "gui_clock",    "()"},
    {"Paint",    "gui_paint",    "/\\"},
};
#define ICON_COUNT ((int)(sizeof(ICONS) / sizeof(ICONS[0])))

static int32_t icon_x[ICON_COUNT], icon_y[ICON_COUNT]; /* filled by layout_icons from the window's own actual geometry */

/* Top-to-bottom columns: place icons one under another until the next one
 * wouldn't clear PANEL_MARGIN above the bottom edge, then start a new
 * column back at the top - the same wrapping shape a real desktop's icon
 * grid uses. `win_h > 0` on the guard (not just "does it fit") is what
 * stops a single icon that itself doesn't fit from wrapping forever. */
static void layout_icons(int32_t win_h) {
    int32_t x = ICON_MARGIN, y = ICON_MARGIN;
    int32_t max_y = win_h - PANEL_MARGIN - ICON_SIZE - FONT_HEIGHT - 4;
    for (int i = 0; i < ICON_COUNT; i++) {
        if (y > max_y && y > ICON_MARGIN) {
            y = ICON_MARGIN;
            x += ICON_CELL_W;
        }
        icon_x[i] = x;
        icon_y[i] = y;
        y += ICON_CELL_H;
    }
}

/* M34: the box-plus-label hit region as one rect (gfx_point_in_rect,
 * user_space/lib/gfx.c) instead of hand-rolled comparisons - top edge is
 * pulled up by FONT_HEIGHT+4 and the height grown by twice that so the
 * label drawn below the box (redraw_icon) is clickable too, not just the
 * box itself. */
static int point_in_icon(int i, int32_t x, int32_t y) {
    return gfx_point_in_rect(x, y, icon_x[i], icon_y[i] - FONT_HEIGHT - 4,
                              ICON_SIZE, ICON_SIZE + 2 * (FONT_HEIGHT + 4));
}

static void redraw_icon(wm_window_t *self, int i, int pressed) {
    uint32_t box_color = pressed ? ICON_HOVER_COLOR : ICON_BOX_COLOR;
    gfx_fill_rect(&self->gfx, icon_x[i], icon_y[i], ICON_SIZE, ICON_SIZE, box_color);

    /* A tiny glyph inside the icon box: a dark "screen" rect with a
     * short mark on top, entirely gfx primitives (no separate icon-image
     * format exists in this project). */
    gfx_fill_rect(&self->gfx, icon_x[i] + 6, icon_y[i] + 6, ICON_SIZE - 12, ICON_SIZE - 16, ICON_GLYPH_COLOR);
    gfx_draw_text(&self->gfx, icon_x[i] + 10, icon_y[i] + 12, ICONS[i].glyph, box_color);

    int32_t label_w = (int32_t)strlen(ICONS[i].label) * FONT_WIDTH;
    int32_t label_x = icon_x[i] + ICON_SIZE / 2 - label_w / 2;
    gfx_draw_text(&self->gfx, label_x, icon_y[i] + ICON_SIZE + 4, ICONS[i].label, LABEL_COLOR);
}

static void redraw(wm_window_t *self, int pressed_icon) {
    gfx_fill_rect(&self->gfx, 0, 0, (int32_t)self->width, (int32_t)self->height, BG_COLOR);
    for (int i = 0; i < ICON_COUNT; i++) {
        redraw_icon(self, i, i == pressed_icon);
    }
}

int main(void) {
    wm_window_t win;
    if (wm_connect_desktop(&win) != 0) {
        sys_exit(1);
    }

    layout_icons((int32_t)win.height);

    int last_click_icon = -1;
    long last_click_ms = -1;
    long pressed_until_ms = 0;
    int pressed_icon = -1;

    redraw(&win, -1);

    for (;;) {
        wm_event_t ev;
        int changed = 0;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                for (int i = 0; i < ICON_COUNT; i++) {
                    if (!point_in_icon(i, ev.x, ev.y)) {
                        continue;
                    }
                    long now = sys_uptime_ms();
                    pressed_until_ms = now + 150;
                    pressed_icon = i;
                    if (last_click_icon == i && last_click_ms >= 0 && now - last_click_ms <= DOUBLE_CLICK_MS) {
                        sys_spawn(ICONS[i].program, "");
                        last_click_icon = -1; /* a third quick click starts a fresh pair, not a third launch */
                        last_click_ms = -1;
                    } else {
                        last_click_icon = i;
                        last_click_ms = now;
                    }
                    changed = 1;
                    break;
                }
            }
        }

        long now = sys_uptime_ms();
        int pressed = now < pressed_until_ms ? pressed_icon : -1;
        static int was_pressed_icon = -1;
        if (changed || pressed != was_pressed_icon) {
            redraw(&win, pressed);
            was_pressed_icon = pressed;
        }
    }
}
