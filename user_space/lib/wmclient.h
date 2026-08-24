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

    /* ---- M55: everything needed to do this again ----------------------
     *
     * A compositor that dies used to take every client with it, because a
     * window was the only thing holding them: the pixels a client draws
     * into are the compositor's shm segment, its events come down the
     * compositor's pipe, and neither survives. Nothing about that is
     * fixable from the compositor's side - it is dead.
     *
     * What makes it fixable from *here* is that a client already supplies
     * everything a window is made of. The geometry, the flags and the
     * title came from this process in the first place, and the pixels it
     * redraws every frame anyway. So the connect handshake is re-runnable
     * as long as its arguments are kept - which is all the fields below
     * are. `compositor_pid` is how the client knows to re-run it (see
     * wm_create_response_t.compositor_pid); `shm_id`/`shm_bytes` are how
     * it drops the mapping to the dead compositor's freed frames instead
     * of leaving it aliasing whatever those frames become next.
     *
     * Written once at connect and never by a caller. */
    uint32_t req_width, req_height, req_panel_dock_h;
    uint8_t req_panel, req_translucent, req_desktop, req_confirm_close;
    char req_title[WM_TITLE_MAX];
    int32_t compositor_pid;
    int32_t shm_id;
    unsigned long shm_bytes;
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
/* M45: `height` is now the whole buffer the panel allocates and `dock_h`
 * how much of it actually docks at the bottom; the difference is the
 * overhang it can raise a menu into (wm_set_panel_overhang). Pass
 * dock_h == height for a panel that has no such thing. */
int wm_connect_panel(uint32_t height, uint32_t dock_h, wm_window_t *out);

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

/* M45: the same call for the one action that carries a number
 * (wm_action_request_t.value). wm_send_action is this with value 0 -
 * kept as its own name because every caller but one passes no value at
 * all and reading `wm_send_action(id, WM_ACTION_CLOSE, 0)` at fifteen
 * call sites would say less, not more. */
int wm_send_action_value(int32_t window_id, uint32_t action, int32_t value);

/* M45: asks the compositor to composite and click-route `rows` of this
 * panel's buffer above its docked strip - see
 * WM_ACTION_SET_PANEL_OVERHANG. 0 puts it away. Only desktop_shell.c
 * calls this. */
int wm_set_panel_overhang(int32_t window_id, int32_t rows);

/* M48: raises a transient toast on the compositor's own notification
 * surface (system_api/include/wm.h's WM_NOTIFY_PIPE). One-way and
 * fire-and-forget - there is no reply and no way to ask whether it was
 * shown, which is the whole contract: a notification that its sender had
 * to wait on would be a dialog. `level` is WM_NOTIFY_INFO/WARN/ERROR and
 * changes only the accent color. Both strings are truncated silently.
 * Returns 0, or -1 if there is no compositor listening. */
int wm_notify(uint32_t level, const char *title, const char *body);

/* M49: announces that this client has begun dragging `payload` (a
 * filename - see WM_DRAG_PAYLOAD_MAX). The compositor holds it until the
 * left button comes up and then delivers WM_EVENT_DROP to whatever window
 * the cursor is over, which may well be this one. There is no "drag end"
 * call: the button coming up is the end, and the compositor sees that
 * itself - so a source that crashes mid-drag cannot leave one stuck.
 * Returns 0, or -1 if there is no compositor listening. */
int wm_drag_begin(const char *payload);

/* M49: the payload of the drop this client has just been told about
 * (WM_EVENT_DROP). Only meaningful immediately on receiving that event -
 * the compositor writes the payload just before sending it. Copies at
 * most `max` bytes including the NUL. Returns 0, or -1 if nothing was
 * waiting. */
int wm_drag_payload(char *out, uint32_t max);

/* M42: shows/hides the compositor's launcher overlay (M43). No window_id -
 * this is the one action that acts on the compositor rather than on a
 * window (see WM_ACTION_TOGGLE_LAUNCHER). desktop_shell.c's Start button
 * is the only caller. */
int wm_toggle_launcher(void);

/* M33/M38/M44: sets the compositor's three global, non-per-window
 * settings together (system_api/include/wm.h's WM_SETTINGS_PIPE,
 * wm_settings_request_t) - background color, focused-titlebar accent, and
 * which wallpaper style the desktop paints. All three at once because a
 * caller that sent only the one control it just touched would reset the
 * other two to whatever it happened to believe they were; settings.c
 * queries first (wm_query_settings) so what it sends back is the real
 * current state with one field changed. Returns 0, or -1 on failure. */
int wm_set_theme(uint32_t bg_color, uint32_t accent_color, uint32_t wallpaper);

/* M44: the read side of the same three settings. desktop_icons.c polls it
 * (it paints the wallpaper, so it has to be told which one) and
 * settings.c calls it once at startup so its own controls open showing
 * the real current choice. Returns 0, or -1 on failure. */
int wm_query_settings(wm_settings_request_t *out);

/* Blocks until the compositor routes this window an event, then fills
 * *out. Returns 0 (never fails once connected - the event pipe only ever
 * closes if the compositor itself exits, which this project's demo apps
 * don't need to handle gracefully). */
int wm_wait_event(wm_window_t *win, wm_event_t *out);

/* Non-blocking: 1 and *out filled if an event was already waiting, 0 if
 * not - for a caller (a clock-style app) that has its own reason to keep
 * running even with no input, and can't afford to block on one. */
int wm_poll_event(wm_window_t *win, wm_event_t *out);

/* M55: has the compositor serving this window died, and if so, has a new
 * one been connected to?
 *
 * Called automatically from wm_poll_event, so an ordinary client gets
 * this for free and needs to know nothing about it. Returns 1 if a
 * reconnect just happened - which is the caller's cue to redraw, since
 * `win->gfx` now points at a brand-new (and blank) pixel buffer. Every
 * client in this project already redraws on demand, so for most of them
 * the return value is worth ignoring; the ones with an expensive frame
 * use it to redraw immediately rather than at their next timer tick.
 *
 * Blocks inside the handshake if a new compositor is not up *yet* - the
 * request sits in the well-known named pipe until one reads it, which is
 * exactly the rendezvous those pipes are for, and there is nothing
 * useful for a client with no screen to be doing in the meantime. */
int wm_reconnect_if_needed(wm_window_t *win);
