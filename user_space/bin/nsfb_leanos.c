/* user_space/bin/nsfb_leanos.c - M100: NetSurf's display, on this OS.
 *
 * ---- why this is the only file the browser port needed --------------
 *
 * The obvious way to put a framebuffer browser on a new operating system
 * is to add a front end to it. This is not that, and the difference is
 * worth stating because it is the reason NetSurf and libnsfb are in this
 * tree with ZERO source edits between them:
 *
 *   - libnsfb's surfaces register themselves AT RUNTIME, through
 *     `_nsfb_register_surface`, which is a non-static function in
 *     libnsfb.a. A surface is not a compile-time table entry.
 *   - NetSurf's framebuffer front end chooses its surface BY NAME at
 *     runtime (`nsfb_type_from_name(fename)`, frontends/framebuffer/
 *     gui.c), from whatever registered itself.
 *   - The front end links libnsfb with `-Wl,--whole-archive`
 *     (frontends/framebuffer/Makefile), so an object added to that
 *     archive keeps its constructor.
 *
 * So this file is compiled here, put into libnsfb.a, and registers a
 * surface called "leanos" when the program starts. NetSurf finds it the
 * same way it would find SDL. Nothing upstream had to be told about
 * lean_os at all.
 *
 * That is a deliberate choice over the alternative and not a lucky
 * accident: a front end is a permanent fork of somebody else's program,
 * and this project's rule is that third-party source is ported
 * *against* rather than merged into. A surface is the seam upstream
 * already built.
 *
 * ---- the two things that made it small ------------------------------
 *
 * The pixels are ZERO COPY. lean_os's compositor gives a client a shared
 * memory segment of tightly packed 0x00RRGGBB words (user_space/lib/
 * gfx.h), and libnsfb's NSFB_FMT_XRGB8888 plotters produce exactly that
 * - `colour_to_pixel` in 32bpp-xrgb8888.c swaps R and B out of NetSurf's
 * own ABGR colour and writes 0x00RRGGBB. So `nsfb->ptr` is pointed
 * straight at the compositor's segment and the layout engine renders
 * into the window. There is no blit in this file and there does not
 * need to be one.
 *
 * There is also no "present" call, because this compositor does not have
 * one: it composites from the segment on its own schedule (M21), so
 * `update` has nothing to do. That is recorded here rather than left as
 * a suspicious empty function.
 */
/* Before libnsfb's own headers: they declare `bool` returns and, like
 * most of libnsfb, expect the including file to have pulled in
 * <stdbool.h> already. Its own sources do. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"

/* libnsfb's private headers. A surface is an internal thing - it touches
 * nsfb_t's fields and calls select_plotters - so these come from the
 * source tree rather than from the installed include directory, which
 * exports only the public API. tools/build-netsurf.sh passes the -I. */
#include "nsfb.h"
#include "plot.h"
#include "surface.h"

#include "input.h"   /* system_api: KBD_KEY_*, KBD_MOD_* */
#include "wm.h"      /* system_api: wm_event_t */
#include "wmclient.h"
#include "syscall_wrappers.h" /* sys_uptime_ms - the deadline in leanos_input */

struct leanos_priv {
    wm_window_t win;
    /* M117: libnsfb's update callback has been called since the last
     * present - NetSurf has plotted something. Presented once per turn
     * of its event loop (leanos_input) rather than per update, because
     * a page repaint is hundreds of updates and the compositor's unit is
     * the window. */
    int damaged;
    /* The button state from the last mouse event the compositor sent.
     * lean_os reports a BUTTONS BITMASK on every mouse event; libnsfb
     * wants a key-down/key-up per button, so the change has to be
     * derived by comparison. Without this field a held button would
     * generate a fresh NSFB_KEY_MOUSE_1 down on every pointer move,
     * which NetSurf reads as a new click and which makes a text
     * selection impossible. */
    uint8_t buttons;
};

/* The compositor's default window. Chosen rather than asked for: this
 * machine's default display is 1024x768 and a browser wants most of it,
 * with room for the taskbar (M43) at the bottom. NetSurf overrides both
 * with -w and -h, which framebuffer_initialise passes through to
 * nsfb_set_geometry before initialise runs. */
#define LEANOS_DEFAULT_W 900
#define LEANOS_DEFAULT_H 640

static int leanos_defaults(nsfb_t *nsfb) {
    nsfb->width = LEANOS_DEFAULT_W;
    nsfb->height = LEANOS_DEFAULT_H;
    /* Not a guess - see the file header. This is the compositor's own
     * word layout, so the plotters write finished pixels. */
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int leanos_initialise(nsfb_t *nsfb) {
    struct leanos_priv *p = calloc(1, sizeof(*p));
    if (!p) {
        return -1;
    }
    if (wm_connect((uint32_t)nsfb->width, (uint32_t)nsfb->height,
                   "NetSurf", &p->win) != 0) {
        /* No compositor listening. Reported rather than worked around:
         * a browser with nowhere to draw should say so and stop, not
         * render into a buffer nobody will ever see. */
        free(p);
        return -1;
    }
    /* The compositor is free to give a different size from the one
     * asked for (it clamps to the display). Believe what came back. */
    nsfb->width = p->win.gfx.width;
    nsfb->height = p->win.gfx.height;
    nsfb->ptr = (uint8_t *)p->win.gfx.pixels;
    nsfb->linelen = nsfb->width * 4;
    nsfb->surface_priv = p;
    select_plotters(nsfb);
    return 0;
}

static int leanos_finalise(nsfb_t *nsfb) {
    struct leanos_priv *p = nsfb->surface_priv;
    if (p) {
        free(p);
        nsfb->surface_priv = 0;
    }
    /* nsfb->ptr belonged to the compositor's shared segment and is not
     * this surface's to free - which is why this function looks lighter
     * than ram.c's, where the buffer really was malloc'ed. */
    nsfb->ptr = 0;
    return 0;
}

static int leanos_set_geometry(nsfb_t *nsfb, int width, int height,
                               enum nsfb_format_e format) {
    if (nsfb->surface_priv != NULL) {
        /* After connecting, the window's size is the compositor's to
         * change (WM_EVENT_DISPLAY_CHANGED), not this caller's. Refused
         * rather than silently ignored: a caller that thinks it resized
         * the window and did not would draw to the wrong geometry. */
        return -1;
    }
    if (width > 0) {
        nsfb->width = width;
    }
    if (height > 0) {
        nsfb->height = height;
    }
    if (format != NSFB_FMT_ANY && format != NSFB_FMT_XRGB8888) {
        /* One format, because there is one compositor and it has one
         * word layout. Accepting a request for another and then not
         * honouring it is how a colour-swapped browser happens. */
        return -1;
    }
    select_plotters(nsfb);
    return 0;
}

/* ---- input ----------------------------------------------------------
 *
 * lean_os delivers a wm_event_t per keystroke and per pointer change;
 * libnsfb wants one nsfb_event_t at a time and asks with a timeout in
 * milliseconds (negative meaning "block"). The awkward part is that one
 * lean_os mouse event can mean two libnsfb events - a move AND a button
 * transition - and there is nowhere in libnsfb's interface to return
 * two. So a pending button event is stashed and returned on the next
 * call, which is what the `pending` state below is for.
 */
/* File scope rather than in struct leanos_priv, which is a real
 * limitation and is stated rather than left to be found: two nsfb
 * contexts on this surface at once would share one pending event.
 * NetSurf's framebuffer front end creates exactly one, and a second
 * would also mean a second window on one compositor connection, which
 * leanos_initialise does not do either. If that ever changes, this
 * moves into the private struct and the change is two lines. */
static int pending_valid;
static nsfb_event_t pending_event;

static enum nsfb_key_code_e translate_key(char ch) {
    switch ((unsigned char)ch) {
    /* M33's arrow sentinels: values 1-4 in the ordinary character
     * stream, which is why they cannot simply be passed through as
     * characters (see system_api/include/input.h). */
    case KBD_KEY_UP:    return NSFB_KEY_UP;
    case KBD_KEY_DOWN:  return NSFB_KEY_DOWN;
    case KBD_KEY_LEFT:  return NSFB_KEY_LEFT;
    case KBD_KEY_RIGHT: return NSFB_KEY_RIGHT;
    case '\b':          return NSFB_KEY_BACKSPACE;
    case '\t':          return NSFB_KEY_TAB;
    case '\n':
    case '\r':          return NSFB_KEY_RETURN;
    case 27:            return NSFB_KEY_ESCAPE;
    default:
        break;
    }
    if ((unsigned char)ch >= KBD_KEY_F1 && (unsigned char)ch <= KBD_KEY_F12) {
        return (enum nsfb_key_code_e)(NSFB_KEY_F1 +
                                      ((unsigned char)ch - KBD_KEY_F1));
    }
    if ((unsigned char)ch >= 32 && (unsigned char)ch < 127) {
        /* NSFB_KEY_* is ASCII through this range by construction - see
         * the enum in libnsfb_event.h, where NSFB_KEY_a is 97. NetSurf
         * uppercases for itself from the shift state it tracks, so the
         * character is passed as it was decoded. */
        return (enum nsfb_key_code_e)(unsigned char)ch;
    }
    return NSFB_KEY_UNKNOWN;
}

/* Points libnsfb back at the compositor's segment if it has moved. */
static void resync(nsfb_t *nsfb, struct leanos_priv *p) {
    if (nsfb->ptr == (uint8_t *)p->win.gfx.pixels &&
        nsfb->width == p->win.gfx.width &&
        nsfb->height == p->win.gfx.height) {
        return;
    }
    nsfb->ptr = (uint8_t *)p->win.gfx.pixels;
    nsfb->width = p->win.gfx.width;
    nsfb->height = p->win.gfx.height;
    nsfb->linelen = nsfb->width * 4;
    select_plotters(nsfb);
}

/* Turns one lean_os event into an nsfb one. Returns 1 if `event` was
 * filled, 0 if this event means nothing to libnsfb. */
static int convert(struct leanos_priv *p, const wm_event_t *in,
                   nsfb_event_t *event) {
    switch (in->type) {
    case WM_EVENT_KEY: {
        enum nsfb_key_code_e code = translate_key(in->ch);
        if (code == NSFB_KEY_UNKNOWN) {
            return 0;
        }
        event->type = NSFB_EVENT_KEY_DOWN;
        event->value.keycode = code;
        /* And the matching key-up, immediately. This compositor reports
         * decoded CHARACTERS, not press and release (M21) - there is no
         * release to forward. NetSurf's fbtk needs the up or it treats
         * the key as held, so it is synthesised here and the fact that
         * it is synthesised is written down rather than hidden: a
         * program on this machine cannot detect a held key through this
         * path, and nothing in the browser needs to. */
        pending_event.type = NSFB_EVENT_KEY_UP;
        pending_event.value.keycode = code;
        pending_valid = 1;
        return 1;
    }
    case WM_EVENT_MOUSE_MOVE:
    case WM_EVENT_MOUSE_BUTTON: {
        uint8_t was = p->buttons, now = in->buttons;
        p->buttons = now;
        /* The move first, because a click's position has to be known
         * before the click is. libnsfb has no position on a button
         * event at all - it tracks the pointer from the moves - so a
         * button event delivered before its move lands at the previous
         * position, which for a browser means clicking the last link
         * rather than this one. */
        event->type = NSFB_EVENT_MOVE_ABSOLUTE;
        event->value.vector.x = in->x;
        event->value.vector.y = in->y;
        event->value.vector.z = 0;
        uint8_t changed = (uint8_t)(was ^ now);
        if (changed) {
            /* ---- the button numbering, which is NOT the same on both
             * sides and would have been an invisible bug ---------------
             *
             * lean_os reports bit0 left, bit1 right, bit2 middle
             * (system_api/include/input.h). libnsfb inherited X11's
             * numbering, where button 2 is MIDDLE and button 3 is
             * RIGHT - and NetSurf acts on MOUSE_1 and MOUSE_3 and
             * ignores MOUSE_2 entirely (frontends/framebuffer/gui.c).
             *
             * So `NSFB_KEY_MOUSE_1 + bit` is wrong in the worst way: a
             * right-click would arrive as MOUSE_2 and be silently
             * dropped, and a middle-click would arrive as MOUSE_3 and
             * open the context menu. Both look like "the browser
             * ignores right-click", which is a plausible thing for a
             * new port to do and not a thing anyone would go looking
             * for a numbering bug about. */
            static const enum nsfb_key_code_e BUTTON[3] = {
                NSFB_KEY_MOUSE_1, /* bit0, left   */
                NSFB_KEY_MOUSE_3, /* bit1, right  */
                NSFB_KEY_MOUSE_2, /* bit2, middle */
            };
            /* One transition per event. Two buttons changing in a single
             * packet is possible and vanishingly rare; the lowest bit
             * wins and the other is lost, because `p->buttons` has
             * already been updated so no later event will report it as
             * changed. Nothing in this browser needs a chord. */
            int bit = 0;
            while (bit < 3 && !(changed & (1u << bit))) {
                bit++;
            }
            if (bit < 3) {
                pending_event.type = (now & (1u << bit)) ? NSFB_EVENT_KEY_DOWN
                                                        : NSFB_EVENT_KEY_UP;
                pending_event.value.keycode = BUTTON[bit];
                pending_valid = 1;
            }
        }
        return 1;
    }
    case WM_EVENT_MOUSE_WHEEL:
        /* NetSurf's fbtk reads the wheel as mouse buttons 4 and 5, the
         * X11 convention it was written against. */
        event->type = NSFB_EVENT_KEY_DOWN;
        event->value.keycode = in->wheel < 0 ? NSFB_KEY_MOUSE_4
                                             : NSFB_KEY_MOUSE_5;
        pending_event.type = NSFB_EVENT_KEY_UP;
        pending_event.value.keycode = event->value.keycode;
        pending_valid = 1;
        return 1;
    case WM_EVENT_EXPOSE:
    case WM_EVENT_DISPLAY_CHANGED:
        /* M55/M58: the pixel buffer has been replaced - by a new
         * compositor, or by a resolution change. wmclient has already
         * re-run the handshake; what is left is to point libnsfb at the
         * new segment and tell NetSurf its window changed size, which
         * makes it re-lay-out and repaint. */
        event->type = NSFB_EVENT_RESIZE;
        event->value.resize.w = p->win.gfx.width;
        event->value.resize.h = p->win.gfx.height;
        return 1;
    case WM_EVENT_CLOSE_REQUEST:
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_QUIT;
        return 1;
    default:
        return 0;
    }
}

static bool leanos_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout) {
    struct leanos_priv *p = nsfb->surface_priv;
    if (!p) {
        return false;
    }
    if (pending_valid) {
        *event = pending_event;
        pending_valid = 0;
        return true;
    }

    /* M117: whatever NetSurf plotted since the last turn goes on screen
     * now, before this turn's wait - which is the moment a page, or a
     * scroll, is complete enough to show. Before this the compositor
     * noticed on a 100 ms poll. */
    if (p->damaged) {
        p->damaged = 0;
        wm_present(&p->win);
    }

    /* The bounded wait libnsfb asks for - NetSurf's scheduler needs to
     * run its timers even when nobody is typing. M117: this was a poll
     * plus a 5 ms usleep, two hundred wakeups a second with nothing to
     * do; it is now wm_wait_ms on the event pipe, which returns the
     * moment the compositor writes a click or a key and otherwise sleeps
     * until the deadline (or the liveness cap, and loops). */
    long started = sys_uptime_ms();
    for (;;) {
        wm_event_t in;
        while (wm_poll_event(&p->win, &in) == 1) {
            /* Re-check the buffer on EVERY event rather than only on an
             * EXPOSE. wm_poll_event calls wm_reconnect_if_needed itself
             * (see wmclient.h), so the shared segment can move under
             * this surface with no event that this code would otherwise
             * recognise as meaning it. Drawing into a freed segment is
             * the one failure here that corrupts somebody else's memory
             * rather than merely looking wrong, so it is checked
             * unconditionally; it costs three compares per event. */
            resync(nsfb, p);
            if (convert(p, &in, event)) {
                return true;
            }
        }
        if (timeout == 0) {
            return false;
        }
        int remaining = -1;
        if (timeout > 0) {
            long waited = sys_uptime_ms() - started;
            if (waited >= timeout) {
                return false;
            }
            remaining = timeout - (int)waited;
        }
        wm_wait_ms(&p->win, NULL, 0, remaining);
    }
}

static int leanos_claim(nsfb_t *nsfb, nsfb_bbox_t *box) {
    (void)nsfb;
    (void)box;
    return 0;
}

static int leanos_update(nsfb_t *nsfb, nsfb_bbox_t *box) {
    (void)box;
    struct leanos_priv *p = nsfb->surface_priv;
    if (p) {
        p->damaged = 1; /* M117 - presented by leanos_input, see there */
    }
    /* Deliberately nothing. This compositor composites from the shared
     * segment on its own schedule (M21) - there is no present, flush or
     * damage call to make, and inventing one that did nothing would be
     * worse than an empty function with this comment on it. */
    return 0;
}

static int leanos_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor) {
    (void)nsfb;
    (void)cursor;
    /* The compositor owns the cursor and draws it above every window
     * (M20). NetSurf's own pointer bitmap is therefore not drawn - the
     * alternative would be two cursors on screen, one of them lagging.
     * Returning 0 rather than -1 because the request was honoured in
     * the only way this machine can honour it. */
    return 0;
}

const nsfb_surface_rtns_t leanos_rtns = {
    .defaults = leanos_defaults,
    .initialise = leanos_initialise,
    .finalise = leanos_finalise,
    .input = leanos_input,
    .geometry = leanos_set_geometry,
    .claim = leanos_claim,
    .update = leanos_update,
    .cursor = leanos_cursor,
};

/* NSFB_SURFACE_LINUX is an enum value libnsfb declares and this tree
 * does not implement - there is no linux.c in libnsfb/src/surface. So
 * the slot is free, and taking it means this surface needs no change to
 * libnsfb's own enum. The NAME is what NetSurf actually selects on
 * (`-f leanos`), and the name is this project's. */
NSFB_SURFACE_DEF(leanos, NSFB_SURFACE_LINUX, &leanos_rtns)
