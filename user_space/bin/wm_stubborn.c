/* user_space/bin/wm_stubborn.c
 *
 * M45: a deliberately unkillable-by-politeness GUI client, for the one
 * case WM_ACTION_CLOSE structurally cannot handle.
 *
 * M36 made confirm_close an opt-in whose contract explicitly permits a
 * client to never answer WM_EVENT_CLOSE_REQUEST - which is exactly why
 * WM_ACTION_KILL had to exist. Asserting that difference needs a client
 * that really does ignore the event forever, and every app this project
 * ships responds to it properly (text_editor.c, the only other opt-in,
 * quits immediately on a clean buffer). So this is that client, in the
 * same "exists only for a self-test" role wm_demo.c has held since M20.
 *
 * It also creates one shm segment of its own, which nothing about being
 * stubborn requires: it is what lets the self-test check that killing a
 * task still runs shm_free_by_owner (kernel/sched/sched.c's
 * task_exit_with_code) on the *signal* path, not just on the SYS_exit
 * one. Without it the victim owns no reclaimable resource at all and
 * "did the kill clean up" has nothing to measure.
 */
#include "syscall_wrappers.h"
#include "wmclient.h"

#define WIN_W 200
#define WIN_H 120
#define FILL_COLOR 0x00B03040u /* nothing else on this desktop is anywhere near this - see the M45 self-test's pixel probes */

/* 16 pages. Big enough that the free-frame count moving by it is
 * unmistakable rather than lost in ordinary allocator noise, small
 * enough to be free. */
#define OWNED_SHM_BYTES (64 * 1024)

int main(void) {
    wm_window_t win;
    if (wm_connect_confirm_close(WIN_W, WIN_H, "Stubborn", &win) != 0) {
        sys_exit(1);
    }
    sys_shm_create(OWNED_SHM_BYTES); /* deliberately never used, never freed - see the header comment */

    gfx_fill_rect(&win.gfx, 0, 0, (int32_t)win.width, (int32_t)win.height, FILL_COLOR);
    wm_present(&win); /* M117: the first frame, like every other one */

    for (;;) {
        wm_event_t ev;
        /* Every event is read and thrown away, WM_EVENT_CLOSE_REQUEST
         * included. Draining rather than ignoring the pipe outright
         * matters: a client that let its event pipe fill would eventually
         * block the compositor's own write, which would be a different
         * (and much less interesting) reason for a window not to close. */
        while (wm_poll_event(&win, &ev)) {
        }
        wm_wait_ms(&win, NULL, 0, -1); /* M117 */
    }
}
