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
 * M42 changes two things about how a panel is treated, both so a
 * Windows-style taskbar can work at all: a click on a panel no longer
 * focuses it (a taskbar that deactivates your app every time you click
 * it isn't one), and mouse events are routed to whichever panel the
 * cursor is over rather than only to the focused window - which is what
 * lets the taskbar hover-highlight its buttons and receive clicks while
 * never holding focus. M42 also adds the launcher overlay, filled in by
 * M43: a surface this process draws itself rather than a client window,
 * because it has to appear over everything including the panel that
 * opened it and to take the keyboard while it is up (see LAUNCHER_W's
 * comment for why that is the one case in this project worth a
 * compositor-owned surface).
 *
 * M43 also adds edge snapping - dragging a titlebar into the screen's
 * left or right edge resizes and repositions the window to that half on
 * release, with a translucent preview of exactly where it will land shown
 * while the pointer is still in the edge zone. Both the gesture and an
 * external WM_ACTION_SNAP_LEFT/RIGHT go through the same
 * apply_window_action, and the preview and the result both come from the
 * one snap_rect, so none of the three can drift apart.
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
 *
 * M51 gives this compositor a z-order, which it had never had - see
 * `zorder` and z_hit_test below. Until then, paint order *was* the order
 * clients happened to connect in, focusing a window changed its titlebar
 * color without bringing it forward, and every hit-test in this file took
 * the first window whose region matched rather than the topmost visible
 * one, so a click could be delivered to a window that was entirely
 * covered. All three are the same missing thing, and they are fixed
 * together by one array of window indices plus one hit-test that walks
 * it. The three depth classes this file already had - desktop background
 * at the bottom, ordinary windows, panels always on top - survive as
 * bands within that array rather than as three separate loops.
 */
#include "children.h" /* M54: the launcher spawns, so the launcher reaps - see children.h */
#include "paths.h" /* system_api/include/paths.h - M53: /bin is where programs live now */
#include "font8x16.h" /* M38: window-title text in the titlebar - drawn through this file's own clip-aware put_pixel, not gfx_draw_text (see draw_text_clipped's own note) */
#include "gfx.h" /* M34: gfx_point_in_rect - shared hit-test helper, see draw_titlebar_buttons' own note on why drawing itself stays on this file's own clip-aware fill_rect */
#include "power_mode.h" /* system_api/include/power_mode.h - POWER_OFF/POWER_REBOOT, M47's launcher Power controls */
#include "spawn_error.h" /* system_api/include/spawn_error.h - M48, so a failed launch can say why */
#include "settings_file.h" /* M47: the desktop's three settings on disk - read once, below, before any client connects */
#include "shortcuts.h" /* system_api/include/shortcuts.h - M49's one table of window-manager chords, shared with settings.c */
#include "signal.h" /* system_api/include/signal.h - SIGTERM, M30's WM_ACTION_CLOSE */
#include "str.h"
#include "syscall_wrappers.h"
#include "wm.h"

/* M41: 8 -> WM_MAX_ROUTABLE_WINDOWS. This used to be an independent
 * number smaller than the protocol's own routable-window cap; there was
 * never a reason for the compositor to hold fewer windows than it can
 * route events to, and once the desktop grew a fourth always-on client
 * (M41's top menu bar) the difference started costing real app slots.
 * Tied to the protocol constant now so the two can't drift again - M42
 * removed that fourth client, but not the reason the two were tied. */
#define MAX_WINDOWS          WM_MAX_ROUTABLE_WINDOWS
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
/* M46: BTN_SIZE is a circle's diameter now rather than a square's side,
 * and is tied to gfx.h's shared table so this file and gfx.c cannot round
 * differently - the same arrangement M44 set up for corners. The house
 * rule for that milestone, and the answer to "which OS is this copying":
 * macOS shapes, Windows positions. The buttons became traffic lights, but
 * they stay right-aligned in minimize/maximize/close order where every
 * window in this project has always had them, and where M30's hit-test,
 * M42's tests and every user's muscle memory already put them - so
 * titlebar_button_rect below is untouched. */
#define BTN_SIZE   GFX_CIRCLE_D
#define BTN_GAP    4
#define BTN_MARGIN 4
#define BTN_CLOSE_COLOR    0x00FF5F57u
#define BTN_MAXIMIZE_COLOR 0x00FEBC2Eu
#define BTN_MINIMIZE_COLOR 0x008FA88Fu /* grey-green: it's the one of the three that isn't a warning, and a saturated green would read as "go" */
/* The mark inside each circle. Dark rather than white - macOS's own
 * choice, and the right one here: all three fills are light, and a white
 * glyph on amber is illegible at 6px. */
#define BTN_GLYPH_COLOR 0x00303030u
#define BTN_GLYPH_INSET 4 /* a 6x6 mark inside a 14px circle - big enough to tell the x from the +, small enough not to touch the rim */
#define BTN_HOVER_LIGHTEN 2 /* halfway to white: the hover feedback no titlebar button in this project has ever had */

#define TITLE_COLOR 0x00F0F0F0u
/* M46: an unfocused window's title dims. Contrast and depth are the cues
 * that survive a user changing the accent color out from under the
 * design; a titlebar hue on its own does not. */
#define TITLE_DIM_COLOR 0x009AA4B0u
#define TITLE_MARGIN 6 /* gap between the titlebar's left edge and the title text */
#define TITLE_BTN_GAP 6 /* gap kept clear between the title text and the leftmost button */

/* M38: a drop shadow - offset down-right from each ordinary window's own
 * outer (border-inclusive) rect, drawn *before* that window's own
 * border/titlebar/content so only the bottom-right sliver the window
 * itself doesn't cover ends up visible, the standard drop-shadow trick.
 * Blended toward black (SHADOW_NUM/SHADOW_DEN opacity) rather than a flat
 * fill - a solid rect would just look like a second, offset window. */
#define SHADOW_OFFSET 6
#define SHADOW_NUM 1
#define SHADOW_DEN 3
/* M46: the focused window's shadow is deeper - the other half of "these
 * two windows differ by more than a titlebar color". Same blend, same
 * offset, stronger ratio. */
#define SHADOW_FOCUS_NUM 1
#define SHADOW_FOCUS_DEN 2

/* M43: the snap preview's translucency, same fixed-ratio integer blend as
 * the shadow above (fill_rect_blend) - just mixed toward the accent color
 * instead of toward black. Weaker than the shadow's 1/3: this sits on top
 * of whatever is already on screen and has to read as a hint of where the
 * window will land, not as the window having landed there already. */
#define SNAP_PREVIEW_NUM 1
#define SNAP_PREVIEW_DEN 4

/* M44: how much of a translucent window's own pixels survive the blend
 * with what is already composited under it. 3/4 is deliberately subtle -
 * the taskbar has to stay readable as a bar with text and buttons on it,
 * so this is "you can tell the desktop is behind it", not "you can see
 * through it". The launcher overlay uses its own, slightly more opaque
 * ratio: it holds a text field you type into. */
#define TRANSLUCENT_NUM 3
#define TRANSLUCENT_DEN 4
#define LAUNCHER_OPACITY_NUM 4
#define LAUNCHER_OPACITY_DEN 5

/* M42/M43: the launcher overlay - compositor-owned rather than a client
 * window, which is the one place in this project a compositor-level
 * surface is actually justified: it has to appear over everything,
 * including the panel that opened it, without being a window that panel
 * could then focus or minimize, and it has to take the keyboard away
 * from whatever is focused for as long as it is up. Every menu since M35
 * has deliberately stayed client-side to avoid exactly this; a
 * type-to-launch box is the case that genuinely needs it.
 *
 * M42 built the surface and the toggle. M43 fills it in:
 * substring-filtered as you type, Enter spawning the selected one.
 * M53 narrows what it lists from "every file on disk" - which is what a
 * flat filesystem forced, and the reason it used to offer to run
 * settings.conf - to the contents of /bin. A launcher that can only
 * offer programs is also what retires M48's "That file is not a program."
 * toast for the common case, leaving it for the genuinely broken one.
 * Opened by WM_ACTION_TOGGLE_LAUNCHER (desktop_shell.c's Start button)
 * or Ctrl+Space, which is handled in handle_keyboard right next to
 * M32's Alt+Tab and for the same reason: a window-manager chord is not
 * something any client should be able to see or swallow. */
#define LAUNCHER_W 480
#define LAUNCHER_H 320
#define LAUNCHER_PAD      GFX_PAD /* M44: the shared dialog inset, not a number of its own - see gfx.h */
#define LAUNCHER_INPUT_H  (FONT_HEIGHT + 8)
#define LAUNCHER_ROW_H    20
#define LAUNCHER_LIST_Y   (LAUNCHER_PAD + LAUNCHER_INPUT_H + 10)
#define LAUNCHER_ROWS     10 /* M47: 12 -> 10, to leave the bottom of the overlay for the Power controls. LAUNCHER_LIST_Y + 10*20 still clears POWER_BTN_Y with room to spare */
#define LAUNCHER_MAX_ENTRIES 48   /* file_manager.c's own MAX_FILES, for the same flat namespace */
#define LAUNCHER_NAME_MAX 32      /* leanfs's real cap is 27 + NUL (kernel/fs/leanfs.h, not visible to user_space builds) - same constant file_manager.c keeps for the same reason */
#define LAUNCHER_QUERY_MAX 24
#define LAUNCHER_LIST_BUF 2048

#define LAUNCHER_BG      0x001C2233u
#define LAUNCHER_BORDER  0x004C99E6u
#define LAUNCHER_INPUT_BG 0x00101820u
#define LAUNCHER_TEXT    0x00FFFFFFu
#define LAUNCHER_HINT    0x006C8098u
#define LAUNCHER_SEL_BG  0x00335577u
#define LAUNCHER_ROW_FG  0x00C8D4E4u

/* M47: the Power controls, along the bottom of the launcher. They sit one
 * click from a search field, so both are behind a confirm step (M36's
 * rule, applied to the one action in this system that cannot be undone) -
 * a mis-click that silently powers the machine off is the worst possible
 * first impression. The confirm box is drawn over the launcher rather
 * than replacing it, so it is obvious what is being confirmed. */
#define POWER_BTN_W   96
#define POWER_BTN_H   22
#define POWER_BTN_GAP 8
#define POWER_BTN_Y   (LAUNCHER_H - LAUNCHER_PAD - POWER_BTN_H)
#define POWER_OFF_X   (LAUNCHER_W - LAUNCHER_PAD - 2 * POWER_BTN_W - POWER_BTN_GAP)
#define POWER_REBOOT_X (LAUNCHER_W - LAUNCHER_PAD - POWER_BTN_W)
#define POWER_BTN_BG      0x00303C52u
#define POWER_BTN_HOVER   0x004C6699u
#define POWER_CONFIRM_W   300
#define POWER_CONFIRM_H   96
#define POWER_CONFIRM_BG  0x00202838u

/* Which power action a confirm box is currently asking about: -1 for
 * "no box up", otherwise POWER_OFF or POWER_REBOOT. */
#define POWER_CONFIRM_NONE (-1)

/* M45: the window context menu the compositor draws for itself - what a
 * right-click on a titlebar raises. Compositor-owned for the same reason
 * the titlebar buttons are: it is about a window's frame, which is this
 * process's own chrome, and no client has any business drawing it. (The
 * *taskbar's* version of this same menu is deliberately the other way
 * round - it belongs to desktop_shell.c and reaches the compositor
 * through WM_ACTION_SET_PANEL_OVERHANG; see that action's own comment for
 * the split.) Both drive apply_window_action, so the three entry points
 * to "close this window" cannot drift apart on what close means. */
#define WMENU_W        124
#define WMENU_ITEM_H   22
#define WMENU_COUNT    3
#define WMENU_BG       0x00243040u
#define WMENU_HOVER_BG 0x003A5A80u
#define WMENU_BORDER   0x00506070u
#define WMENU_TEXT     0x00FFFFFFu

/* M48: transient toasts - the surface this system has never had for
 * telling its user anything. Compositor-owned like the launcher, and for
 * a sharper version of the same reason: the two things that most need to
 * speak here are this process (a window it had to refuse) and a client
 * that has just died, neither of which can be asked to draw its own.
 *
 * Stacked down from the top-right corner, oldest at the top, each
 * auto-dismissing on its own deadline and dismissable early by a click.
 * Top-right rather than above the taskbar: the bottom-right corner is
 * where the tray is, and a toast that covers the clock is a toast in the
 * way. */
#define TOAST_MAX      4
#define TOAST_W        300
#define TOAST_H        56
#define TOAST_GAP      8
#define TOAST_MARGIN   12
#define TOAST_STRIPE_W 4  /* the level's accent, down the left edge - the only thing that differs between an info and an error */
#define TOAST_TTL_MS   4000
#define TOAST_BG       0x00222A38u
#define TOAST_BORDER   0x00465266u
#define TOAST_TITLE_FG 0x00FFFFFFu
#define TOAST_BODY_FG  0x00B4C0D0u
#define TOAST_INFO_C   0x004C99E6u
#define TOAST_WARN_C   0x00E0A33Cu
#define TOAST_ERROR_C  0x00E05C55u

/* M49: the label that follows the cursor during a client drag. Without
 * it a drag is invisible - the source window doesn't move and the target
 * hasn't been told anything yet - so this is what makes the gesture
 * something a person can see they are doing. */
#define DRAG_LABEL_H      20
#define DRAG_LABEL_PAD    6
#define DRAG_LABEL_BG     0x00335577u
#define DRAG_LABEL_BORDER 0x004C99E6u
#define DRAG_LABEL_FG     0x00FFFFFFu

/* M31's resize-edge hit-test bitmask - moved up here (still used first by
 * resize_hit_mask, far below) because M38's cursor-shape selection in
 * redraw_rect needs these bit values earlier in the file than that
 * function is defined. */
#define RESIZE_MARGIN 5
#define RESIZE_LEFT   1
#define RESIZE_RIGHT  2
#define RESIZE_TOP    4
#define RESIZE_BOTTOM 8

typedef struct {
    int32_t x, y, w, h; /* current on-screen content geometry */
    int32_t buf_w, buf_h; /* M30: the shm-backed pixel buffer's actual, fixed dimensions (set once at connect, never mutated) - w/h above can now shrink below this (WM_ACTION_MAXIMIZE clamps to it) but never exceed it; blit_window strides by buf_w, not w, so a cropped display never reads past what this window's buffer actually holds */
    uint32_t *pixels;
    int32_t shm_id; /* M50: so reclaim_window can hand this window's pixel buffer back - see its own comment for the eleven milestones this leaked */
    int evt_write_fd; /* write end of this window's own event pipe - see wm_event_pipe_name */
    uint8_t is_panel;  /* M22: chrome-less, always-on-top, bottom-docked - see wm_create_request_t.panel */
    /* M45: a panel's buffer may be taller than the strip it docks. buf_y0
     * is the buffer row that lands on win->y (0 for every non-panel
     * window and every panel that docks its whole buffer); `overhang` is
     * how many rows immediately above win->y are currently composited and
     * click-routed - WM_ACTION_SET_PANEL_OVERHANG is the only thing that
     * ever changes it. win->h stays the docked height throughout, which
     * is what keeps window placement and maximize (both of which reserve
     * room for a panel) indifferent to a menu that is up for a moment. */
    int32_t buf_y0;
    int32_t overhang;
    uint8_t translucent; /* M44: blend rather than blit - see wm_create_request_t.translucent */
    uint8_t is_desktop; /* chrome-less, full-screen, always-on-*bottom* - see wm_create_request_t.desktop */
    uint8_t minimized; /* M22: hidden from redraw() and from click hit-testing, but the client process keeps running (WM_ACTION_TOGGLE_MINIMIZE) */
    uint8_t maximized; /* M30: WM_ACTION_MAXIMIZE/RESTORE toggle - see saved_x/y/w/h below */
    int32_t saved_x, saved_y, saved_w, saved_h; /* M30: pre-maximize geometry, restored by WM_ACTION_RESTORE - meaningless while !maximized */
    uint8_t alive; /* M29: 0 once this slot has been reclaimed (owning client died or closed) - excluded from redraw/hit-test/query, and eligible for accept_pending_window to hand to the next connecting client. Slots below window_count that are !alive are exactly the "holes" reap_dead_clients leaves behind. */
    /* M48: this window's client was asked to stop - a titlebar close, a
     * context menu, an external WM_ACTION_CLOSE/KILL. It is what keeps
     * reap_dead_clients from announcing an ordinary close as a crash:
     * both arrive here as "the process exited with a nonzero code" (a
     * SIGTERM death is 143), and only the compositor knows whether it
     * asked. Cleared when the slot is reused. */
    uint8_t close_requested;
    uint8_t confirm_close; /* M36: from wm_create_request_t.confirm_close - see its own comment. Changes what apply_window_action's WM_ACTION_CLOSE branch does, nothing else. */
    int32_t client_pid; /* M29: from wm_create_request_t.client_pid - who to watch via SYS_task_alive so a crash (not just an orderly close) still frees this slot. -1 for a slot that's never been assigned. */
    char title[WM_TITLE_MAX]; /* echoed straight from wm_create_request_t.title into wm_window_info_t.title on every query - see accept_pending_query */
} window_t;

static window_t windows[MAX_WINDOWS];
static int window_count;

static int focused_window = -1; /* -1 = nothing focused yet */
static uint32_t bg_color = DEFAULT_BG_COLOR; /* M33: settings.c's WM_SETTINGS_PIPE is the only way this ever changes at runtime */
static uint32_t accent_color = TITLEBAR_FOCUS_COLOR; /* M38: settings.c's WM_SETTINGS_PIPE is the only way this ever changes at runtime, same as bg_color above */
/* M44: which wallpaper style the desktop paints. Stored and relayed, never
 * interpreted - this process has no idea what any style looks like (see
 * wm_settings_request_t.wallpaper and user_space/lib/wallpaper.h).
 * Defaults to the gradient rather than to flat, because a desktop that
 * only looks polished after you visit Settings isn't polished. */
static uint32_t wallpaper_id = 1; /* WALLPAPER_GRADIENT */

/* Titlebar counts as part of a window's clickable/routable area, same as
 * its content - a real WM lets you drag/focus by the titlebar too. A
 * panel or desktop background has no titlebar (both are undecorated), so
 * its clickable area is just its own content rect. */
static int point_in_window(const window_t *win, int32_t x, int32_t y) {
    /* M45: whatever a panel has raised above its dock line is part of
     * what it can be clicked on - otherwise the menu it just drew would
     * be visible and inert, and the click would fall through to the
     * window underneath it. */
    int32_t top = (win->is_panel || win->is_desktop) ? win->y - win->overhang : win->y - TITLEBAR_H;
    return x >= win->x && x < win->x + win->w && y >= top && y < win->y + win->h;
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
 * (diagonal resize). Zero means "not on a resize handle at all". (The
 * RESIZE_* bit values themselves are #defined up near BTN_SIZE/TITLE_COLOR -
 * M38's cursor-shape selection in redraw_rect needs them earlier in the
 * file than this function itself is defined.) */
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

/* M55: this process's own pid, read once at startup and echoed in every
 * create response - see wm_create_response_t.compositor_pid. */
static int32_t self_pid;

static int32_t cursor_x, cursor_y;
static uint8_t prev_buttons;
static int last_hovered_panel = -1; /* M42: which panel (if any) the cursor was over on the previous mouse event - see the routing block at the bottom of handle_mouse for the one thing this is for */
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

/* ---- M51: the z-order -------------------------------------------------
 *
 * Until this milestone `windows[]`'s own index order *was* the paint
 * order, which made a window's depth a property of when its client
 * happened to connect and left every hit-test in this file walking the
 * array backwards and taking the first *match* rather than the topmost
 * *visible* window - so a click could be delivered to a window nobody
 * could see, and clicking a window you could see never brought it
 * forward. `zorder` fixes both by making depth its own thing:
 * `windows[]` indices are stable slot ids and say nothing about depth,
 * and every walk for painting, hit-testing or event routing goes through
 * here instead.
 *
 * Bottom-most first, topmost last - so painting is a plain forward walk
 * (strictly back-to-front, which is also what finally makes M38's drop
 * shadows correct: each is drawn immediately before its own window, so
 * back-to-front is exactly the order in which a nearer window covers a
 * further one's shadow) and hit-testing is a plain backward one.
 *
 * The three depth *classes* this project already had - the desktop
 * background pinned to the bottom, ordinary windows in the middle, panels
 * always on top - used to be three separate loops in redraw_rect and
 * three separate passes in window_under_cursor. They are now bands within
 * this one array: the array is kept sorted by band, insertion goes to the
 * top of the inserting window's own band, and raising can only ever move
 * a window within its band. A window's band is fixed at connect time
 * (is_panel/is_desktop never change afterwards), so the invariant cannot
 * be broken by anything but a bug in this block.
 *
 * Only alive windows appear here, exactly once each. */
static int zorder[MAX_WINDOWS];
static int z_count;

#define ZBAND_DESKTOP  0
#define ZBAND_ORDINARY 1
#define ZBAND_PANEL    2

static int window_band(const window_t *win) {
    if (win->is_desktop) {
        return ZBAND_DESKTOP;
    }
    if (win->is_panel) {
        return ZBAND_PANEL;
    }
    return ZBAND_ORDINARY;
}

/* Position of `idx` in the z-order, or -1 if it isn't in it. */
static int z_position_of(int idx) {
    for (int z = 0; z < z_count; z++) {
        if (zorder[z] == idx) {
            return z;
        }
    }
    return -1;
}

static void z_remove(int idx) {
    int z = z_position_of(idx);
    if (z < 0) {
        return;
    }
    for (int k = z; k + 1 < z_count; k++) {
        zorder[k] = zorder[k + 1];
    }
    z_count--;
}

/* Puts `idx` at the top of its own band - the only insertion this file
 * ever does, and therefore the only thing that has to preserve the
 * band-sorted invariant. Idempotent: a window already in the z-order is
 * lifted rather than duplicated, which is what makes this double as
 * "raise" (see z_raise). */
static void z_insert_top_of_band(int idx) {
    z_remove(idx);
    int band = window_band(&windows[idx]);
    int at = z_count;
    for (int z = 0; z < z_count; z++) {
        if (window_band(&windows[zorder[z]]) > band) {
            at = z;
            break;
        }
    }
    for (int k = z_count; k > at; k--) {
        zorder[k] = zorder[k - 1];
    }
    zorder[at] = idx;
    z_count++;
}

/* M51: raise-on-focus. set_focus is the only caller, because set_focus is
 * the one thing every focus path in this file genuinely goes through -
 * see its own comment for why the WM_ACTION_FOCUS branch, which looks
 * like the funnel, isn't quite one.
 *
 * A panel never holds focus at all (focus_window_under_cursor), and the
 * desktop background has nothing above it inside its own band, so in
 * practice this only ever reorders ordinary windows. It is written for
 * all three anyway because "raise within your band" is the rule, not
 * "raise if ordinary". */
static void z_raise(int idx) {
    if (z_count > 0 && zorder[z_count - 1] == idx) {
        return; /* already topmost overall - nothing to do, and no needless repaint */
    }
    int before = z_position_of(idx);
    z_insert_top_of_band(idx);
    if (z_position_of(idx) != before) {
        dirty = 1;
    }
}
/* M42/M43: the launcher's whole state - see LAUNCHER_W's own comment.
 * `entries` is every file on disk as of the last time it was opened (not
 * kept live: a list that changed under the cursor while you were typing
 * would be worse than a slightly stale one, and opening it is exactly
 * when re-reading is free). `matches` indexes into it. */
static int launcher_open;
static char launcher_entries[LAUNCHER_MAX_ENTRIES][LAUNCHER_NAME_MAX];
static int launcher_entry_count;
static int launcher_matches[LAUNCHER_MAX_ENTRIES];
static int launcher_match_count;
static int launcher_selected; /* index into launcher_matches, not into launcher_entries */
static int launcher_scroll;   /* first match drawn - see launcher_clamp_scroll */
static char launcher_query[LAUNCHER_QUERY_MAX];
static int launcher_query_len;
/* M47: -1, POWER_OFF or POWER_REBOOT - see POWER_CONFIRM_NONE. */
static int power_confirm = POWER_CONFIRM_NONE;
static int power_hover = POWER_CONFIRM_NONE; /* which Power button the cursor is over */

/* M48: the live toasts, oldest first. A fixed array compacted on removal
 * rather than a ring: at four entries the copy is nothing, and "oldest is
 * index 0" is what makes the stacking order a property of the array
 * instead of something the drawing has to work out. */
typedef struct {
    uint32_t level;
    char title[WM_NOTIFY_TITLE_MAX];
    char body[WM_NOTIFY_BODY_MAX];
    long expires_ms;
} toast_t;

static toast_t toasts[TOAST_MAX];
static int toast_count;

/* M49: a client-initiated drag in flight - the payload the source
 * announced on WM_DRAG_PIPE, held until the button comes up. Separate
 * from drag_mode below, which is this process's own window-frame drags:
 * those move a window, this one carries a filename between two clients
 * that know nothing about each other. */
/* Write end of WM_DRAG_DATA_PIPE, opened once in main - handle_mouse is
 * where a drop is delivered and it has no other way to reach it. */
static int drag_data_write_fd = -1;

static int client_drag_active;
static char client_drag_payload[WM_DRAG_PAYLOAD_MAX];
static int client_drag_last_target = -1;

/* M45: which window the titlebar context menu is open for (-1 = closed),
 * where it was raised, and which row the cursor is over. */
static int wmenu_window = -1;
static int32_t wmenu_x, wmenu_y;
static int wmenu_hover = -1;

/* M43: the snap preview's rect, in the same outer (border- and
 * titlebar-inclusive) coordinates a window's own frame is drawn in.
 * Computed by handle_mouse whenever the drag's snap target changes and
 * only read here, rather than recomputed per frame: redraw_rect runs for
 * every cursor-sized partial redraw too, and the drag state it would
 * otherwise have to reach forward into is declared much further down. */
static int snap_preview_active;
static int32_t snap_preview_x, snap_preview_y, snap_preview_w, snap_preview_h;

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

/* M38: edge-aware resize cursors - compositor.c's resize_hit_mask (M31)
 * already knows exactly which edge/corner the cursor is over; nothing
 * before this milestone ever changed what the cursor itself *looked*
 * like in response, so a resize handle was only ever discoverable by
 * trial-and-drag. Same 8x8 one-bit-per-pixel shape as cursor_shape. */
static const uint8_t cursor_shape_horizontal[CURSOR_SIZE] = { /* RESIZE_LEFT|RESIZE_RIGHT */
    0b00011000,
    0b00111100,
    0b01100110,
    0b11000011,
    0b11000011,
    0b01100110,
    0b00111100,
    0b00011000,
};

static const uint8_t cursor_shape_vertical[CURSOR_SIZE] = { /* RESIZE_TOP|RESIZE_BOTTOM */
    0b00011000,
    0b00111100,
    0b01111110,
    0b00011000,
    0b00011000,
    0b01111110,
    0b00111100,
    0b00011000,
};

static const uint8_t cursor_shape_diag_nw_se[CURSOR_SIZE] = { /* top-left <-> bottom-right corner */
    0b11110000,
    0b11000000,
    0b10000000,
    0b00000000,
    0b00000000,
    0b00000001,
    0b00000011,
    0b00001111,
};

/* M46: the move cursor, shown over a window's titlebar - the one band
 * where dragging moves the whole window rather than resizing an edge. A
 * four-way arrow: this is the same 8x8 one-bit format as the four shapes
 * above, so it costs one table and nothing else. */
static const uint8_t cursor_shape_move[CURSOR_SIZE] = {
    0b00011000,
    0b00111100,
    0b01011010,
    0b11011011,
    0b11011011,
    0b01011010,
    0b00111100,
    0b00011000,
};

static const uint8_t cursor_shape_diag_ne_sw[CURSOR_SIZE] = { /* top-right <-> bottom-left corner */
    0b00001111,
    0b00000011,
    0b00000001,
    0b00000000,
    0b00000000,
    0b10000000,
    0b11000000,
    0b11110000,
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

/* M46: the same thing with the clip test put_pixel deliberately omits -
 * for the one caller that plots individual pixels along a diagonal (the
 * close button's x) instead of walking an already-clipped rect. */
static inline void put_pixel_clipped(int32_t x, int32_t y, uint32_t color) {
    if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1) {
        put_pixel(x, y, color);
    }
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
    /* Row base hoisted out of the inner loop rather than going through
     * put_pixel, which recomputes row * pitch + col for every pixel. The
     * bounds are already clipped above, so there is nothing else
     * put_pixel would add here - it stays for the scattered single-pixel
     * callers (draw_char_clipped, draw_cursor) where it's the right
     * shape. */
    for (int32_t row = y0; row < y1; row++) {
        uint32_t *dst = back_buf + (uint32_t)row * back_pitch_pixels;
        for (int32_t col = x0; col < x1; col++) {
            dst[col] = color;
        }
    }
}

/* M38: like fill_rect, but blends each pixel toward black instead of
 * overwriting it - the drop-shadow fill. Reads back_buf (whatever the
 * desktop-background fill above already wrote into this clip pass), so
 * it has to run after that and before the window's own border/titlebar/
 * content paint over it - see redraw_rect's z-order comment. */
/* M43: M38's shadow blend, generalized to blend toward any color rather
 * than only toward black - the snap preview needs a translucent *accent*
 * rect, and a second blend loop that differed only in what it was mixing
 * with would be the kind of near-copy this file has avoided everywhere
 * else. Still fixed-ratio integer math, still no floating point (see the
 * Makefile's -mgeneral-regs-only note). Reads the pixel already composited
 * into back_buf, so it blends against whatever is genuinely underneath. */
static void fill_rect_blend(int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t color, uint32_t num, uint32_t den) {
    int32_t x0 = max_i32(x, clip_x0);
    int32_t y0 = max_i32(y, clip_y0);
    int32_t x1 = min_i32(x + w, clip_x1);
    int32_t y1 = min_i32(y + h, clip_y1);
    uint32_t sr = (color >> 16) & 0xFF;
    uint32_t sg = (color >> 8) & 0xFF;
    uint32_t sb = color & 0xFF;
    for (int32_t row = y0; row < y1; row++) {
        for (int32_t col = x0; col < x1; col++) {
            uint32_t existing = back_buf[(uint32_t)row * back_pitch_pixels + (uint32_t)col];
            uint32_t r = (((existing >> 16) & 0xFF) * (den - num) + sr * num) / den;
            uint32_t g = (((existing >> 8) & 0xFF) * (den - num) + sg * num) / den;
            uint32_t b = ((existing & 0xFF) * (den - num) + sb * num) / den;
            put_pixel(col, row, (r << 16) | (g << 8) | b);
        }
    }
}

/* Defined much further down, with the rest of the panel-aware placement
 * limits; forward-declared here because the shadow fill (immediately
 * below) is one of its callers and has to come earlier in the file, for
 * the same z-order reasons draw_titlebar_buttons already does. */
static int32_t content_bottom_limit(void);

/* M45: clipped so a window's shadow never falls on the docked taskbar.
 * With M44's translucency the bar blends against whatever is composited
 * under it, so a shadow reaching that far doesn't sit *behind* the panel,
 * it tints it - which makes the bar's own colors depend on which windows
 * happen to be open. It also broke the input harness, whose taskbar
 * probes are how it counts windows at all. The rule is simple enough to
 * state: the panel is chrome, and window shadows stop at it. */
static void fill_rect_shadow(int32_t x, int32_t y, int32_t w, int32_t h, int focused) {
    int32_t limit = content_bottom_limit();
    if (y + h > limit) {
        h = limit - y;
    }
    if (h <= 0) {
        return;
    }
    fill_rect_blend(x, y, w, h, 0x00000000u,
                     focused ? SHADOW_FOCUS_NUM : SHADOW_NUM,
                     focused ? SHADOW_FOCUS_DEN : SHADOW_DEN);
}

/* M42: the sub-rect variant M41's panel-overhang blit needed went away
 * with the overhang itself - there is one caller again and it always
 * wants the whole window, so this is back to being one function. */
static void blit_window(const window_t *win) {
    int32_t x0 = max_i32(win->x, clip_x0);
    /* M45: a panel with a raised overhang paints the rows above its dock
     * line too - one rect, not a second blit path, because the source row
     * for any screen row is the same expression either way (see buf_y0).
     * `overhang` is 0 for every window that has no such thing. */
    int32_t y0 = max_i32(win->y - win->overhang, clip_y0);
    int32_t x1 = min_i32(win->x + win->w, clip_x1);
    int32_t y1 = min_i32(win->y + win->h, clip_y1);
    if (x1 <= x0) {
        return;
    }
    /* M44: a translucent window (only the taskbar - see
     * wm_create_request_t.translucent) is blended against whatever is
     * already composited underneath instead of overwriting it. Same
     * fixed-ratio integer math as the shadow and the snap preview, just
     * with a per-pixel source instead of one color - which is exactly
     * what costs it the memcpy below, and why it is opt-in. */
    if (win->translucent) {
        for (int32_t row = y0; row < y1; row++) {
            const uint32_t *src_row = win->pixels + (uint32_t)(row - win->y + win->buf_y0) * (uint32_t)win->buf_w;
            uint32_t *dst = back_buf + (uint32_t)row * back_pitch_pixels;
            for (int32_t col = x0; col < x1; col++) {
                uint32_t src = src_row[col - win->x];
                uint32_t under = dst[col];
                uint32_t r = (((under >> 16) & 0xFF) * (TRANSLUCENT_DEN - TRANSLUCENT_NUM) + ((src >> 16) & 0xFF) * TRANSLUCENT_NUM) / TRANSLUCENT_DEN;
                uint32_t g = (((under >> 8) & 0xFF) * (TRANSLUCENT_DEN - TRANSLUCENT_NUM) + ((src >> 8) & 0xFF) * TRANSLUCENT_NUM) / TRANSLUCENT_DEN;
                uint32_t b = ((under & 0xFF) * (TRANSLUCENT_DEN - TRANSLUCENT_NUM) + (src & 0xFF) * TRANSLUCENT_NUM) / TRANSLUCENT_DEN;
                dst[col] = (r << 16) | (g << 8) | b;
            }
        }
        return;
    }
    size_t row_bytes = (size_t)(x1 - x0) * sizeof(uint32_t);
    for (int32_t row = y0; row < y1; row++) {
        /* buf_w, not w - M30 lets w shrink below buf_w (WM_ACTION_MAXIMIZE
         * clamping), but the underlying pixel buffer's real row stride
         * never changes, so indexing by anything else would read the
         * wrong bytes (or, once w > buf_w could ever happen, off the end
         * of it entirely - see window_t's own comment on buf_w). */
        const uint32_t *src_row = win->pixels + (uint32_t)(row - win->y + win->buf_y0) * (uint32_t)win->buf_w;
        /* One memcpy per row instead of a per-pixel put_pixel loop. This
         * is the hottest loop in the system - every window, every full
         * redraw, and the desktop background alone is a whole screen of
         * pixels - and the clip rect has already reduced it to a
         * contiguous run in both buffers, which is exactly what memcpy
         * wants. Nothing about *what* gets copied changed. */
        memcpy(back_buf + (uint32_t)row * back_pitch_pixels + (uint32_t)x0,
               src_row + (x0 - win->x), row_bytes);
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

/* M46: which window's which titlebar button the cursor is over, or -1 for
 * none. The compositor already receives every mouse move for hit-testing,
 * so hover feedback is new state, not new plumbing. */
static int hover_btn_window = -1;
static titlebar_button_t hover_btn;

/* ---- M51: the one hit-test ------------------------------------------
 *
 * Before this milestone there were five near-copies of the same backwards
 * `for (i = window_count - 1; ...)` loop - the plain window pick, the
 * titlebar-button pick, the resize-edge pick (twice: once for the click,
 * once for the cursor shape) and the move-drag titlebar pick - and every
 * one of them carried the same apology in a comment: it took the first
 * window whose *region* matched rather than the topmost *visible* one, so
 * a click could be delivered to a window that was completely covered.
 *
 * They are one function now, and it is occlusion-correct: it walks the
 * z-order from the top down, and the first window whose painted frame
 * contains the point is the only window allowed to answer. If the region
 * the caller asked about isn't there, the answer is "nothing" - not "keep
 * looking underneath", which is exactly the bug.
 *
 * The one deliberate exception is HIT_RESIZE. A resize handle straddles
 * the frame edge and reaches RESIZE_MARGIN pixels *outside* it, so a
 * window's own grab halo is checked before that window is asked whether
 * it occludes the point. That keeps the halo of a window on top winning
 * over the frame of one beneath it, which is the behavior you want, while
 * still refusing to hand a covered window's edge a click.
 */
typedef enum {
    HIT_FRAME,    /* anywhere this window is painted, border and titlebar included - what a focus click and event routing want */
    HIT_TITLEBAR, /* the titlebar band only, for a move-drag or a context menu */
    HIT_BUTTON,   /* a titlebar button; *out_detail receives which one (titlebar_button_t) */
    HIT_RESIZE,   /* a resize edge or corner; *out_detail receives the RESIZE_* mask */
} hit_region_t;

/* Which windows may *answer*. Every alive, non-minimized window occludes
 * regardless of this - that is the point of the fix, and it is why a
 * titlebar under the taskbar is unreachable rather than reachable through
 * it. */
#define WCLASS_DESKTOP  1u
#define WCLASS_ORDINARY 2u
#define WCLASS_PANEL    4u
#define WCLASS_ALL      (WCLASS_DESKTOP | WCLASS_ORDINARY | WCLASS_PANEL)

/* The rect this window actually paints over: its content, plus (for an
 * ordinary window) the border and titlebar drawn around it, plus (for a
 * panel) whatever it has raised above its dock line. Anything inside here
 * belongs to this window even if the region the caller asked about is
 * somewhere else in it. */
static int point_occluded_by(const window_t *win, int32_t px, int32_t py) {
    if (win->is_panel || win->is_desktop) {
        return point_in_window(win, px, py);
    }
    return px >= win->x - BORDER && px < win->x + win->w + BORDER &&
           py >= win->y - TITLEBAR_H - BORDER && py < win->y + win->h + BORDER;
}

static int window_class_bit(const window_t *win) {
    if (win->is_desktop) {
        return WCLASS_DESKTOP;
    }
    if (win->is_panel) {
        return WCLASS_PANEL;
    }
    return WCLASS_ORDINARY;
}

static int hit_region_matches(const window_t *win, int32_t px, int32_t py,
                               hit_region_t region, int *out_detail) {
    switch (region) {
    case HIT_FRAME:
        return point_occluded_by(win, px, py);
    case HIT_TITLEBAR:
        return !win->is_panel && !win->is_desktop && point_in_titlebar(win, px, py);
    case HIT_BUTTON:
        if (win->is_panel || win->is_desktop) {
            return 0;
        }
        for (int b = 0; b < BTN_COUNT; b++) {
            int32_t bx, by;
            titlebar_button_rect(win, (titlebar_button_t)b, &bx, &by);
            if (gfx_point_in_rect(px, py, bx, by, BTN_SIZE, BTN_SIZE)) {
                if (out_detail) {
                    *out_detail = b;
                }
                return 1;
            }
        }
        return 0;
    case HIT_RESIZE: {
        if (win->is_panel || win->is_desktop) {
            return 0;
        }
        int mask = resize_hit_mask(win, px, py);
        if (mask && out_detail) {
            *out_detail = mask;
        }
        return mask != 0;
    }
    }
    return 0;
}

static int z_hit_test(int32_t px, int32_t py, unsigned classes, hit_region_t region, int *out_detail) {
    for (int z = z_count - 1; z >= 0; z--) {
        int idx = zorder[z];
        const window_t *w = &windows[idx];
        if (!w->alive || w->minimized) {
            continue; /* nothing there to click and nothing there to hide what's under it */
        }
        if ((window_class_bit(w) & classes) &&
            hit_region_matches(w, px, py, region, out_detail)) {
            return idx;
        }
        if (point_occluded_by(w, px, py)) {
            return -1; /* this window covers the point and didn't want it - nothing below it can have it either */
        }
    }
    return -1;
}

/* Which window's which titlebar button the cursor is over, or -1 for
 * none. Shared by the click handler and M46's hover tracker, so the
 * button that lights and the button that acts can't disagree. */
static int titlebar_button_at(int32_t px, int32_t py, titlebar_button_t *out_btn) {
    int detail = 0;
    int idx = z_hit_test(px, py, WCLASS_ORDINARY, HIT_BUTTON, &detail);
    if (idx >= 0) {
        *out_btn = (titlebar_button_t)detail;
    }
    return idx;
}

static uint32_t lighten(uint32_t color, uint32_t num, uint32_t den) {
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        uint32_t c = (color >> shift) & 0xFF;
        out |= (c + (255 - c) * num / den) << shift;
    }
    return out;
}

/* A filled GFX_CIRCLE_D disc through this file's own clip-aware
 * fill_rect, using gfx.h's shared inset table - see gfx_circle_inset for
 * why the table is shared even though the drawing can't be. */
static void fill_circle(int32_t x, int32_t y, uint32_t color) {
    for (int32_t row = 0; row < GFX_CIRCLE_D; row++) {
        int32_t inset = gfx_circle_inset(row);
        fill_rect(x + inset, y + row, GFX_CIRCLE_D - 2 * inset, 1, color);
    }
}

/* x on close, + on maximize, - on minimize: 1px strokes inside a
 * BTN_GLYPH_INSET-inset box, so all three share one size and one center
 * and read as a set. The x is what the request was really about - a red
 * square says "something", a red circle with an x in it says "close". */
static void draw_button_glyph(int32_t bx, int32_t by, titlebar_button_t btn) {
    int32_t g0 = BTN_GLYPH_INSET;
    int32_t g1 = BTN_SIZE - 1 - BTN_GLYPH_INSET;
    int32_t mid = BTN_SIZE / 2;
    if (btn == BTN_CLOSE) {
        for (int32_t k = 0; k <= g1 - g0; k++) {
            put_pixel_clipped(bx + g0 + k, by + g0 + k, BTN_GLYPH_COLOR);
            put_pixel_clipped(bx + g1 - k, by + g0 + k, BTN_GLYPH_COLOR);
        }
        return;
    }
    fill_rect(bx + g0, by + mid - 1, g1 - g0 + 1, 1, BTN_GLYPH_COLOR);
    if (btn == BTN_MAXIMIZE) {
        fill_rect(bx + mid - 1, by + g0, 1, g1 - g0 + 1, BTN_GLYPH_COLOR);
    }
}

/* M46: the glyphs are drawn on the focused window and on whichever window
 * the cursor is over, and omitted otherwise - macOS's own rule, and what
 * keeps three saturated dots from shouting out of every unfocused window
 * on the desktop. The circles themselves are always drawn: a titlebar
 * with no buttons at all would be worse than a quiet one. */
static void draw_titlebar_buttons(const window_t *win, int idx, int focused) {
    static const uint32_t colors[BTN_COUNT] = {BTN_MINIMIZE_COLOR, BTN_MAXIMIZE_COLOR, BTN_CLOSE_COLOR};
    int hovered_here = (hover_btn_window == idx);
    for (int b = 0; b < BTN_COUNT; b++) {
        int32_t bx, by;
        titlebar_button_rect(win, (titlebar_button_t)b, &bx, &by);
        uint32_t color = colors[b];
        if (hovered_here && hover_btn == (titlebar_button_t)b) {
            color = lighten(color, 1, BTN_HOVER_LIGHTEN);
        }
        fill_circle(bx, by, color);
        if (focused || hovered_here) {
            draw_button_glyph(bx, by, (titlebar_button_t)b);
        }
    }
}

/* M38: window-title text, drawn through this file's own clip-aware
 * put_pixel rather than gfx.h's gfx_draw_char/gfx_draw_text - the same
 * reason gfx_point_in_rect (M34) was fine to share but fill_rect wasn't:
 * gfx.c's primitives only clip to a ctx's own 0..width/height, with no
 * idea this file's clip_x0..clip_y1 partial-redraw rect exists, and
 * title text (drawn on every window, every full redraw) is exactly the
 * kind of per-pixel work that rect exists to bound.
 *
 * M39: bold is now a lookup into the generated font8x16_bold table
 * rather than the `bits | (bits >> 1)` this did per-pixel at draw time.
 * Same one-column dilation, but done once in tools/gen-font.c and - the
 * actual fix - lossless: the old smear thickened rightward *inside the
 * byte*, so any glyph with ink already in column 7 had that column
 * silently dropped instead of thickened. Every glyph now leaves column
 * 7 blank as a reserved advance gap (font8x16.h's FONT_GLYPH_COLS, which
 * gen-font.c enforces), so there is nothing left to fall off the end. */
static void draw_char_clipped(int32_t x, int32_t y, char c, uint32_t color, int bold) {
    uint8_t code = (uint8_t)c;
    if (code >= 128) {
        return;
    }
    const uint8_t *glyph = bold ? font8x16_bold[code] : font8x16[code];
    for (int32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        int32_t py = y + row;
        if (py < clip_y0 || py >= clip_y1) {
            continue;
        }
        for (int32_t col = 0; col < FONT_WIDTH; col++) {
            int32_t px = x + col;
            if (px < clip_x0 || px >= clip_x1) {
                continue;
            }
            if (bits & (0x80 >> col)) {
                put_pixel(px, py, color);
            }
        }
    }
}

static void draw_text_clipped(int32_t x, int32_t y, const char *s, uint32_t color, int bold) {
    int32_t cx = x;
    for (const char *p = s; *p; p++) {
        draw_char_clipped(cx, y, *p, color, bold);
        cx += FONT_WIDTH;
    }
}

/* Truncates win->title (already NUL-terminated, at most WM_TITLE_MAX-1
 * chars) to however many whole glyphs fit before the leftmost titlebar
 * button - a window shrunk below M31's MIN_WIN_W could otherwise draw
 * title text straight through the close button. Writes into out (must be
 * >= WM_TITLE_MAX bytes), doesn't touch win->title itself. */
static void fit_title(const window_t *win, char *out) {
    int32_t leftmost_btn_x;
    int32_t unused_y;
    titlebar_button_rect(win, (titlebar_button_t)(BTN_COUNT - 1), &leftmost_btn_x, &unused_y);
    int32_t avail = leftmost_btn_x - TITLE_BTN_GAP - (win->x + TITLE_MARGIN);
    int32_t max_chars = avail > 0 ? avail / FONT_WIDTH : 0;
    int i = 0;
    for (; win->title[i] && i < max_chars && i < WM_TITLE_MAX - 1; i++) {
        out[i] = win->title[i];
    }
    out[i] = '\0';
}

/* M38: which resize-cursor shape (if any) belongs over the cursor's
 * current position - defined further down, after resize_hit_mask and the
 * drag state it needs (M31) actually exist in the file; forward-declared
 * here so redraw_rect (needs it, but is defined earlier for the same
 * z-order reasons draw_titlebar_buttons etc. already are) can call it. */
static int hovered_resize_mask(void);

/* M46: whether the cursor is over an ordinary window's titlebar band -
 * checked after the resize mask, since a titlebar's own top edge is also
 * a resize handle and the resize cursor is the more specific answer
 * there. Forward-declared for the same reason hovered_resize_mask is:
 * redraw_rect needs it, and it needs the drag state declared far below. */
static int cursor_over_titlebar(void);

static void draw_cursor(const uint8_t *shape) {
    int32_t x0 = max_i32(cursor_x, clip_x0);
    int32_t y0 = max_i32(cursor_y, clip_y0);
    int32_t x1 = min_i32(cursor_x + CURSOR_SIZE, clip_x1);
    int32_t y1 = min_i32(cursor_y + CURSOR_SIZE, clip_y1);
    for (int32_t row = y0; row < y1; row++) {
        uint8_t bits = shape[row - cursor_y];
        for (int32_t col = x0; col < x1; col++) {
            if (bits & (0x80 >> (col - cursor_x))) {
                put_pixel(col, row, CURSOR_COLOR);
            }
        }
    }
}

/* A 1px outline, four fill_rects - the clip-aware counterpart of gfx.c's
 * gfx_draw_rect, which this file can't use for the same reason it has
 * its own fill_rect (see draw_text_clipped's note). */
static void stroke_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

/* M44: the same corner shape gfx.c's rounded rects draw, through this
 * file's clip-aware fill_rect. The inset table itself is shared
 * (gfx_corner_inset) rather than copied - the launcher and the taskbar
 * have to round identically or the desktop reads as two designs. */
static int32_t rounded_row_inset(int32_t row, int32_t h) {
    if (row < GFX_CORNER_R) {
        return gfx_corner_inset(row);
    }
    if (row >= h - GFX_CORNER_R) {
        return gfx_corner_inset(h - 1 - row);
    }
    return 0;
}

static void fill_rect_rounded(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (w < 2 * GFX_CORNER_R || h < 2 * GFX_CORNER_R) {
        fill_rect(x, y, w, h, color);
        return;
    }
    for (int32_t row = 0; row < h; row++) {
        int32_t inset = rounded_row_inset(row, h);
        fill_rect(x + inset, y + row, w - 2 * inset, 1, color);
    }
}

static void fill_rect_rounded_blend(int32_t x, int32_t y, int32_t w, int32_t h,
                                     uint32_t color, uint32_t num, uint32_t den) {
    if (w < 2 * GFX_CORNER_R || h < 2 * GFX_CORNER_R) {
        fill_rect_blend(x, y, w, h, color, num, den);
        return;
    }
    for (int32_t row = 0; row < h; row++) {
        int32_t inset = rounded_row_inset(row, h);
        fill_rect_blend(x + inset, y + row, w - 2 * inset, 1, color, num, den);
    }
}

/* A filled rect whose *top* corners are rounded and whose bottom ones are
 * square - see the window-frame call site for why a window can only have
 * the top half of the treatment. */
static void draw_frame_top_rounded(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (w < 2 * GFX_CORNER_R || h < GFX_CORNER_R) {
        fill_rect(x, y, w, h, color);
        return;
    }
    for (int32_t row = 0; row < GFX_CORNER_R; row++) {
        int32_t inset = gfx_corner_inset(row);
        fill_rect(x + inset, y + row, w - 2 * inset, 1, color);
    }
    fill_rect(x, y + GFX_CORNER_R, w, h - GFX_CORNER_R, color);
}

/* The horizontal run each row of a rounded outline needs at either end -
 * the same rule (and the same reason for it) as gfx.c's outline_run; see
 * that function's comment. */
static int32_t rounded_outline_run(int32_t row, int32_t w, int32_t h) {
    int32_t inset = rounded_row_inset(row, h);
    int32_t above = (row == 0) ? w : rounded_row_inset(row - 1, h);
    int32_t below = (row == h - 1) ? w : rounded_row_inset(row + 1, h);
    int32_t reach = above > below ? above : below;
    if (reach > w - inset) {
        reach = w - inset;
    }
    int32_t run = reach - inset;
    return run < 1 ? 1 : run;
}

static void stroke_rect_rounded(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (w < 2 * GFX_CORNER_R || h < 2 * GFX_CORNER_R) {
        stroke_rect(x, y, w, h, color);
        return;
    }
    for (int32_t row = 0; row < h; row++) {
        int32_t inset = rounded_row_inset(row, h);
        int32_t run = rounded_outline_run(row, w, h);
        fill_rect(x + inset, y + row, run, 1, color);
        fill_rect(x + w - inset - run, y + row, run, 1, color);
    }
}

/* Where the launcher overlay sits: horizontally centered, and a third of
 * the way down rather than dead center - the conventional placement for a
 * search box you type into, and it keeps the results list clear of the
 * taskbar that opened it. */
static void launcher_rect(int32_t *out_x, int32_t *out_y) {
    *out_x = ((int32_t)fb_info.width - LAUNCHER_W) / 2;
    *out_y = ((int32_t)fb_info.height - LAUNCHER_H) / 3;
}

/* The y of match row `i` (0-based from the top of the *visible* list),
 * in absolute screen coordinates. Shared by the drawing below and the
 * click hit-test (launcher_click), so the two can't disagree about where
 * a row is - the same reason titlebar_button_rect exists. */
static int32_t launcher_row_y(int32_t launcher_y, int i) {
    return launcher_y + LAUNCHER_LIST_Y + i * LAUNCHER_ROW_H;
}

/* M47: the two Power buttons, along the bottom of the overlay. Drawn
 * through this file's own clip-aware primitives for the same reason
 * everything else here is (see draw_text_clipped's note); the label is
 * centered by measuring it, since these are the only two buttons in this
 * process and gfx.c's gfx_draw_button is not reachable from here. */
static void draw_power_button(int32_t x, int32_t y, int32_t bx, const char *label, int mode) {
    int32_t px = x + bx;
    int32_t py = y + POWER_BTN_Y;
    fill_rect_rounded(px, py, POWER_BTN_W, POWER_BTN_H,
                       power_hover == mode ? POWER_BTN_HOVER : POWER_BTN_BG);
    stroke_rect_rounded(px, py, POWER_BTN_W, POWER_BTN_H, LAUNCHER_BORDER);
    int32_t label_w = 0;
    for (const char *p = label; *p; p++) {
        label_w += FONT_WIDTH;
    }
    draw_text_clipped(px + (POWER_BTN_W - label_w) / 2, py + (POWER_BTN_H - FONT_HEIGHT) / 2,
                       label, LAUNCHER_TEXT, 0);
}

static void draw_power_row(int32_t x, int32_t y) {
    draw_power_button(x, y, POWER_OFF_X, "Shut Down", POWER_OFF);
    draw_power_button(x, y, POWER_REBOOT_X, "Restart", POWER_REBOOT);
}

/* The confirm step. Keyboard-driven (Y/Enter confirms, Escape/N cancels)
 * rather than a second pair of buttons: the launcher already owns the
 * keyboard while it is up, and two more click targets over the two that
 * raised them is how a mis-click becomes a double mis-click. */
static void draw_power_confirm(int32_t x, int32_t y) {
    int32_t cx = x + (LAUNCHER_W - POWER_CONFIRM_W) / 2;
    int32_t cy = y + (LAUNCHER_H - POWER_CONFIRM_H) / 2;
    fill_rect_rounded(cx, cy, POWER_CONFIRM_W, POWER_CONFIRM_H, POWER_CONFIRM_BG);
    stroke_rect_rounded(cx, cy, POWER_CONFIRM_W, POWER_CONFIRM_H, LAUNCHER_BORDER);
    draw_text_clipped(cx + GFX_PAD, cy + GFX_PAD,
                       power_confirm == POWER_REBOOT ? "Restart this machine?" : "Shut down this machine?",
                       LAUNCHER_TEXT, 1);
    draw_text_clipped(cx + GFX_PAD, cy + GFX_PAD + 2 * FONT_HEIGHT,
                       "Y / Enter = yes", LAUNCHER_ROW_FG, 0);
    draw_text_clipped(cx + GFX_PAD, cy + GFX_PAD + 3 * FONT_HEIGHT,
                       "N / Esc / click = cancel", LAUNCHER_ROW_FG, 0);
}

static void draw_launcher(void) {
    int32_t x, y;
    launcher_rect(&x, &y);
    /* M44: rounded and slightly translucent - enough to show that the
     * desktop is still behind it, not enough to make the text field you
     * type into hard to read. */
    fill_rect_rounded_blend(x, y, LAUNCHER_W, LAUNCHER_H, LAUNCHER_BG,
                             LAUNCHER_OPACITY_NUM, LAUNCHER_OPACITY_DEN);
    stroke_rect_rounded(x, y, LAUNCHER_W, LAUNCHER_H, LAUNCHER_BORDER);

    /* The search field. An empty query shows a hint rather than nothing,
     * since an empty box with a caret in it says less about what to do
     * with it than three words do. */
    int32_t input_x = x + LAUNCHER_PAD;
    int32_t input_y = y + LAUNCHER_PAD;
    int32_t input_w = LAUNCHER_W - 2 * LAUNCHER_PAD;
    fill_rect_rounded(input_x, input_y, input_w, LAUNCHER_INPUT_H, LAUNCHER_INPUT_BG);
    stroke_rect_rounded(input_x, input_y, input_w, LAUNCHER_INPUT_H, LAUNCHER_BORDER);
    int32_t text_y = input_y + (LAUNCHER_INPUT_H - FONT_HEIGHT) / 2;
    if (launcher_query_len > 0) {
        draw_text_clipped(input_x + 6, text_y, launcher_query, LAUNCHER_TEXT, 0);
    } else {
        draw_text_clipped(input_x + 6, text_y, "Type to search", LAUNCHER_HINT, 0);
    }
    fill_rect(input_x + 6 + launcher_query_len * FONT_WIDTH, text_y, 2, FONT_HEIGHT, LAUNCHER_TEXT);

    if (launcher_match_count == 0) {
        draw_text_clipped(x + LAUNCHER_PAD, launcher_row_y(y, 0) + 2, "No matches", LAUNCHER_HINT, 0);
    } else {
        for (int i = 0; i < LAUNCHER_ROWS; i++) {
            int m = launcher_scroll + i;
            if (m >= launcher_match_count) {
                break;
            }
            int32_t ry = launcher_row_y(y, i);
            if (m == launcher_selected) {
                fill_rect_rounded(x + LAUNCHER_PAD / 2, ry, LAUNCHER_W - LAUNCHER_PAD, LAUNCHER_ROW_H, LAUNCHER_SEL_BG);
            }
            draw_text_clipped(x + LAUNCHER_PAD, ry + 2, launcher_entries[launcher_matches[m]],
                               m == launcher_selected ? LAUNCHER_TEXT : LAUNCHER_ROW_FG,
                               m == launcher_selected);
        }
    }

    draw_power_row(x, y);
    if (power_confirm != POWER_CONFIRM_NONE) {
        draw_power_confirm(x, y);
    }
}

/* M48: where toast `i` sits - stacked down from the top-right corner,
 * oldest at index 0. One function, so the drawing and the click hit-test
 * cannot disagree about where a toast is (the same reason
 * titlebar_button_rect exists). */
static void toast_rect(int i, int32_t *out_x, int32_t *out_y) {
    *out_x = (int32_t)fb_info.width - TOAST_W - TOAST_MARGIN;
    *out_y = TOAST_MARGIN + i * (TOAST_H + TOAST_GAP);
}

static uint32_t toast_accent(uint32_t level) {
    if (level == WM_NOTIFY_ERROR) {
        return TOAST_ERROR_C;
    }
    return level == WM_NOTIFY_WARN ? TOAST_WARN_C : TOAST_INFO_C;
}

static void draw_toasts(void) {
    for (int i = 0; i < toast_count; i++) {
        int32_t x, y;
        toast_rect(i, &x, &y);
        fill_rect_rounded(x, y, TOAST_W, TOAST_H, TOAST_BG);
        stroke_rect_rounded(x, y, TOAST_W, TOAST_H, TOAST_BORDER);
        /* The stripe is inset by a pixel so the rounded border still
         * reads as the toast's outline rather than being overdrawn at
         * the corners. */
        fill_rect(x + 1, y + GFX_CORNER_R, TOAST_STRIPE_W, TOAST_H - 2 * GFX_CORNER_R,
                   toast_accent(toasts[i].level));
        draw_text_clipped(x + TOAST_STRIPE_W + 10, y + 10, toasts[i].title, TOAST_TITLE_FG, 1 /* bold */);
        draw_text_clipped(x + TOAST_STRIPE_W + 10, y + 10 + FONT_HEIGHT + 4, toasts[i].body, TOAST_BODY_FG, 0);
    }
}

/* M45: the three verbs a window context menu offers. Item 0's label
 * follows the window's own state, so the menu never offers to minimize
 * something that already is - the taskbar's copy of this menu
 * (desktop_shell.c) makes the same choice from the same field, which is
 * why both read the state rather than hardcoding a label. */
static const char *wmenu_label(int idx, const window_t *win) {
    if (idx == 0) {
        return win->minimized ? "Restore" : "Minimize";
    }
    return idx == 1 ? "Close" : "Force Quit";
}

static void draw_window_menu(void) {
    const window_t *win = &windows[wmenu_window];
    int32_t h = WMENU_ITEM_H * WMENU_COUNT;
    fill_rect_rounded(wmenu_x, wmenu_y, WMENU_W, h, WMENU_BG);
    stroke_rect_rounded(wmenu_x, wmenu_y, WMENU_W, h, WMENU_BORDER);
    for (int i = 0; i < WMENU_COUNT; i++) {
        int32_t ry = wmenu_y + i * WMENU_ITEM_H;
        if (i == wmenu_hover) {
            fill_rect_rounded(wmenu_x + 2, ry + 1, WMENU_W - 4, WMENU_ITEM_H - 2, WMENU_HOVER_BG);
        }
        draw_text_clipped(wmenu_x + 8, ry + (WMENU_ITEM_H - FONT_HEIGHT) / 2,
                           wmenu_label(i, win), WMENU_TEXT, 0);
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
    /* M51: one strictly back-to-front walk of the z-order, where there
     * used to be three loops over windows[] in creation order - the
     * desktop band, then the ordinary band, then the panel band. The
     * bands still exist; they are positions in `zorder` now rather than
     * three separate passes, which is what lets a click raise a window
     * (z_raise) and have the paint order follow.
     *
     * Back-to-front is also what finally makes M38's drop shadows right.
     * fill_rect_shadow draws each window's shadow immediately before that
     * window, so whatever is painted *after* covers it - correct only if
     * "after" means "in front of". In creation order it did not: a window
     * in front of an occluded one still had its shadow painted over by
     * whatever happened to connect later. No new code, just the right
     * order. */
    for (int z = 0; z < z_count; z++) {
        int i = zorder[z];
        const window_t *win = &windows[i];
        if (!win->alive || win->minimized || win->is_panel) {
            continue; /* panels are the top band and are drawn below, after the snap preview */
        }
        if (win->is_desktop) {
            blit_window(win); /* no border/titlebar - a desktop background is its own chrome, same as a panel */
            continue;
        }
        int focused = (i == focused_window);
        uint32_t titlebar_color = focused ? accent_color : TITLEBAR_COLOR;
        fill_rect_shadow(win->x - BORDER + SHADOW_OFFSET, win->y - TITLEBAR_H - BORDER + SHADOW_OFFSET,
                          win->w + 2 * BORDER, win->h + TITLEBAR_H + 2 * BORDER, focused);
        /* M44: the frame's *top* corners are rounded, its bottom ones are
         * not. Only the top is safe to round without alpha: the content
         * area is a straight memcpy of the client's own buffer (see
         * blit_window), so a rounded bottom corner would just show that
         * client's square pixels poking through the curve. Rounding the
         * titlebar to match keeps the two curves concentric rather than
         * leaving a square bar inside a curved border. */
        draw_frame_top_rounded(win->x - BORDER, win->y - TITLEBAR_H - BORDER,
                                win->w + 2 * BORDER, win->h + TITLEBAR_H + 2 * BORDER, BORDER_COLOR);
        draw_frame_top_rounded(win->x, win->y - TITLEBAR_H, win->w, TITLEBAR_H, titlebar_color);
        char fitted_title[WM_TITLE_MAX];
        fit_title(win, fitted_title);
        draw_text_clipped(win->x + TITLE_MARGIN, win->y - TITLEBAR_H + (TITLEBAR_H - FONT_HEIGHT) / 2,
                           fitted_title, focused ? TITLE_COLOR : TITLE_DIM_COLOR, 1 /* bold */);
        draw_titlebar_buttons(win, i, focused);
        blit_window(win);
    }
    /* M43: the snap preview, above every ordinary window (it is about
     * where one is going, so it has to be visible over the one being
     * dragged) but below the panels, which stay topmost as always. */
    if (snap_preview_active) {
        fill_rect_blend(snap_preview_x, snap_preview_y, snap_preview_w, snap_preview_h,
                         accent_color, SNAP_PREVIEW_NUM, SNAP_PREVIEW_DEN);
        stroke_rect(snap_preview_x, snap_preview_y, snap_preview_w, snap_preview_h, accent_color);
    }
    for (int z = 0; z < z_count; z++) {
        const window_t *win = &windows[zorder[z]];
        if (win->alive && win->is_panel && !win->minimized) {
            blit_window(win); /* no border/titlebar - a panel is its own chrome */
        }
    }
    /* M45: above the panels (it can be raised on a window whose titlebar
     * sits right against the taskbar) but below the launcher, which owns
     * the screen outright while it is up. */
    if (wmenu_window >= 0 && windows[wmenu_window].alive) {
        draw_window_menu();
    }
    /* Above every window and every panel, below only the cursor - see
     * draw_launcher's own note on why this is compositor-owned. */
    if (launcher_open) {
        draw_launcher();
    }
    /* M48: above even the launcher. A toast is usually *about* something
     * that just failed, and the launcher is one of the things that raises
     * them (a spawn that didn't work) - so a toast hidden behind it would
     * be hidden at exactly the moment it mattered. */
    draw_toasts();
    /* M49: below the cursor and above everything else, because it is
     * *attached* to the cursor - a drag label the pointer disappeared
     * behind would be worse than none. */
    if (client_drag_active) {
        int32_t len = 0;
        for (const char *p = client_drag_payload; *p; p++) {
            len += FONT_WIDTH;
        }
        int32_t lw = len + 2 * DRAG_LABEL_PAD;
        int32_t lx = min_i32(cursor_x + CURSOR_SIZE, (int32_t)fb_info.width - lw);
        int32_t ly = min_i32(cursor_y + CURSOR_SIZE, (int32_t)fb_info.height - DRAG_LABEL_H);
        fill_rect_rounded(lx, ly, lw, DRAG_LABEL_H, DRAG_LABEL_BG);
        stroke_rect_rounded(lx, ly, lw, DRAG_LABEL_H, DRAG_LABEL_BORDER);
        draw_text_clipped(lx + DRAG_LABEL_PAD, ly + (DRAG_LABEL_H - FONT_HEIGHT) / 2,
                           client_drag_payload, DRAG_LABEL_FG, 0);
    }
    const uint8_t *cursor_shape_now = cursor_shape;
    int rmask = hovered_resize_mask();
    if ((rmask & (RESIZE_TOP | RESIZE_LEFT)) == (RESIZE_TOP | RESIZE_LEFT) ||
        (rmask & (RESIZE_BOTTOM | RESIZE_RIGHT)) == (RESIZE_BOTTOM | RESIZE_RIGHT)) {
        cursor_shape_now = cursor_shape_diag_nw_se;
    } else if ((rmask & (RESIZE_TOP | RESIZE_RIGHT)) == (RESIZE_TOP | RESIZE_RIGHT) ||
               (rmask & (RESIZE_BOTTOM | RESIZE_LEFT)) == (RESIZE_BOTTOM | RESIZE_LEFT)) {
        cursor_shape_now = cursor_shape_diag_ne_sw;
    } else if (rmask & (RESIZE_LEFT | RESIZE_RIGHT)) {
        cursor_shape_now = cursor_shape_horizontal;
    } else if (rmask & (RESIZE_TOP | RESIZE_BOTTOM)) {
        cursor_shape_now = cursor_shape_vertical;
    } else if (cursor_over_titlebar()) {
        cursor_shape_now = cursor_shape_move;
    }
    draw_cursor(cursor_shape_now);
    present();
}

static void redraw(void) {
    redraw_rect(0, 0, (int32_t)fb_info.width, (int32_t)fb_info.height);
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

/* M43: how close to a screen edge the *cursor* has to get during a
 * move-drag before releasing there snaps the window to that half. The
 * cursor rather than the window's own edge, deliberately: the window is
 * clamped so MOVE_MIN_VISIBLE px of it always stay on screen, so its edge
 * can never actually reach x=0 - but the pointer can, and "shove the
 * pointer into the edge" is the gesture every desktop that has this uses. */
/* M46: double-clicking a titlebar toggles maximize/restore, through
 * apply_window_action like everything else. M40 already made double-click
 * detection latency-independent by timestamping events in the PS/2
 * handler (input.h's mouse_event_t.time_ms); this is that machinery's
 * second user, and it uses the same 500ms window desktop_icons.c and
 * file_manager.c already do. */
#define TITLEBAR_DOUBLE_CLICK_MS 500

#define SNAP_EDGE_MARGIN 8
#define SNAP_NONE  0
#define SNAP_LEFT  1
#define SNAP_RIGHT 2
static int drag_snap_hint = SNAP_NONE; /* which half a release right now would snap to; only meaningful while drag_mode == DRAG_MOVE */

/* M46: the previous titlebar press, for double-click detection - the
 * window it landed on and the *event's own* timestamp, never
 * SYS_uptime_ms here. See TITLEBAR_DOUBLE_CLICK_MS. */
static int titlebar_last_click_window = -1;
static uint32_t titlebar_last_click_ms;

/* M38: the mask draw_cursor's shape selection (redraw_rect) uses - a
 * resize *in progress* keeps showing the shape for whichever edge/corner
 * started it (drag_resize_mask), even if the cursor drifts outside that
 * edge's own RESIZE_MARGIN mid-drag; otherwise, literally the same
 * z_hit_test handle_mouse's own resize hit-test uses (M51), so the shape
 * the cursor shows and the edge a press would actually grab cannot
 * disagree. */
static int hovered_resize_mask(void) {
    if (drag_mode == DRAG_RESIZE) {
        return drag_resize_mask;
    }
    int mask = 0;
    return z_hit_test(cursor_x, cursor_y, WCLASS_ORDINARY, HIT_RESIZE, &mask) >= 0 ? mask : 0;
}

/* M46: a move-drag in progress keeps showing the move cursor even if the
 * pointer drifts off the titlebar it grabbed - same rule
 * hovered_resize_mask already applies to a resize in progress, and for
 * the same reason: the gesture, not the pixel under the pointer, is what
 * the cursor is reporting. */
static int cursor_over_titlebar(void) {
    if (drag_mode == DRAG_MOVE) {
        return 1;
    }
    if (drag_mode != DRAG_NONE || hover_btn_window >= 0) {
        return 0; /* a titlebar button is its own target, not the band around it */
    }
    return z_hit_test(cursor_x, cursor_y, WCLASS_ORDINARY, HIT_TITLEBAR, 0) >= 0;
}

static void send_event(const window_t *win, const wm_event_t *ev) {
    sys_write(win->evt_write_fd, ev, sizeof(*ev));
}

static void set_focus(int idx) {
    /* M51: focusing a window brings it forward, and this is where that
     * happens because this - not apply_window_action's WM_ACTION_FOCUS
     * branch - is the actual funnel every focus path in this file goes
     * through. WM_ACTION_FOCUS covers a taskbar button, Alt+Tab and an
     * external WM_ACTION_PIPE request, but a plain click on a window's
     * body (focus_window_under_cursor), a titlebar move-drag and a
     * resize-edge grab all call set_focus directly; putting the raise in
     * the action branch would have left exactly the three gestures a
     * person uses most not raising anything.
     *
     * Above the early-return below, deliberately: a window can be focused
     * and still not be topmost (something else was raised while it kept
     * focus), and clicking it must then still bring it forward. */
    if (idx >= 0) {
        z_raise(idx);
    }
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

/* M48: raise a toast. Bounded copies from the caller's strings, and a
 * full stack drops its *oldest* entry rather than refusing the new one -
 * a burst of failures should show you the most recent ones, and the
 * dropped one was already on its way out. */
static void toast_post(uint32_t level, const char *title, const char *body) {
    if (toast_count == TOAST_MAX) {
        for (int i = 1; i < TOAST_MAX; i++) {
            toasts[i - 1] = toasts[i];
        }
        toast_count--;
    }
    toast_t *t = &toasts[toast_count++];
    t->level = level;
    int i = 0;
    for (; title && title[i] && i < WM_NOTIFY_TITLE_MAX - 1; i++) {
        t->title[i] = title[i];
    }
    t->title[i] = '\0';
    i = 0;
    for (; body && body[i] && i < WM_NOTIFY_BODY_MAX - 1; i++) {
        t->body[i] = body[i];
    }
    t->body[i] = '\0';
    t->expires_ms = sys_uptime_ms() + TOAST_TTL_MS;
    dirty = 1;
}

/* Drops every toast whose deadline has passed, keeping the rest packed
 * from index 0 so toast_rect's stacking stays a property of the array.
 * Called once per main-loop pass - the deadline is the only thing that
 * removes a toast on its own, and nothing else will notice it. */
static void toasts_expire(long now_ms) {
    int out = 0;
    for (int i = 0; i < toast_count; i++) {
        if (toasts[i].expires_ms > now_ms) {
            if (out != i) {
                toasts[out] = toasts[i];
            }
            out++;
        }
    }
    if (out != toast_count) {
        toast_count = out;
        dirty = 1;
    }
}

/* A click on a toast dismisses it early. Returns 1 if one was hit, in
 * which case the click is consumed and must not also reach whatever is
 * underneath - a toast that appears over a window's close button and
 * passes the click through would be worse than one you cannot dismiss. */
static int toast_click(int32_t px, int32_t py) {
    for (int i = 0; i < toast_count; i++) {
        int32_t x, y;
        toast_rect(i, &x, &y);
        if (gfx_point_in_rect(px, py, x, y, TOAST_W, TOAST_H)) {
            for (int j = i + 1; j < toast_count; j++) {
                toasts[j - 1] = toasts[j];
            }
            toast_count--;
            dirty = 1;
            return 1;
        }
    }
    return 0;
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
    if (win->shm_id >= 0) {
        sys_shm_free(win->shm_id, win->pixels);
        win->shm_id = -1;
        win->pixels = (uint32_t *)0;
    }
    win->alive = 0;
    z_remove(idx); /* M51: only live windows are in the z-order - see zorder's own comment */
    win->minimized = 0;
    win->overhang = 0;
    win->close_requested = 0;
    win->client_pid = -1;
    /* M45: a context menu raised on this window has nothing left to act
     * on - and Force Quit is one of its rows, so this is the common case,
     * not a corner one. */
    if (wmenu_window == idx) {
        wmenu_window = -1;
        wmenu_hover = -1;
    }
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
        /* M54: `<= 0`, not `== 0`. SYS_task_alive gained a third way to
         * say "not running": -1, meaning the kernel has no such task -
         * which since M54 includes a task that terminated and had its
         * slot reaped by whoever was waiting on it. A window whose client
         * the kernel has never heard of is a dead window either way, and
         * a compositor that kept it because it could not tell *how* the
         * client went would leave a permanently inert window on screen.
         * 2 (terminated cleanly) still keeps its window - that is M20's
         * wm_demo, which draws one frame and exits on purpose. */
        if (windows[i].alive && sys_task_alive(windows[i].client_pid) <= 0) {
            /* M48: SYS_task_alive's 0-vs-2 split has been able to tell a
             * crash from an orderly exit since M29 and had never
             * mentioned it to anyone. But "nonzero exit code" is not the
             * same question as "did this surprise us": a SIGTERM death is
             * 143, so an ordinary titlebar close arrives here looking
             * exactly like a crash. close_requested is the difference -
             * only a death the compositor did not ask for is news. */
            if (!windows[i].close_requested) {
                toast_post(WM_NOTIFY_ERROR,
                            windows[i].title[0] ? windows[i].title : "A program",
                            "stopped unexpectedly.");
            }
            reclaim_window(i);
        }
    }
}

/* M30: how much of the bottom edge the docked taskbar is reserving, or 0
 * if nothing is docked there - the amount window placement, maximize and
 * drag bounds all have to leave clear. There is at most one panel in
 * practice (desktop_shell.c is the only client that ever asks for one),
 * but nothing enforces that, so this uses whichever is found first.
 *
 * M41 made this edge-aware for a second, top-docked bar; M42 removed that
 * bar and with it the edge parameter, since "which edge" only ever had
 * one answer again. */
static int32_t connected_panel_height(void) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i].alive && windows[i].is_panel) {
            return windows[i].h;
        }
    }
    return 0;
}

/* The top edge every ordinary window's *titlebar* has to stay below, and
 * the bottom edge its content has to stay above - one place, so the
 * placement, maximize and drag-clamp callers can't drift apart on what
 * they leave clear. */
static int32_t content_top_limit(void) {
    return TITLEBAR_H + BORDER;
}

static int32_t content_bottom_limit(void) {
    return (int32_t)fb_info.height - connected_panel_height();
}

/* M43: exactly where WM_ACTION_SNAP_LEFT/RIGHT will put `win` - the one
 * definition of that geometry, used both by the action itself and by the
 * drag preview, so what you see before releasing is what you get after.
 *
 * Same clamp discipline as WM_ACTION_MAXIMIZE, and for the same reason:
 * there is still no protocol for a client to grow its own shm-backed
 * buffer (M31's drag only ever shrinks a window within the one it
 * allocated), so a window whose buffer is narrower than half the screen
 * is placed at that half's edge rather than stretched past what it can
 * actually paint. The MIN_WIN_* floors are what keep this sane on a
 * display too small to have two usable halves - a half-width that came
 * out negative would otherwise clamp every window to nothing. */
static void snap_rect(const window_t *win, uint32_t action,
                       int32_t *out_x, int32_t *out_y, int32_t *out_w, int32_t *out_h) {
    int32_t half_w = max_i32((int32_t)fb_info.width / 2 - 2 * BORDER, MIN_WIN_W);
    int32_t avail_h = max_i32(content_bottom_limit() - content_top_limit() - BORDER, MIN_WIN_H);
    *out_w = min_i32(win->buf_w, half_w);
    *out_h = min_i32(win->buf_h, avail_h);
    *out_x = (action == WM_ACTION_SNAP_LEFT) ? BORDER : (int32_t)fb_info.width / 2 + BORDER;
    *out_y = content_top_limit();
}

/* M40: a refused connection used to be entirely silent - the client got
 * window_id = -1, exited, and the only evidence anywhere was an app that
 * "did nothing" when you launched it. That is precisely how the fd-table
 * exhaustion M40 root-caused stayed invisible for several milestones
 * (see milestones.md's M40 section). Every refusal now names its own
 * reason on the compositor's stdout, which SYS_write routes to klog and
 * so into tools/qemu-serial-test.sh's own capture - so the next time
 * this happens it is one grep away instead of a bisect. */
static void refuse_window(int resp_write_fd, int32_t client_pid, const char *reason) {
    wm_create_response_t resp;
    resp.window_id = -1;
    resp.shm_id = -1;
    resp.width = 0;
    resp.height = 0;
    resp.client_pid = client_pid; /* M56: a refusal has to be addressed too, or the client it was meant for waits out its whole timeout */
    resp.compositor_pid = self_pid; /* M55: even a refusal says who refused - a client that retries needs to know whether the answer came from the compositor it is waiting on */
    sys_write(resp_write_fd, &resp, sizeof(resp));

    /* M48: M40 gave this a klog line so it would be one grep away. That
     * is still true and still useful, but nobody reads stdout on a
     * desktop - so it says it on screen too. */
    toast_post(WM_NOTIFY_WARN, "Window refused", reason);

    const char prefix[] = "[wm] window request refused: ";
    sys_write(1, prefix, sizeof(prefix) - 1);
    int len = 0;
    while (reason[len]) {
        len++;
    }
    sys_write(1, reason, (size_t)len);
    sys_write(1, "\n", 1);
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
        refuse_window(resp_write_fd, -1, "short/torn create request");
        return;
    }

    /* M56: a client that already has a live window is re-asking, not
     * asking again. M55's reconnect retry can put two requests in flight
     * - that is the whole point of the retry, since the first may have
     * been discarded by a replacement compositor clearing these pipes -
     * and serving both would leave this client with two windows, one of
     * which it will never draw into and nothing will ever reclaim (its
     * owner is very much alive). Answering the second request with the
     * first request's window makes the retry idempotent, which is what a
     * retry has to be. Every client in this project has exactly one
     * window, so "which one" is never ambiguous. */
    if (req.client_pid > 0) {
        for (int i = 0; i < window_count; i++) {
            if (windows[i].alive && windows[i].client_pid == req.client_pid) {
                resp.window_id = i;
                resp.shm_id = windows[i].shm_id;
                resp.width = (uint32_t)windows[i].buf_w;
                resp.height = (uint32_t)windows[i].buf_h;
                resp.compositor_pid = self_pid;
                resp.client_pid = req.client_pid;
                sys_write(resp_write_fd, &resp, sizeof(resp));
                return;
            }
        }
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
            refuse_window(resp_write_fd, req.client_pid, "no free window slot (MAX_WINDOWS)");
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
        refuse_window(resp_write_fd, req.client_pid, "SYS_shm_create/SYS_shm_map failed (MAX_SHM_SEGMENTS, or out of memory)");
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
        char evt_name[WM_EVENT_PIPE_NAME_LEN];
        wm_event_pipe_name(idx, evt_name);
        int evt_fds[2];
        if (sys_pipe_open(evt_name, evt_fds) != 0) {
            refuse_window(resp_write_fd, req.client_pid, "SYS_pipe_open for this window's event pipe failed (this process's MAX_FDS, or MAX_NAMED_PIPES)");
            return;
        }
        evt_write_fd = evt_fds[1];
    }

    window_t *win = &windows[idx];
    win->is_panel = req.panel;
    /* M45: a panel's docked strip may be shorter than the buffer it
     * allocated - the rest is the overhang it can raise a menu into (see
     * wm_create_request_t.panel_dock_h). Clamped rather than trusted:
     * 0 (every panel before M45) and anything past the buffer both mean
     * "dock the whole thing". */
    int32_t dock_h = (int32_t)height;
    if (req.panel && req.panel_dock_h > 0 && req.panel_dock_h < height) {
        dock_h = (int32_t)req.panel_dock_h;
    }
    if (req.panel) {
        win->x = 0;
        win->y = (int32_t)fb_info.height - dock_h;
    } else if (req.desktop) {
        win->x = 0;
        win->y = 0;
    } else {
        win->x = 100 + idx * 40;
        /* Cascade, clamped so a titlebar never starts off the top of the
         * screen - the same limit every other placement path uses.
         *
         * M45: and clamped at the *bottom* too, so a window whose height
         * would carry it under the docked taskbar is moved up instead.
         * This was always wrong (the bottom of such a window is behind an
         * always-on-top panel, so it is genuinely unreachable, not just
         * crowded) but nothing had ever hit it: it takes a tall window
         * far enough down the cascade, which is exactly what adding a
         * seventh desktop icon produced. The top limit wins if a window
         * is too tall to fit at all - a titlebar off the top of the
         * screen cannot be grabbed either, and that is the worse of the
         * two failures. */
        int32_t cascade_y = 100 + idx * 40;
        int32_t bottom_fit = content_bottom_limit() - (int32_t)height;
        win->y = max_i32(min_i32(cascade_y, bottom_fit), content_top_limit());
    }
    win->w = (int32_t)width;
    win->h = req.panel ? dock_h : (int32_t)height;
    win->buf_w = (int32_t)width;  /* M30: fixed for this connection's whole lifetime - see window_t's own comment */
    win->buf_h = (int32_t)height;
    /* The buffer row that lands on win->y: the docked strip is the
     * *bottom* dock_h rows, so everything above it is the overhang. */
    win->buf_y0 = (int32_t)height - win->h;
    win->overhang = 0;
    win->pixels = (uint32_t *)vaddr;
    win->shm_id = (int32_t)shm_id;
    win->evt_write_fd = evt_write_fd;
    win->translucent = req.translucent;
    win->is_desktop = req.desktop;
    win->minimized = 0;
    win->maximized = 0;
    win->alive = 1;
    win->close_requested = 0;
    win->confirm_close = req.confirm_close;
    win->client_pid = req.client_pid;
    int ti = 0;
    for (; req.title[ti] && ti < WM_TITLE_MAX - 1; ti++) {
        win->title[ti] = req.title[ti];
    }
    win->title[ti] = '\0';

    /* M51: a new window enters at the top of its own band - the top of
     * the ordinary band for an app, above every other panel for a panel,
     * above nothing at all for the desktop background. Below set_focus,
     * an ordinary window is then raised again by the focus it is given;
     * z_insert_top_of_band is idempotent, so that costs nothing and this
     * still leaves a *panel* (which never takes focus) correctly placed. */
    z_insert_top_of_band(idx);

    resp.window_id = idx;
    resp.shm_id = (int32_t)shm_id;
    resp.width = width;
    resp.height = height;
    resp.compositor_pid = self_pid; /* M55: so this client can tell "quiet" from "gone" - see wm_create_response_t */
    resp.client_pid = req.client_pid; /* M56: and who this answer is for - see wm_create_response_t.client_pid */
    if (!reused_slot) {
        window_count++;
    }
    sys_write(resp_write_fd, &resp, sizeof(resp));
    /* M42: a brand-new window takes focus immediately, same as most real
     * window managers - except a panel, which never holds focus at all
     * now (see focus_window_under_cursor). A taskbar that grabbed focus
     * the moment it connected left the desktop deactivated from boot,
     * which is what made M35's right-click desktop menu unreachable
     * until M40 worked around it from the other end. */
    if (!win->is_panel) {
        set_focus(idx);
    }
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
        /* M51: depth, so a shell can show which window is frontmost
         * without having to reorder the buttons it draws - see
         * wm_window_info_t.z_index. Already dense: only alive windows are
         * in the z-order and only alive windows are reported here, so
         * z_count and resp.count are the same number and the ranks run
         * 0..count-1 with no gaps. */
        resp.windows[out].z_index = z_position_of(i);
        memcpy(resp.windows[out].title, win->title, WM_TITLE_MAX);
        resp.count++;
    }
    sys_write(query_resp_write_fd, &resp, sizeof(resp));
}

/* M30: the single place every window-state-changing action funnels
 * through - a titlebar button click (handle_mouse, below) and an
 * external WM_ACTION_PIPE request (accept_pending_action) both call this
 * directly, so "click the panel's minimize toggle" and "click the
 * titlebar's minimize button" (M22 and M30's own bullet asking for
 * exactly this) drive the literal same code, not two copies that could
 * drift apart. */
static void apply_window_action(int idx, uint32_t action, int32_t value) {
    window_t *win = &windows[idx];
    if (action == WM_ACTION_FOCUS) {
        win->minimized = 0;
        set_focus(idx); /* M51: which raises it - see set_focus */
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
            win->close_requested = 1; /* M48: so its death isn't announced as a crash */
            sys_kill(win->client_pid, SIGTERM);
        }
    } else if (action == WM_ACTION_KILL) {
        /* M45: the verb that always works. Unlike WM_ACTION_CLOSE just
         * above, confirm_close is deliberately not consulted - M36's
         * contract lets a client never answer WM_EVENT_CLOSE_REQUEST, and
         * an app that cannot be forced is an app that is on your screen
         * permanently. The slot itself is untouched here for exactly the
         * same reason the SIGTERM path leaves it alone: M29's
         * reap_dead_clients notices the death and reclaims it, so a force
         * quit, an ordinary close and a real crash all converge on one
         * teardown path rather than three. */
        win->close_requested = 1; /* M48: the user asked for this one too */
        sys_kill(win->client_pid, SIGKILL);
    } else if (action == WM_ACTION_SET_PANEL_OVERHANG) {
        /* M45: only a panel has anywhere to put one, and never more than
         * the buffer it actually allocated above its dock line. */
        int32_t want = value;
        if (!win->is_panel) {
            want = 0;
        }
        if (want < 0) {
            want = 0;
        }
        if (want > win->buf_y0) {
            want = win->buf_y0;
        }
        if (want != win->overhang) {
            win->overhang = want;
            dirty = 1;
        }
    } else if (action == WM_ACTION_MAXIMIZE) {
        if (!win->maximized) {
            win->saved_x = win->x;
            win->saved_y = win->y;
            win->saved_w = win->w;
            win->saved_h = win->h;
            int32_t avail_w = (int32_t)fb_info.width - 2 * BORDER;
            /* content_top_limit already folds TITLEBAR_H + BORDER in. */
            int32_t avail_h = content_bottom_limit() - content_top_limit() - BORDER;
            win->x = BORDER;
            win->y = content_top_limit();
            /* Clamped to buf_w/buf_h - see window_t's own comment on why
             * this can only ever shrink a window that's bigger than the
             * available area, never grow one past what its buffer holds. */
            win->w = min_i32(win->buf_w, avail_w);
            win->h = min_i32(win->buf_h, avail_h);
            win->maximized = 1;
            dirty = 1;
        }
    } else if (action == WM_ACTION_SNAP_LEFT || action == WM_ACTION_SNAP_RIGHT) {
        /* M43: a snapped window is not a maximized one - clearing the
         * flag keeps WM_ACTION_RESTORE (and the titlebar's maximize
         * button, which reads it) from claiming it can put back geometry
         * that this just replaced. */
        snap_rect(win, action, &win->x, &win->y, &win->w, &win->h);
        win->maximized = 0;
        dirty = 1;
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

/* M45: raise the window context menu for `idx` at the click point,
 * clamped so it is drawn wholly on screen rather than half off an edge -
 * the same clamp desktop_icons.c's own right-click menu has done since
 * M35. */
static void wmenu_open_at(int idx, int32_t px, int32_t py) {
    wmenu_window = idx;
    wmenu_hover = -1;
    int32_t h = WMENU_ITEM_H * WMENU_COUNT;
    wmenu_x = min_i32(px, (int32_t)fb_info.width - WMENU_W);
    wmenu_y = min_i32(py, (int32_t)fb_info.height - h);
    wmenu_x = max_i32(wmenu_x, 0);
    wmenu_y = max_i32(wmenu_y, 0);
    dirty = 1;
}

static void wmenu_close(void) {
    if (wmenu_window >= 0) {
        wmenu_window = -1;
        wmenu_hover = -1;
        dirty = 1;
    }
}

/* Which row (0..WMENU_COUNT-1) is at (px, py), or -1 if the point is
 * outside the menu entirely. */
static int wmenu_row_at(int32_t px, int32_t py) {
    if (!gfx_point_in_rect(px, py, wmenu_x, wmenu_y, WMENU_W, WMENU_ITEM_H * WMENU_COUNT)) {
        return -1;
    }
    return (py - wmenu_y) / WMENU_ITEM_H;
}

/* An open menu owns the next click outright, the same rule every other
 * menu in this project follows - it either picks a row or dismisses, and
 * never also reaches whatever is underneath it. */
static void wmenu_click(int32_t px, int32_t py) {
    int idx = wmenu_window;
    int row = wmenu_row_at(px, py);
    wmenu_close();
    if (row < 0 || idx < 0 || !windows[idx].alive) {
        return;
    }
    if (row == 0) {
        apply_window_action(idx, WM_ACTION_TOGGLE_MINIMIZE, 0);
    } else if (row == 1) {
        apply_window_action(idx, WM_ACTION_CLOSE, 0);
    } else {
        apply_window_action(idx, WM_ACTION_KILL, 0);
    }
}

/* M43: the launcher's logic. Everything it needs is already here -
 * /bin's contents via SYS_listdir (M53), SYS_spawn to launch, and the
 * keyboard, which this process already owns (handle_keyboard routes every
 * keystroke). No new syscall and no new protocol channel: the only thing
 * that crosses a process boundary is the one WM_ACTION_TOGGLE_LAUNCHER
 * the Start button sends. */

static char lower_char(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive substring match, and substring rather than prefix on
 * purpose: this filesystem's names are things like "gui_terminal" and
 * "text_editor", where the word you actually think of ("terminal",
 * "editor") is in the middle. An empty query matches everything. */
static int launcher_name_matches(const char *name, const char *query) {
    if (!query[0]) {
        return 1;
    }
    for (int i = 0; name[i]; i++) {
        int j = 0;
        while (query[j] && lower_char(name[i + j]) == lower_char(query[j])) {
            j++;
        }
        if (!query[j]) {
            return 1;
        }
    }
    return 0;
}

/* Keeps the selected row on screen, and the list scrolled no further than
 * it has content for. */
static void launcher_clamp_scroll(void) {
    if (launcher_selected < 0) {
        launcher_selected = 0;
    }
    if (launcher_selected >= launcher_match_count) {
        launcher_selected = launcher_match_count - 1;
    }
    if (launcher_selected < launcher_scroll) {
        launcher_scroll = launcher_selected;
    }
    if (launcher_selected >= launcher_scroll + LAUNCHER_ROWS) {
        launcher_scroll = launcher_selected - LAUNCHER_ROWS + 1;
    }
    if (launcher_scroll < 0) {
        launcher_scroll = 0;
    }
}

static void launcher_apply_filter(void) {
    launcher_match_count = 0;
    for (int i = 0; i < launcher_entry_count; i++) {
        if (launcher_name_matches(launcher_entries[i], launcher_query)) {
            launcher_matches[launcher_match_count++] = i;
        }
    }
    /* Typing always re-aims at the top match: the whole point of the box
     * is that narrowing the query converges on what you meant, and
     * keeping a stale selection index would fight that. */
    launcher_selected = 0;
    launcher_scroll = 0;
}

/* Re-reads /bin. Only on open - see launcher_entries' own comment on why
 * this isn't kept live.
 *
 * M53: a name ending in '/' is a subdirectory (SYS_listdir marks them)
 * and is skipped rather than listed. /bin has none today; skipping them
 * is what keeps that from becoming a launchable entry the moment one
 * appears. */
static void launcher_reload(void) {
    static char buf[LAUNCHER_LIST_BUF];
    launcher_entry_count = 0;
    long n = sys_listdir(PATH_BIN, buf, sizeof(buf));
    if (n <= 0) {
        return;
    }
    if (n > (long)sizeof(buf)) {
        n = (long)sizeof(buf);
    }
    int col = 0;
    for (long i = 0; i < n && launcher_entry_count < LAUNCHER_MAX_ENTRIES; i++) {
        if (buf[i] == '\n') {
            if (col > 0 && launcher_entries[launcher_entry_count][col - 1] == '/') {
                col = 0;
                continue; /* a directory, not something to launch */
            }
            launcher_entries[launcher_entry_count][col] = '\0';
            launcher_entry_count++;
            col = 0;
        } else if (col < LAUNCHER_NAME_MAX - 1) {
            launcher_entries[launcher_entry_count][col++] = buf[i];
        }
    }
}

static void launcher_set_open(int open) {
    launcher_open = open;
    power_confirm = POWER_CONFIRM_NONE; /* M47: never leave a confirm box armed across an open/close */
    power_hover = POWER_CONFIRM_NONE;
    if (open) {
        launcher_query[0] = '\0';
        launcher_query_len = 0;
        launcher_reload();
        launcher_apply_filter();
    }
    dirty = 1;
}

static void launcher_launch_selected(void) {
    if (launcher_selected >= 0 && launcher_selected < launcher_match_count) {
        /* M53: the list is /bin, so the name has to be turned back into
         * a path before it can be spawned. M48's error message stays -
         * a /bin entry can still fail to load (a truncated image, a full
         * task table), and that is now the only reason it ever will. */
        const char *name = launcher_entries[launcher_matches[launcher_selected]];
        char path[PATH_MAX_LEN];
        if (path_join(path, PATH_BIN_DIR, name) != 0) {
            toast_post(WM_NOTIFY_ERROR, name, "Name too long to launch.");
        } else {
            long rc = sys_spawn(path, "");
            if (rc < 0) {
                toast_post(WM_NOTIFY_ERROR, name, spawn_error_message(rc));
            }
            child_track(rc); /* M54: so its task slot comes back when it closes - see children.h */
        }
    }
    launcher_set_open(0);
}

/* Every keystroke while the launcher is up belongs to it - the one place
 * in this project where the compositor takes the keyboard away from the
 * focused window, and the reason a compositor-owned surface was justified
 * here at all (see LAUNCHER_W's comment). Escape and Enter both close it,
 * so it can never be left holding input with no way out. */
static void launcher_key(char ch) {
    /* M47: an armed confirm box takes the keyboard from the search field
     * outright. Anything that isn't an explicit yes cancels - including a
     * stray letter, which is the right default for the one action in this
     * system that cannot be undone. */
    if (power_confirm != POWER_CONFIRM_NONE) {
        if (ch == 'y' || ch == 'Y' || ch == '\n' || ch == '\r') {
            sys_shutdown(power_confirm);
            /* Only reached if the kernel refused the mode, which it
             * cannot for these two - fall through to cancelling rather
             * than leaving a box up that did nothing. */
        }
        power_confirm = POWER_CONFIRM_NONE;
        dirty = 1;
        return;
    }
    if (ch == 27) { /* Escape */
        launcher_set_open(0);
        return;
    }
    if (ch == '\n' || ch == '\r') {
        launcher_launch_selected();
        return;
    }
    if (ch == (char)KBD_KEY_UP) {
        launcher_selected--;
    } else if (ch == (char)KBD_KEY_DOWN) {
        launcher_selected++;
    } else if (ch == '\b' || ch == 0x7F) {
        if (launcher_query_len > 0) {
            launcher_query[--launcher_query_len] = '\0';
            launcher_apply_filter();
        }
    } else if (ch >= 0x20 && ch < 0x7F && launcher_query_len < LAUNCHER_QUERY_MAX - 1) {
        launcher_query[launcher_query_len++] = ch;
        launcher_query[launcher_query_len] = '\0';
        launcher_apply_filter();
    }
    launcher_clamp_scroll();
    dirty = 1;
}

/* A left-click while the launcher is up: on a result row it launches it,
 * anywhere else it dismisses - the same "an open menu owns the next
 * click outright" rule every menu in this project already follows
 * (text_editor.c's File menu, desktop_icons.c's context menu). Returns 1
 * either way, since the click is consumed and must not also reach a
 * window underneath. */
/* Which Power button (POWER_OFF/POWER_REBOOT) is at (px, py), or
 * POWER_CONFIRM_NONE. Shared by the click handler and the hover
 * highlight, so the lit button and the acting button can't disagree -
 * the same reason titlebar_button_at exists. */
static int power_button_at(int32_t px, int32_t py) {
    int32_t lx, ly;
    launcher_rect(&lx, &ly);
    if (gfx_point_in_rect(px, py, lx + POWER_OFF_X, ly + POWER_BTN_Y, POWER_BTN_W, POWER_BTN_H)) {
        return POWER_OFF;
    }
    if (gfx_point_in_rect(px, py, lx + POWER_REBOOT_X, ly + POWER_BTN_Y, POWER_BTN_W, POWER_BTN_H)) {
        return POWER_REBOOT;
    }
    return POWER_CONFIRM_NONE;
}

static int launcher_click(int32_t px, int32_t py) {
    int32_t lx, ly;
    launcher_rect(&lx, &ly);
    /* M47: while a confirm box is up, any click cancels it - confirming
     * is deliberately keyboard-only (see draw_power_confirm). */
    if (power_confirm != POWER_CONFIRM_NONE) {
        power_confirm = POWER_CONFIRM_NONE;
        dirty = 1;
        return 1;
    }
    if (!gfx_point_in_rect(px, py, lx, ly, LAUNCHER_W, LAUNCHER_H)) {
        launcher_set_open(0);
        return 1;
    }
    int power = power_button_at(px, py);
    if (power != POWER_CONFIRM_NONE) {
        power_confirm = power;
        dirty = 1;
        return 1;
    }
    for (int i = 0; i < LAUNCHER_ROWS; i++) {
        if (launcher_scroll + i >= launcher_match_count) {
            break;
        }
        if (gfx_point_in_rect(px, py, lx + LAUNCHER_PAD / 2, launcher_row_y(ly, i),
                               LAUNCHER_W - LAUNCHER_PAD, LAUNCHER_ROW_H)) {
            launcher_selected = launcher_scroll + i;
            launcher_launch_selected();
            return 1;
        }
    }
    return 1; /* inside the overlay but not on a row - swallowed, nothing else */
}

/* M49: the wheel over the launcher's result list, one row per detent -
 * the same unit Up/Down move, so the two agree about what "one step"
 * means. Moves the *selection* rather than the scroll offset alone,
 * because launcher_clamp_scroll already keeps the selection on screen and
 * a selection scrolled out of view would make Enter act on something the
 * user can't see. */
static void launcher_wheel(int32_t detents) {
    if (launcher_match_count == 0) {
        return;
    }
    launcher_selected += detents;
    launcher_clamp_scroll();
    dirty = 1;
}

/* Hovering a row selects it, so a click and the keyboard's Enter always
 * act on the same thing. Only repaints when the answer changes: this runs
 * on every mouse-move event. */
static void launcher_hover(int32_t px, int32_t py) {
    int32_t lx, ly;
    launcher_rect(&lx, &ly);
    int power = power_button_at(px, py);
    if (power != power_hover) {
        power_hover = power;
        dirty = 1;
    }
    for (int i = 0; i < LAUNCHER_ROWS; i++) {
        if (launcher_scroll + i >= launcher_match_count) {
            break;
        }
        if (gfx_point_in_rect(px, py, lx + LAUNCHER_PAD / 2, launcher_row_y(ly, i),
                               LAUNCHER_W - LAUNCHER_PAD, LAUNCHER_ROW_H)) {
            if (launcher_selected != launcher_scroll + i) {
                launcher_selected = launcher_scroll + i;
                dirty = 1;
            }
            return;
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
    /* M42: the one action that isn't about a window, so it's handled
     * before (and instead of) the window_id validation every other one
     * goes through - see WM_ACTION_TOGGLE_LAUNCHER. */
    if (req.action == WM_ACTION_TOGGLE_LAUNCHER) {
        launcher_set_open(!launcher_open);
        return;
    }
    if (req.window_id < 0 || req.window_id >= window_count || !windows[req.window_id].alive) {
        return;
    }
    apply_window_action(req.window_id, req.action, req.value);
}

/* M33/M38: the compositor's two global (non-per-window) settings - see
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
    accent_color = req.accent_color;
    wallpaper_id = req.wallpaper;
    dirty = 1;
}

/* M49: a client announcing that it has started a drag. One-way, same
 * poll-then-read shape as every other request channel here. The drag ends
 * when the left button comes up, which this process sees anyway - so
 * there is no "drag end" message for a client to forget to send, and a
 * source that dies mid-drag simply releases nothing and the next button-up
 * clears it. */
static void accept_pending_drag(int drag_read_fd) {
    if (sys_pipe_poll(drag_read_fd) < (long)sizeof(wm_drag_request_t)) {
        return;
    }
    wm_drag_request_t req;
    if (read_exact(drag_read_fd, &req, sizeof(req)) != (long)sizeof(req)) {
        return;
    }
    req.payload[WM_DRAG_PAYLOAD_MAX - 1] = '\0';
    memcpy(client_drag_payload, req.payload, WM_DRAG_PAYLOAD_MAX);
    client_drag_active = 1;
    client_drag_last_target = -1;
    dirty = 1;
}

/* M48: any client's one-way notification request. Same non-blocking
 * poll-then-read shape as accept_pending_action, and the same
 * fire-and-forget contract - there is nothing to reply to. */
static void accept_pending_notify(int notify_read_fd) {
    if (sys_pipe_poll(notify_read_fd) < (long)sizeof(wm_notify_request_t)) {
        return;
    }
    wm_notify_request_t req;
    if (read_exact(notify_read_fd, &req, sizeof(req)) != (long)sizeof(req)) {
        return;
    }
    /* The strings arrive from another process, so they are not trusted to
     * be terminated - toast_post copies them bounded, but it stops at a
     * NUL, so one has to exist. */
    req.title[WM_NOTIFY_TITLE_MAX - 1] = '\0';
    req.body[WM_NOTIFY_BODY_MAX - 1] = '\0';
    toast_post(req.level, req.title, req.body);
}

/* M44: the read side of the same three settings - see wm.h's own note on
 * why a one-way channel stopped being enough once the desktop, not the
 * compositor, became the thing that paints the background. Same
 * poll-then-read-then-reply shape as accept_pending_query. */
static void accept_pending_settings_query(int query_read_fd, int query_resp_write_fd) {
    if (sys_pipe_poll(query_read_fd) < 1) {
        return;
    }
    uint8_t ping;
    if (read_exact(query_read_fd, &ping, sizeof(ping)) != (long)sizeof(ping)) {
        return;
    }
    wm_settings_request_t resp;
    resp.bg_color = bg_color;
    resp.accent_color = accent_color;
    resp.wallpaper = wallpaper_id;
    sys_write(query_resp_write_fd, &resp, sizeof(resp));
}

/* M40: the plain "which window is under the cursor" hit-test, lifted out
 * of handle_mouse's left-button branch so a right-button press could use
 * the exact same one rather than a near-copy. M42 splits the answer from
 * what's done with it, because the two callers now want different things:
 * a click focuses (focus_window_under_cursor, below) while event routing
 * only wants to know what's being hovered.
 *
 * M51: three backwards passes over windows[] - panels, then ordinary
 * windows, then the desktop background - collapsed into one z_hit_test.
 * That precedence used to be hand-written here; it is now simply where
 * each class of window sits in the z-order, so it cannot disagree with
 * what the screen shows. HIT_FRAME rather than the content rect, so a
 * click on a window's 2px border focuses that window instead of falling
 * through to whatever is behind it. A minimized window still can't be
 * hit: there's nothing there to click. */
static int window_under_cursor(void) {
    return z_hit_test(cursor_x, cursor_y, WCLASS_ALL, HIT_FRAME, 0);
}

/* M42: a click on the taskbar no longer takes focus away from the app you
 * were using - the Windows behavior, and the one this project actually
 * wants: clicking a taskbar button to minimize a window shouldn't first
 * deactivate a different one, and the Start button shouldn't deactivate
 * anything at all. The panel still gets the click (see the routing at the
 * bottom of handle_mouse), it just isn't focused to receive it. Nothing
 * else on screen changes: an ordinary window and the desktop background
 * are focused by a click exactly as before. */
static void focus_window_under_cursor(void) {
    int hit = window_under_cursor();
    if (hit >= 0 && !windows[hit].is_panel) {
        set_focus(hit);
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
        int left_up_edge = !(mev.buttons & 1) && (prev_buttons & 1);
        /* M40: a *right*-button press has to pick a window too, not just
         * a left one. Events are routed to the focused window only (see
         * the send_event block at the bottom of this loop), so before
         * this, right-clicking anything that wasn't already focused sent
         * the event to whatever was - which meant M35's
         * right-click-on-the-desktop context menu simply never opened
         * from a fresh desktop, because the panel holds focus after boot.
         * Found by M40's input harness (tools/qemu-input-test.sh); no
         * protocol-level self-test could have, since they all send events
         * to a window they already named. Only the plain focus hit-test
         * is shared - titlebar buttons, resize handles and move-drags
         * stay deliberately left-button-only, the way they are
         * everywhere else. */
        int right_down_edge = (mev.buttons & 2) && !(prev_buttons & 2);

        /* M46: which titlebar button (if any) the cursor is over, updated
         * on every event including the ones that go on to be consumed by
         * a drag or a menu - a button left lit because the pointer
         * happened to leave during a drag is exactly the kind of stale
         * highlight this is supposed to be the fix for. */
        {
            titlebar_button_t over_btn = BTN_CLOSE;
            int over_idx = (drag_mode == DRAG_NONE) ? titlebar_button_at(cursor_x, cursor_y, &over_btn) : -1;
            if (over_idx != hover_btn_window || (over_idx >= 0 && over_btn != hover_btn)) {
                hover_btn_window = over_idx;
                hover_btn = over_btn;
                dirty = 1;
            }
        }

        /* M43: an open launcher owns the pointer the same way it owns the
         * keyboard - it is drawn over everything, so a click that fell
         * through to a window underneath it would land somewhere the user
         * cannot even see. Checked before the drag state machine, which
         * cannot be running anyway while the launcher is up (opening it
         * takes a click on the taskbar or a keychord, neither of which
         * can happen mid-drag). */
        /* M49: the wheel acts on whatever is under the pointer, not on
         * whatever holds focus - which is what every desktop with a wheel
         * does, and the only behavior that makes scrolling a background
         * window's list possible at all. Handled before the drag state
         * machine and the launcher, since both of those consume events
         * they have no scroll meaning for. */
        if (mev.wheel != 0) {
            if (launcher_open) {
                launcher_wheel(mev.wheel);
            } else {
                int target = window_under_cursor();
                if (target >= 0 && !windows[target].is_panel) {
                    const window_t *win = &windows[target];
                    wm_event_t ev = {0};
                    ev.type = WM_EVENT_MOUSE_WHEEL;
                    ev.x = cursor_x - win->x;
                    ev.y = cursor_y - win->y;
                    ev.buttons = mev.buttons;
                    ev.time_ms = mev.time_ms;
                    ev.wheel = mev.wheel;
                    send_event(win, &ev);
                }
            }
            prev_buttons = mev.buttons;
            continue;
        }

        /* M49: a client drag owns the pointer until the button comes up.
         * The source window keeps receiving its own motion events (it is
         * still focused, and this process routes those by focus), so it
         * can keep tracking the gesture; what happens here is only the
         * part no client can do - telling a *different* window that
         * something is over it, and handing the payload across on
         * release. */
        if (client_drag_active) {
            int target = window_under_cursor();
            if (target >= 0 && windows[target].is_panel) {
                target = -1; /* a taskbar is not a drop target */
            }
            if (left_up_edge) {
                if (target >= 0) {
                    /* Payload first, then the event: a client that reads
                     * WM_DRAG_DATA_PIPE the instant it sees WM_EVENT_DROP
                     * must find it already there. */
                    wm_drag_request_t data;
                    memcpy(data.payload, client_drag_payload, WM_DRAG_PAYLOAD_MAX);
                    sys_write(drag_data_write_fd, &data, sizeof(data));
                    wm_event_t ev = {0};
                    ev.type = WM_EVENT_DROP;
                    ev.x = cursor_x - windows[target].x;
                    ev.y = cursor_y - windows[target].y;
                    ev.time_ms = mev.time_ms;
                    send_event(&windows[target], &ev);
                }
                client_drag_active = 0;
                client_drag_last_target = -1;
                dirty = 1;
            } else {
                /* One motion event per target change, not per pixel: this
                 * exists so a target can highlight itself, and a client
                 * being told forty times a second that nothing changed is
                 * how the event pipe fills. */
                if (target != client_drag_last_target) {
                    client_drag_last_target = target;
                    if (target >= 0) {
                        wm_event_t ev = {0};
                        ev.type = WM_EVENT_DRAG_MOTION;
                        ev.x = cursor_x - windows[target].x;
                        ev.y = cursor_y - windows[target].y;
                        ev.time_ms = mev.time_ms;
                        send_event(&windows[target], &ev);
                    }
                }
                dirty = 1; /* the label follows the cursor */
            }
            prev_buttons = mev.buttons;
            continue;
        }

        /* M48: a toast is drawn over everything, so it takes its own
         * click before any other hit-test - including the launcher's,
         * which it is drawn on top of. */
        if (left_down_edge && toast_click(cursor_x, cursor_y)) {
            prev_buttons = mev.buttons;
            continue;
        }

        if (launcher_open) {
            if (left_down_edge) {
                launcher_click(cursor_x, cursor_y);
            } else if (!(mev.buttons & 1)) {
                launcher_hover(cursor_x, cursor_y);
            }
            prev_buttons = mev.buttons;
            continue;
        }

        /* M45: an open window context menu owns the pointer the same way
         * the launcher does - it is drawn over the window it acts on, so
         * a click falling through would land on something the user can't
         * see. A right-click while it is up re-raises it wherever the
         * cursor now is, rather than leaving a stale one behind. */
        if (wmenu_window >= 0) {
            if (left_down_edge) {
                wmenu_click(cursor_x, cursor_y);
            } else if (right_down_edge) {
                wmenu_close();
            } else if (!(mev.buttons & 1)) {
                int row = wmenu_row_at(cursor_x, cursor_y);
                if (row != wmenu_hover) {
                    wmenu_hover = row;
                    dirty = 1;
                }
            }
            if (!right_down_edge) {
                prev_buttons = mev.buttons;
                continue;
            }
        }

        /* M31: a drag in progress owns every event until release - no
         * hit-testing, no focus changes, no forwarding to the window's
         * own content, just updating its geometry. drag_window's own
         * `alive` is re-checked every event (not just at drag-start)
         * since M29's reap_dead_clients can reclaim it mid-drag if its
         * owning client crashes while being dragged. */
        if (drag_mode != DRAG_NONE) {
            if (left_up_edge || !windows[drag_window].alive) {
                /* M43: releasing inside an edge zone is what commits a
                 * snap - through the very same apply_window_action an
                 * external WM_ACTION_SNAP_LEFT/RIGHT goes through, so the
                 * gesture and the protocol can't drift apart (the same
                 * arrangement M30's titlebar buttons already have). */
                if (left_up_edge && drag_mode == DRAG_MOVE && drag_snap_hint != SNAP_NONE &&
                    windows[drag_window].alive) {
                    apply_window_action(drag_window, drag_snap_hint == SNAP_LEFT
                                                          ? WM_ACTION_SNAP_LEFT
                                                          : WM_ACTION_SNAP_RIGHT, 0);
                }
                drag_mode = DRAG_NONE;
                drag_window = -1;
                drag_snap_hint = SNAP_NONE;
                if (snap_preview_active) {
                    snap_preview_active = 0;
                    dirty = 1;
                }
            } else if (mev.buttons & 1) {
                window_t *win = &windows[drag_window];
                int32_t dx = cursor_x - drag_start_cursor_x;
                int32_t dy = cursor_y - drag_start_cursor_y;
                if (drag_mode == DRAG_MOVE) {
                    int32_t min_x = -(win->w - MOVE_MIN_VISIBLE);
                    int32_t max_x = (int32_t)fb_info.width - MOVE_MIN_VISIBLE;
                    int32_t min_y = content_top_limit(); /* titlebar top can't go above the screen's own top edge */
                    int32_t max_y = content_bottom_limit(); /* titlebar bottom can't dip below the dock's top edge */
                    win->x = clamp_i32(drag_start_x + dx, min_x, max_x);
                    win->y = clamp_i32(drag_start_y + dy, min_y, max_y);
                    /* M43: which half releasing here would snap to, and
                     * the preview rect for it - recomputed only when the
                     * answer changes, since this runs per mouse event. */
                    int hint = SNAP_NONE;
                    if (cursor_x <= SNAP_EDGE_MARGIN) {
                        hint = SNAP_LEFT;
                    } else if (cursor_x >= (int32_t)fb_info.width - 1 - SNAP_EDGE_MARGIN) {
                        hint = SNAP_RIGHT;
                    }
                    if (hint != drag_snap_hint) {
                        drag_snap_hint = hint;
                        snap_preview_active = (hint != SNAP_NONE);
                        if (snap_preview_active) {
                            int32_t sx, sy, sw, sh;
                            snap_rect(win, hint == SNAP_LEFT ? WM_ACTION_SNAP_LEFT : WM_ACTION_SNAP_RIGHT,
                                       &sx, &sy, &sw, &sh);
                            snap_preview_x = sx - BORDER;
                            snap_preview_y = sy - TITLEBAR_H - BORDER;
                            snap_preview_w = sw + 2 * BORDER;
                            snap_preview_h = sh + TITLEBAR_H + 2 * BORDER;
                        }
                    }
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
             * test below. M51: through the one occlusion-correct
             * z_hit_test, so a button belonging to a window that is
             * covered at that point can no longer take the click - which
             * is exactly what the comment that used to sit here admitted
             * it did. Panels/desktop have no titlebar, so they're never
             * candidates. */
            /* M46: the same titlebar_button_at the hover highlight uses -
             * this used to be a second, identical loop, and a lit button
             * that wasn't the button that acted would be a particularly
             * annoying way to find that out. */
            titlebar_button_t btn_hit = BTN_CLOSE;
            int btn_hit_idx = titlebar_button_at(cursor_x, cursor_y, &btn_hit);
            if (btn_hit_idx >= 0) {
                if (btn_hit == BTN_CLOSE) {
                    apply_window_action(btn_hit_idx, WM_ACTION_CLOSE, 0);
                } else if (btn_hit == BTN_MINIMIZE) {
                    apply_window_action(btn_hit_idx, WM_ACTION_TOGGLE_MINIMIZE, 0);
                } else {
                    apply_window_action(btn_hit_idx, windows[btn_hit_idx].maximized ? WM_ACTION_RESTORE : WM_ACTION_MAXIMIZE, 0);
                }
                prev_buttons = mev.buttons;
                continue; /* consumed by chrome - not also a focus-changing click on whatever's under it */
            }

            /* M31: resize handles (window border edges/corners) come next -
             * a small, precise target that has to win over both the
             * titlebar-move check right after it and the generic content
             * hit-test further down. M51: the same z_hit_test as every
             * other hit-test in this function, with the one documented
             * exception a resize handle needs - see HIT_RESIZE. */
            int rz_mask = 0;
            int rz_idx = z_hit_test(cursor_x, cursor_y, WCLASS_ORDINARY, HIT_RESIZE, &rz_mask);
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
            int mv_idx = z_hit_test(cursor_x, cursor_y, WCLASS_ORDINARY, HIT_TITLEBAR, 0);
            if (mv_idx >= 0) {
                /* M46: a second press on the same titlebar inside the
                 * double-click window maximizes (or restores) instead of
                 * starting another move-drag. Checked before the drag is
                 * armed, so the gesture can't do both. */
                if (titlebar_last_click_window == mv_idx &&
                    mev.time_ms - titlebar_last_click_ms <= TITLEBAR_DOUBLE_CLICK_MS) {
                    titlebar_last_click_window = -1; /* a third quick press starts a fresh pair, not a third toggle */
                    set_focus(mv_idx);
                    apply_window_action(mv_idx,
                                         windows[mv_idx].maximized ? WM_ACTION_RESTORE : WM_ACTION_MAXIMIZE, 0);
                    prev_buttons = mev.buttons;
                    continue;
                }
                titlebar_last_click_window = mv_idx;
                titlebar_last_click_ms = mev.time_ms;
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

            focus_window_under_cursor();
        } else if (right_down_edge) {
            /* M45: a right-click on an ordinary window's titlebar raises
             * the window context menu instead of only focusing it. Same
             * occlusion-correct z_hit_test order as every other
             * hit-test in this function (M51); anywhere else, right-click keeps
             * doing exactly what M40 made it do (pick a window so the
             * event routes to the one actually under the cursor). */
            int tb_idx = z_hit_test(cursor_x, cursor_y, WCLASS_ORDINARY, HIT_TITLEBAR, 0);
            if (tb_idx >= 0) {
                set_focus(tb_idx);
                wmenu_open_at(tb_idx, cursor_x, cursor_y);
                prev_buttons = mev.buttons;
                continue;
            }
            focus_window_under_cursor();
        }

        /* M42: mouse events go to the panel the cursor is over, if any,
         * and to the focused window otherwise. A panel never holds focus
         * (focus_window_under_cursor), so routing purely by focus would
         * mean a taskbar that can't be clicked at all - and a taskbar
         * that only hears about clicks can't do hover highlighting on its
         * Start and running-app buttons, which is the other half of what
         * makes it feel like a real one. Everything else still routes by
         * focus exactly as before. */
        int hovered_panel = window_under_cursor();
        if (hovered_panel >= 0 && !windows[hovered_panel].is_panel) {
            hovered_panel = -1;
        }
        /* One last event to the panel the cursor just left, so it can
         * clear its own hover highlight - without it the cursor leaving
         * the bar would freeze whichever button it was over as lit. */
        if (last_hovered_panel >= 0 && last_hovered_panel != hovered_panel &&
            windows[last_hovered_panel].alive && windows[last_hovered_panel].is_panel) {
            const window_t *left = &windows[last_hovered_panel];
            wm_event_t ev = {0};
            ev.type = WM_EVENT_MOUSE_MOVE;
            ev.x = cursor_x - left->x;
            ev.y = cursor_y - left->y;
            ev.buttons = mev.buttons;
            ev.time_ms = mev.time_ms;
            send_event(left, &ev);
        }
        last_hovered_panel = hovered_panel;

        int target = hovered_panel >= 0 ? hovered_panel : focused_window;
        if (target >= 0) {
            const window_t *win = &windows[target];
            wm_event_t ev = {0};
            ev.type = WM_EVENT_MOUSE_MOVE;
            ev.x = cursor_x - win->x;
            ev.y = cursor_y - win->y;
            ev.buttons = mev.buttons;
            ev.time_ms = mev.time_ms; /* M40: the driver's timestamp, forwarded untouched - see wm_event_t.time_ms */
            send_event(win, &ev);
            if (mev.buttons != prev_buttons) {
                ev.type = WM_EVENT_MOUSE_BUTTON;
                send_event(win, &ev);
            }
        }
        prev_buttons = mev.buttons;
    }
}

/* M32: cycles focus through every alive ordinary (non-panel, non-desktop)
 * window, un-minimizing the target the same way a titlebar/panel
 * WM_ACTION_FOCUS click already does - reuses apply_window_action rather
 * than duplicating its focus/un-minimize logic.
 *
 * M49: `direction` is +1 for Alt+Tab and -1 for Shift+Alt+Tab.
 *
 * M51: in z-order rather than in slot order. This used to walk windows[]
 * by index, so on a desktop with four windows open it visited them in the
 * order they were *launched* regardless of what you had been using - and
 * with M51's raise-on-focus that would have been actively confusing,
 * since the screen now shows a use order the keyboard didn't follow.
 *
 * Now that focus raises, the z-order *is* the most-recently-used order,
 * so "the next window back" is simply the next one down. Alt+Tab goes
 * backwards through it (the window you used before this one), Shift+Alt+
 * Tab forwards, both wrapping. Tapping Alt+Tab repeatedly therefore
 * swaps the front two rather than touring every window - which is what a
 * tap-without-holding does on Windows too, and is the honest consequence
 * of raising on focus rather than a shortcut taken here. */
static void alt_tab_cycle(int direction) {
    if (z_count == 0) {
        return;
    }
    /* Where the focused window sits in the z-order. If nothing is focused,
     * start above the top so a backwards step lands on the topmost. */
    int start = focused_window >= 0 ? z_position_of(focused_window) : z_count;
    if (start < 0) {
        start = z_count;
    }
    /* direction +1 (Alt+Tab) means "one further back", which is one step
     * *down* the z-order - hence the negation. */
    int step = -direction;
    for (int n = 1; n <= z_count; n++) {
        int z = (start + n * step) % z_count;
        if (z < 0) {
            z += z_count;
        }
        int idx = zorder[z];
        if (windows[idx].alive && !windows[idx].is_panel && !windows[idx].is_desktop) {
            apply_window_action(idx, WM_ACTION_FOCUS, 0);
            return;
        }
    }
}

/* M49: everything a window-manager chord can do to the focused window,
 * in one place. Every arm is an apply_window_action call that some other
 * entry point (a titlebar button, a context menu, a drag) already makes -
 * what was missing was only the binding, which is exactly why this is a
 * dispatch table rather than nine new behaviors. */
static void run_shortcut(int id) {
    switch (id) {
    case SHORTCUT_CYCLE_FORWARD:
        alt_tab_cycle(1);
        return;
    case SHORTCUT_CYCLE_BACKWARD:
        alt_tab_cycle(-1);
        return;
    case SHORTCUT_LAUNCHER:
        launcher_set_open(!launcher_open);
        return;
    case SHORTCUT_TASK_MANAGER: {
        /* An ordinary program on disk, so this is one spawn and nothing
         * else - and it says so if the spawn fails, like every other
         * launch path since M48. */
        long rc = sys_spawn(PATH_BIN_DIR "task_manager", "");
        child_track(rc); /* M54 - see children.h */
        if (rc < 0) {
            toast_post(WM_NOTIFY_ERROR, "Task manager", spawn_error_message(rc));
        }
        return;
    }
    default:
        break;
    }

    /* Everything below acts on the focused window, and there may not be
     * one - a chord with no subject is a no-op, not an error. */
    if (focused_window < 0 || !windows[focused_window].alive) {
        return;
    }
    int idx = focused_window;
    switch (id) {
    case SHORTCUT_CLOSE_WINDOW:
        /* Honors confirm_close, because it goes through the same
         * WM_ACTION_CLOSE the titlebar button does - Alt+F4 is the
         * polite verb, not the forceful one. */
        apply_window_action(idx, WM_ACTION_CLOSE, 0);
        break;
    case SHORTCUT_SNAP_LEFT:
        apply_window_action(idx, WM_ACTION_SNAP_LEFT, 0);
        break;
    case SHORTCUT_SNAP_RIGHT:
        apply_window_action(idx, WM_ACTION_SNAP_RIGHT, 0);
        break;
    case SHORTCUT_MAXIMIZE:
        apply_window_action(idx, WM_ACTION_MAXIMIZE, 0);
        break;
    case SHORTCUT_MINIMIZE:
        /* Restore-then-minimize: from maximized this puts the window
         * back to its own size, and from there it minimizes - so holding
         * the chord walks a window down rather than doing nothing to a
         * maximized one. */
        if (windows[idx].maximized) {
            apply_window_action(idx, WM_ACTION_RESTORE, 0);
        } else {
            apply_window_action(idx, WM_ACTION_TOGGLE_MINIMIZE, 0);
        }
        break;
    default:
        break;
    }
}

static void handle_keyboard(void) {
    char ch;
    while (sys_kbd_read(&ch)) {
        /* M32: window-manager chords are intercepted here, before ever
         * reaching a client - they are not something any app's own input
         * handling should see, or could even tell apart from the plain
         * keypress underneath (see SYS_kbd_modifiers' doc comment).
         *
         * M49: what used to be two hand-written `if`s is a lookup in
         * system_api/include/shortcuts.h's table, which settings.c's
         * Shortcuts pane lists from - so a chord cannot exist without
         * being discoverable, and the pane cannot describe one that
         * isn't wired up. */
        long mods = sys_kbd_modifiers();
        int shortcut = shortcut_lookup(ch, (int)mods);
        if (shortcut != SHORTCUT_NONE) {
            run_shortcut(shortcut);
            continue;
        }
        /* M43: while it is up, the launcher has the keyboard outright -
         * see launcher_key. Checked after the chords so Ctrl+Space
         * toggles it shut as well as open. */
        if (launcher_open) {
            launcher_key(ch);
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

    self_pid = (int32_t)sys_getpid(); /* M55 - echoed in every create response, see wm_create_response_t.compositor_pid */

    cursor_x = (int32_t)(fb_info.width / 2);
    cursor_y = (int32_t)(fb_info.height / 2);

    /* M47: whatever settings.c last wrote, applied before the first
     * client connects - so the desktop comes up the way it was left
     * rather than snapping to it a moment later. A missing or malformed
     * file leaves the compiled-in defaults these three already hold,
     * which is exactly what settings_file_load's all-or-nothing contract
     * is for (see its own doc comment). */
    {
        wm_settings_request_t saved;
        saved.bg_color = bg_color;
        saved.accent_color = accent_color;
        saved.wallpaper = wallpaper_id;
        if (settings_file_load(&saved)) {
            bg_color = saved.bg_color;
            accent_color = saved.accent_color;
            wallpaper_id = saved.wallpaper;
        }
    }

    int req_fds[2];
    int resp_fds[2];
    if (sys_pipe_open(WM_REQUEST_PIPE, req_fds) != 0 || sys_pipe_open(WM_RESPONSE_PIPE, resp_fds) != 0) {
        sys_exit(1);
    }
    int query_fds[2];
    int query_resp_fds[2];
    int action_fds[2];
    int settings_fds[2];
    int settings_query_fds[2];
    int settings_query_resp_fds[2];
    int notify_fds[2];
    int drag_fds[2];
    int drag_data_fds[2];
    if (sys_pipe_open(WM_QUERY_PIPE, query_fds) != 0 || sys_pipe_open(WM_QUERY_RESP_PIPE, query_resp_fds) != 0 ||
        sys_pipe_open(WM_ACTION_PIPE, action_fds) != 0 || sys_pipe_open(WM_SETTINGS_PIPE, settings_fds) != 0 ||
        sys_pipe_open(WM_SETTINGS_QUERY_PIPE, settings_query_fds) != 0 ||
        sys_pipe_open(WM_SETTINGS_QUERY_RESP_PIPE, settings_query_resp_fds) != 0 ||
        sys_pipe_open(WM_NOTIFY_PIPE, notify_fds) != 0 ||
        sys_pipe_open(WM_DRAG_PIPE, drag_fds) != 0 ||
        sys_pipe_open(WM_DRAG_DATA_PIPE, drag_data_fds) != 0) {
        sys_exit(1);
    }
    drag_data_write_fd = drag_data_fds[1];

    /* M55: whatever a previous compositor left buffered in these
     * rendezvous points is not this one's business, and is actively
     * dangerous. A named pipe deliberately outlives every fd that ever
     * pointed at it (kernel/ipc/pipe.h) - that is the whole mechanism -
     * so a compositor that died mid-`sys_write`, or a client that queued
     * a request nobody ever read, leaves a partial struct at the head of
     * the stream. The next compositor's very first read would then be
     * misaligned against every message after it, which is the difference
     * between "the desktop came back" and "the desktop came back and
     * nothing works". Resetting is one syscall per channel, at the one
     * moment when there is definitionally nothing worth keeping. */
    sys_pipe_reset(req_fds[0]);
    sys_pipe_reset(resp_fds[0]);
    sys_pipe_reset(query_fds[0]);
    sys_pipe_reset(query_resp_fds[0]);
    sys_pipe_reset(action_fds[0]);
    sys_pipe_reset(settings_fds[0]);
    sys_pipe_reset(settings_query_fds[0]);
    sys_pipe_reset(settings_query_resp_fds[0]);
    sys_pipe_reset(notify_fds[0]);
    sys_pipe_reset(drag_fds[0]);
    sys_pipe_reset(drag_data_fds[0]);
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
        accept_pending_settings_query(settings_query_fds[0], settings_query_resp_fds[1]);
        accept_pending_notify(notify_fds[0]);
        accept_pending_drag(drag_fds[0]);
        reap_dead_clients();
        child_reap(); /* M54: and the task slots of whatever this process launched */
        handle_mouse();
        handle_keyboard();

        long now = sys_uptime_ms();
        toasts_expire(now); /* M48: a deadline is the only thing that retires a toast on its own */
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
