#include "syscall_wrappers.h"
#include "window_manager_client.h"

#define WIN_W 200
#define WIN_H 120
#define FILL_COLOR 0x00B03040u

#define OWNED_SHM_BYTES (64 * 1024)

int main(void) {
    wm_window_t win;
    if (wm_connect_confirm_close(WIN_W, WIN_H, "Stubborn", &win) != 0) {
        sys_exit(1);
    }
    sys_shm_create(OWNED_SHM_BYTES);

    gfx_fill_rect(&win.gfx, 0, 0, (int32_t)win.width, (int32_t)win.height, FILL_COLOR);
    wm_present(&win);

    for (;;) {
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
        }
        wm_wait_ms(&win, NULL, 0, -1);
    }
}
