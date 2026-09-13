#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"

#include "nsfb.h"
#include "plot.h"
#include "surface.h"

#include "input.h"
#include "wm.h"
#include "window_manager_client.h"
#include "syscall_wrappers.h"

struct leanos_priv {
    window_manager_window_t win;
    int damaged;
    uint8_t buttons;
};

#define LEANOS_DEFAULT_W 900
#define LEANOS_DEFAULT_H 640

static int leanos_defaults(nsfb_t *nsfb) {
    nsfb->width = LEANOS_DEFAULT_W;
    nsfb->height = LEANOS_DEFAULT_H;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int leanos_initialise(nsfb_t *nsfb) {
    struct leanos_priv *p = calloc(1, sizeof(*p));
    if (!p) {
        return -1;
    }
    if (window_manager_connect((uint32_t)nsfb->width, (uint32_t)nsfb->height,
                   "NetSurf", &p->win) != 0) {
        free(p);
        return -1;
    }
    nsfb->width = p->win.graphics.width;
    nsfb->height = p->win.graphics.height;
    nsfb->ptr = (uint8_t *)p->win.graphics.pixels;
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
    nsfb->ptr = 0;
    return 0;
}

static int leanos_set_geometry(nsfb_t *nsfb, int width, int height,
                               enum nsfb_format_e format) {
    if (nsfb->surface_priv != NULL) {
        return -1;
    }
    if (width > 0) {
        nsfb->width = width;
    }
    if (height > 0) {
        nsfb->height = height;
    }
    if (format != NSFB_FMT_ANY && format != NSFB_FMT_XRGB8888) {
        return -1;
    }
    select_plotters(nsfb);
    return 0;
}

static int pending_valid;
static nsfb_event_t pending_event;

static enum nsfb_key_code_e translate_key(char ch) {
    switch ((unsigned char)ch) {
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
        return (enum nsfb_key_code_e)(unsigned char)ch;
    }
    return NSFB_KEY_UNKNOWN;
}

static void resync(nsfb_t *nsfb, struct leanos_priv *p) {
    if (nsfb->ptr == (uint8_t *)p->win.graphics.pixels &&
        nsfb->width == p->win.graphics.width &&
        nsfb->height == p->win.graphics.height) {
        return;
    }
    nsfb->ptr = (uint8_t *)p->win.graphics.pixels;
    nsfb->width = p->win.graphics.width;
    nsfb->height = p->win.graphics.height;
    nsfb->linelen = nsfb->width * 4;
    select_plotters(nsfb);
}

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
        pending_event.type = NSFB_EVENT_KEY_UP;
        pending_event.value.keycode = code;
        pending_valid = 1;
        return 1;
    }
    case WM_EVENT_MOUSE_MOVE:
    case WM_EVENT_MOUSE_BUTTON: {
        uint8_t was = p->buttons, now = in->buttons;
        p->buttons = now;
        event->type = NSFB_EVENT_MOVE_ABSOLUTE;
        event->value.vector.x = in->x;
        event->value.vector.y = in->y;
        event->value.vector.z = 0;
        uint8_t changed = (uint8_t)(was ^ now);
        if (changed) {
            static const enum nsfb_key_code_e BUTTON[3] = {
                NSFB_KEY_MOUSE_1,
                NSFB_KEY_MOUSE_3,
                NSFB_KEY_MOUSE_2,
            };
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
        event->type = NSFB_EVENT_KEY_DOWN;
        event->value.keycode = in->wheel < 0 ? NSFB_KEY_MOUSE_4
                                             : NSFB_KEY_MOUSE_5;
        pending_event.type = NSFB_EVENT_KEY_UP;
        pending_event.value.keycode = event->value.keycode;
        pending_valid = 1;
        return 1;
    case WM_EVENT_EXPOSE:
    case WM_EVENT_DISPLAY_CHANGED:
        event->type = NSFB_EVENT_RESIZE;
        event->value.resize.w = p->win.graphics.width;
        event->value.resize.h = p->win.graphics.height;
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

    if (p->damaged) {
        p->damaged = 0;
        window_manager_present(&p->win);
    }

    long started = sys_uptime_ms();
    for (;;) {
        wm_event_t in;
        while (window_manager_poll_event(&p->win, &in) == 1) {
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
        window_manager_wait_ms(&p->win, NULL, 0, remaining);
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
        p->damaged = 1;
    }
    return 0;
}

static int leanos_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor) {
    (void)nsfb;
    (void)cursor;
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

NSFB_SURFACE_DEF(leanos, NSFB_SURFACE_LINUX, &leanos_rtns)
