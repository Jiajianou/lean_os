/* user_space/bin/menu_bar.c
 *
 * M41: the macOS-shaped half of "menu bar top, dock bottom" - a
 * screen-top-docked panel (wm_connect_panel_top) showing the focused
 * app's name on the left, that app's own menus next to it, and the
 * system clock on the right.
 *
 * The clock moved here from desktop_shell.c's bottom-right corner. It
 * was already the one unmistakably-a-taskbar element there; on a real
 * desktop of this shape it belongs at the menu bar's right edge, and
 * moving it frees that corner of the dock for M42.
 *
 * The menus are the interesting part. M35 established that a menu is
 * drawn client-side, inside the window that owns it - deliberately, to
 * avoid a compositor-owned overlay. A shared menu bar has to break that
 * in exactly one way: the labels are drawn by *this* process, not by the
 * app they belong to. So the split is drawn as narrowly as possible -
 * the owning app still declares what its menus are (wm_declare_menus)
 * and still decides what picking an item does (WM_EVENT_MENU_COMMAND);
 * all that crosses the boundary is a list of strings going out and an
 * index coming back. Neither this file nor the compositor knows what
 * "Save As" means. See system_api/include/wm.h for the protocol.
 *
 * Two things about being a panel shape this file:
 *
 *   * A dropdown is taller than the bar. This window's *buffer* is tall
 *     enough for the deepest possible menu, but only MENU_BAR_H rows of
 *     it are painted to the screen until a menu opens
 *     (WM_ACTION_SET_PANEL_EXTENT). The height every other window has to
 *     stay clear of stays MENU_BAR_H throughout - opening a menu must
 *     not shove the whole desktop down and then pull it back up.
 *
 *   * Clicking a panel focuses it (compositor.c's hit-test checks panels
 *     first, since they're drawn on top). That would drop the very app
 *     whose menus are on screen, so an open menu holds onto the owner's
 *     window id and menu set rather than re-querying while it's open,
 *     and focus is handed back to that app once the menu closes.
 */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "str.h"      /* strlen */
#include "syscall_wrappers.h"
#include "wmclient.h"

#define MENU_BAR_H 24
#define MENU_ITEM_W 132
#define MENU_ITEM_H (FONT_HEIGHT + 4)
/* The buffer this panel allocates: the bar itself plus the deepest
 * dropdown the protocol allows (WM_MENU_MAX_ITEMS), so no menu can ever
 * be too tall to paint. Only MENU_BAR_H of it is on screen at rest. */
#define PANEL_BUF_H (MENU_BAR_H + WM_MENU_MAX_ITEMS * MENU_ITEM_H + 2)

#define TEXT_Y ((MENU_BAR_H - FONT_HEIGHT) / 2)
#define APP_NAME_X 10
#define TITLE_PAD 10 /* horizontal padding either side of a menu title's clickable box */

#define BAR_BG        0x001E2233u
#define BAR_BORDER    0x00445566u /* 1px bottom edge - the bar's only separation from the desktop below it */
#define BAR_BEVEL     0x002A3145u
#define APP_NAME_FG   0x00FFFFFFu
#define MENU_TITLE_FG 0x00C8D4E4u
#define MENU_OPEN_BG  0x003A5A80u
#define CLOCK_FG      0x00C8D4E4u

#define DROP_BG     0x00243040u
#define DROP_HOVER  0x003A5A80u
#define DROP_BORDER 0x00506070u
#define DROP_TEXT   0x00FFFFFFu

#define CLOCK_TEXT_W (5 * FONT_WIDTH)
#define CLOCK_PAD    10
#define CLOCK_AREA_W (CLOCK_PAD * 2 + CLOCK_TEXT_W)

#define REFRESH_INTERVAL_MS 300

/* What the bar is currently showing. While a menu is open this is
 * deliberately *not* refreshed - see this file's header comment on why
 * clicking the bar can't be allowed to change it out from under an open
 * dropdown. */
static wm_menu_set_t menus;
static char focused_app[WM_TITLE_MAX];

static int open_menu = -1;      /* index into menus.menus[], or -1 */
static int32_t menu_title_x[WM_MENU_MAX_MENUS];
static int32_t menu_title_w[WM_MENU_MAX_MENUS];

/* "MM:SS" of uptime - this project has no RTC/wall-clock source (see
 * gui_clock.c for the same note). Lifted verbatim from desktop_shell.c
 * along with the clock itself rather than left behind as a second copy;
 * minutes wrap at 100 so the field never outgrows CLOCK_TEXT_W. */
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

/* Where each menu title's clickable box sits, laid out left to right
 * after the app name. Recomputed whenever the menu set changes rather
 * than cached across it - the titles themselves change with focus. */
static void layout_menu_titles(void) {
    int32_t x = APP_NAME_X + (int32_t)strlen(focused_app) * FONT_WIDTH + 24;
    for (int i = 0; i < menus.menu_count; i++) {
        int32_t w = (int32_t)strlen(menus.menus[i].title) * FONT_WIDTH + 2 * TITLE_PAD;
        menu_title_x[i] = x;
        menu_title_w[i] = w;
        x += w;
    }
}

static int32_t open_menu_height(void) {
    if (open_menu < 0) {
        return 0;
    }
    return menus.menus[open_menu].item_count * MENU_ITEM_H + 2;
}

static void redraw(wm_window_t *self) {
    gfx_fill_rect(&self->gfx, 0, 0, (int32_t)self->width, MENU_BAR_H, BAR_BG);
    gfx_draw_line(&self->gfx, 0, MENU_BAR_H - 1, (int32_t)self->width - 1, MENU_BAR_H - 1, BAR_BORDER);
    gfx_draw_line(&self->gfx, 0, MENU_BAR_H - 2, (int32_t)self->width - 1, MENU_BAR_H - 2, BAR_BEVEL);

    gfx_draw_text(&self->gfx, APP_NAME_X, TEXT_Y, focused_app, APP_NAME_FG);

    for (int i = 0; i < menus.menu_count; i++) {
        if (i == open_menu) {
            gfx_fill_rect(&self->gfx, menu_title_x[i], 0, menu_title_w[i], MENU_BAR_H - 2, MENU_OPEN_BG);
        }
        gfx_draw_text(&self->gfx, menu_title_x[i] + TITLE_PAD, TEXT_Y,
                      menus.menus[i].title, MENU_TITLE_FG);
    }

    char clock_text[6];
    format_clock(sys_uptime_ms(), clock_text);
    gfx_draw_text(&self->gfx, (int32_t)self->width - CLOCK_AREA_W + CLOCK_PAD, TEXT_Y,
                  clock_text, CLOCK_FG);

    if (open_menu >= 0) {
        const wm_menu_t *menu = &menus.menus[open_menu];
        /* gfx_draw_menu takes an array of pointers; wm_menu_t stores its
         * items as fixed-width rows (it has to - it travels through a
         * pipe). One small stack array bridges the two rather than
         * giving gfx.h a second, strided variant of the same helper. */
        const char *items[WM_MENU_MAX_ITEMS];
        for (int i = 0; i < menu->item_count; i++) {
            items[i] = menu->items[i];
        }
        gfx_draw_menu(&self->gfx, menu_title_x[open_menu], MENU_BAR_H,
                      MENU_ITEM_W, MENU_ITEM_H, items, menu->item_count, -1,
                      DROP_BG, DROP_HOVER, DROP_BORDER, DROP_TEXT);
    }
}

/* Tells the compositor exactly which rect below the bar an open dropdown
 * occupies - not the full panel width, which would paint an opaque band
 * across everything either side of the menu. Always called *after*
 * redraw() has already painted the new state into the shm buffer, so the
 * compositor never shows a pixel this process hasn't drawn yet. */
static void sync_panel_overhang(wm_window_t *self) {
    if (open_menu < 0) {
        wm_set_panel_overhang(self->window_id, 0, 0, 0);
        return;
    }
    wm_set_panel_overhang(self->window_id, menu_title_x[open_menu],
                           MENU_ITEM_W, open_menu_height());
}

/* Pulls the focused app's name and menus from the compositor. Skipped
 * entirely while a menu is open - see this file's header comment. */
static void refresh_focus(wm_window_t *self) {
    if (open_menu >= 0) {
        return;
    }
    wm_query_response_t q;
    focused_app[0] = '\0';
    if (wm_query_windows(&q) == 0) {
        for (int32_t i = 0; i < q.count; i++) {
            const wm_window_info_t *info = &q.windows[i];
            if (info->is_panel || info->is_desktop || !info->focused || info->minimized) {
                continue;
            }
            strlcpy(focused_app, info->title, sizeof(focused_app));
            break;
        }
    }
    /* An empty bar still says something: with no app focused, the
     * desktop itself is what's in front. */
    if (focused_app[0] == '\0') {
        strlcpy(focused_app, "Desktop", sizeof(focused_app));
    }

    if (wm_query_focused_menus(&menus) != 0 || menus.window_id < 0) {
        menus.window_id = -1;
        menus.menu_count = 0;
    }
    (void)self;
    layout_menu_titles();
}

static void close_menu(wm_window_t *self, int refocus_owner) {
    int32_t owner = menus.window_id;
    open_menu = -1;
    redraw(self);
    sync_panel_overhang(self);
    /* Hand focus back to the app the menu belonged to: clicking this
     * panel took it (compositor.c hit-tests panels first), and a menu
     * bar that leaves your app deactivated behind it isn't one. */
    if (refocus_owner && owner >= 0) {
        wm_send_action(owner, WM_ACTION_FOCUS);
    }
}

static void handle_click(wm_window_t *self, int32_t x, int32_t y) {
    if (open_menu >= 0) {
        /* An open menu owns the next click outright - it either picks an
         * item or dismisses. Same rule text_editor.c's File menu and
         * desktop_icons.c's context menu already follow. */
        int idx = gfx_menu_hit_test(x, y, menu_title_x[open_menu], MENU_BAR_H,
                                     MENU_ITEM_W, MENU_ITEM_H,
                                     menus.menus[open_menu].item_count);
        int32_t owner = menus.window_id;
        int picked_menu = open_menu;
        close_menu(self, 1);
        if (idx >= 0 && owner >= 0) {
            wm_send_menu_command(owner, picked_menu, idx);
        }
        return;
    }

    for (int i = 0; i < menus.menu_count; i++) {
        if (gfx_point_in_rect(x, y, menu_title_x[i], 0, menu_title_w[i], MENU_BAR_H)) {
            open_menu = i;
            redraw(self);
            sync_panel_overhang(self);
            return;
        }
    }
}

int main(void) {
    wm_window_t win;
    if (wm_connect_panel_top(PANEL_BUF_H, MENU_BAR_H, &win) != 0) {
        sys_exit(1);
    }

    /* Paint before asking to be shown at bar height: the compositor
     * blits whatever is in the buffer, and PANEL_BUF_H rows of it are
     * nominally live until this first call lands. */
    refresh_focus(&win);
    redraw(&win);
    sync_panel_overhang(&win);

    long next_refresh = 0;
    for (;;) {
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                handle_click(&win, ev.x, ev.y);
            } else if (ev.type == WM_EVENT_UNFOCUS && open_menu >= 0) {
                /* Something else took focus while a menu was open (a
                 * click on a window, Alt+Tab) - the menu is stale, so
                 * drop it rather than leaving a dropdown hanging over a
                 * different app's window. No refocus: whoever just took
                 * focus is meant to have it. */
                close_menu(&win, 0);
            }
        }

        long now = sys_uptime_ms();
        if (now >= next_refresh) {
            next_refresh = now + REFRESH_INTERVAL_MS;
            refresh_focus(&win);
            redraw(&win);
        }
        sys_yield();
    }
}
