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
 */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "syscall_wrappers.h"
#include "wmclient.h"

#define BG_COLOR       0x00203040u
#define ICON_BOX_COLOR 0x004C99E6u
#define ICON_HOVER_COLOR 0x006CB9FFu
#define ICON_GLYPH_COLOR 0x000A1420u
#define LABEL_COLOR    0x00FFFFFFu

#define ICON_X       40
#define ICON_Y       40
#define ICON_SIZE    48
#define LABEL_TEXT   "Terminal"
#define DOUBLE_CLICK_MS 500

static int32_t icon_x, icon_y; /* set from the window's own geometry once connected - fixed offsets above, not size-dependent */

static int point_in_icon(int32_t x, int32_t y) {
    return x >= icon_x && x < icon_x + ICON_SIZE &&
           y >= icon_y - FONT_HEIGHT - 4 && y < icon_y + ICON_SIZE + FONT_HEIGHT + 4;
}

static void redraw(wm_window_t *self, int pressed) {
    gfx_fill_rect(&self->gfx, 0, 0, (int32_t)self->width, (int32_t)self->height, BG_COLOR);

    uint32_t box_color = pressed ? ICON_HOVER_COLOR : ICON_BOX_COLOR;
    gfx_fill_rect(&self->gfx, icon_x, icon_y, ICON_SIZE, ICON_SIZE, box_color);

    /* A tiny "terminal window" glyph inside the icon box: a dark screen
     * rect with a ">" prompt-style mark, entirely gfx primitives (no
     * separate icon image format exists in this project). */
    gfx_fill_rect(&self->gfx, icon_x + 6, icon_y + 6, ICON_SIZE - 12, ICON_SIZE - 16, ICON_GLYPH_COLOR);
    gfx_draw_text(&self->gfx, icon_x + 10, icon_y + 12, ">_", box_color);

    int32_t label_w = (int32_t)(sizeof(LABEL_TEXT) - 1) * FONT_WIDTH;
    int32_t label_x = icon_x + ICON_SIZE / 2 - label_w / 2;
    gfx_draw_text(&self->gfx, label_x, icon_y + ICON_SIZE + 4, LABEL_TEXT, LABEL_COLOR);
}

int main(void) {
    wm_window_t win;
    if (wm_connect_desktop(&win) != 0) {
        sys_exit(1);
    }

    icon_x = ICON_X;
    icon_y = ICON_Y;

    long last_click_ms = -1;
    long pressed_until_ms = 0;
    int was_pressed = 0;

    redraw(&win, 0);

    for (;;) {
        wm_event_t ev;
        int changed = 0;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1) && point_in_icon(ev.x, ev.y)) {
                long now = sys_uptime_ms();
                pressed_until_ms = now + 150;
                if (last_click_ms >= 0 && now - last_click_ms <= DOUBLE_CLICK_MS) {
                    sys_spawn("gui_terminal", "");
                    last_click_ms = -1; /* a third quick click starts a fresh pair, not a third launch */
                } else {
                    last_click_ms = now;
                }
                changed = 1;
            }
        }

        long now = sys_uptime_ms();
        int pressed = now < pressed_until_ms;
        if (changed || pressed != was_pressed) {
            redraw(&win, pressed);
            was_pressed = pressed;
        }
    }
}
