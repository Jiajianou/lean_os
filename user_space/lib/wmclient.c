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

static int connect_common(uint32_t width, uint32_t height, uint8_t panel, uint8_t desktop, wm_window_t *out) {
    int req_fds[2];
    int resp_fds[2];
    if (sys_pipe_open(WM_REQUEST_PIPE, req_fds) != 0 || sys_pipe_open(WM_RESPONSE_PIPE, resp_fds) != 0) {
        return -1;
    }

    wm_create_request_t req;
    req.width = width;
    req.height = height;
    req.panel = panel;
    req.desktop = desktop;
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

    char evt_name[8];
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

int wm_connect(uint32_t width, uint32_t height, wm_window_t *out) {
    return connect_common(width, height, 0, 0, out);
}

int wm_connect_panel(uint32_t height, wm_window_t *out) {
    return connect_common(0, height, 1, 0, out);
}

int wm_connect_desktop(wm_window_t *out) {
    return connect_common(0, 0, 0, 1, out);
}

int wm_wait_event(wm_window_t *win, wm_event_t *out) {
    read_exact(win->evt_fd, out, sizeof(*out));
    return 0;
}

int wm_poll_event(wm_window_t *win, wm_event_t *out) {
    if (sys_pipe_poll(win->evt_fd) < (long)sizeof(*out)) {
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

int wm_send_action(int32_t window_id, uint32_t action) {
    if (action_fds[0] < 0) {
        if (sys_pipe_open(WM_ACTION_PIPE, action_fds) != 0) {
            return -1;
        }
    }
    wm_action_request_t req;
    req.window_id = window_id;
    req.action = action;
    return sys_write(action_fds[1], &req, sizeof(req)) == (long)sizeof(req) ? 0 : -1;
}
