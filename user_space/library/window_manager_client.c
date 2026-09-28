#include "window_manager_client.h"

#include "syscall.h"
#include "syscall_wrappers.h"

static long read_exact(int fd, void *buffer, size_t length) {
    uint8_t *p = (uint8_t *)buffer;
    size_t got = 0;
    while (got < length) {
        long n = sys_read(fd, p + got, length - got);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    return (long)got;
}

static long read_exact_before(int fd, void *buffer, size_t length, long deadline) {
    uint8_t *p = (uint8_t *)buffer;
    size_t got = 0;
    while (got < length) {
        long n = sys_read(fd, p + got, length - got);
        if (n == -OS_ERROR_AGAIN) {
            if (sys_uptime_ms() >= deadline) {
                return (long)got;
            }
            sys_yield();
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    return (long)got;
}

#define WINDOW_MANAGER_CONNECT_TIMEOUT_MS 400
#define WINDOW_MANAGER_CONNECT_ATTEMPTS   12

static int connect_common(uint32_t width, uint32_t height, uint32_t panel_dock_h, uint8_t panel, uint8_t translucent, uint8_t desktop, uint8_t confirm_close, const char *title, uint8_t popup, int32_t parent_window_id, int32_t popup_x, int32_t popup_y, window_manager_window_t *out) {
    int request_file_descriptors[2];
    int response_file_descriptors[2];
    if (sys_pipe_open(WINDOW_MANAGER_REQUEST_PIPE, request_file_descriptors) != 0 || sys_pipe_open(WINDOW_MANAGER_RESPONSE_PIPE, response_file_descriptors) != 0) {
        return -1;
    }
    /* Non-blocking, because the reply that made the poll below say "ready"
       can be read by another client before this one reads it. A blocking read
       then waited for a reply that was never coming when the compositor's
       startup reset had already thrown this client's request away - it never
       reached the re-send that exists for exactly that, and on a laptop with
       eight processors desktop_icons slept there for good. */
    sys_fcntl(response_file_descriptors[0], F_SETFL_COMMAND, OS_NONBLOCK_BIT);

    window_manager_create_request_t request;
    request.width = width;
    request.height = height;
    request.panel_dock_h = panel_dock_h;
    request.panel = panel;
    request.translucent = translucent;
    request.desktop = desktop;
    request.confirm_close = confirm_close;
    request.popup = popup;
    request.parent_window_id = parent_window_id;
    request.popup_x = popup_x;
    request.popup_y = popup_y;
    request.client_pid = (int32_t)sys_getpid();
    int i = 0;
    for (; title && title[i] && i < WINDOW_MANAGER_TITLE_MAX - 1; i++) {
        request.title[i] = title[i];
    }
    request.title[i] = '\0';
    window_manager_create_response_t response;
    int got_response = 0;
    for (int attempt = 0; attempt < WINDOW_MANAGER_CONNECT_ATTEMPTS && !got_response; attempt++) {
        long room_deadline = sys_uptime_ms() + WINDOW_MANAGER_CONNECT_TIMEOUT_MS;
        while (sys_pipe_poll(request_file_descriptors[0]) + (long)sizeof(request) > SYS_PIPE_CAPACITY &&
               sys_uptime_ms() < room_deadline) {
            sys_yield();
        }
        if (sys_pipe_poll(request_file_descriptors[0]) + (long)sizeof(request) > SYS_PIPE_CAPACITY) {
            continue;
        }
        if (sys_write(request_file_descriptors[1], &request, sizeof(request)) != (long)sizeof(request)) {
            sys_close(request_file_descriptors[0]);
            sys_close(request_file_descriptors[1]);
            sys_close(response_file_descriptors[0]);
            sys_close(response_file_descriptors[1]);
            return -1;
        }
        long deadline = sys_uptime_ms() + WINDOW_MANAGER_CONNECT_TIMEOUT_MS;
        while (!got_response && sys_uptime_ms() < deadline) {
            if (sys_pipe_poll(response_file_descriptors[0]) < (long)sizeof(response)) {
                sys_yield();
                continue;
            }
            long got = read_exact_before(response_file_descriptors[0], &response, sizeof(response), deadline);
            if (got == 0) {
                continue;
            }
            if (got != (long)sizeof(response)) {
                break;
            }
            got_response = (response.client_pid == request.client_pid);
            /* Every client shares this pipe, so the reply just read may be
               another's. Dropping it left that client waiting on an answer
               that had already been taken, and whichever of desktop_icons and
               desktop_shell lost the race - a boot slow enough for them to
               overlap, a USB stick on real hardware - had no window at all.
               It goes back for its owner unless its owner is gone. */
            if (!got_response && sys_task_alive(response.client_pid) > 0) {
                sys_write(response_file_descriptors[1], &response, sizeof(response));
                sys_yield();
            }
        }
    }
    if (!got_response || response.shared_memory_id < 0) {
        sys_close(request_file_descriptors[0]);
        sys_close(request_file_descriptors[1]);
        sys_close(response_file_descriptors[0]);
        sys_close(response_file_descriptors[1]);
        return -1;
    }

    long vaddr = sys_shared_memory_map(response.shared_memory_id);
    if (vaddr < 0) {
        return -1;
    }

    char evt_name[WINDOW_MANAGER_EVENT_PIPE_NAME_LENGTH];
    window_manager_event_pipe_name(response.window_id, evt_name);
    int evt_file_descriptors[2];
    if (sys_pipe_open(evt_name, evt_file_descriptors) != 0) {
        return -1;
    }

    out->window_id = response.window_id;
    out->width = response.width;
    out->height = response.height;
    out->graphics.pixels = (uint32_t *)vaddr;
    out->graphics.width = (int32_t)response.width;
    out->graphics.height = (int32_t)response.height;
    out->evt_file_descriptor = evt_file_descriptors[0];
    out->request_width = width;
    out->request_height = height;
    out->request_panel_dock_h = panel_dock_h;
    out->request_panel = panel;
    out->request_translucent = translucent;
    out->request_desktop = desktop;
    out->request_confirm_close = confirm_close;
    for (int t = 0; t < WINDOW_MANAGER_TITLE_MAX; t++) {
        out->request_title[t] = request.title[t];
    }
    out->compositor_pid = response.compositor_pid;
    out->shared_memory_id = response.shared_memory_id;
    out->shared_memory_bytes = (unsigned long)response.width * response.height * sizeof(uint32_t);

    sys_close(request_file_descriptors[0]);
    sys_close(request_file_descriptors[1]);
    sys_close(response_file_descriptors[0]);
    sys_close(response_file_descriptors[1]);
    sys_close(evt_file_descriptors[1]);
    return 0;
}

int window_manager_connect(uint32_t width, uint32_t height, const char *title, window_manager_window_t *out) {
    return connect_common(width, height, 0, WINDOW_MANAGER_PANEL_NONE, 0, 0, 0, title, 0, -1, 0, 0, out);
}

int window_manager_connect_panel(uint32_t height, uint32_t dock_h, window_manager_window_t *out) {
    return connect_common(0, height, dock_h, WINDOW_MANAGER_PANEL_BOTTOM, 1, 0, 0, "", 0, -1, 0, 0, out);
}

int window_manager_connect_desktop(window_manager_window_t *out) {
    return connect_common(0, 0, 0, WINDOW_MANAGER_PANEL_NONE, 0, 1, 0, "", 0, -1, 0, 0, out);
}

int window_manager_connect_confirm_close(uint32_t width, uint32_t height, const char *title, window_manager_window_t *out) {
    return connect_common(width, height, 0, WINDOW_MANAGER_PANEL_NONE, 0, 0, 1, title, 0, -1, 0, 0, out);
}

static int rehandshake(window_manager_window_t *win) {
    if (win->graphics.pixels && win->shared_memory_bytes) {
        sys_shared_memory_unmap(win->graphics.pixels, win->shared_memory_bytes);
        win->graphics.pixels = (uint32_t *)0;
        win->graphics.width = 0;
        win->graphics.height = 0;
    }
    if (win->evt_file_descriptor >= 0) {
        sys_close(win->evt_file_descriptor);
        win->evt_file_descriptor = -1;
    }

    window_manager_window_t fresh;
    if (connect_common(win->request_width, win->request_height, win->request_panel_dock_h,
                        win->request_panel, win->request_translucent, win->request_desktop,
                        win->request_confirm_close, win->request_title, 0, -1, 0, 0, &fresh) != 0) {
        return 0;
    }
    *win = fresh;
    return 1;
}

int window_manager_connect_popup(int32_t parent_window_id, int32_t x, int32_t y, uint32_t width, uint32_t height, window_manager_window_t *out) {
    return connect_common(width, height, 0, 0, 0, 0, 0, "", 1, parent_window_id, x, y, out);
}

int window_manager_set_capture(window_manager_window_t *win, int captured) {
    if (!win || win->window_id < 0) {
        return -1;
    }
    return window_manager_send_action(win->window_id, captured ? WINDOW_MANAGER_ACTION_CAPTURE
                                                               : WINDOW_MANAGER_ACTION_RELEASE_CAPTURE);
}

int window_manager_reconnect_if_needed(window_manager_window_t *win) {
    if (win->compositor_pid < 0 || sys_task_alive(win->compositor_pid) == 1) {
        return 0;
    }
    return rehandshake(win);
}

int window_manager_wait_event(window_manager_window_t *win, window_manager_event_t *out) {
    for (;;) {
        if (window_manager_poll_event(win, out)) {
            return 0;
        }
    }
}

int window_manager_poll_event(window_manager_window_t *win, window_manager_event_t *out) {
    if (window_manager_reconnect_if_needed(win)) {
        out->type = WINDOW_MANAGER_EVENT_EXPOSE;
        out->x = 0;
        out->y = 0;
        out->buttons = 0;
        out->time_ms = 0;
        out->ch = 0;
        out->wheel = 0;
        return 1;
    }
    if (sys_pipe_poll(win->evt_file_descriptor) < (long)sizeof(*out)) {
        sys_yield();
        return 0;
    }
    if (read_exact(win->evt_file_descriptor, out, sizeof(*out)) != (long)sizeof(*out)) {
        sys_yield();
        return 0;
    }
    if (out->type == WINDOW_MANAGER_EVENT_DISPLAY_CHANGED) {
        rehandshake(win);
    }
    return 1;
}

static int query_file_descriptors[2] = {-1, -1};
static int query_response_file_descriptors[2] = {-1, -1};
static int action_file_descriptors[2] = {-1, -1};

int window_manager_query_windows(window_manager_query_response_t *out) {
    if (query_file_descriptors[0] < 0) {
        if (sys_pipe_open(WINDOW_MANAGER_QUERY_PIPE, query_file_descriptors) != 0 ||
            sys_pipe_open(WINDOW_MANAGER_QUERY_RESPONSE_PIPE, query_response_file_descriptors) != 0) {
            return -1;
        }
    }
    uint8_t ping = 1;
    if (sys_write(query_file_descriptors[1], &ping, sizeof(ping)) != (long)sizeof(ping)) {
        return -1;
    }
    return read_exact(query_response_file_descriptors[0], out, sizeof(*out)) == (long)sizeof(*out) ? 0 : -1;
}

int window_manager_send_action_value(int32_t window_id, uint32_t action, int32_t value) {
    if (action_file_descriptors[0] < 0) {
        if (sys_pipe_open(WINDOW_MANAGER_ACTION_PIPE, action_file_descriptors) != 0) {
            return -1;
        }
    }
    window_manager_action_request_t request;
    request.window_id = window_id;
    request.action = action;
    request.value = value;
    return sys_write(action_file_descriptors[1], &request, sizeof(request)) == (long)sizeof(request) ? 0 : -1;
}

int window_manager_send_action(int32_t window_id, uint32_t action) {
    return window_manager_send_action_value(window_id, action, 0);
}

int window_manager_present(window_manager_window_t *win) {
    if (!win || win->window_id < 0) {
        return -1;
    }
    return window_manager_send_action(win->window_id, WINDOW_MANAGER_ACTION_PRESENT);
}

int window_manager_wait_ms(window_manager_window_t *win, const int *extra_file_descriptors, int n_extra, int timeout_ms) {
    int file_descriptors[1 + 8];
    int n = 0;
    if (win && win->evt_file_descriptor >= 0) {
        file_descriptors[n++] = win->evt_file_descriptor;
    }
    for (int i = 0; i < n_extra && n < (int)(sizeof(file_descriptors) / sizeof(file_descriptors[0])); i++) {
        if (extra_file_descriptors[i] >= 0) {
            file_descriptors[n++] = extra_file_descriptors[i];
        }
    }
    if (timeout_ms < 0 || timeout_ms > WINDOW_MANAGER_WAIT_CAP_MS) {
        timeout_ms = WINDOW_MANAGER_WAIT_CAP_MS;
    }
    long r = sys_waitfds(file_descriptors, n, timeout_ms);
    return r >= 0 ? 1 : 0;
}

int window_manager_set_panel_overhang(int32_t window_id, int32_t rows) {
    return window_manager_send_action_value(window_id, WINDOW_MANAGER_ACTION_SET_PANEL_OVERHANG, rows);
}

int window_manager_veto_shutdown(int32_t window_id) {
    return window_manager_send_action(window_id, WINDOW_MANAGER_ACTION_VETO_SHUTDOWN);
}

int window_manager_toggle_launcher(void) {
    return window_manager_send_action(-1, WINDOW_MANAGER_ACTION_TOGGLE_LAUNCHER);
}

static int notify_file_descriptors[2] = {-1, -1};

int window_manager_set_display_mode(uint32_t width, uint32_t height) {
    return window_manager_send_action_value(-1, WINDOW_MANAGER_ACTION_SET_MODE, window_manager_pack_mode(width, height));
}

int window_manager_confirm_display_mode(void) {
    return window_manager_send_action_value(-1, WINDOW_MANAGER_ACTION_CONFIRM_MODE, 0);
}

int window_manager_notify(uint32_t level, const char *title, const char *body) {
    if (notify_file_descriptors[0] < 0) {
        if (sys_pipe_open(WINDOW_MANAGER_NOTIFY_PIPE, notify_file_descriptors) != 0) {
            return -1;
        }
    }
    window_manager_notify_request_t request;
    request.level = level;
    int i = 0;
    for (; title && title[i] && i < WINDOW_MANAGER_NOTIFY_TITLE_MAX - 1; i++) {
        request.title[i] = title[i];
    }
    request.title[i] = '\0';
    i = 0;
    for (; body && body[i] && i < WINDOW_MANAGER_NOTIFY_BODY_MAX - 1; i++) {
        request.body[i] = body[i];
    }
    request.body[i] = '\0';
    return sys_write(notify_file_descriptors[1], &request, sizeof(request)) == (long)sizeof(request) ? 0 : -1;
}

static int drag_file_descriptors[2] = {-1, -1};
static int drag_data_file_descriptors[2] = {-1, -1};

int window_manager_drag_begin(const char *payload) {
    if (drag_file_descriptors[0] < 0) {
        if (sys_pipe_open(WINDOW_MANAGER_DRAG_PIPE, drag_file_descriptors) != 0) {
            return -1;
        }
    }
    window_manager_drag_request_t request;
    uint32_t i = 0;
    for (; payload && payload[i] && i < WINDOW_MANAGER_DRAG_PAYLOAD_MAX - 1; i++) {
        request.payload[i] = payload[i];
    }
    request.payload[i] = '\0';
    return sys_write(drag_file_descriptors[1], &request, sizeof(request)) == (long)sizeof(request) ? 0 : -1;
}

int window_manager_drag_payload(char *out, uint32_t max) {
    if (drag_data_file_descriptors[0] < 0) {
        if (sys_pipe_open(WINDOW_MANAGER_DRAG_DATA_PIPE, drag_data_file_descriptors) != 0) {
            return -1;
        }
    }
    window_manager_drag_request_t request;
    if (sys_pipe_poll(drag_data_file_descriptors[0]) < (long)sizeof(request)) {
        return -1;
    }
    if (read_exact(drag_data_file_descriptors[0], &request, sizeof(request)) != (long)sizeof(request)) {
        return -1;
    }
    request.payload[WINDOW_MANAGER_DRAG_PAYLOAD_MAX - 1] = '\0';
    uint32_t i = 0;
    for (; request.payload[i] && i + 1 < max; i++) {
        out[i] = request.payload[i];
    }
    out[i] = '\0';
    return 0;
}

static int settings_file_descriptors[2] = {-1, -1};
static int settings_query_file_descriptors[2] = {-1, -1};
static int settings_query_response_file_descriptors[2] = {-1, -1};

int window_manager_set_settings(const window_manager_settings_request_t *in) {
    if (settings_file_descriptors[0] < 0) {
        if (sys_pipe_open(WINDOW_MANAGER_SETTINGS_PIPE, settings_file_descriptors) != 0) {
            return -1;
        }
    }
    return sys_write(settings_file_descriptors[1], in, sizeof(*in)) == (long)sizeof(*in) ? 0 : -1;
}

int window_manager_set_theme(uint32_t bg_color, uint32_t accent_color, uint32_t wallpaper) {
    window_manager_settings_request_t request;
    request.volume = 70;
    request.animations = 1;
    request.bg_color = bg_color;
    request.accent_color = accent_color;
    request.wallpaper = wallpaper;
    return window_manager_set_settings(&request);
}

int window_manager_set_taskbar_slot(int32_t window_id, int32_t x, int32_t width) {
    return window_manager_send_action_value(window_id, WINDOW_MANAGER_ACTION_SET_TASKBAR_SLOT,
                                 (int32_t)(((uint32_t)x << 16) | ((uint32_t)width & 0xFFFFu)));
}

int window_manager_query_settings(window_manager_settings_request_t *out) {
    if (settings_query_file_descriptors[0] < 0) {
        if (sys_pipe_open(WINDOW_MANAGER_SETTINGS_QUERY_PIPE, settings_query_file_descriptors) != 0 ||
            sys_pipe_open(WINDOW_MANAGER_SETTINGS_QUERY_RESPONSE_PIPE, settings_query_response_file_descriptors) != 0) {
            return -1;
        }
    }
    uint8_t ping = 1;
    if (sys_write(settings_query_file_descriptors[1], &ping, sizeof(ping)) != (long)sizeof(ping)) {
        return -1;
    }
    return read_exact(settings_query_response_file_descriptors[0], out, sizeof(*out)) == (long)sizeof(*out) ? 0 : -1;
}
