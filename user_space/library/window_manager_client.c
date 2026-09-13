#include "window_manager_client.h"

#include "syscall.h"
#include "syscall_wrappers.h"

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
    wm_create_response_t resp;
    int got_response = 0;
    for (int attempt = 0; attempt < WM_CONNECT_ATTEMPTS && !got_response; attempt++) {
        long room_deadline = sys_uptime_ms() + WM_CONNECT_TIMEOUT_MS;
        while (sys_pipe_poll(req_fds[0]) + (long)sizeof(req) > SYS_PIPE_CAPACITY &&
               sys_uptime_ms() < room_deadline) {
            sys_yield();
        }
        if (sys_pipe_poll(req_fds[0]) + (long)sizeof(req) > SYS_PIPE_CAPACITY) {
            continue;
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
            got_response = (resp.client_pid == req.client_pid);
        }
    }
    if (!got_response || resp.shm_id < 0) {
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
    out->width = resp.width;
    out->height = resp.height;
    out->gfx.pixels = (uint32_t *)vaddr;
    out->gfx.width = (int32_t)resp.width;
    out->gfx.height = (int32_t)resp.height;
    out->evt_fd = evt_fds[0];
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
    return connect_common(0, height, dock_h, WM_PANEL_BOTTOM, 1, 0, 0, "", out);
}

int wm_connect_desktop(wm_window_t *out) {
    return connect_common(0, 0, 0, WM_PANEL_NONE, 0, 1, 0, "", out);
}

int wm_connect_confirm_close(uint32_t width, uint32_t height, const char *title, wm_window_t *out) {
    return connect_common(width, height, 0, WM_PANEL_NONE, 0, 0, 1, title, out);
}

static int rehandshake(wm_window_t *win) {
    if (win->gfx.pixels && win->shm_bytes) {
        sys_shm_unmap(win->gfx.pixels, win->shm_bytes);
        win->gfx.pixels = (uint32_t *)0;
        win->gfx.width = 0;
        win->gfx.height = 0;
    }
    if (win->evt_fd >= 0) {
        sys_close(win->evt_fd);
        win->evt_fd = -1;
    }

    wm_window_t fresh;
    if (connect_common(win->req_width, win->req_height, win->req_panel_dock_h,
                        win->req_panel, win->req_translucent, win->req_desktop,
                        win->req_confirm_close, win->req_title, &fresh) != 0) {
        return 0;
    }
    *win = fresh;
    return 1;
}

int wm_reconnect_if_needed(wm_window_t *win) {
    if (win->compositor_pid < 0 || sys_task_alive(win->compositor_pid) == 1) {
        return 0;
    }
    return rehandshake(win);
}

int wm_wait_event(wm_window_t *win, wm_event_t *out) {
    for (;;) {
        if (wm_poll_event(win, out)) {
            return 0;
        }
    }
}

int wm_poll_event(wm_window_t *win, wm_event_t *out) {
    if (wm_reconnect_if_needed(win)) {
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
        sys_yield();
        return 0;
    }
    if (read_exact(win->evt_fd, out, sizeof(*out)) != (long)sizeof(*out)) {
        sys_yield();
        return 0;
    }
    if (out->type == WM_EVENT_DISPLAY_CHANGED) {
        rehandshake(win);
    }
    return 1;
}

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
    return wm_send_action(-1, WM_ACTION_TOGGLE_LAUNCHER);
}

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
