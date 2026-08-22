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
 * pipe) and fills *out. Returns 0 on success, -1 on any failure (no
 * compositor listening, compositor rejected the request, or the shm/
 * event-pipe setup that follows failed). */
int wm_connect(uint32_t width, uint32_t height, wm_window_t *out);

/* M22: like wm_connect, but requests a chrome-less, always-on-top panel
 * docked to the bottom of the screen (system_api/include/wm.h's
 * wm_create_request_t.panel) instead of an ordinary floating window -
 * width is the compositor's own call (always the full display), so only
 * a height is asked for here. Only user_space/bin/desktop_shell.c calls
 * this. */
int wm_connect_panel(uint32_t height, wm_window_t *out);

/* Like wm_connect, but requests the chrome-less, full-screen, always-on-
 * *bottom* desktop background (wm_create_request_t.desktop) instead of an
 * ordinary floating window - both dimensions are the compositor's own
 * call (always the full display), so no size is asked for here at all.
 * Only user_space/bin/desktop_icons.c calls this. */
int wm_connect_desktop(wm_window_t *out);

/* M22: fills *out with a snapshot of every window the compositor
 * currently knows about (system_api/include/wm.h's wm_query_response_t).
 * Returns 0, or -1 on failure (no compositor listening). */
int wm_query_windows(wm_query_response_t *out);

/* M22: asks the compositor to focus or toggle-minimize window_id (see
 * wm_action_type_t). Returns 0, or -1 on failure. */
int wm_send_action(int32_t window_id, uint32_t action);

/* Blocks until the compositor routes this window an event, then fills
 * *out. Returns 0 (never fails once connected - the event pipe only ever
 * closes if the compositor itself exits, which this project's demo apps
 * don't need to handle gracefully). */
int wm_wait_event(wm_window_t *win, wm_event_t *out);

/* Non-blocking: 1 and *out filled if an event was already waiting, 0 if
 * not - for a caller (a clock-style app) that has its own reason to keep
 * running even with no input, and can't afford to block on one. */
int wm_poll_event(wm_window_t *win, wm_event_t *out);
