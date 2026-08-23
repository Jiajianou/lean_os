/* user_space/lib/wmclient.h
 *
 * M21: the client half of system_api/include/wm.h's window-creation and
 * event-routing protocol, factored out once a second real app (this
 * milestone's two demo GUI apps, on top of M20's wm_demo) needed the
 * exact same request/response/shm-map/event-pipe-open handshake -
 * kernel/ipc/pipe.c's own header comment on pipe_named uses the same
 * "not written speculatively ahead of a real need" reasoning for when
 * something in this project earns being pulled out like this.
 */
#pragma once

#include <stdint.h>

#include "gfx.h"
#include "wm.h" /* system_api/include/wm.h */

typedef struct {
    int32_t window_id;
    uint32_t width, height;
    gfx_ctx_t gfx; /* gfx.h context over this window's own shm-mapped pixels - draw into this directly */
    int evt_fd;     /* read end of this window's event pipe (system_api/include/wm.h's wm_event_t stream) */
} wm_window_t;

/* Requests a width x height window from the compositor (blocks on the
 * well-known request/response pipes, then opens this window's own event
 * pipe) and fills *out. title (may be NULL/empty) is the short app name
 * shown in a panel's running-window list (system_api/include/wm.h's
 * WM_TITLE_MAX) - truncated silently if longer. Returns 0 on success, -1
 * on any failure (no compositor listening, compositor rejected the
 * request, or the shm/event-pipe setup that follows failed). */
int wm_connect(uint32_t width, uint32_t height, const char *title, wm_window_t *out);

/* M22: like wm_connect, but requests a chrome-less, always-on-top panel
 * docked to the bottom of the screen (system_api/include/wm.h's
 * wm_create_request_t.panel) instead of an ordinary floating window -
 * width is the compositor's own call (always the full display), so only
 * a height is asked for here. Only user_space/bin/desktop_shell.c calls
 * this. */
int wm_connect_panel(uint32_t height, wm_window_t *out);

/* M41: the screen-*top* counterpart, for the shared menu bar. `height` is
 * the whole buffer, tall enough to paint an open dropdown into;
 * `dock_height` is the much smaller strip every window has to stay clear
 * of. Use wm_send_action_value(self, WM_ACTION_SET_PANEL_EXTENT, n) to
 * change how much of the buffer is actually shown. See
 * wm_create_request_t.panel_dock_h for why the two are separate. */
int wm_connect_panel_top(uint32_t height, uint32_t dock_height, wm_window_t *out);

/* Like wm_connect, but requests the chrome-less, full-screen, always-on-
 * *bottom* desktop background (wm_create_request_t.desktop) instead of an
 * ordinary floating window - both dimensions are the compositor's own
 * call (always the full display), so no size is asked for here at all.
 * Only user_space/bin/desktop_icons.c calls this. */
int wm_connect_desktop(wm_window_t *out);

/* M36: like wm_connect, but opts into wm_create_request_t.confirm_close -
 * a titlebar close (or external WM_ACTION_CLOSE) delivers this window a
 * WM_EVENT_CLOSE_REQUEST instead of an immediate SIGTERM, so the caller
 * gets a chance to prompt before actually exiting (text_editor.c's
 * unsaved-changes confirm dialog is the one client that needs this).
 * Everything else about connecting is identical to wm_connect. */
int wm_connect_confirm_close(uint32_t width, uint32_t height, const char *title, wm_window_t *out);

/* M22: fills *out with a snapshot of every window the compositor
 * currently knows about (system_api/include/wm.h's wm_query_response_t).
 * Returns 0, or -1 on failure (no compositor listening). */
int wm_query_windows(wm_query_response_t *out);

/* M22: asks the compositor to focus or toggle-minimize window_id (see
 * wm_action_type_t). Returns 0, or -1 on failure. */
int wm_send_action(int32_t window_id, uint32_t action);

/* M41: WM_ACTION_SET_PANEL_OVERHANG with its rect filled in - the one
 * region below a panel's docked height the compositor should also show
 * and route clicks into (a menu bar's open dropdown). A zero width or
 * height clears it. Only a panel's own client ever calls this. */
int wm_set_panel_overhang(int32_t window_id, int32_t x, int32_t w, int32_t h);

/* M41: declare (or replace) this window's menus, for the shared top menu
 * bar to show whenever this window is focused. Sticky - call it once at
 * startup, or again whenever the menus themselves change. The window_id
 * field of `menus` is ignored and filled in from `win`.
 *
 * A client that declares menus must also handle WM_EVENT_MENU_COMMAND on
 * its own event pipe; nothing happens otherwise, the same "an app that
 * doesn't implement it just doesn't respond to it" contract
 * WM_EVENT_CLOSE_REQUEST already uses. */
int wm_declare_menus(const wm_window_t *win, const wm_menu_set_t *menus);

/* M41: what the *focused* window has declared, for the menu bar to draw.
 * out->window_id is -1 when nothing focused has any menus (including
 * "nothing is focused"). menu_bar.c is the only caller. */
int wm_query_focused_menus(wm_menu_set_t *out);

/* M41: the pick coming back - the compositor turns this into a
 * WM_EVENT_MENU_COMMAND on window_id's own event pipe. menu_bar.c is the
 * only caller. */
int wm_send_menu_command(int32_t window_id, int32_t menu_index, int32_t item_index);

/* M33/M38: sets the compositor's desktop background and focused-titlebar
 * accent colors together (system_api/include/wm.h's WM_SETTINGS_PIPE,
 * wm_settings_request_t) - the compositor's only two global, non-per-
 * window settings. Returns 0, or -1 on failure. */
int wm_set_theme(uint32_t bg_color, uint32_t accent_color);

/* Blocks until the compositor routes this window an event, then fills
 * *out. Returns 0 (never fails once connected - the event pipe only ever
 * closes if the compositor itself exits, which this project's demo apps
 * don't need to handle gracefully). */
int wm_wait_event(wm_window_t *win, wm_event_t *out);

/* Non-blocking: 1 and *out filled if an event was already waiting, 0 if
 * not - for a caller (a clock-style app) that has its own reason to keep
 * running even with no input, and can't afford to block on one. */
int wm_poll_event(wm_window_t *win, wm_event_t *out);
