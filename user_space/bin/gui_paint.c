#include "syscall_wrappers.h"
#include "wmclient.h"

#define WIN_W 220
#define WIN_H 140
#define BG_COLOR      0x00202020u
#define BORDER_COLOR  0x0088AA55u
#define TEXT_COLOR    0x00FFFFFFu
#define SEPARATOR_Y   24
#define STROKE_COLOR  0x00FFCC00u
#define STROKE_SIZE   5

static void clear_canvas(gfx_ctx_t *gfx) {
    gfx_fill_rect(gfx, 1, SEPARATOR_Y + 1, WIN_W - 2, WIN_H - SEPARATOR_Y - 2, BG_COLOR);
}

static void draw_chrome(gfx_ctx_t *g) {
    gfx_fill_rect(g, 0, 0, WIN_W, WIN_H, BG_COLOR);
    gfx_draw_rect(g, 0, 0, WIN_W, WIN_H, BORDER_COLOR);
    gfx_draw_text(g, 10, 6, "PAINT", TEXT_COLOR);
    gfx_draw_line(g, 5, SEPARATOR_Y, WIN_W - 5, SEPARATOR_Y, BORDER_COLOR);
}

int main(void) {
    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Paint", &win) != 0) {
        sys_exit(1);
    }

    draw_chrome(&win.gfx);

    for (;;) {
        wm_event_t ev;
        wm_wait_event(&win, &ev);

        if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
            draw_chrome(&win.gfx);
            continue;
        }

        if (ev.type == WM_EVENT_KEY && (ev.ch == 'c' || ev.ch == 'C')) {
            clear_canvas(&win.gfx);
            continue;
        }

        if ((ev.type == WM_EVENT_MOUSE_MOVE || ev.type == WM_EVENT_MOUSE_BUTTON) &&
            (ev.buttons & 1) && ev.y > SEPARATOR_Y) {
            gfx_fill_rect(&win.gfx, ev.x - STROKE_SIZE / 2, ev.y - STROKE_SIZE / 2,
                          STROKE_SIZE, STROKE_SIZE, STROKE_COLOR);
        }
    }
}
