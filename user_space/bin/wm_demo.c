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

    /* M29: zeroed first (panel/desktop/title were already implicitly
     * relying on this - only width/height were ever set here - and the
     * new client_pid field would otherwise carry uninitialized stack
     * garbage into the compositor's SYS_task_alive liveness tracking). */
    wm_create_request_t req;
    memset(&req, 0, sizeof(req));
    req.width = WIN_W;
    req.height = WIN_H;
    req.client_pid = (int32_t)sys_getpid();

    /* ---- ask, and ask again if nobody answers -------------------------
     *
     * This used to be one write and one blocking read, which is the
     * shortest possible statement of the protocol and was wrong for a
     * reason that took a long time to find.
     *
     * A compositor CLEARS these rendezvous pipes when it starts (M55: a
     * dead compositor's leftovers are not the new one's business, and
     * acting on them is worse than losing them). This process and the
     * compositor are spawned within a few instructions of each other, so
     * a request written before that clear is discarded - and a client
     * that then blocks forever on a response nobody will write is a
     * process nothing can distinguish from a slow one.
     *
     * It was invisible for fifty milestones because on a first boot the
     * compositor always won the race. It appeared the moment a *second*
     * boot skipped the "seeding disk" step and shifted the timing by a
     * few seconds, and it presented as "the second boot after a reboot
     * hangs", which is a very long way from "one client has no retry".
     *
     * user_space/lib/wmclient.c's connect_common has had exactly this
     * retry, with exactly this comment, since M55. This is the one
     * client in the project that hand-rolls the handshake instead of
     * using it - deliberately, so the raw protocol is exercised by
     * something - and hand-rolling it meant hand-rolling this too. */
    wm_create_response_t resp;
    int got_response = 0;
    for (int attempt = 0; attempt < 4 && !got_response; attempt++) {
        if (sys_write(req_fds[1], &req, sizeof(req)) != (long)sizeof(req)) {
            sys_exit(1);
        }
        long deadline = sys_uptime_ms() + 500;
        while (!got_response && sys_uptime_ms() < deadline) {
            if (sys_pipe_poll(resp_fds[0]) < (long)sizeof(resp)) {
                sys_yield();
                continue;
            }
            if (sys_read(resp_fds[0], &resp, sizeof(resp)) != (long)sizeof(resp)) {
                sys_exit(1);
            }
            /* WM_RESPONSE_PIPE is one shared stream, so this may be
             * somebody else's answer - same check wmclient makes, and
             * the reason the retry above has to exist even when the
             * first request did arrive. */
            got_response = (resp.client_pid == req.client_pid);
        }
    }
    if (!got_response || resp.shm_id < 0) {
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
