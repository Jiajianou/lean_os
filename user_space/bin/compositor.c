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
 *
 * M30 adds real titlebar chrome: three small hit-testable buttons drawn
 * in every ordinary window's titlebar (close/maximize/minimize, right-
 * aligned - see draw_titlebar_buttons/BTN_SIZE below), handled by
 * apply_window_action - the same function accept_pending_action already
 * calls for an external WM_ACTION_PIPE request, so a titlebar click and
 * a panel's own click-to-minimize (M22) drive the exact literal same
 * code path instead of two parallel ones that could drift. Close doesn't
 * touch the window slot directly - it SIGTERMs the owning client and lets
 * M29's reap_dead_clients notice it died and reclaim the slot, exactly
 * the "closed and crashed share one reclaim path" M29's own intro
 * promised. Maximize/restore is deliberately NOT a real resize (there's
 * no protocol yet for a client to grow its own shm-backed pixel buffer -
 * that's M31's job): it repositions to fill the screen minus any docked
 * panel, clamped to never exceed the window's own buffer dimensions.
 */
#include "gfx.h" /* M34: gfx_point_in_rect - shared hit-test helper, see draw_titlebar_buttons' own note on why drawing itself stays on this file's own clip-aware fill_rect */
#include "signal.h" /* system_api/include/signal.h - SIGTERM, M30's WM_ACTION_CLOSE */
#include "str.h"
#include "syscall_wrappers.h"
#include "wm.h"

#define MAX_WINDOWS          8
#define TITLEBAR_H           20
#define BORDER               2
#define DEFAULT_BG_COLOR     0x001A1A2Eu
#define BORDER_COLOR         0x00444466u
#define TITLEBAR_COLOR       0x00335577u
#define TITLEBAR_FOCUS_COLOR 0x004C99E6u
#define CURSOR_COLOR         0x00FFFFFFu
#define CURSOR_SIZE          8
#define REDRAW_INTERVAL_MS   100 /* fallback cadence for changes the compositor has no way to notice itself - a client (M21's clock demo) redrawing its own window's pixels with no input involved at all. Input-driven changes no longer wait on this - see `dirty`, below. */

/* M30: titlebar buttons, right-aligned, close nearest the edge (the
 * conventional rightmost slot) - minimize/maximize/close, left to right.
 * BTN_SIZE fits comfortably inside TITLEBAR_H (20) with 3px of vertical
 * padding on each side; every window this project ships is at least
 * 200px wide (see gui_clock.c/gui_paint.c/gui_terminal.c's own WIN_W),
 * well past the ~54px these three buttons plus margins need. */
#define BTN_SIZE   14
#define BTN_GAP    4
#define BTN_MARGIN 4
#define BTN_CLOSE_COLOR    0x00CC3333u
#define BTN_MAXIMIZE_COLOR 0x0033AA55u
#define BTN_MINIMIZE_COLOR 0x00888899u

typedef struct {
    int32_t x, y, w, h; /* current on-screen content geometry */
    int32_t buf_w, buf_h; /* M30: the shm-backed pixel buffer's actual, fixed dimensions (set once at connect, never mutated) - w/h above can now shrink below this (WM_ACTION_MAXIMIZE clamps to it) but never exceed it; blit_window strides by buf_w, not w, so a cropped display never reads past what this window's buffer actually holds */
    uint32_t *pixels;
    int evt_write_fd; /* write end of this window's own event pipe - see wm_event_pipe_name */
    uint8_t is_panel;  /* M22: chrome-less, always-on-top, screen-bottom-docked - see wm_create_request_t.panel */
    uint8_t is_desktop; /* chrome-less, full-screen, always-on-*bottom* - see wm_create_request_t.desktop */
    uint8_t minimized; /* M22: hidden from redraw() and from click hit-testing, but the client process keeps running (WM_ACTION_TOGGLE_MINIMIZE) */
    uint8_t maximized; /* M30: WM_ACTION_MAXIMIZE/RESTORE toggle - see saved_x/y/w/h below */
    int32_t saved_x, saved_y, saved_w, saved_h; /* M30: pre-maximize geometry, restored by WM_ACTION_RESTORE - meaningless while !maximized */
    uint8_t alive; /* M29: 0 once this slot has been reclaimed (owning client died or closed) - excluded from redraw/hit-test/query, and eligible for accept_pending_window to hand to the next connecting client. Slots below window_count that are !alive are exactly the "holes" reap_dead_clients leaves behind. */
    uint8_t confirm_close; /* M36: from wm_create_request_t.confirm_close - see its own comment. Changes what apply_window_action's WM_ACTION_CLOSE branch does, nothing else. */
    int32_t client_pid; /* M29: from wm_create_request_t.client_pid - who to watch via SYS_task_alive so a crash (not just an orderly close) still frees this slot. -1 for a slot that's never been assigned. */
    char title[WM_TITLE_MAX]; /* echoed straight from wm_create_request_t.title into wm_window_info_t.title on every query - see accept_pending_query */
} window_t;

static window_t windows[MAX_WINDOWS];
static int window_count;
static int focused_window = -1; /* -1 = nothing focused yet */
static uint32_t bg_color = DEFAULT_BG_COLOR; /* M33: settings.c's WM_SETTINGS_PIPE is the only way this ever changes at runtime */

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
        /* buf_w, not w - M30 lets w shrink below buf_w (WM_ACTION_MAXIMIZE
         * clamping), but the underlying pixel buffer's real row stride
         * never changes, so indexing by anything else would read the
         * wrong bytes (or, once w > buf_w could ever happen, off the end
         * of it entirely - see window_t's own comment on buf_w). */
        const uint32_t *src_row = win->pixels + (uint32_t)(row - win->y) * (uint32_t)win->buf_w;
        for (int32_t col = x0; col < x1; col++) {
            put_pixel(col, row, src_row[col - win->x]);
        }
    }
}

/* M30: titlebar buttons live entirely inside the titlebar strip
 * (win->y - TITLEBAR_H .. win->y), so drawing and hit-testing them share
 * the exact same rects - see titlebar_button_rects. M34: the hit-test
 * itself (point_in_rect) moved to user_space/lib/gfx.c's gfx_point_in_rect -
 * settings.c had an identical copy; this file's own drawing calls
 * (fill_rect, below) stay put, since they're clip-rect-aware for the
 * partial-redraw perf reasons documented above put_pixel/fill_rect, and
 * gfx.c's own fill_rect has no idea that clip rect exists. */

typedef enum { BTN_MINIMIZE = 0, BTN_MAXIMIZE = 1, BTN_CLOSE = 2, BTN_COUNT } titlebar_button_t;

static void titlebar_button_rect(const window_t *win, titlebar_button_t btn, int32_t *out_x, int32_t *out_y) {
    int32_t by = win->y - TITLEBAR_H + (TITLEBAR_H - BTN_SIZE) / 2;
    int32_t bx = win->x + win->w - BTN_MARGIN - BTN_SIZE - (int32_t)btn * (BTN_SIZE + BTN_GAP);
    *out_x = bx;
    *out_y = by;
}

static void draw_titlebar_buttons(const window_t *win) {
    static const uint32_t colors[BTN_COUNT] = {BTN_MINIMIZE_COLOR, BTN_MAXIMIZE_COLOR, BTN_CLOSE_COLOR};
    for (int b = 0; b < BTN_COUNT; b++) {
        int32_t bx, by;
        titlebar_button_rect(win, (titlebar_button_t)b, &bx, &by);
        fill_rect(bx, by, BTN_SIZE, BTN_SIZE, colors[b]);
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

    fill_rect(0, 0, (int32_t)fb_info.width, (int32_t)fb_info.height, bg_color);
    /* Desktop windows first (a background layer under everything else -
     * the opposite end of the z-order from panels below), then ordinary
     * windows in creation order (no z-order raise on focus - a later
     * connection or click just changes titlebar color, not paint order;
     * see M20/M21's own notes on this simplification), then panels last
     * so they're always on top regardless of when they connected - the
     * one piece of z-ordering this compositor does enforce, since a
     * taskbar that could be occluded wouldn't be much of a taskbar. */
    for (int i = 0; i < window_count; i++) {
        if (windows[i].alive && windows[i].is_desktop) {
            blit_window(&windows[i]); /* no border/titlebar - a desktop background is its own chrome, same as a panel */
        }
    }
    for (int i = 0; i < window_count; i++) {
        const window_t *win = &windows[i];
        if (!win->alive || win->is_panel || win->is_desktop || win->minimized) {
            continue;
        }
        uint32_t titlebar_color = (i == focused_window) ? TITLEBAR_FOCUS_COLOR : TITLEBAR_COLOR;
        fill_rect(win->x - BORDER, win->y - TITLEBAR_H - BORDER,
                  win->w + 2 * BORDER, win->h + TITLEBAR_H + 2 * BORDER, BORDER_COLOR);
        fill_rect(win->x, win->y - TITLEBAR_H, win->w, TITLEBAR_H, titlebar_color);
        draw_titlebar_buttons(win);
        blit_window(win);
    }
    for (int i = 0; i < window_count; i++) {
        if (windows[i].alive && windows[i].is_panel) {
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

/* M31: the titlebar *band* only - excludes the content area point_in_window
 * also counts, since a titlebar click starts a move-drag (below) while a
 * content click doesn't. Buttons are checked separately, and first (see
 * handle_mouse) - clicking one is not a titlebar-body click. */
static int point_in_titlebar(const window_t *win, int32_t x, int32_t y) {
    return x >= win->x && x < win->x + win->w &&
           y >= win->y - TITLEBAR_H && y < win->y;
}

/* M31: which edge(s) of win's *outer* (border-inclusive) rect (px, py) is
 * within RESIZE_MARGIN of - a bitmask so a corner can hit two at once
 * (diagonal resize). Zero means "not on a resize handle at all". */
#define RESIZE_MARGIN 5
#define RESIZE_LEFT   1
#define RESIZE_RIGHT  2
#define RESIZE_TOP    4
#define RESIZE_BOTTOM 8

static int resize_hit_mask(const window_t *win, int32_t px, int32_t py) {
    int32_t x0 = win->x - BORDER;
    int32_t y0 = win->y - TITLEBAR_H - BORDER;
    int32_t x1 = win->x + win->w + BORDER;
    int32_t y1 = win->y + win->h + BORDER;
    int within_x = px >= x0 - RESIZE_MARGIN && px < x1 + RESIZE_MARGIN;
    int within_y = py >= y0 - RESIZE_MARGIN && py < y1 + RESIZE_MARGIN;
    int mask = 0;
    if (within_y) {
        if (px >= x0 - RESIZE_MARGIN && px < x0 + RESIZE_MARGIN) {
            mask |= RESIZE_LEFT;
        }
        if (px >= x1 - RESIZE_MARGIN && px < x1 + RESIZE_MARGIN) {
            mask |= RESIZE_RIGHT;
        }
    }
    if (within_x) {
        if (py >= y0 - RESIZE_MARGIN && py < y0 + RESIZE_MARGIN) {
            mask |= RESIZE_TOP;
        }
        if (py >= y1 - RESIZE_MARGIN && py < y1 + RESIZE_MARGIN) {
            mask |= RESIZE_BOTTOM;
        }
    }
    return mask;
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* M31: mouse-down-on-titlebar-or-edge starts one of these; every further
 * mouse event until button-up updates the dragged window instead of
 * going through the normal hit-test/focus/event-forwarding path at all
 * (see handle_mouse) - a drag in progress owns the input stream. */
typedef enum { DRAG_NONE = 0, DRAG_MOVE, DRAG_RESIZE } drag_mode_t;
static drag_mode_t drag_mode = DRAG_NONE;
static int drag_window = -1;
static int drag_resize_mask; /* RESIZE_LEFT/RIGHT/TOP/BOTTOM bits - meaningful only when drag_mode == DRAG_RESIZE */
static int32_t drag_start_cursor_x, drag_start_cursor_y;
static int32_t drag_start_x, drag_start_y, drag_start_w, drag_start_h;

#define MIN_WIN_W 60  /* "a sane minimum size" - M31's own wording; comfortably below every window this project ships (smallest is gui_clock's 200x90) */
#define MIN_WIN_H 40
#define MOVE_MIN_VISIBLE 40 /* at least this many px of a dragged window's titlebar must stay on-screen and above the panel - see the MOVE clamp below */

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

/* M29: shared teardown for a window slot, however it stops being valid -
 * a client crashing (reap_dead_clients, below) today, an explicit close
 * (M30's WM_ACTION_CLOSE) later. Clears focus if this was the focused
 * window, hides it from redraw/hit-testing (the `alive` checks throughout
 * this file), and leaves the slot free for accept_pending_window to hand
 * to the next connecting client. Deliberately does NOT free the shm
 * segment backing win->pixels (there's no SYS_shm_free, and freeing it
 * out from under this process's own still-present vmm mapping without
 * unmapping first would alias live physical memory to whatever gets
 * allocated next - a worse bug than the leak) or the event pipe (kept
 * alive and reset in place by accept_pending_window when the slot is
 * reused, instead of torn down) - see MAX_SHM_SEGMENTS/MAX_WINDOWS'
 * headroom, sized with exactly this in mind. */
static void reclaim_window(int idx) {
    window_t *win = &windows[idx];
    if (!win->alive) {
        return;
    }
    win->alive = 0;
    win->minimized = 0;
    win->client_pid = -1;
    if (focused_window == idx) {
        set_focus(-1);
    }
    dirty = 1;
}

/* M29: the crash half of the shared reclaim path - polls every live
 * window's owning client (SYS_task_alive, non-reaping so it doesn't
 * disturb whatever the client's real parent - the shell, desktop_shell's
 * launcher - later does with SYS_wait) once per main-loop iteration, and
 * reclaims only the ones that died *unexpectedly* (a nonzero exit code -
 * SYS_task_alive returns 0). A client that ran to completion and called
 * SYS_exit(0) on purpose (return 2, not 0) keeps its window - M20's
 * wm_demo self-test is exactly this: draws one static frame, exits
 * cleanly, and the window it drew is still what the rest of that
 * self-test verifies against. Cheap: window_count is at most MAX_WINDOWS
 * (8), same headroom accept_pending_query already leans on. */
static void reap_dead_clients(void) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i].alive && sys_task_alive(windows[i].client_pid) == 0) {
            reclaim_window(i);
        }
    }
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
    if (n != (long)sizeof(req)) {
        resp.window_id = -1;
        resp.shm_id = -1;
        sys_write(resp_write_fd, &resp, sizeof(resp));
        return;
    }

    /* M29: prefer a reclaimed (!alive) slot below window_count over
     * growing past it - a crashed-and-reconnected client shouldn't
     * permanently cost a slot out of the fixed MAX_WINDOWS table. Only
     * once every existing slot is genuinely live does this fall back to
     * appending a brand-new one, still bounded by MAX_WINDOWS exactly as
     * before. */
    int idx = -1;
    for (int i = 0; i < window_count; i++) {
        if (!windows[i].alive) {
            idx = i;
            break;
        }
    }
    int reused_slot = (idx >= 0);
    if (idx < 0) {
        if (window_count >= MAX_WINDOWS) {
            resp.window_id = -1;
            resp.shm_id = -1;
            sys_write(resp_write_fd, &resp, sizeof(resp));
            return;
        }
        idx = window_count;
    }

    /* A panel's width, or a desktop background's width and height, are
     * never the requester's call - always the full display, so each
     * genuinely spans edge to edge regardless of what the client happens
     * to ask for. */
    uint32_t width = (req.panel || req.desktop) ? fb_info.width : req.width;
    uint32_t height = req.desktop ? fb_info.height : req.height;

    /* Always a fresh segment, even when reusing a slot - see
     * reclaim_window's comment for why the previous occupant's segment
     * is deliberately left leaked rather than freed out from under this
     * process's own mapping of it. */
    long shm_id = sys_shm_create((size_t)width * height * sizeof(uint32_t));
    long vaddr = shm_id < 0 ? -1 : sys_shm_map(shm_id);
    if (shm_id < 0 || vaddr < 0) {
        resp.window_id = -1;
        resp.shm_id = -1;
        sys_write(resp_write_fd, &resp, sizeof(resp));
        return;
    }

    int evt_write_fd;
    if (reused_slot) {
        /* Same underlying named pipe (pipe_named looks it up by name,
         * unchanged since this slot's previous occupant) - reset it in
         * place rather than opening it again, which would just leak
         * another fd-table slot in this already-long-lived process for
         * no benefit (see SYS_pipe_reset's own doc comment). */
        evt_write_fd = windows[idx].evt_write_fd;
        sys_pipe_reset(evt_write_fd);
    } else {
        char evt_name[8];
        wm_event_pipe_name(idx, evt_name);
        int evt_fds[2];
        if (sys_pipe_open(evt_name, evt_fds) != 0) {
            resp.window_id = -1;
            resp.shm_id = -1;
            sys_write(resp_write_fd, &resp, sizeof(resp));
            return;
        }
        evt_write_fd = evt_fds[1];
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
    win->buf_w = (int32_t)width;  /* M30: fixed for this connection's whole lifetime - see window_t's own comment */
    win->buf_h = (int32_t)height;
    win->pixels = (uint32_t *)vaddr;
    win->evt_write_fd = evt_write_fd;
    win->is_panel = req.panel;
    win->is_desktop = req.desktop;
    win->minimized = 0;
    win->maximized = 0;
    win->alive = 1;
    win->confirm_close = req.confirm_close;
    win->client_pid = req.client_pid;
    int ti = 0;
    for (; req.title[ti] && ti < WM_TITLE_MAX - 1; ti++) {
        win->title[ti] = req.title[ti];
    }
    win->title[ti] = '\0';

    resp.window_id = idx;
    resp.shm_id = (int32_t)shm_id;
    resp.width = width;
    resp.height = height;
    if (!reused_slot) {
        window_count++;
    }
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
    resp.count = 0;
    for (int i = 0; i < window_count; i++) {
        const window_t *win = &windows[i];
        if (!win->alive) { /* M29: a reclaimed slot is gone, not a "running app" - skip it */
            continue;
        }
        int out = resp.count;
        resp.windows[out].window_id = i;
        resp.windows[out].x = win->x;
        resp.windows[out].y = win->y;
        resp.windows[out].w = win->w;
        resp.windows[out].h = win->h;
        resp.windows[out].focused = (i == focused_window);
        resp.windows[out].minimized = win->minimized;
        resp.windows[out].maximized = win->maximized;
        resp.windows[out].is_panel = win->is_panel;
        resp.windows[out].is_desktop = win->is_desktop;
        memcpy(resp.windows[out].title, win->title, WM_TITLE_MAX);
        resp.count++;
    }
    sys_write(query_resp_write_fd, &resp, sizeof(resp));
}

/* M30: the height of whichever panel is currently connected (there's at
 * most one in practice - desktop_shell.c is the only client that ever
 * asks for one - but nothing enforces that, so this just uses whichever
 * is found first), or 0 if none is - how much of the screen's bottom
 * edge WM_ACTION_MAXIMIZE has to leave clear. */
static int32_t connected_panel_height(void) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i].alive && windows[i].is_panel) {
            return windows[i].h;
        }
    }
    return 0;
}

/* M30: the single place every window-state-changing action funnels
 * through - a titlebar button click (handle_mouse, below) and an
 * external WM_ACTION_PIPE request (accept_pending_action) both call this
 * directly, so "click the panel's minimize toggle" and "click the
 * titlebar's minimize button" (M22 and M30's own bullet asking for
 * exactly this) drive the literal same code, not two copies that could
 * drift apart. */
static void apply_window_action(int idx, uint32_t action) {
    window_t *win = &windows[idx];
    if (action == WM_ACTION_FOCUS) {
        win->minimized = 0;
        set_focus(idx);
    } else if (action == WM_ACTION_TOGGLE_MINIMIZE) {
        win->minimized = !win->minimized;
        if (win->minimized && focused_window == idx) {
            set_focus(-1);
        }
        dirty = 1;
    } else if (action == WM_ACTION_CLOSE) {
        if (win->confirm_close) {
            /* M36: opted in (wm_connect_confirm_close) - give the client
             * a chance to decide instead of an unconditional SIGTERM. It
             * stays running (and its window slot stays alive) until it
             * calls SYS_exit on its own - reap_dead_clients (M29) picks
             * that up like any other termination, whatever the exit
             * code. If it never responds, its window simply never closes
             * via this path - see confirm_close's own doc comment. */
            wm_event_t ev = {0};
            ev.type = WM_EVENT_CLOSE_REQUEST;
            send_event(win, &ev);
        } else {
            /* Deliberately does not touch windows[idx] at all here - see
             * this file's header comment and M29's reap_dead_clients,
             * which will notice win->client_pid terminated (a nonzero
             * exit code - signal deaths always are, system_api/include/
             * signal.h) within one loop iteration and reclaim the slot
             * then, the exact same path an actual crash goes through. */
            sys_kill(win->client_pid, SIGTERM);
        }
    } else if (action == WM_ACTION_MAXIMIZE) {
        if (!win->maximized) {
            win->saved_x = win->x;
            win->saved_y = win->y;
            win->saved_w = win->w;
            win->saved_h = win->h;
            int32_t avail_w = (int32_t)fb_info.width - 2 * BORDER;
            int32_t avail_h = (int32_t)fb_info.height - TITLEBAR_H - 2 * BORDER - connected_panel_height();
            win->x = BORDER;
            win->y = TITLEBAR_H + BORDER;
            /* Clamped to buf_w/buf_h - see window_t's own comment on why
             * this can only ever shrink a window that's bigger than the
             * available area, never grow one past what its buffer holds. */
            win->w = min_i32(win->buf_w, avail_w);
            win->h = min_i32(win->buf_h, avail_h);
            win->maximized = 1;
            dirty = 1;
        }
    } else if (action == WM_ACTION_RESTORE) {
        if (win->maximized) {
            win->x = win->saved_x;
            win->y = win->saved_y;
            win->w = win->saved_w;
            win->h = win->saved_h;
            win->maximized = 0;
            dirty = 1;
        }
    }
}

static void accept_pending_action(int action_read_fd) {
    if (sys_pipe_poll(action_read_fd) < (long)sizeof(wm_action_request_t)) {
        return;
    }
    wm_action_request_t req;
    if (read_exact(action_read_fd, &req, sizeof(req)) != (long)sizeof(req)) {
        return;
    }
    if (req.window_id < 0 || req.window_id >= window_count || !windows[req.window_id].alive) {
        return;
    }
    apply_window_action(req.window_id, req.action);
}

/* M33: the compositor's first global (non-per-window) setting - see
 * wm.h's own comment on WM_SETTINGS_PIPE. Same non-blocking poll-then-
 * read shape as accept_pending_action, just with no window_id to
 * validate. */
static void accept_pending_settings(int settings_read_fd) {
    if (sys_pipe_poll(settings_read_fd) < (long)sizeof(wm_settings_request_t)) {
        return;
    }
    wm_settings_request_t req;
    if (read_exact(settings_read_fd, &req, sizeof(req)) != (long)sizeof(req)) {
        return;
    }
    bg_color = req.bg_color;
    dirty = 1;
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
        int left_up_edge = !(mev.buttons & 1) && (prev_buttons & 1);

        /* M31: a drag in progress owns every event until release - no
         * hit-testing, no focus changes, no forwarding to the window's
         * own content, just updating its geometry. drag_window's own
         * `alive` is re-checked every event (not just at drag-start)
         * since M29's reap_dead_clients can reclaim it mid-drag if its
         * owning client crashes while being dragged. */
        if (drag_mode != DRAG_NONE) {
            if (left_up_edge || !windows[drag_window].alive) {
                drag_mode = DRAG_NONE;
                drag_window = -1;
            } else if (mev.buttons & 1) {
                window_t *win = &windows[drag_window];
                int32_t dx = cursor_x - drag_start_cursor_x;
                int32_t dy = cursor_y - drag_start_cursor_y;
                if (drag_mode == DRAG_MOVE) {
                    int32_t panel_h = connected_panel_height();
                    int32_t min_x = -(win->w - MOVE_MIN_VISIBLE);
                    int32_t max_x = (int32_t)fb_info.width - MOVE_MIN_VISIBLE;
                    int32_t min_y = TITLEBAR_H + BORDER; /* titlebar top can't go above the screen's own top edge */
                    int32_t max_y = (int32_t)fb_info.height - panel_h; /* titlebar bottom can't dip below the panel's top edge */
                    win->x = clamp_i32(drag_start_x + dx, min_x, max_x);
                    win->y = clamp_i32(drag_start_y + dy, min_y, max_y);
                } else { /* DRAG_RESIZE */
                    int32_t new_x = drag_start_x, new_y = drag_start_y;
                    int32_t new_w = drag_start_w, new_h = drag_start_h;
                    /* Opposite edge from whichever one is being dragged
                     * stays fixed - new_w/new_h are derived from the drag
                     * first, then x/y are re-derived from that fixed edge,
                     * rather than tracking x/y independently and patching
                     * them after clamping (which edge case that badly). */
                    if (drag_resize_mask & RESIZE_RIGHT) {
                        new_w = clamp_i32(drag_start_w + dx, MIN_WIN_W, win->buf_w);
                    } else if (drag_resize_mask & RESIZE_LEFT) {
                        int32_t right_edge = drag_start_x + drag_start_w;
                        new_w = clamp_i32(drag_start_w - dx, MIN_WIN_W, win->buf_w);
                        new_x = right_edge - new_w;
                    }
                    if (drag_resize_mask & RESIZE_BOTTOM) {
                        new_h = clamp_i32(drag_start_h + dy, MIN_WIN_H, win->buf_h);
                    } else if (drag_resize_mask & RESIZE_TOP) {
                        int32_t bottom_edge = drag_start_y + drag_start_h;
                        new_h = clamp_i32(drag_start_h - dy, MIN_WIN_H, win->buf_h);
                        new_y = bottom_edge - new_h;
                    }
                    win->x = new_x;
                    win->y = new_y;
                    win->w = new_w;
                    win->h = new_h;
                }
                dirty = 1;
            }
            prev_buttons = mev.buttons;
            continue;
        }

        if (left_down_edge) {
            /* M30: titlebar buttons take priority over every other hit-
             * test below - checked topmost-window-first, same order and
             * same "first match wins" limitation as the ordinary-window
             * hit-test just below it (occlusion-unaware - see that
             * loop's own comment; a real fix is z-order work, out of
             * scope here). Panels/desktop have no titlebar, so they're
             * never candidates. */
            int btn_hit_idx = -1;
            titlebar_button_t btn_hit = BTN_CLOSE;
            for (int i = window_count - 1; i >= 0 && btn_hit_idx < 0; i--) {
                const window_t *w = &windows[i];
                if (!w->alive || w->is_panel || w->is_desktop || w->minimized) {
                    continue;
                }
                for (int b = 0; b < BTN_COUNT; b++) {
                    int32_t bx, by;
                    titlebar_button_rect(w, (titlebar_button_t)b, &bx, &by);
                    if (gfx_point_in_rect(cursor_x, cursor_y, bx, by, BTN_SIZE, BTN_SIZE)) {
                        btn_hit_idx = i;
                        btn_hit = (titlebar_button_t)b;
                        break;
                    }
                }
            }
            if (btn_hit_idx >= 0) {
                if (btn_hit == BTN_CLOSE) {
                    apply_window_action(btn_hit_idx, WM_ACTION_CLOSE);
                } else if (btn_hit == BTN_MINIMIZE) {
                    apply_window_action(btn_hit_idx, WM_ACTION_TOGGLE_MINIMIZE);
                } else {
                    apply_window_action(btn_hit_idx, windows[btn_hit_idx].maximized ? WM_ACTION_RESTORE : WM_ACTION_MAXIMIZE);
                }
                prev_buttons = mev.buttons;
                continue; /* consumed by chrome - not also a focus-changing click on whatever's under it */
            }

            /* M31: resize handles (window border edges/corners) come next -
             * a small, precise target that has to win over both the
             * titlebar-move check right after it and the generic content
             * hit-test further down. Same topmost-first, first-match-wins
             * order as every other hit-test in this function. */
            int rz_idx = -1;
            int rz_mask = 0;
            for (int i = window_count - 1; i >= 0; i--) {
                const window_t *w = &windows[i];
                if (!w->alive || w->is_panel || w->is_desktop || w->minimized) {
                    continue;
                }
                int mask = resize_hit_mask(w, cursor_x, cursor_y);
                if (mask) {
                    rz_idx = i;
                    rz_mask = mask;
                    break;
                }
            }
            if (rz_idx >= 0) {
                drag_mode = DRAG_RESIZE;
                drag_window = rz_idx;
                drag_resize_mask = rz_mask;
                drag_start_cursor_x = cursor_x;
                drag_start_cursor_y = cursor_y;
                drag_start_x = windows[rz_idx].x;
                drag_start_y = windows[rz_idx].y;
                drag_start_w = windows[rz_idx].w;
                drag_start_h = windows[rz_idx].h;
                set_focus(rz_idx);
                prev_buttons = mev.buttons;
                continue;
            }

            /* M31: a titlebar-body click (not a button, not a resize
             * handle) starts a move-drag instead of falling through to
             * the plain focus-click hit-test below. */
            int mv_idx = -1;
            for (int i = window_count - 1; i >= 0; i--) {
                const window_t *w = &windows[i];
                if (w->alive && !w->is_panel && !w->is_desktop && !w->minimized && point_in_titlebar(w, cursor_x, cursor_y)) {
                    mv_idx = i;
                    break;
                }
            }
            if (mv_idx >= 0) {
                drag_mode = DRAG_MOVE;
                drag_window = mv_idx;
                drag_start_cursor_x = cursor_x;
                drag_start_cursor_y = cursor_y;
                drag_start_x = windows[mv_idx].x;
                drag_start_y = windows[mv_idx].y;
                set_focus(mv_idx);
                prev_buttons = mev.buttons;
                continue;
            }

            /* Panels are checked first (they're drawn on top, so they'd
             * visually win any overlap anyway) and a minimized window
             * can't be clicked - there's nothing on screen to click. */
            int hit = -1;
            for (int i = window_count - 1; i >= 0; i--) {
                if (windows[i].alive && windows[i].is_panel && !windows[i].minimized && point_in_window(&windows[i], cursor_x, cursor_y)) {
                    hit = i;
                    break;
                }
            }
            if (hit < 0) {
                for (int i = window_count - 1; i >= 0; i--) {
                    if (windows[i].alive && !windows[i].is_panel && !windows[i].is_desktop && !windows[i].minimized &&
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
                    if (windows[i].alive && windows[i].is_desktop && !windows[i].minimized && point_in_window(&windows[i], cursor_x, cursor_y)) {
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

/* M32: cycles focus forward through every alive ordinary (non-panel,
 * non-desktop) window, un-minimizing the target the same way a titlebar/
 * panel WM_ACTION_FOCUS click already does - reuses apply_window_action
 * rather than duplicating its focus/un-minimize logic. `focused_window`
 * may be -1 (nothing focused) going in; `(start + step) % window_count`
 * still lands correctly on 0 for step 1 in that case. A no-op (focuses
 * itself back) if there's only one eligible window - harmless, set_focus
 * already no-ops on a same-window call. */
static void alt_tab_cycle(void) {
    if (window_count == 0) {
        return;
    }
    for (int step = 1; step <= window_count; step++) {
        int idx = (focused_window + step) % window_count;
        if (idx < 0) {
            idx += window_count;
        }
        if (windows[idx].alive && !windows[idx].is_panel && !windows[idx].is_desktop) {
            apply_window_action(idx, WM_ACTION_FOCUS);
            return;
        }
    }
}

static void handle_keyboard(void) {
    char ch;
    while (sys_kbd_read(&ch)) {
        /* M32: Alt+Tab is intercepted here, before ever reaching a
         * client - a real WM shortcut, not something any app's own input
         * handling should see (or could even tell apart from a plain Tab
         * keypress on its own - see SYS_kbd_modifiers' doc comment). A
         * plain Tab (Alt not held) still forwards exactly as before. */
        if (ch == '\t' && (sys_kbd_modifiers() & KBD_MOD_ALT)) {
            alt_tab_cycle();
            continue;
        }
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
    int settings_fds[2];
    if (sys_pipe_open(WM_QUERY_PIPE, query_fds) != 0 || sys_pipe_open(WM_QUERY_RESP_PIPE, query_resp_fds) != 0 ||
        sys_pipe_open(WM_ACTION_PIPE, action_fds) != 0 || sys_pipe_open(WM_SETTINGS_PIPE, settings_fds) != 0) {
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
        accept_pending_settings(settings_fds[0]);
        reap_dead_clients();
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
