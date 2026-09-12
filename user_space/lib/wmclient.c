#include "wmclient.h"

#include "syscall.h" /* system_api/include/syscall.h - SYS_PIPE_CAPACITY, so a connect need never block on a full rendezvous pipe */
#include "syscall_wrappers.h"

/* kernel/ipc/pipe.h's pipe_read only guarantees "at least one byte, then
 * whatever else is immediately ready" - it does NOT guarantee a whole
 * struct arrives in one sys_read call, since the writer's own pipe_write
 * can be preempted mid-copy (SCHED_QUANTUM_TICKS, sched.h) and a reader
 * that's already blocked wakes as soon as the first byte lands. A single
 * sys_read call worked in practice for M20/M21's small structs (tens of
 * bytes, low odds of landing mid-write), but M22's wm_query_response_t
 * is big enough - and called often enough (desktop_shell.c's periodic
 * refresh) - that the race showed up for real during bring-up: query
 * replies started arriving short, desyncing every read after. Looping
 * until the exact byte count is in is the correct fix for any fixed-size
 * struct read off one of these pipes, not just this one call site. */
static long read_exact(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        long n = sys_read(fd, p + got, len - got);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    return (long)got;
}

/* M55: how long a client waits for a create response before assuming its
 * request went nowhere, and how many times it re-sends.
 *
 * 400ms x 12 rather than a couple of long waits, and the short interval
 * is the load-bearing half. A *live* compositor answers in milliseconds -
 * it is a poll loop whose slowest iteration is a full-screen redraw - so
 * 400ms is already a hundredfold margin against "the answer was merely
 * slow", which is the only way a re-send could produce two windows for
 * one client. What the interval actually costs is how long a reconnecting
 * client shows nothing after its request lost the race with a replacement
 * compositor clearing these pipes, and 1.5s of blank window is long
 * enough to look like the crash rather than the recovery. Twelve attempts
 * keeps the total patience near five seconds either way. */
#define WM_CONNECT_TIMEOUT_MS 400
#define WM_CONNECT_ATTEMPTS   12

static int connect_common(uint32_t width, uint32_t height, uint32_t panel_dock_h, uint8_t panel, uint8_t translucent, uint8_t desktop, uint8_t confirm_close, const char *title, wm_window_t *out) {
    int req_fds[2];
    int resp_fds[2];
    if (sys_pipe_open(WM_REQUEST_PIPE, req_fds) != 0 || sys_pipe_open(WM_RESPONSE_PIPE, resp_fds) != 0) {
        return -1;
    }

    wm_create_request_t req;
    req.width = width;
    req.height = height;
    req.panel_dock_h = panel_dock_h;
    req.panel = panel;
    req.translucent = translucent;
    req.desktop = desktop;
    req.confirm_close = confirm_close;
    req.client_pid = (int32_t)sys_getpid();
    int i = 0;
    for (; title && title[i] && i < WM_TITLE_MAX - 1; i++) {
        req.title[i] = title[i];
    }
    req.title[i] = '\0';
    /* M55: send, wait with a deadline, and send again if nothing came
     * back - rather than the single write and blocking read this was
     * until now.
     *
     * That was fine while there was only ever one compositor and it
     * started before anything else. It stops being fine the moment a
     * *replacement* one is what a client is trying to reach: a client
     * notices the death and queues its request the instant it happens,
     * while the new compositor clears these rendezvous points on startup
     * (see its own note on why - a torn message left by a process that
     * died mid-write would misalign the stream forever). The request and
     * the reset race, and a client that lost that race blocked in
     * sys_read on a response nobody was ever going to write.
     *
     * The deadline is long enough that a live compositor always answers
     * inside it - this one is a tight poll loop and replies in
     * milliseconds - so a re-send only happens when the first request
     * genuinely went nowhere, not merely when the answer was slow. */
    wm_create_response_t resp;
    int got_response = 0;
    for (int attempt = 0; attempt < WM_CONNECT_ATTEMPTS && !got_response; attempt++) {
        /* M56: room first. SYS_write to a full pipe blocks, and this is
         * the one write in the system whose reader may not exist yet -
         * the compositor might be starting, or might have died leaving a
         * rendezvous pipe with somebody's unread request still in it (a
         * named pipe outlives every process that ever held it, which is
         * the whole mechanism). A client that blocked here was stuck
         * forever with no way for init to tell: still alive, just never
         * going to draw anything. Waiting for room with a deadline, and
         * giving up if it never comes, turns that into an ordinary failed
         * connect - which the caller reports and init answers by
         * restarting the session. */
        long room_deadline = sys_uptime_ms() + WM_CONNECT_TIMEOUT_MS;
        while (sys_pipe_poll(req_fds[0]) + (long)sizeof(req) > SYS_PIPE_CAPACITY &&
               sys_uptime_ms() < room_deadline) {
            sys_yield();
        }
        if (sys_pipe_poll(req_fds[0]) + (long)sizeof(req) > SYS_PIPE_CAPACITY) {
            continue; /* still no room - try again next attempt rather than block */
        }
        if (sys_write(req_fds[1], &req, sizeof(req)) != (long)sizeof(req)) {
            sys_close(req_fds[0]);
            sys_close(req_fds[1]);
            sys_close(resp_fds[0]);
            sys_close(resp_fds[1]);
            return -1;
        }
        long deadline = sys_uptime_ms() + WM_CONNECT_TIMEOUT_MS;
        while (!got_response && sys_uptime_ms() < deadline) {
            if (sys_pipe_poll(resp_fds[0]) < (long)sizeof(resp)) {
                sys_yield();
                continue;
            }
            if (read_exact(resp_fds[0], &resp, sizeof(resp)) != (long)sizeof(resp)) {
                break;
            }
            /* M56: WM_RESPONSE_PIPE is one shared stream, so this may be
             * somebody else's answer - see wm_create_response_t.client_pid.
             * Reading and discarding it is destructive to that client,
             * which is exactly why the retry above exists: it will not get
             * its response, will time out, and will ask again. Acting on
             * an answer meant for another process, which is what happened
             * before this check, is the outcome worth avoiding - it hands
             * this client somebody else's shm segment and window id. */
            got_response = (resp.client_pid == req.client_pid);
        }
    }
    if (!got_response || resp.shm_id < 0) {
        /* The fds are the caller's to lose either way - a failed connect
         * is not a state anything here recovers into, it just tries the
         * whole handshake again from scratch. */
        sys_close(req_fds[0]);
        sys_close(req_fds[1]);
        sys_close(resp_fds[0]);
        sys_close(resp_fds[1]);
        return -1;
    }

    long vaddr = sys_shm_map(resp.shm_id);
    if (vaddr < 0) {
        return -1;
    }

    char evt_name[WM_EVENT_PIPE_NAME_LEN];
    wm_event_pipe_name(resp.window_id, evt_name);
    int evt_fds[2];
    if (sys_pipe_open(evt_name, evt_fds) != 0) {
        return -1;
    }

    out->window_id = resp.window_id;
    out->width = resp.width;   /* the compositor's actual allocation, not the request - see wm_create_response_t */
    out->height = resp.height;
    out->gfx.pixels = (uint32_t *)vaddr;
    out->gfx.width = (int32_t)resp.width;
    out->gfx.height = (int32_t)resp.height;
    out->evt_fd = evt_fds[0];
    /* M55: everything needed to do all of this again - see wm_window_t. */
    out->req_width = width;
    out->req_height = height;
    out->req_panel_dock_h = panel_dock_h;
    out->req_panel = panel;
    out->req_translucent = translucent;
    out->req_desktop = desktop;
    out->req_confirm_close = confirm_close;
    for (int t = 0; t < WM_TITLE_MAX; t++) {
        out->req_title[t] = req.title[t];
    }
    out->compositor_pid = resp.compositor_pid;
    out->shm_id = resp.shm_id;
    out->shm_bytes = (unsigned long)resp.width * resp.height * sizeof(uint32_t);

    /* M50: five fd-table slots handed straight back. The two
     * request/response pipes are a handshake - nothing here touches them
     * again once the response has been read - and this client will never
     * write to its own event pipe, so its write end was a slot claimed
     * and abandoned at birth. Five of MAX_FDS (64) per client is not
     * fatal on its own; being unable to give any of it back is the
     * shape of the bug M40 root-caused, and SYS_close (M50) is the first
     * thing in this project that could.
     *
     * Closing a *named* pipe's fd deliberately does not close the pipe -
     * see SYS_close's contract. The compositor's own ends stay exactly as
     * they were, and the next client to connect finds the same
     * rendezvous points waiting. */
    sys_close(req_fds[0]);
    sys_close(req_fds[1]);
    sys_close(resp_fds[0]);
    sys_close(resp_fds[1]);
    sys_close(evt_fds[1]);
    return 0;
}

int wm_connect(uint32_t width, uint32_t height, const char *title, wm_window_t *out) {
    return connect_common(width, height, 0, WM_PANEL_NONE, 0, 0, 0, title, out);
}

int wm_connect_panel(uint32_t height, uint32_t dock_h, wm_window_t *out) {
    /* M44: the taskbar is the one translucent surface in this system - see
     * wm_create_request_t.translucent for why that isn't simply on for
     * everything. */
    return connect_common(0, height, dock_h, WM_PANEL_BOTTOM, 1, 0, 0, "", out);
}

int wm_connect_desktop(wm_window_t *out) {
    return connect_common(0, 0, 0, WM_PANEL_NONE, 0, 1, 0, "", out);
}

int wm_connect_confirm_close(uint32_t width, uint32_t height, const char *title, wm_window_t *out) {
    return connect_common(width, height, 0, WM_PANEL_NONE, 0, 0, 1, title, out);
}

/* M55's re-handshake, split out at M58 because there are now two reasons
 * to perform it and only one of them is a dead compositor. The other is a
 * live compositor that has replaced this window's buffer because the
 * screen changed size - identical work, and deliberately so: "your pixel
 * buffer is gone, ask for a new one" is one mechanism, not two.
 *
 * Returns 1 if the client has a usable window again, 0 if not (in which
 * case it is left with a zero-sized gfx context, which every gfx.c
 * primitive clips to nothing, and the next poll tries again). */
static int rehandshake(wm_window_t *win) {
    /* Drop the old mapping *before* asking for a new one. Those frames
     * were the dead compositor's shm segment and the kernel handed them
     * back the moment it exited (shm_free_by_owner), so this mapping now
     * points at memory that belongs to whoever gets it next - and the
     * caller may still be mid-frame writing into win->gfx. Unmapping
     * turns a silent corruption into a fault, which M52 made survivable
     * and which is the right way for a bug here to present. */
    if (win->gfx.pixels && win->shm_bytes) {
        sys_shm_unmap(win->gfx.pixels, win->shm_bytes);
        win->gfx.pixels = (uint32_t *)0;
        /* Zero dimensions rather than only a null pointer, because that
         * is what actually makes drawing safe: every gfx.c primitive
         * clips to ctx->width/height before touching ctx->pixels, so a
         * client that redraws on a timer between losing its buffer and
         * getting a new one draws nothing at all instead of
         * dereferencing null. It gets the real numbers back below. */
        win->gfx.width = 0;
        win->gfx.height = 0;
    }
    /* The old event pipe's fd too: the pipe object outlives the
     * compositor (that is what a named pipe is for), and the new
     * compositor may well hand this client a different window id and
     * therefore a different pipe. */
    if (win->evt_fd >= 0) {
        sys_close(win->evt_fd);
        win->evt_fd = -1;
    }

    wm_window_t fresh;
    if (connect_common(win->req_width, win->req_height, win->req_panel_dock_h,
                        win->req_panel, win->req_translucent, win->req_desktop,
                        win->req_confirm_close, win->req_title, &fresh) != 0) {
        /* Leave compositor_pid alone so the next poll tries again. There
         * is nothing else a client with no screen can usefully do. */
        return 0;
    }
    *win = fresh;
    return 1;
}

int wm_reconnect_if_needed(wm_window_t *win) {
    /* 1 is "still running". Anything else - 0 or 2 for terminated, -1 for
     * a pid this kernel no longer knows - means the process serving this
     * window is gone. Asking about a pid rather than inferring from
     * silence is the whole point: an idle desktop is silent too. */
    if (win->compositor_pid < 0 || sys_task_alive(win->compositor_pid) == 1) {
        return 0;
    }
    return rehandshake(win);
}

int wm_wait_event(wm_window_t *win, wm_event_t *out) {
    /* M55: a poll loop, not a blocking read. It used to sit in sys_read
     * on the event pipe forever, which is fine right up until the process
     * on the other end of that pipe dies - at which point nothing will
     * ever write to it again and the client is wedged with no way to
     * notice. Going through wm_poll_event gets the compositor-liveness
     * check and the yield that makes spinning cheap, and keeps the
     * contract this function has always had: it returns when there is an
     * event, and not before. */
    for (;;) {
        if (wm_poll_event(win, out)) {
            return 0;
        }
    }
}

int wm_poll_event(wm_window_t *win, wm_event_t *out) {
    /* M55: checked here because this is the one call every GUI client's
     * main loop already makes on every iteration - so a client gets
     * compositor-crash survival without a line of its own, the same way
     * it got SYS_yield in this function when M21 needed one. */
    if (wm_reconnect_if_needed(win)) {
        /* The new window's buffer is blank, and the caller is the only
         * thing that knows what belongs in it - so tell it so, through
         * the same event stream it is already reading. Synthesized here
         * rather than sent by the compositor because the compositor has
         * no idea this window is a *re*-connection; from its side it is
         * an ordinary new client. */
        out->type = WM_EVENT_EXPOSE;
        out->x = 0;
        out->y = 0;
        out->buttons = 0;
        out->time_ms = 0;
        out->ch = 0;
        out->wheel = 0;
        return 1;
    }
    if (sys_pipe_poll(win->evt_fd) < (long)sizeof(*out)) {
        /* Every GUI client's own loop (gui_clock.c, gui_terminal.c,
         * desktop_shell.c, desktop_icons.c) is `for (;;) { while
         * (wm_poll_event(...)) {...} ... }` with no blocking call of its
         * own - nothing stops a whole 50ms scheduler quantum (sched.h's
         * SCHED_QUANTUM_TICKS) going to a client just re-checking an
         * empty pipe millions of times. Yielding here, the one place
         * every such loop already passes through on the "nothing to do"
         * result, hands the rest of that quantum back to round-robin
         * immediately instead - see SYS_yield's comment in
         * system_api/include/syscall.h for the full picture. */
        sys_yield();
        return 0;
    }
    if (read_exact(win->evt_fd, out, sizeof(*out)) != (long)sizeof(*out)) {
        /* A torn read. Yield rather than return straight away: this is
         * the "nothing usable" result, and every caller's loop treats it
         * as one - spinning on it would burn a whole scheduler quantum. */
        sys_yield();
        return 0;
    }
    /* M58: the compositor has changed the display mode and replaced this
     * window's pixel buffer to match. The re-handshake happens here
     * rather than in every client for the same reason the crash-recovery
     * one does: it is the identical work, and a client's whole obligation
     * should be "redraw everything", which is what this event then says.
     * The compositor has already stopped reading the old segment before
     * sending this, so unmapping it is safe - and it is what lets the
     * compositor free it. */
    if (out->type == WM_EVENT_DISPLAY_CHANGED) {
        rehandshake(win);
    }
    return 1;
}

/* Opened once, on this process's first query/action call, and reused
 * forever after - unlike wm_connect (called exactly once per client) a
 * desktop shell calls these repeatedly (once per redraw tick), and
 * sys_pipe_open allocates two fresh fd-table slots on every call even
 * for an already-existing named pipe, so reopening on every poll would
 * exhaust MAX_FDS (sched.h) within seconds. */
static int query_fds[2] = {-1, -1};
static int query_resp_fds[2] = {-1, -1};
static int action_fds[2] = {-1, -1};

int wm_query_windows(wm_query_response_t *out) {
    if (query_fds[0] < 0) {
        if (sys_pipe_open(WM_QUERY_PIPE, query_fds) != 0 ||
            sys_pipe_open(WM_QUERY_RESP_PIPE, query_resp_fds) != 0) {
            return -1;
        }
    }
    uint8_t ping = 1;
    if (sys_write(query_fds[1], &ping, sizeof(ping)) != (long)sizeof(ping)) {
        return -1;
    }
    return read_exact(query_resp_fds[0], out, sizeof(*out)) == (long)sizeof(*out) ? 0 : -1;
}

int wm_send_action_value(int32_t window_id, uint32_t action, int32_t value) {
    if (action_fds[0] < 0) {
        if (sys_pipe_open(WM_ACTION_PIPE, action_fds) != 0) {
            return -1;
        }
    }
    wm_action_request_t req;
    req.window_id = window_id;
    req.action = action;
    req.value = value;
    return sys_write(action_fds[1], &req, sizeof(req)) == (long)sizeof(req) ? 0 : -1;
}

int wm_send_action(int32_t window_id, uint32_t action) {
    return wm_send_action_value(window_id, action, 0);
}

int wm_present(wm_window_t *win) {
    if (!win || win->window_id < 0) {
        return -1;
    }
    return wm_send_action(win->window_id, WM_ACTION_PRESENT);
}

int wm_wait_ms(wm_window_t *win, const int *extra_fds, int n_extra, int timeout_ms) {
    int fds[1 + 8];
    int n = 0;
    if (win && win->evt_fd >= 0) {
        fds[n++] = win->evt_fd;
    }
    for (int i = 0; i < n_extra && n < (int)(sizeof(fds) / sizeof(fds[0])); i++) {
        if (extra_fds[i] >= 0) {
            fds[n++] = extra_fds[i];
        }
    }
    /* The cap is the liveness check's period (see the header). A caller
     * asking for less waits for less; a caller asking for more, or for no
     * deadline at all, is woken at the cap and loops - at 4 Hz that is a
     * rounding error next to the yield storm this replaces. */
    if (timeout_ms < 0 || timeout_ms > WM_WAIT_CAP_MS) {
        timeout_ms = WM_WAIT_CAP_MS;
    }
    long r = sys_waitfds(fds, n, timeout_ms);
    return r >= 0 ? 1 : 0;
}

int wm_set_panel_overhang(int32_t window_id, int32_t rows) {
    return wm_send_action_value(window_id, WM_ACTION_SET_PANEL_OVERHANG, rows);
}

int wm_veto_shutdown(int32_t window_id) {
    return wm_send_action(window_id, WM_ACTION_VETO_SHUTDOWN);
}

int wm_toggle_launcher(void) {
    /* -1 rather than a real id: this action never looks at one (see
     * WM_ACTION_TOGGLE_LAUNCHER), and passing the caller's own window
     * would read like it acts on that window. */
    return wm_send_action(-1, WM_ACTION_TOGGLE_LAUNCHER);
}

/* Opened once and reused, same as the query/action pipes above and for
 * the same reason: a client that notifies on every failed launch would
 * otherwise burn two fd-table slots per notification. */
static int notify_fds[2] = {-1, -1};

int wm_set_display_mode(uint32_t width, uint32_t height) {
    return wm_send_action_value(-1, WM_ACTION_SET_MODE, wm_pack_mode(width, height));
}

int wm_confirm_display_mode(void) {
    return wm_send_action_value(-1, WM_ACTION_CONFIRM_MODE, 0);
}

int wm_notify(uint32_t level, const char *title, const char *body) {
    if (notify_fds[0] < 0) {
        if (sys_pipe_open(WM_NOTIFY_PIPE, notify_fds) != 0) {
            return -1;
        }
    }
    wm_notify_request_t req;
    req.level = level;
    int i = 0;
    for (; title && title[i] && i < WM_NOTIFY_TITLE_MAX - 1; i++) {
        req.title[i] = title[i];
    }
    req.title[i] = '\0';
    i = 0;
    for (; body && body[i] && i < WM_NOTIFY_BODY_MAX - 1; i++) {
        req.body[i] = body[i];
    }
    req.body[i] = '\0';
    return sys_write(notify_fds[1], &req, sizeof(req)) == (long)sizeof(req) ? 0 : -1;
}

/* M49: opened once and reused, like every other well-known channel here.
 * Both ends of both pipes: a client can be a drag source, a drop target,
 * or (dragging a file onto its own window) both. */
static int drag_fds[2] = {-1, -1};
static int drag_data_fds[2] = {-1, -1};

int wm_drag_begin(const char *payload) {
    if (drag_fds[0] < 0) {
        if (sys_pipe_open(WM_DRAG_PIPE, drag_fds) != 0) {
            return -1;
        }
    }
    wm_drag_request_t req;
    uint32_t i = 0;
    for (; payload && payload[i] && i < WM_DRAG_PAYLOAD_MAX - 1; i++) {
        req.payload[i] = payload[i];
    }
    req.payload[i] = '\0';
    return sys_write(drag_fds[1], &req, sizeof(req)) == (long)sizeof(req) ? 0 : -1;
}

int wm_drag_payload(char *out, uint32_t max) {
    if (drag_data_fds[0] < 0) {
        if (sys_pipe_open(WM_DRAG_DATA_PIPE, drag_data_fds) != 0) {
            return -1;
        }
    }
    wm_drag_request_t req;
    if (sys_pipe_poll(drag_data_fds[0]) < (long)sizeof(req)) {
        return -1;
    }
    if (read_exact(drag_data_fds[0], &req, sizeof(req)) != (long)sizeof(req)) {
        return -1;
    }
    req.payload[WM_DRAG_PAYLOAD_MAX - 1] = '\0';
    uint32_t i = 0;
    for (; req.payload[i] && i + 1 < max; i++) {
        out[i] = req.payload[i];
    }
    out[i] = '\0';
    return 0;
}

static int settings_fds[2] = {-1, -1};
static int settings_query_fds[2] = {-1, -1};
static int settings_query_resp_fds[2] = {-1, -1};

int wm_set_settings(const wm_settings_request_t *in) {
    if (settings_fds[0] < 0) {
        if (sys_pipe_open(WM_SETTINGS_PIPE, settings_fds) != 0) {
            return -1;
        }
    }
    return sys_write(settings_fds[1], in, sizeof(*in)) == (long)sizeof(*in) ? 0 : -1;
}

int wm_set_theme(uint32_t bg_color, uint32_t accent_color, uint32_t wallpaper) {
    /* M61: settings.c is the only caller and always sends every field, so
     * this stays the three-colour name it has had since M44 and fills the
     * fourth in from the default. A caller that wants to change the
     * animation setting uses wm_set_settings, which is what having a
     * struct on the wire was always for. */
    wm_settings_request_t req;
    req.volume = 70;
    req.animations = 1;
    req.bg_color = bg_color;
    req.accent_color = accent_color;
    req.wallpaper = wallpaper;
    return wm_set_settings(&req);
}

int wm_set_taskbar_slot(int32_t window_id, int32_t x, int32_t width) {
    return wm_send_action_value(window_id, WM_ACTION_SET_TASKBAR_SLOT,
                                 (int32_t)(((uint32_t)x << 16) | ((uint32_t)width & 0xFFFFu)));
}

int wm_query_settings(wm_settings_request_t *out) {
    if (settings_query_fds[0] < 0) {
        if (sys_pipe_open(WM_SETTINGS_QUERY_PIPE, settings_query_fds) != 0 ||
            sys_pipe_open(WM_SETTINGS_QUERY_RESP_PIPE, settings_query_resp_fds) != 0) {
            return -1;
        }
    }
    uint8_t ping = 1;
    if (sys_write(settings_query_fds[1], &ping, sizeof(ping)) != (long)sizeof(ping)) {
        return -1;
    }
    return read_exact(settings_query_resp_fds[0], out, sizeof(*out)) == (long)sizeof(*out) ? 0 : -1;
}
