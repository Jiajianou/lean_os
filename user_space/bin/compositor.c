/* user_space/bin/compositor.c
 *
 * M20 gave this process exclusive ownership of the real framebuffer and
 * a way for exactly one client to get a window on it. M21 makes it a
 * real (if still small) window manager: any number of clients (up to
 * MAX_WINDOWS) can connect over the process's whole lifetime rather than
 * just once, the newest connection or a left-click on a window takes
 * input focus (focus-follows-click), and every routed keyboard/mouse
 * event goes out over that window's own event pipe (system_api/include/
 * wm.h's wm_event_t, user_space/lib/wmclient.h on the client side)
 * instead of nothing at all.
 *
 * Accepting connections is now non-blocking (SYS_pipe_poll before ever
 * calling the blocking SYS_read) so the same loop that accepts new
 * windows also drains mouse/keyboard input and redraws - M20's version
 * could get away with a single blocking accept because it only ever
 * needed to serve one client, once.
 *
 * M22 adds one more kind of client: a "panel" (wm_create_request_t's
 * panel flag - user_space/bin/desktop_shell.c is the only one that ever
 * sets it), which docks full-width to the bottom of the screen with no
 * border/titlebar chrome and always draws on top of every ordinary
 * window regardless of connection order, plus a query/action protocol
 * (system_api/include/wm.h's WM_QUERY_PIPE/WM_ACTION_PIPE) so a panel
 * can see every other window and focus/minimize one - the two things an
 * ordinary client's own per-window event pipe was never meant to do.
 *
 * A "desktop" client (wm_create_request_t.desktop - user_space/bin/
 * desktop_icons.c is the only one that ever sets it) is a panel's mirror
 * image: also chrome-less and full-screen, but drawn *first*, underneath
 * every ordinary window and panel, and only ever wins a click hit-test
 * that nothing else on screen claimed - the background layer a desktop
 * icon gets drawn on and double-clicked through gaps in other windows.
 */
#include "str.h"
#include "syscall_wrappers.h"
#include "wm.h"

#define MAX_WINDOWS          8
#define TITLEBAR_H           20
#define BORDER               2
#define BG_COLOR             0x001A1A2Eu
#define BORDER_COLOR         0x00444466u
#define TITLEBAR_COLOR       0x00335577u
#define TITLEBAR_FOCUS_COLOR 0x004C99E6u
#define CURSOR_COLOR         0x00FFFFFFu
#define CURSOR_SIZE          8
#define REDRAW_INTERVAL_MS   100 /* fallback cadence for changes the compositor has no way to notice itself - a client (M21's clock demo) redrawing its own window's pixels with no input involved at all. Input-driven changes no longer wait on this - see `dirty`, below. */

typedef struct {
    int32_t x, y, w, h;
    uint32_t *pixels;
    int evt_write_fd; /* write end of this window's own event pipe - see wm_event_pipe_name */
    uint8_t is_panel;  /* M22: chrome-less, always-on-top, screen-bottom-docked - see wm_create_request_t.panel */
    uint8_t is_desktop; /* chrome-less, full-screen, always-on-*bottom* - see wm_create_request_t.desktop */
    uint8_t minimized; /* M22: hidden from redraw() and from click hit-testing, but the client process keeps running (WM_ACTION_TOGGLE_MINIMIZE) */
    char title[WM_TITLE_MAX]; /* echoed straight from wm_create_request_t.title into wm_window_info_t.title on every query - see accept_pending_query */
} window_t;

static window_t windows[MAX_WINDOWS];
static int window_count;
static int focused_window = -1; /* -1 = nothing focused yet */

static wm_fb_info_t fb_info;
static uint32_t *real_fb;       /* the live, scanned-out hardware framebuffer - write-only, touched only by present() */
static uint32_t fb_pitch_pixels;

/* Off-screen render target: every draw call in redraw() (fill_rect,
 * blit_window, draw_cursor) targets this, never real_fb directly.
 * Drawing straight onto the hardware framebuffer - clearing it, then
 * blitting windows back over the clear, one syscall's worth of pixels
 * at a time - let the display scan out those intermediate, half-drawn
 * frames, which is what the visible flicker was. Compositing into back_buf
 * first and copying the whole finished frame to real_fb in one pass
 * (present(), below) means the hardware only ever shows complete frames. */
static uint32_t *back_buf;
static uint32_t back_pitch_pixels; /* == fb_info.width - back_buf is allocated tightly packed, no pitch padding */

static int32_t cursor_x, cursor_y;
static uint8_t prev_buttons;
static int32_t last_drawn_cursor_x, last_drawn_cursor_y; /* cursor position as of the last redraw - compared against cursor_x/y each loop to detect motion needing a (cheap, cursor-sized) partial redraw */

/* Set whenever something changed that a small cursor-sized partial
 * redraw can't account for on its own - a window was created/focused/
 * minimized, or the periodic fallback (below) fired - so the next
 * redraw must recomposite the *whole* screen rather than just the
 * cursor's old/new footprint. Plain cursor movement (the common,
 * highest-frequency case) is handled separately - see
 * last_drawn_cursor_x/y above and the main loop below - specifically
 * because folding it into this flag would mean every mouse-move event
 * re-drew and re-presented the entire screen, by far the largest cost
 * of any single redraw, for a change that only ever touches an 8x8
 * pixel box. */
static int dirty = 1; /* starts dirty: draw the first frame */

/* Every draw call below (fill_rect/blit_window/draw_cursor, all via
 * put_pixel) is clipped to this rect, and present() only ever copies
 * this same rect to the real framebuffer - see redraw_rect(), the only
 * place that sets it. A full redraw sets it to the whole screen; a
 * cursor-only partial redraw sets it to just the cursor's old/new
 * bounding box, so neither the compositing passes nor the final copy
 * to real_fb do any more work than the actual change requires. */
static int32_t clip_x0, clip_y0, clip_x1, clip_y1;

static inline int32_t min_i32(int32_t a, int32_t b) {
    return a < b ? a : b;
}

static inline int32_t max_i32(int32_t a, int32_t b) {
    return a > b ? a : b;
}

/* Same silhouette as kernel/drivers/cursor.c's arrow - reimplemented
 * here rather than shared, since that file is kernel-only and this
 * process has no way to link against it. */
static const uint8_t cursor_shape[CURSOR_SIZE] = {
    0b10000000,
    0b11000000,
    0b10100000,
    0b10010000,
    0b10001000,
    0b10111000,
    0b11000100,
    0b10000100,
};

/* See user_space/lib/wmclient.c's identical helper for why a single
 * sys_read isn't safe for a multi-byte struct off a pipe - the same
 * partial-write-preemption race applies to this side of the request/
 * action pipes too, not just the client side. */
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

/* No bounds/clip check of its own - every caller (fill_rect/blit_window/
 * draw_cursor below) already intersects against clip_x0..clip_y1 (itself
 * always clamped to the real screen - see redraw_rect) before ever
 * computing an (x, y) to pass in here, so doing it again per-pixel would
 * just be redundant branching on the hottest loop in this process. */
static inline void put_pixel(int32_t x, int32_t y, uint32_t color) {
    back_buf[(uint32_t)y * back_pitch_pixels + (uint32_t)x] = color;
}

/* Copies just the current clip rect from back_buf to the real hardware
 * framebuffer - the only place this process ever writes to real_fb. A
 * full redraw's clip rect is the whole screen; a cursor-only partial
 * redraw's is a handful of rows a few pixels wide, so this ends up doing
 * anywhere from "the whole frame" down to "next to nothing" depending on
 * what redraw_rect was actually asked to recomposite. */
static void present(void) {
    for (int32_t y = clip_y0; y < clip_y1; y++) {
        memcpy(&real_fb[(uint32_t)y * fb_pitch_pixels + (uint32_t)clip_x0],
               &back_buf[(uint32_t)y * back_pitch_pixels + (uint32_t)clip_x0],
               (size_t)(clip_x1 - clip_x0) * sizeof(uint32_t));
    }
}

static void fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    int32_t x0 = max_i32(x, clip_x0);
    int32_t y0 = max_i32(y, clip_y0);
    int32_t x1 = min_i32(x + w, clip_x1);
    int32_t y1 = min_i32(y + h, clip_y1);
    for (int32_t row = y0; row < y1; row++) {
        for (int32_t col = x0; col < x1; col++) {
            put_pixel(col, row, color);
        }
    }
}

static void blit_window(const window_t *win) {
    int32_t x0 = max_i32(win->x, clip_x0);
    int32_t y0 = max_i32(win->y, clip_y0);
    int32_t x1 = min_i32(win->x + win->w, clip_x1);
    int32_t y1 = min_i32(win->y + win->h, clip_y1);
    for (int32_t row = y0; row < y1; row++) {
        const uint32_t *src_row = win->pixels + (uint32_t)(row - win->y) * (uint32_t)win->w;
        for (int32_t col = x0; col < x1; col++) {
            put_pixel(col, row, src_row[col - win->x]);
        }
    }
}

static void draw_cursor(void) {
    int32_t x0 = max_i32(cursor_x, clip_x0);
    int32_t y0 = max_i32(cursor_y, clip_y0);
    int32_t x1 = min_i32(cursor_x + CURSOR_SIZE, clip_x1);
    int32_t y1 = min_i32(cursor_y + CURSOR_SIZE, clip_y1);
    for (int32_t row = y0; row < y1; row++) {
        uint8_t bits = cursor_shape[row - cursor_y];
        for (int32_t col = x0; col < x1; col++) {
            if (bits & (0x80 >> (col - cursor_x))) {
                put_pixel(col, row, CURSOR_COLOR);
            }
        }
    }
}

/* Recomposites and re-presents only [x0,x1) x [y0,y1) (clamped to the
 * real screen) rather than assuming the whole display - see clip_x0..
 * clip_y1's own comment above for why: a cursor moving is by far the
 * most frequent reason this runs, and it only ever needs an 8x8-ish box
 * touched, not a full-screen clear/recomposite/copy every single time.
 * Every draw call in here still walks the same z-order a full redraw
 * would (desktop, then windows, then panels, then cursor) - correctness
 * doesn't depend on how big the clip rect is, only speed does. */
static void redraw_rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    clip_x0 = max_i32(x0, 0);
    clip_y0 = max_i32(y0, 0);
    clip_x1 = min_i32(x1, (int32_t)fb_info.width);
    clip_y1 = min_i32(y1, (int32_t)fb_info.height);
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) {
        return;
    }

    fill_rect(0, 0, (int32_t)fb_info.width, (int32_t)fb_info.height, BG_COLOR);
    /* Desktop windows first (a background layer under everything else -
     * the opposite end of the z-order from panels below), then ordinary
     * windows in creation order (no z-order raise on focus - a later
     * connection or click just changes titlebar color, not paint order;
     * see M20/M21's own notes on this simplification), then panels last
     * so they're always on top regardless of when they connected - the
     * one piece of z-ordering this compositor does enforce, since a
     * taskbar that could be occluded wouldn't be much of a taskbar. */
    for (int i = 0; i < window_count; i++) {
        if (windows[i].is_desktop) {
            blit_window(&windows[i]); /* no border/titlebar - a desktop background is its own chrome, same as a panel */
        }
    }
    for (int i = 0; i < window_count; i++) {
        const window_t *win = &windows[i];
        if (win->is_panel || win->is_desktop || win->minimized) {
            continue;
        }
        uint32_t titlebar_color = (i == focused_window) ? TITLEBAR_FOCUS_COLOR : TITLEBAR_COLOR;
        fill_rect(win->x - BORDER, win->y - TITLEBAR_H - BORDER,
                  win->w + 2 * BORDER, win->h + TITLEBAR_H + 2 * BORDER, BORDER_COLOR);
        fill_rect(win->x, win->y - TITLEBAR_H, win->w, TITLEBAR_H, titlebar_color);
        blit_window(win);
    }
    for (int i = 0; i < window_count; i++) {
        if (windows[i].is_panel) {
            blit_window(&windows[i]); /* no border/titlebar - a panel is its own chrome */
        }
    }
    draw_cursor();
    present();
}

static void redraw(void) {
    redraw_rect(0, 0, (int32_t)fb_info.width, (int32_t)fb_info.height);
}

/* Titlebar counts as part of a window's clickable/routable area, same as
 * its content - a real WM lets you drag/focus by the titlebar too. A
 * panel or desktop background has no titlebar (both are undecorated), so
 * its clickable area is just its own content rect. */
static int point_in_window(const window_t *win, int32_t x, int32_t y) {
    int32_t top = (win->is_panel || win->is_desktop) ? win->y : win->y - TITLEBAR_H;
    return x >= win->x && x < win->x + win->w &&
           y >= top && y < win->y + win->h;
}

static void send_event(const window_t *win, const wm_event_t *ev) {
    sys_write(win->evt_write_fd, ev, sizeof(*ev));
}

static void set_focus(int idx) {
    if (focused_window == idx) {
        return;
    }
    if (focused_window >= 0) {
        wm_event_t ev = {0};
        ev.type = WM_EVENT_UNFOCUS;
        send_event(&windows[focused_window], &ev);
    }
    focused_window = idx;
    if (focused_window >= 0) {
        wm_event_t ev = {0};
        ev.type = WM_EVENT_FOCUS;
        send_event(&windows[focused_window], &ev);
    }
    dirty = 1;
}

/* Non-blocking: only touches the request pipe (and does the one
 * necessarily-blocking-in-practice SYS_read, guaranteed immediate since
 * SYS_pipe_poll already confirmed a full request is buffered) when a
 * whole wm_create_request_t is actually waiting. A brand-new window
 * takes focus immediately, same as most real window managers. */
static void accept_pending_window(int req_read_fd, int resp_write_fd) {
    if (sys_pipe_poll(req_read_fd) < (long)sizeof(wm_create_request_t)) {
        return;
    }
    wm_create_request_t req;
    long n = read_exact(req_read_fd, &req, sizeof(req));
    wm_create_response_t resp;
    if (n != (long)sizeof(req) || window_count >= MAX_WINDOWS) {
        resp.window_id = -1;
        resp.shm_id = -1;
        sys_write(resp_write_fd, &resp, sizeof(resp));
        return;
    }

    /* A panel's width, or a desktop background's width and height, are
     * never the requester's call - always the full display, so each
     * genuinely spans edge to edge regardless of what the client happens
     * to ask for. */
    uint32_t width = (req.panel || req.desktop) ? fb_info.width : req.width;
    uint32_t height = req.desktop ? fb_info.height : req.height;

    long shm_id = sys_shm_create((size_t)width * height * sizeof(uint32_t));
    long vaddr = shm_id < 0 ? -1 : sys_shm_map(shm_id);
    if (shm_id < 0 || vaddr < 0) {
        resp.window_id = -1;
        resp.shm_id = -1;
        sys_write(resp_write_fd, &resp, sizeof(resp));
        return;
    }

    int idx = window_count;
    char evt_name[8];
    wm_event_pipe_name(idx, evt_name);
    int evt_fds[2];
    if (sys_pipe_open(evt_name, evt_fds) != 0) {
        resp.window_id = -1;
        resp.shm_id = -1;
        sys_write(resp_write_fd, &resp, sizeof(resp));
        return;
    }

    window_t *win = &windows[idx];
    if (req.panel) {
        win->x = 0;
        win->y = (int32_t)fb_info.height - (int32_t)height;
    } else if (req.desktop) {
        win->x = 0;
        win->y = 0;
    } else {
        win->x = 100 + idx * 40;
        win->y = 100 + idx * 40;
    }
    win->w = (int32_t)width;
    win->h = (int32_t)height;
    win->pixels = (uint32_t *)vaddr;
    win->evt_write_fd = evt_fds[1];
    win->is_panel = req.panel;
    win->is_desktop = req.desktop;
    win->minimized = 0;
    int ti = 0;
    for (; req.title[ti] && ti < WM_TITLE_MAX - 1; ti++) {
        win->title[ti] = req.title[ti];
    }
    win->title[ti] = '\0';

    resp.window_id = idx;
    resp.shm_id = (int32_t)shm_id;
    resp.width = width;
    resp.height = height;
    window_count++;
    sys_write(resp_write_fd, &resp, sizeof(resp));
    set_focus(idx);
}

/* M22: fills a wm_query_response_t from the live windows[] array on
 * every call rather than caching one - window_count is at most
 * MAX_WINDOWS (8), so this is cheap enough to just do it fresh whenever
 * asked. */
static void accept_pending_query(int query_read_fd, int query_resp_write_fd) {
    if (sys_pipe_poll(query_read_fd) < 1) {
        return;
    }
    uint8_t ping;
    sys_read(query_read_fd, &ping, sizeof(ping));

    wm_query_response_t resp;
    resp.count = window_count;
    for (int i = 0; i < window_count; i++) {
        const window_t *win = &windows[i];
        resp.windows[i].window_id = i;
        resp.windows[i].x = win->x;
        resp.windows[i].y = win->y;
        resp.windows[i].w = win->w;
        resp.windows[i].h = win->h;
        resp.windows[i].focused = (i == focused_window);
        resp.windows[i].minimized = win->minimized;
        resp.windows[i].is_panel = win->is_panel;
        resp.windows[i].is_desktop = win->is_desktop;
        memcpy(resp.windows[i].title, win->title, WM_TITLE_MAX);
    }
    sys_write(query_resp_write_fd, &resp, sizeof(resp));
}

static void accept_pending_action(int action_read_fd) {
    if (sys_pipe_poll(action_read_fd) < (long)sizeof(wm_action_request_t)) {
        return;
    }
    wm_action_request_t req;
    if (read_exact(action_read_fd, &req, sizeof(req)) != (long)sizeof(req)) {
        return;
    }
    if (req.window_id < 0 || req.window_id >= window_count) {
        return;
    }
    window_t *win = &windows[req.window_id];
    if (req.action == WM_ACTION_FOCUS) {
        win->minimized = 0;
        set_focus(req.window_id);
    } else if (req.action == WM_ACTION_TOGGLE_MINIMIZE) {
        win->minimized = !win->minimized;
        if (win->minimized && focused_window == req.window_id) {
            set_focus(-1);
        }
        dirty = 1;
    }
}

static void handle_mouse(void) {
    mouse_event_t mev;
    while (sys_mouse_read(&mev)) {
        /* Cursor motion itself isn't `dirty = 1` (full redraw) - see
         * last_drawn_cursor_x/y's comment. A click that changes focus
         * still goes through set_focus() below, which sets `dirty`
         * itself. */
        cursor_x += mev.dx;
        cursor_y += mev.dy;
        if (cursor_x < 0) {
            cursor_x = 0;
        }
        if (cursor_y < 0) {
            cursor_y = 0;
        }
        if (cursor_x >= (int32_t)fb_info.width) {
            cursor_x = (int32_t)fb_info.width - 1;
        }
        if (cursor_y >= (int32_t)fb_info.height) {
            cursor_y = (int32_t)fb_info.height - 1;
        }

        int left_down_edge = (mev.buttons & 1) && !(prev_buttons & 1);
        if (left_down_edge) {
            /* Panels are checked first (they're drawn on top, so they'd
             * visually win any overlap anyway) and a minimized window
             * can't be clicked - there's nothing on screen to click. */
            int hit = -1;
            for (int i = window_count - 1; i >= 0; i--) {
                if (windows[i].is_panel && !windows[i].minimized && point_in_window(&windows[i], cursor_x, cursor_y)) {
                    hit = i;
                    break;
                }
            }
            if (hit < 0) {
                for (int i = window_count - 1; i >= 0; i--) {
                    if (!windows[i].is_panel && !windows[i].is_desktop && !windows[i].minimized &&
                        point_in_window(&windows[i], cursor_x, cursor_y)) {
                        hit = i;
                        break;
                    }
                }
            }
            /* Desktop background checked last, at the very bottom of the
             * z-order - only ever wins a click that landed on empty
             * desktop, nothing else on screen. */
            if (hit < 0) {
                for (int i = window_count - 1; i >= 0; i--) {
                    if (windows[i].is_desktop && !windows[i].minimized && point_in_window(&windows[i], cursor_x, cursor_y)) {
                        hit = i;
                        break;
                    }
                }
            }
            if (hit >= 0) {
                set_focus(hit);
            }
        }

        if (focused_window >= 0) {
            const window_t *win = &windows[focused_window];
            wm_event_t ev = {0};
            ev.type = WM_EVENT_MOUSE_MOVE;
            ev.x = cursor_x - win->x;
            ev.y = cursor_y - win->y;
            ev.buttons = mev.buttons;
            send_event(win, &ev);
            if (mev.buttons != prev_buttons) {
                ev.type = WM_EVENT_MOUSE_BUTTON;
                send_event(win, &ev);
            }
        }
        prev_buttons = mev.buttons;
    }
}

static void handle_keyboard(void) {
    char ch;
    while (sys_kbd_read(&ch)) {
        if (focused_window >= 0) {
            wm_event_t ev = {0};
            ev.type = WM_EVENT_KEY;
            ev.ch = ch;
            send_event(&windows[focused_window], &ev);
        }
    }
}

int main(void) {
    if (sys_fb_info(&fb_info) != 0) {
        sys_exit(1);
    }
    long fb_vaddr = sys_fb_map();
    if (fb_vaddr < 0) {
        sys_exit(1);
    }
    real_fb = (uint32_t *)fb_vaddr;
    fb_pitch_pixels = fb_info.pitch / sizeof(uint32_t);

    long back_shm_id = sys_shm_create((size_t)fb_info.width * fb_info.height * sizeof(uint32_t));
    long back_vaddr = back_shm_id < 0 ? -1 : sys_shm_map(back_shm_id);
    if (back_vaddr < 0) {
        sys_exit(1);
    }
    back_buf = (uint32_t *)back_vaddr;
    back_pitch_pixels = fb_info.width;

    cursor_x = (int32_t)(fb_info.width / 2);
    cursor_y = (int32_t)(fb_info.height / 2);

    int req_fds[2];
    int resp_fds[2];
    if (sys_pipe_open(WM_REQUEST_PIPE, req_fds) != 0 || sys_pipe_open(WM_RESPONSE_PIPE, resp_fds) != 0) {
        sys_exit(1);
    }
    int query_fds[2];
    int query_resp_fds[2];
    int action_fds[2];
    if (sys_pipe_open(WM_QUERY_PIPE, query_fds) != 0 || sys_pipe_open(WM_QUERY_RESP_PIPE, query_resp_fds) != 0 ||
        sys_pipe_open(WM_ACTION_PIPE, action_fds) != 0) {
        sys_exit(1);
    }

    /* Every message this process (or any client) prints to stdout goes
     * through the kernel's own graphical console (M17) - the same
     * framebuffer this process is compositing onto. Printed once, before
     * the loop, for the same reason M20's version stopped printing after
     * its one client connected: a console scroll mid- or post-frame
     * would shift whatever's already been drawn before anything can
     * verify it landed correctly. */
    const char msg[] = "[compositor] framebuffer mapped, accepting windows.\n";
    sys_write(1, msg, strlen(msg));

    redraw(); /* first frame: empty desktop + cursor, before any client connects */
    dirty = 0;
    last_drawn_cursor_x = cursor_x;
    last_drawn_cursor_y = cursor_y;

    long last_redraw_ms = sys_uptime_ms();
    for (;;) {
        accept_pending_window(req_fds[0], resp_fds[1]);
        accept_pending_query(query_fds[0], query_resp_fds[1]);
        accept_pending_action(action_fds[0]);
        handle_mouse();
        handle_keyboard();

        long now = sys_uptime_ms();
        if (dirty || now - last_redraw_ms >= REDRAW_INTERVAL_MS) {
            redraw();
            dirty = 0;
            last_redraw_ms = now;
            last_drawn_cursor_x = cursor_x;
            last_drawn_cursor_y = cursor_y;
        } else if (cursor_x != last_drawn_cursor_x || cursor_y != last_drawn_cursor_y) {
            /* Nothing else changed - just recomposite the small box the
             * cursor has moved through (old footprint union new one)
             * instead of the whole screen. This is the hot path: every
             * mouse-move event used to force a full-screen redraw +
             * present, by far the largest cost in this process, for a
             * change that only ever touches an 8x8 pixel box. */
            int32_t x0 = min_i32(last_drawn_cursor_x, cursor_x);
            int32_t y0 = min_i32(last_drawn_cursor_y, cursor_y);
            int32_t x1 = max_i32(last_drawn_cursor_x, cursor_x) + CURSOR_SIZE;
            int32_t y1 = max_i32(last_drawn_cursor_y, cursor_y) + CURSOR_SIZE;
            redraw_rect(x0, y0, x1, y1);
            last_drawn_cursor_x = cursor_x;
            last_drawn_cursor_y = cursor_y;
        }

        /* Every check above is a non-blocking poll - accept_pending_*
         * on empty pipes, handle_mouse/handle_keyboard on empty ring
         * buffers - so with nothing to do this pass, this loop would
         * otherwise busy-spin for its full 50ms scheduler quantum
         * (sched.h's SCHED_QUANTUM_TICKS) doing nothing, and every
         * *other* runnable task (every client window) would wait that
         * same 50ms for its own turn to come back around. Yielding here
         * hands the rest of the quantum back to round-robin immediately
         * instead - see SYS_yield's comment in system_api/include/
         * syscall.h. */
        sys_yield();
    }
}
