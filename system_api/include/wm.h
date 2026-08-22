/* system_api/include/wm.h
 *
 * M20's window-system ABI: what SYS_fb_info reports, and the tiny
 * window-creation protocol carried over a pair of named pipes
 * (SYS_pipe_open - see kernel/ipc/pipe.h) rather than a dedicated
 * syscall of its own, since named pipes already give any two unrelated
 * processes (a compositor and a client the shell launched separately,
 * not parent/child) a rendezvous point without inventing a new
 * mechanism. Named "wm.h" rather than "fb.h" deliberately - a
 * kernel-internal kernel/drivers/fb.h already exists, and quoted
 * #includes resolve same-directory-first, so reusing that name here
 * would risk the exact silent-shadowing bug M8's syscall.h/
 * syscall_entry.h split was already renamed to avoid.
 */
#pragma once

#include <stdint.h>

/* SYS_fb_info fills one of these - read-only geometry, not the pixels
 * themselves (SYS_fb_map is the separate call that actually maps the
 * framebuffer in). */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} wm_fb_info_t;

/* Well-known named-pipe names (SYS_pipe_open) the compositor listens on
 * and any client writes/reads. Single global pair - fine for M20's
 * bring-up scope (one compositor, requests handled one at a time); a
 * real multi-client protocol is further out than this milestone. */
#define WM_REQUEST_PIPE  "wm_req"
#define WM_RESPONSE_PIPE "wm_resp"

typedef struct {
    uint32_t width;  /* ignored if panel != 0 - a panel always spans the full display width, the compositor's own call */
    uint32_t height;
    uint8_t panel; /* M22: 0 for a normal window, 1 for a chrome-less, always-on-top, screen-bottom-docked panel - see wm.h's M22 comment below and desktop_shell.c, the one client that ever sets this */
} wm_create_request_t;

typedef struct {
    int32_t window_id; /* -1 on failure */
    int32_t shm_id;     /* -1 on failure; pass to sys_shm_map to get a drawable pointer */
    uint32_t width, height; /* M22: the size the compositor actually allocated - for a panel request this is NOT an echo of what was asked for (width in particular is always overridden to the full display width), so a client sizes its own gfx_ctx_t from this response, never from its own request */
} wm_create_response_t;

/* M21: input-routing protocol. Once a client's window is accepted (the
 * response above), the compositor also finds-or-creates a second named
 * pipe (WM_EVENT_PIPE_PREFIX + the window's single-digit id -
 * wm_event_pipe_name, below) and writes one wm_event_t per routed input
 * event to it; the client opens the same name (sys_pipe_open again - the
 * exact same rendezvous-by-name mechanism the window-creation request
 * itself uses, not a new kind of channel) and blocks reading it, the same
 * way M13's shell already blocks reading keyboard input a line at a time.
 * A single event pipe per window rather than a shared one across all
 * clients - kernel/ipc/pipe.h's pipe_read consumes what it reads, so a
 * shared channel would mean two clients stealing each other's events. */
typedef enum {
    WM_EVENT_KEY = 1,          /* key.ch valid */
    WM_EVENT_MOUSE_MOVE = 2,   /* mouse.x/mouse.y/mouse.buttons valid - cursor position in this window's own coordinates (may be outside 0..w/0..h if the cursor is over the window's border/titlebar or has since moved off the window entirely) */
    WM_EVENT_MOUSE_BUTTON = 3, /* same fields as MOUSE_MOVE, sent in addition to it whenever buttons actually changes */
    WM_EVENT_FOCUS = 4,        /* this window just became the focused one - no extra fields */
    WM_EVENT_UNFOCUS = 5,      /* this window just stopped being the focused one - no extra fields */
} wm_event_type_t;

typedef struct {
    uint32_t type; /* wm_event_type_t */
    int32_t x, y;
    uint8_t buttons;
    char ch;
} wm_event_t;

#define WM_EVENT_PIPE_PREFIX "wm_evt"
#define WM_MAX_ROUTABLE_WINDOWS 10 /* window ids 0-9 - the name below only reserves one ASCII digit */

/* Builds this window's event-pipe name into out (must be >= 8 bytes).
 * window_id must be 0-9 (WM_MAX_ROUTABLE_WINDOWS) - both compositor.c
 * (the writer) and wmclient.c (the reader) call this instead of
 * formatting the string twice by hand, so the two can't silently drift
 * apart on the naming scheme. A plain function rather than a macro,
 * since building a string needs real code either way and a static
 * inline here is no different from one in a .c file except not needing
 * its own translation unit for two call sites. */
static inline void wm_event_pipe_name(int window_id, char out[8]) {
    out[0] = WM_EVENT_PIPE_PREFIX[0];
    out[1] = WM_EVENT_PIPE_PREFIX[1];
    out[2] = WM_EVENT_PIPE_PREFIX[2];
    out[3] = WM_EVENT_PIPE_PREFIX[3];
    out[4] = WM_EVENT_PIPE_PREFIX[4];
    out[5] = WM_EVENT_PIPE_PREFIX[5];
    out[6] = (char)('0' + (window_id % 10));
    out[7] = '\0';
}

/* M22: a read-only snapshot query plus a two-verb action request - the
 * pair of things a desktop shell (user_space/bin/desktop_shell.c) needs
 * that no ordinary client does: seeing every *other* window (not just
 * events routed to its own) and being able to change one of them (focus
 * it, or toggle it hidden) instead of only ever acting on itself. Same
 * request/response-over-named-pipes shape as window creation - a query
 * "request" carries no real payload (any single byte written to
 * WM_QUERY_PIPE triggers one), kept as a byte rather than an empty
 * struct only because sizeof(struct{}) isn't standard C. */
#define WM_QUERY_PIPE       "wm_query"
#define WM_QUERY_RESP_PIPE  "wm_query_resp"
#define WM_ACTION_PIPE      "wm_action"

typedef struct {
    int32_t window_id;
    int32_t x, y, w, h; /* on-screen content geometry - same fields compositor.c already tracks per window */
    uint8_t focused;
    uint8_t minimized;
    uint8_t is_panel; /* lets a panel client filter itself (and any other panel) out of what it lists as a "running app" */
} wm_window_info_t;

typedef struct {
    int32_t count;
    wm_window_info_t windows[WM_MAX_ROUTABLE_WINDOWS];
} wm_query_response_t;

typedef enum {
    WM_ACTION_FOCUS = 1,            /* un-minimizes if needed, then focuses window_id */
    WM_ACTION_TOGGLE_MINIMIZE = 2,  /* hides/shows window_id without touching its process; clears focus if it was focused */
} wm_action_type_t;

typedef struct {
    int32_t window_id;
    uint32_t action; /* wm_action_type_t */
} wm_action_request_t;
