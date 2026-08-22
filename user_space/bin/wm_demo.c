/* user_space/bin/wm_demo.c
 *
 * M20 self-test client: requests a window from the compositor
 * (user_space/bin/compositor.c) over the well-known named pipes
 * (system_api/include/wm.h), then draws a deterministic pattern - a
 * solid content color with a smaller accent-colored square inside it -
 * so kernel_main (kernel/kernel.c) can verify specific pixels landed
 * exactly where expected once the compositor composites this window
 * onto the real framebuffer.
 */
#include "str.h"
#include "syscall_wrappers.h"
#include "wm.h"

#define WIN_W 200
#define WIN_H 120
#define CONTENT_COLOR 0x00336699u
#define ACCENT_COLOR  0x00CC8822u
#define ACCENT_X      20
#define ACCENT_Y      20
#define ACCENT_SIZE   40

int main(void) {
    int req_fds[2];
    int resp_fds[2];
    if (sys_pipe_open(WM_REQUEST_PIPE, req_fds) != 0 || sys_pipe_open(WM_RESPONSE_PIPE, resp_fds) != 0) {
        sys_exit(1);
    }

    wm_create_request_t req;
    req.width = WIN_W;
    req.height = WIN_H;
    if (sys_write(req_fds[1], &req, sizeof(req)) != (long)sizeof(req)) {
        sys_exit(1);
    }

    wm_create_response_t resp;
    long n = sys_read(resp_fds[0], &resp, sizeof(resp));
    if (n != (long)sizeof(resp) || resp.shm_id < 0) {
        sys_exit(1);
    }

    long vaddr = sys_shm_map(resp.shm_id);
    if (vaddr < 0) {
        sys_exit(1);
    }

    uint32_t *pixels = (uint32_t *)vaddr;
    for (int i = 0; i < WIN_W * WIN_H; i++) {
        pixels[i] = CONTENT_COLOR;
    }
    for (int y = ACCENT_Y; y < ACCENT_Y + ACCENT_SIZE; y++) {
        for (int x = ACCENT_X; x < ACCENT_X + ACCENT_SIZE; x++) {
            pixels[y * WIN_W + x] = ACCENT_COLOR;
        }
    }

    /* No stdout message after drawing: like compositor.c, this process's
     * stdout goes through the same shared framebuffer console the
     * compositor is compositing onto, and a console scroll after this
     * point could shift the just-drawn frame before anything verifies
     * it - see compositor.c's header comment on this exact point. */
    return 0;
}
