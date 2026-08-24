#include "wmclient.h"

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
    if (sys_write(req_fds[1], &req, sizeof(req)) != (long)sizeof(req)) {
        return -1;
    }

    wm_create_response_t resp;
    if (read_exact(resp_fds[0], &resp, sizeof(resp)) != (long)sizeof(resp) || resp.shm_id < 0) {
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

int wm_wait_event(wm_window_t *win, wm_event_t *out) {
    read_exact(win->evt_fd, out, sizeof(*out));
    return 0;
}

int wm_poll_event(wm_window_t *win, wm_event_t *out) {
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
    return read_exact(win->evt_fd, out, sizeof(*out)) == (long)sizeof(*out);
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

int wm_set_panel_overhang(int32_t window_id, int32_t rows) {
    return wm_send_action_value(window_id, WM_ACTION_SET_PANEL_OVERHANG, rows);
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

int wm_set_theme(uint32_t bg_color, uint32_t accent_color, uint32_t wallpaper) {
    if (settings_fds[0] < 0) {
        if (sys_pipe_open(WM_SETTINGS_PIPE, settings_fds) != 0) {
            return -1;
        }
    }
    wm_settings_request_t req;
    req.bg_color = bg_color;
    req.accent_color = accent_color;
    req.wallpaper = wallpaper;
    return sys_write(settings_fds[1], &req, sizeof(req)) == (long)sizeof(req) ? 0 : -1;
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
