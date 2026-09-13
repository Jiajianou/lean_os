#include "syscall_wrappers.h"
#include "window_manager_client.h"

#define WIN_W 200
#define WIN_H 120
#define FILL_COLOR 0x0020C0A0u

#define OWNED_SHARED_MEMORY_BYTES (64 * 1024)

#define ALIVE_MS 1200

int main(void) {
    window_manager_window_t win;
    if (window_manager_connect(WIN_W, WIN_H, "Faulter", &win) != 0) {
        sys_exit(1);
    }
    sys_shared_memory_create(OWNED_SHARED_MEMORY_BYTES);
    graphics_fill_rect(&win.graphics, 0, 0, (int32_t)win.width, (int32_t)win.height, FILL_COLOR);
    window_manager_present(&win);

    long deadline = sys_uptime_ms() + ALIVE_MS;
    while (sys_uptime_ms() < deadline) {
        window_manager_event_t ev;
        while (window_manager_poll_event(&win, &ev)) {
        }
        window_manager_wait_ms(&win, NULL, 0, (int)(deadline - sys_uptime_ms()));
    }

    volatile uint32_t *wild = (volatile uint32_t *)0;
    *wild = 0xDEADBEEFu;

    return 0;
}
