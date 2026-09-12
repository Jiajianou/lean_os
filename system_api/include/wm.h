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

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

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

/* Short human-readable app name (e.g. "Terminal") a client hands the
 * compositor at connect time - carried through to wm_window_info_t.title
 * below so a panel (desktop_shell.c's taskbar) can label a running
 * window by name instead of only its numeric window_id. Always
 * NUL-terminated; a client that doesn't care leaves it empty. */
#define WM_TITLE_MAX 16

/* Which screen edge a panel docks to. M22's original flag was a plain
 * 0/1 "is this a panel", which is exactly WM_PANEL_NONE and
 * WM_PANEL_BOTTOM here - named rather than bare numbers since M41, kept
 * that way through M42 even though there is only one edge again, because
 * the name is what makes `req.panel = WM_PANEL_BOTTOM` readable at the
 * call site.
 *
 * M42: WM_PANEL_TOP is gone with menu_bar.c. A top-docked bar existed
 * for exactly one client - M41's macOS-style menu bar - and this project
 * settled on the Windows convention instead: one bottom taskbar, and
 * each app's menus drawn in its own window. Nothing else ever asked to
 * dock anywhere but the bottom, so the second edge (and every clamp that
 * had to be edge-aware for it) came back out. */
#define WM_PANEL_NONE   0
#define WM_PANEL_BOTTOM 1

typedef struct {
    uint32_t width;  /* ignored if panel or desktop != 0 - both always span the full display width, the compositor's own call */
    uint32_t height; /* ignored if desktop != 0 - a desktop window is always the full display, both dimensions */
    /* M45: for a panel, how many of `height` rows are actually docked at
     * the bottom of the screen. 0 means "all of them" - every panel
     * before this milestone, and the shape wm_connect_panel had until
     * M45 gave the taskbar a menu to raise. The rows *above* the docked
     * strip are the overhang: allocated and drawn by the client like any
     * other part of its buffer, but composited and click-routed only
     * while WM_ACTION_SET_PANEL_OVERHANG has asked for them. Ignored for
     * a non-panel window. See that action for why the mechanism lives
     * here rather than in a second compositor-owned overlay. */
    uint32_t panel_dock_h;
    uint8_t panel; /* M22: 0 for a normal window, WM_PANEL_BOTTOM for a chrome-less, always-on-top, bottom-docked panel. See wm.h's M22 comment below and desktop_shell.c, the only client that ever sets this */
    uint8_t translucent; /* M44: blend this window's pixels over what is already composited underneath instead of blitting them opaquely (compositor.c's blit_window). Opt-in and currently only the taskbar, which wm_connect_panel sets it for - a translucent *ordinary* window would let you read one app's text through another's, which is a different feature from a bar you can see the desktop through. Costs the memcpy fast path for whatever sets it, which is why it isn't simply on for everything. */
    uint8_t desktop; /* chrome-less, full-screen, always-on-*bottom* (the exact opposite z-order from panel) background window - see desktop_icons.c, the one client that ever sets this. Mutually exclusive with panel; nothing enforces that since only one client ever sets either flag. */
    char title[WM_TITLE_MAX];
    int32_t client_pid; /* M29: this client's own SYS_getpid() - lets the compositor notice (SYS_task_alive) when a connected client dies without an orderly disconnect, and reclaim its window slot. Not a security boundary (nothing stops a client lying about it), just bookkeeping - same trust level as everything else in this protocol. */
    uint8_t confirm_close; /* M36: opt-in - 0 (every client before this milestone, via wmclient.h's wm_connect/wm_connect_panel/wm_connect_desktop, which all pass 0 explicitly) keeps M30's original WM_ACTION_CLOSE behavior (an immediate SIGTERM). 1 (only wm_connect_confirm_close, currently only text_editor.c) makes the compositor send WM_EVENT_CLOSE_REQUEST to this window's own event pipe instead and leave the process running - the client decides for itself when (or whether) to actually SYS_exit, e.g. after a confirm-discard prompt. A client that opts in but never handles the event, or never exits, simply never closes via this path - same "an app that doesn't implement it just doesn't respond to it" contract real window systems use for this exact event (X11's WM_DELETE_WINDOW is the closest precedent), not something this protocol tries to force. */
} wm_create_request_t;

typedef struct {
    int32_t window_id; /* -1 on failure */
    int32_t shm_id;     /* -1 on failure; pass to sys_shm_map to get a drawable pointer */
    uint32_t width, height; /* M22: the size the compositor actually allocated - for a panel request this is NOT an echo of what was asked for (width in particular is always overridden to the full display width), so a client sizes its own gfx_ctx_t from this response, never from its own request */
    /* M55: which process is serving this window. The mirror image of
     * client_pid above, and it exists for the mirror-image reason: the
     * compositor has been able to notice a dead client since M29, and
     * until this milestone a client had no way at all to notice a dead
     * compositor. Without it, "my events stopped arriving" is
     * indistinguishable from "nothing is happening", which is the normal
     * state of an idle desktop - so a client would have to guess, and the
     * only safe guess is to do nothing forever. With it the question is
     * one SYS_task_alive call (user_space/lib/wmclient.c's
     * reconnect_if_compositor_died). */
    int32_t compositor_pid;
    /* M56: which client this answer is *for*, echoed from the request.
     *
     * WM_RESPONSE_PIPE is one shared stream - there has only ever been
     * one, since M20, when there was only ever one client. With several
     * connecting at once (init starts three back to back) whichever
     * client the scheduler happens to wake first reads whatever is at the
     * head, which may be somebody else's window: its id, its shm segment,
     * its size. That was survivable while every client *blocked* on the
     * read and the compositor answered one request per loop iteration, so
     * the orders lined up in practice.
     *
     * M55's reconnect retry stopped it lining up: a client that re-sends
     * after a timeout can have two requests in flight, and two clients
     * can be polling the same pipe at once. So the answer says who it is
     * for, and a client that reads someone else's keeps waiting for its
     * own (see wmclient.c). Cheaper and far smaller than a response pipe
     * per client, which is the other way to fix this and would cost a
     * named pipe and two fds per connection. */
    int32_t client_pid;
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
    WM_EVENT_MOUSE_WHEEL = 7,  /* M49: mouse.wheel valid (detents, negative up), plus x/y/buttons as of this event - routed to whatever the cursor is over, not to whatever holds focus, because a wheel acts on the thing under the pointer and every desktop that has one behaves that way. Sent only when the wheel actually moved, so a client that ignores it sees nothing new. */
    WM_EVENT_DRAG_MOTION = 8,  /* M49: a client-initiated drag is in progress and the cursor is over this window - x/y are in this window's coordinates. For a would-be drop target to highlight itself; ignoring it costs nothing. */
    WM_EVENT_DROP = 9,         /* M49: a drag was released over this window. The payload is NOT in this struct - the compositor has already written it to WM_DRAG_DATA_PIPE, which wmclient.h's wm_drag_payload reads. See WM_DRAG_PIPE for why it isn't carried inline. */
    WM_EVENT_EXPOSE = 10,      /* M55: this window's pixel buffer is new and blank - redraw everything. Sent by wmclient.c itself, not by the compositor, and exactly once after a client reconnects to a *replacement* compositor (see wm_window_t's own M55 note): the pixels a client had were the dead compositor's shm segment and are gone with it, but everything needed to draw them again lives in the client. A client that ignores this comes back as an empty rectangle, which is why every GUI program in this project handles it - it is one line in each, next to whatever already sets its redraw flag. */
    /* M74: the machine is about to stop, and this window is being asked
     * whether it minds. A client with nothing unsaved does nothing - the
     * shutdown proceeds after a short timeout it never notices. A client
     * that would lose work replies with WM_ACTION_VETO_SHUTDOWN, and the
     * shutdown is abandoned rather than delayed: a longer grace period
     * would still lose the work, it would just take longer about it.
     *
     * Sent only to windows that set confirm_close, for the same reason
     * WM_EVENT_CLOSE_REQUEST is - a client that never opted into being
     * asked anything is a client that has nothing to say here, and asking
     * every window would mean waiting out the timeout on every desktop
     * that has a clock open. */
    WM_EVENT_QUERY_SHUTDOWN = 12,
    WM_EVENT_DISPLAY_CHANGED = 11, /* M58: the screen is a different size, and this window's pixel buffer has been replaced to match. Deliberately shaped so that a client already handling WM_EVENT_EXPOSE handles this by falling into the same code - "your buffer is new and blank, redraw everything from state you still hold" is the exact lesson M55 taught every GUI program in this project, and re-teaching it with a separate resize protocol would have been the wrong kind of new. wmclient.c does the work (it unmaps the old segment and re-runs the create handshake, which the compositor answers with the new one) and then hands this event on, so a client's whole obligation is one more label on its redraw case. A client that ignores it comes back as a stale rectangle at the wrong size. */
    WM_EVENT_CLOSE_REQUEST = 6, /* M36: sent instead of an immediate SIGTERM when this window's own wm_create_request_t.confirm_close was set and a close was requested (a titlebar close button or an external WM_ACTION_CLOSE) - no extra fields. The client decides what to do next; see confirm_close's own comment above. */
} wm_event_type_t;

typedef struct {
    uint32_t type; /* wm_event_type_t */
    int32_t x, y;
    uint8_t buttons;
    char ch;
    /* M40: for a mouse event, input.h's mouse_event_t.time_ms carried
     * straight through - when the PS/2 interrupt decoded this packet, not
     * when the compositor forwarded it or when the client read it. A
     * client timing a gesture (desktop_icons.c's double-click) has to use
     * this rather than calling SYS_uptime_ms itself, or it ends up
     * measuring its own scheduling latency. Zero for key/focus events,
     * which nothing times. */
    uint32_t time_ms;
    /* M-fix: the modifiers held when THIS key was decoded, carried in the
     * event for exactly the reason time_ms above is.
     *
     * A client used to ask SYS_kbd_modifiers itself, which reports the
     * modifiers of the character the *kernel* most recently handed out -
     * and the kernel handed that character to the compositor, one pipe
     * hop and an unknown number of scheduler quanta before this client
     * reads the event. So a client asking is asking about a keystroke
     * that may not be its own. M40 already made exactly this correction
     * for timing ("a client timing a gesture has to use this rather than
     * calling SYS_uptime_ms itself, or it ends up measuring its own
     * scheduling latency") and the same argument was never applied to
     * modifiers.
     *
     * The symptom was Ctrl+C in the terminal sometimes typing a 'c'
     * instead of copying, which left the clipboard empty and made paste
     * in the editor look broken - a bug that had outlived several
     * milestones because it presents in a different program from the one
     * that causes it. input.h's KBD_MOD_* values. Zero for mouse and
     * focus events. */
    uint8_t mods;
    /* M49: wheel detents for a WM_EVENT_MOUSE_WHEEL, carried straight
     * through from input.h's mouse_event_t.wheel. Zero for every other
     * event type, and zero forever on hardware without a wheel. */
    int32_t wheel;
} wm_event_t;

#define WM_EVENT_PIPE_PREFIX "wm_evt"
/* M41: 10 -> 12, and the event-pipe name below grew a second digit to
 * carry it, after a fourth always-on desktop client left too few slots
 * for the six apps a desktop icon can launch - the first thing M40's
 * harness caught after the menu bar landed. M42 took that fourth client
 * back out again but kept the cap: headroom is what stopped this being a
 * to-the-last-slot fit, which is the state it was in when it broke.
 * Bounded above by what a wm_query_response_t can be without approaching
 * PIPE_BUF_SIZE (kernel/ipc/pipe.h, 1024): at 12 that response is under
 * 600 bytes. */
#define WM_MAX_ROUTABLE_WINDOWS 12

/* Builds this window's event-pipe name into out (must be >= 9 bytes).
 * window_id must be 0..WM_MAX_ROUTABLE_WINDOWS-1. Both compositor.c (the
 * writer) and wmclient.c (the reader) call this instead of formatting the
 * string twice by hand, so the two can't silently drift apart on the
 * naming scheme. A plain function rather than a macro, since building a
 * string needs real code either way and a static inline here is no
 * different from one in a .c file except not needing its own translation
 * unit for two call sites.
 *
 * M41: always two digits ("wm_evt00", not "wm_evt0"), now that ids run
 * past 9. Fixed-width rather than only widening past 9 so there is
 * exactly one name per id, with no chance of "wm_evt1" and "wm_evt01"
 * both existing and being different pipes. */
#define WM_EVENT_PIPE_NAME_LEN 9

static inline void wm_event_pipe_name(int window_id, char out[WM_EVENT_PIPE_NAME_LEN]) {
    out[0] = WM_EVENT_PIPE_PREFIX[0];
    out[1] = WM_EVENT_PIPE_PREFIX[1];
    out[2] = WM_EVENT_PIPE_PREFIX[2];
    out[3] = WM_EVENT_PIPE_PREFIX[3];
    out[4] = WM_EVENT_PIPE_PREFIX[4];
    out[5] = WM_EVENT_PIPE_PREFIX[5];
    out[6] = (char)('0' + (window_id / 10) % 10);
    out[7] = (char)('0' + window_id % 10);
    out[8] = '\0';
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
    uint8_t maximized; /* M30: mirrors compositor.c's window_t.maximized - lets a caller (a future titlebar/panel indicator) reflect current state without guessing */
    uint8_t is_panel; /* lets a panel client filter itself (and any other panel) out of what it lists as a "running app" */
    uint8_t is_desktop; /* same idea as is_panel - the desktop background is never a "running app" either */
    /* M51: where this window sits in the compositor's z-order, 0 for the
     * bottom-most and increasing upward - so a shell can say which window
     * is frontmost *without* reordering the list it draws. That split is
     * the whole reason this is a field rather than the query simply
     * returning windows in z-order: a taskbar button that moved every
     * time you focused something is a button you can't build muscle
     * memory for, so desktop_shell.c keeps its buttons in window_id order
     * (stable for a window's whole life) and reads frontmost-ness from
     * here. Windows the query skips (dead slots) leave no gap: this is a
     * dense rank over what the response actually contains. */
    int32_t z_index;
    /* M63 stretch goal: which virtual desktop this window is on, or -1
     * for the two kinds of surface that are on all of them (a panel and
     * the desktop background are chrome, not windows you put somewhere).
     * A taskbar filters on this so its buttons describe the desktop you
     * are looking at; a task manager deliberately does not, because a
     * process you cannot see is exactly the one you might be looking
     * for. */
    int32_t workspace;
    char title[WM_TITLE_MAX]; /* echo of wm_create_request_t.title - may be empty */
} wm_window_info_t;

/* M63 stretch goal: how many virtual desktops there are. Four, which is
 * the number every desktop that ships this feature seems to settle on -
 * enough to be worth switching between, few enough to hold in your head
 * without a map. */
#define WM_WORKSPACE_COUNT 4

typedef struct {
    int32_t count;
    /* Which workspace is on screen right now, so a panel can say so. */
    int32_t current_workspace;
    wm_window_info_t windows[WM_MAX_ROUTABLE_WINDOWS];
} wm_query_response_t;

/* M30: titlebar close/minimize/maximize buttons are drawn and hit-tested
 * entirely inside compositor.c's own mouse handling (user_space/bin/
 * compositor.c) - a click there calls the same apply_window_action every
 * external WM_ACTION_PIPE request goes through, not a separate code path,
 * so nothing about wm_send_action's shape needed to change for this: an
 * ordinary client (gui_clock, gui_paint, gui_terminal) needs zero code of
 * its own to get real close/minimize/maximize, and an external caller
 * (desktop_shell, or anything else that already knows a window_id from
 * wm_query_windows) can drive the exact same three actions itself. */
typedef enum {
    WM_ACTION_FOCUS = 1,            /* un-minimizes if needed, then focuses window_id */
    WM_ACTION_TOGGLE_MINIMIZE = 2,  /* hides/shows window_id without touching its process; clears focus if it was focused */
    WM_ACTION_CLOSE = 3,            /* SIGTERMs window_id's owning client (system_api/include/signal.h) - the process's own termination is what actually frees the window slot, via M29's SYS_task_alive-driven reap_dead_clients, the same reclaim path a real crash goes through. Not instantaneous (signal delivery isn't - see signal.h) but visibly so: the window stops responding immediately, and disappears within one compositor loop iteration. */
    WM_ACTION_MAXIMIZE = 4,         /* saves window_id's current x/y/w/h and grows it to fill the screen minus any docked panel - clamped to never exceed its own shm-backed pixel buffer (there's no resize protocol yet - M31 - so a window smaller than the available area is repositioned to fill as much of it as its buffer actually holds, never stretched past what it allocated). No-op if already maximized. */
    WM_ACTION_RESTORE = 5,          /* undoes WM_ACTION_MAXIMIZE - restores the saved x/y/w/h. No-op if not maximized. */
    WM_ACTION_TOGGLE_LAUNCHER = 6,  /* M42: shows/hides the compositor-owned launcher overlay (M43). The one action here with no window_id at all - it acts on the compositor itself, not on a window, so accept_pending_action handles it before the window_id validation every other action goes through, and wmclient.h's wm_toggle_launcher passes -1. desktop_shell.c's Start button is what sends it; M43's Ctrl+Space keychord reaches the exact same toggle from inside the compositor. */
    WM_ACTION_SNAP_LEFT = 7,        /* M43: resizes and repositions window_id to exactly the left half of the content area (the screen minus the taskbar and minus the room a titlebar needs), clamped to its own buffer the same way WM_ACTION_MAXIMIZE is - see that action's note on why a window smaller than the target area is repositioned rather than stretched. Dragging a window's titlebar into the screen's left edge is what normally sends this, but it is an ordinary action on the pipe like every other one here, so the drag and an external caller drive literally the same code. */
    WM_ACTION_SNAP_RIGHT = 8,       /* M43: the mirror image of WM_ACTION_SNAP_LEFT - the right half. */
    WM_ACTION_KILL = 9,             /* M45: SIGKILL window_id's owning client, and - the whole reason this exists rather than reusing WM_ACTION_CLOSE - deliberately ignoring `confirm_close`. M36's contract explicitly permits a client to never answer WM_EVENT_CLOSE_REQUEST, which means an app that hangs, or simply chooses not to, is on your screen until the machine is reset. Close stays the polite verb (SIGTERM, or the close-request event for opted-in clients); this is the one that always works. The window slot is reclaimed by exactly the same M29 reap_dead_clients path a crash or an ordinary close goes through - nothing here touches windows[] directly. */
    WM_ACTION_SET_PANEL_OVERHANG = 10, /* M45: how many rows *above* its docked strip the panel window_id wants composited and click-routed right now (wm_action_request_t.value; 0 to put it away). A panel is 32px tall and clipped to its own buffer, and a three-item context menu is ~72px that has to rise out of the bar - this is that. Originally M41's mechanism for its top-bar dropdowns, removed in M42 along with the menu bar itself, and brought back rather than reinvented: what made it go away was the bar disappearing, not the overhang being wrong. Nothing about the panel's own geometry changes - win->h stays the docked height, so maximize and window placement (which reserve room for a panel) are unaffected by a menu that is up for a second and a half. */
    WM_ACTION_SET_MODE = 11,        /* M58: change the display resolution to wm_action_request_t.value's packed geometry - (width << 16) | height, both of which fit in 16 bits by the DISPI interface's own limits. No window_id (-1 by convention), like WM_ACTION_TOGGLE_LAUNCHER: it acts on the display, not on a window. The compositor performs the whole change - SYS_display_set_mode, re-map its framebuffer, reallocate every window's segment, re-span the panels, clamp windows and the cursor back on-screen - because it is the process that owns the screen; the kernel syscall changes the mode and nothing else. Packed into `value` rather than given two new fields so that every other action's message does not grow eight bytes for one action's sake. */
    WM_ACTION_SET_TASKBAR_SLOT = 13, /* M61: window_id's button on the taskbar occupies wm_action_request_t.value's packed (x << 16) | width, in screen coordinates. Sent by desktop_shell.c whenever its slot layout changes - which is when a window opens or closes, not per frame. The compositor needs it for exactly one thing: a minimize that animates *toward the button it went to*, which is the difference between motion that says where a window went and motion that is decoration. It deliberately does not derive the layout itself - the taskbar's geometry is the taskbar's, and a compositor that recomputed it would be a second copy of it to keep in step. A window the compositor has never been told about animates toward the bottom of the screen at its own x, which is the honest answer when nothing has said better. */
    /* M74: "do not switch this machine off - I would lose something".
     * Sent by a client in answer to WM_EVENT_QUERY_SHUTDOWN. window_id is
     * the vetoing window, so the compositor can say *which* program
     * objected rather than raising an anonymous "something is unsaved" -
     * which is the difference between a message that tells you where to
     * go and one that tells you to go looking.
     *
     * One veto with a timeout, and deliberately not a negotiation: there
     * is no "I have saved it now, carry on" reply, because the person is
     * right there and can simply press the button again. M47's shutdown
     * gives everything a second to die and then kills it, which is
     * correct for a *stopping* machine and is exactly why the question
     * has to be asked before that path starts rather than inside it. */
    WM_ACTION_VETO_SHUTDOWN = 14,
    WM_ACTION_CONFIRM_MODE = 12,    /* M58: keep the mode that was just applied. A WM_ACTION_SET_MODE arms a revert timer (roughly ten seconds); this is what disarms it. Every desktop that ships a resolution setting ships this countdown, for the same reason: choosing a mode the display cannot show is how a person loses their machine with no way to get it back - and it matters more here, not less, since there is no second machine to log in from and no config file to edit blind. No window_id, same as WM_ACTION_SET_MODE. */
    WM_ACTION_PRESENT = 15,         /* M117: window_id's pixel buffer has changed - composite it. Until this existed a client's own drawing (a clock ticking, a browser laying out a page, an editor after a keystroke that arrived through the compositor and so already counted as input) reached the screen on a 100 ms fallback poll, because the compositor had no way to notice a client writing into a shared segment. That poll is now a 1 s safety net (compositor.c REDRAW_INTERVAL_MS) and this is the path: one 16-byte write on the shared action pipe after a frame, coalesced by the compositor into one rectangle per loop pass, so a client presenting fifty times a second costs one composite of its own rectangle per pass rather than fifty. Whole-window on purpose - a damage rectangle would not fit in `value`, a window is the unit the compositor already composites in, and every client here redraws all of itself anyway. No authority is implied: presenting somebody else's window redraws pixels the compositor already owns, which is what the 100 ms poll did for every window unasked. */
} wm_action_type_t;

/* M58: how WM_ACTION_SET_MODE packs a geometry into wm_action_request_t's
 * single int32 `value`. Here rather than in either of the two files that
 * use it, so the pack and the unpack cannot disagree. */
static inline int32_t wm_pack_mode(uint32_t width, uint32_t height) {
    return (int32_t)(((width & 0xFFFFu) << 16) | (height & 0xFFFFu));
}
static inline uint32_t wm_mode_width(int32_t value)  { return ((uint32_t)value >> 16) & 0xFFFFu; }
static inline uint32_t wm_mode_height(int32_t value) { return (uint32_t)value & 0xFFFFu; }

/* How long a new mode stays on trial before the compositor puts the old
 * one back. Ten seconds is the number every desktop that ships this
 * feature has settled on - long enough to find the button on a display
 * you can suddenly barely read, short enough not to feel broken. */
#define WM_MODE_REVERT_MS 10000

typedef struct {
    int32_t window_id; /* ignored (and -1 by convention) for WM_ACTION_TOGGLE_LAUNCHER, the one action that isn't about a window */
    uint32_t action; /* wm_action_type_t */
    int32_t value; /* M45: the one action that carries a number - WM_ACTION_SET_PANEL_OVERHANG's row count. 0 for every other action, which all ignore it. A field rather than a second pipe or a second request struct: this channel already has exactly one writer per action and one reader, and both change together (the same reasoning wm_settings_request_t's own comment gives for growing rather than versioning). */
} wm_action_request_t;

/* M49: drag and drop, scoped to the one case worth having - dragging a
 * file out of file_manager.c onto the editor or onto the desktop opens it
 * there. This is the only genuinely new plumbing in that milestone: the
 * compositor has to carry a payload across two windows that know nothing
 * about each other.
 *
 * Two pipes, not three. The source announces a drag on WM_DRAG_PIPE and
 * the compositor holds the payload until the button comes up; at that
 * point it writes the payload to WM_DRAG_DATA_PIPE and *then* sends
 * WM_EVENT_DROP to whichever window the cursor is over. A single
 * well-known data pipe is safe because exactly one drop can be in flight
 * at a time and only the dropped-on client is ever told there is one.
 *
 * The payload deliberately does not ride inside wm_event_t: a filename is
 * 28 bytes, which would more than double every event this system routes
 * and cut what an event pipe can buffer from 42 events to 19 - a real
 * cost paid on every keystroke and mouse move, for a field used by one
 * event type that fires at most once per gesture. */
#define WM_DRAG_PIPE      "wm_drag"
#define WM_DRAG_DATA_PIPE "wm_drag_data"

/* leanfs's real cap is 27 + NUL (kernel/fs/leanfs.h, not visible to
 * user_space builds), and a filename is the only payload this protocol
 * carries - "a filename and nothing more" was the scope. */
#define WM_DRAG_PAYLOAD_MAX 28

typedef struct {
    char payload[WM_DRAG_PAYLOAD_MAX]; /* always NUL-terminated */
} wm_drag_request_t;

/* M48: transient toasts. One-way and fire-and-forget, the same shape
 * WM_SETTINGS_PIPE already has - a notification with a reply channel
 * would be a dialog, and M36 already built dialogs.
 *
 * Compositor-owned rather than a client, for the same reason M43's
 * launcher is: the two things that most need to speak are the compositor
 * itself (a window it had to refuse) and a client that has just *died* -
 * neither of which can be asked to draw its own notification. It is also
 * why this is a surface and not a window: a toast has to appear over
 * whatever is on screen, including the panel, without becoming something
 * the panel can then list or minimize.
 *
 * Levels differ only in the accent stripe's color. No buttons and no
 * actions on a toast: that is a dialog's job.
 */
#define WM_NOTIFY_PIPE "wm_notify"

#define WM_NOTIFY_TITLE_MAX 28
#define WM_NOTIFY_BODY_MAX  40

#define WM_NOTIFY_INFO  0
#define WM_NOTIFY_WARN  1
#define WM_NOTIFY_ERROR 2

typedef struct {
    uint32_t level; /* WM_NOTIFY_INFO/WARN/ERROR */
    char title[WM_NOTIFY_TITLE_MAX]; /* always NUL-terminated; truncated by the sender, never by the compositor */
    char body[WM_NOTIFY_BODY_MAX];
} wm_notify_request_t;

/* M33: the compositor's first genuinely global (not per-window) setting -
 * the desktop background color, previously a compile-time constant in
 * compositor.c. Same one-way "fire a request, no response expected"
 * shape as an action request; user_space/bin/settings.c is the one
 * client that ever sends one.
 *
 * M44 adds the read side. Until this milestone the compositor was the
 * only thing that ever needed to *know* the theme, so one-way was
 * enough; now desktop_icons.c paints the wallpaper (it owns the
 * full-screen desktop window, so the compositor's own background fill
 * has been invisible behind it since M32) and has to be told what was
 * picked, and settings.c wants to open showing the real current choice
 * rather than assuming the defaults. Same request/response-over-two-
 * named-pipes shape as WM_QUERY_PIPE: any single byte in, one whole
 * wm_settings_request_t back. */
#define WM_SETTINGS_PIPE            "wm_settings"
#define WM_SETTINGS_QUERY_PIPE      "wm_set_query"
#define WM_SETTINGS_QUERY_RESP_PIPE "wm_set_qresp"

typedef struct {
    /* M62: 0-100, applied to the AC'97 mixer and - as a mute, which is
     * all it can honestly mean there - to the PC speaker. Carried with
     * the rest of the settings because it is one: a person sets it once
     * and expects it to still be set tomorrow. */
    uint32_t volume;
    /* M61: whether window animations play. On by default, and a real
     * setting rather than a constant because motion that cannot be
     * disabled is a genuine accessibility problem for some people, not a
     * preference. Off means every transition is instantaneous - exactly
     * what this desktop did before M61 - rather than merely faster. */
    uint32_t animations;
    uint32_t bg_color;
    uint32_t accent_color; /* M38: the focused-window titlebar color - compositor.c's second global setting, previously TITLEBAR_FOCUS_COLOR, a compile-time constant. settings.c is still the only client that ever sends this request, so (unlike wm_create_request_t's confirm_close) there's no back-compat concern about adding a field here - the one writer and the one reader change together. */
    uint32_t wallpaper;    /* M44: which wallpaper style (user_space/lib/wallpaper.h's WALLPAPER_*) the desktop paints. The compositor stores and relays it and nothing more - it never draws a wallpaper and has no idea what any style looks like, the same relay-only role it had for the menu protocol in M41. */
} wm_settings_request_t;

#ifdef __cplusplus
}
#endif
