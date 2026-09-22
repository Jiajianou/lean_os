#include <stdint.h>

#include "window_manager.h"
#include "window_manager_client.h"
#include "syscall_wrappers.h"

/* A first-party exercise of M174's popup path: a main window opens a popup
   the compositor places, the popup grabs the mouse, and a click outside it -
   which capture delivers to the popup with out-of-bounds coordinates - closes
   it. The desktop's launcher starts this by name, and the interactive suite
   grades the pixels: a magenta popup over a blue window, then the popup gone
   and the window still there. */

#define MAIN_W 460
#define MAIN_H 340
#define POPUP_W 200
#define POPUP_H 140
#define POPUP_X 60
#define POPUP_Y 60
#define MAIN_COLOR  0x00203A8Cu
#define POPUP_COLOR 0x00E020E0u

static void fill(window_manager_window_t *win, uint32_t color) {
    uint32_t *pixels = (uint32_t *)win->graphics.pixels;
    int32_t n = win->graphics.width * win->graphics.height;
    for (int32_t i = 0; i < n; i++) {
        pixels[i] = color;
    }
    window_manager_present(win);
}

int main(void) {
    window_manager_window_t main_window;
    if (window_manager_connect(MAIN_W, MAIN_H, "Popup", &main_window) != 0) {
        return 1;
    }
    fill(&main_window, MAIN_COLOR);

    window_manager_window_t popup;
    if (window_manager_connect_popup(main_window.window_id, POPUP_X, POPUP_Y,
                                     POPUP_W, POPUP_H, &popup) != 0) {
        return 2;
    }
    fill(&popup, POPUP_COLOR);
    window_manager_set_capture(&popup, 1);

    for (;;) {
        window_manager_event_t event;
        if (window_manager_poll_event(&popup, &event) != 1) {
            window_manager_wait_ms(&popup, 0, 0, 100);
            continue;
        }
        if (event.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && (event.buttons & 1)) {
            int outside = event.x < 0 || event.y < 0 ||
                          event.x >= POPUP_W || event.y >= POPUP_H;
            if (outside) {
                window_manager_set_capture(&popup, 0);
                window_manager_send_action(popup.window_id, WINDOW_MANAGER_ACTION_CLOSE);
                break;
            }
        }
        if (event.type == WINDOW_MANAGER_EVENT_CLOSE_REQUEST) {
            break;
        }
    }

    /* The main window stays until it is closed, so the suite can see that the
       popup is gone and the window is not. */
    for (;;) {
        window_manager_event_t event;
        if (window_manager_poll_event(&main_window, &event) == 1 &&
            event.type == WINDOW_MANAGER_EVENT_CLOSE_REQUEST) {
            break;
        }
        window_manager_wait_ms(&main_window, 0, 0, 100);
    }
    return 0;
}
