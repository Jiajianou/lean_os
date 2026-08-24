/* user_space/bin/wm_faulter.c
 *
 * M52: a program that dereferences a null pointer on purpose, for the
 * one thing this milestone cannot check any other way - that a wild
 * pointer in a user program kills that program and leaves the machine
 * running.
 *
 * Same self-test-only role wm_demo.c has held since M20, wm_stubborn.c
 * since M45 and wm_zorder.c since M51. It is the only program in this
 * project that is *supposed* to crash, which is worth saying out loud:
 * a reader finding a null dereference in this tree should assume it is a
 * bug everywhere except here.
 *
 * It connects to the compositor and paints a window first, deliberately.
 * A process that faults before owning anything proves only that the
 * fault handler runs; one that faults holding a window, an shm segment
 * and an event pipe proves that everything downstream of a crash still
 * works - M29's reap_dead_clients noticing, M48's crash toast, the shm
 * segment coming back. The window is what the self-test reads to know it
 * really was alive first.
 *
 * The fault is written so the compiler cannot fold it away: -O1 is
 * entitled to treat a store through a null pointer as unreachable and
 * delete it. `volatile` on the pointer itself is what stops that - the
 * value has to be loaded and then used, so the store really is emitted.
 */
#include "syscall_wrappers.h"
#include "wmclient.h"

#define WIN_W 200
#define WIN_H 120
#define FILL_COLOR 0x0020C0A0u /* teal - distinct from every other self-test client's fill, so a pixel probe cannot confuse them */

/* 16 pages, created and never freed - the same trick wm_stubborn.c uses
 * for the same reason. The window's own pixel buffer belongs to the
 * *compositor* (it is the side that calls SYS_shm_create), so without a
 * segment of its own this process would own no reclaimable resource at
 * all and "did the fault path run shm_free_by_owner" would have nothing
 * to measure. */
#define OWNED_SHM_BYTES (64 * 1024)

/* Milliseconds to stay alive after painting, so the compositor has
 * composited the window and the self-test has read it back before this
 * process goes away. Generous rather than tight: the assertion is about
 * what happens *after* the fault, and arriving there early proves
 * nothing. */
#define ALIVE_MS 1200

int main(void) {
    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Faulter", &win) != 0) {
        sys_exit(1);
    }
    sys_shm_create(OWNED_SHM_BYTES); /* deliberately never used, never freed - see OWNED_SHM_BYTES */
    gfx_fill_rect(&win.gfx, 0, 0, (int32_t)win.width, (int32_t)win.height, FILL_COLOR);

    long deadline = sys_uptime_ms() + ALIVE_MS;
    while (sys_uptime_ms() < deadline) {
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
        }
        sys_yield();
    }

    /* The whole point of this program. Before M52 this took the machine
     * down; now it takes exactly this process down, with exit code
     * 128 + SIGSEGV. */
    volatile uint32_t *wild = (volatile uint32_t *)0;
    *wild = 0xDEADBEEFu;

    return 0; /* unreachable */
}
