#include "syscall_wrappers.h"
#include "window_manager_client.h"

#define WIN_W 220
#define WIN_H 140
#define BG_COLOR      0x00202020u
#define BORDER_COLOR  0x0088AA55u
#define TEXT_COLOR    0x00FFFFFFu
#define SEPARATOR_Y   24
#define STROKE_COLOR  0x00FFCC00u
#define STROKE_SIZE   5

static void clear_canvas(graphics_context_t *graphics) {
    graphics_fill_rect(graphics, 1, SEPARATOR_Y + 1, WIN_W - 2, WIN_H - SEPARATOR_Y - 2, BG_COLOR);
}

static void draw_chrome(graphics_context_t *g) {
    graphics_fill_rect(g, 0, 0, WIN_W, WIN_H, BG_COLOR);
    graphics_draw_rect(g, 0, 0, WIN_W, WIN_H, BORDER_COLOR);
    graphics_draw_text(g, 10, 6, "PAINT", TEXT_COLOR);
    graphics_draw_line(g, 5, SEPARATOR_Y, WIN_W - 5, SEPARATOR_Y, BORDER_COLOR);
}

int main(void) {
    window_manager_window_t win;
    if (window_manager_connect(WIN_W, WIN_H, "Paint", &win) != 0) {
        sys_exit(1);
    }

    draw_chrome(&win.graphics);

    for (;;) {
        window_manager_event_t ev;
        window_manager_wait_event(&win, &ev);

        if (ev.type == WINDOW_MANAGER_EVENT_EXPOSE || ev.type == WINDOW_MANAGER_EVENT_DISPLAY_CHANGED) {
            draw_chrome(&win.graphics);
            continue;
        }

        if (ev.type == WINDOW_MANAGER_EVENT_KEY && (ev.ch == 'c' || ev.ch == 'C')) {
            clear_canvas(&win.graphics);
            continue;
        }

        if ((ev.type == WINDOW_MANAGER_EVENT_MOUSE_MOVE || ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON) &&
            (ev.buttons & 1) && ev.y > SEPARATOR_Y) {
            graphics_fill_rect(&win.graphics, ev.x - STROKE_SIZE / 2, ev.y - STROKE_SIZE / 2,
                          STROKE_SIZE, STROKE_SIZE, STROKE_COLOR);
        }
    }
}
