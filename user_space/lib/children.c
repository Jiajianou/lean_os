#include "children.h"

#include "syscall_wrappers.h"

/* More than any desktop will have outstanding at once - the compositor
 * caps its own windows at WM_MAX_ROUTABLE_WINDOWS (12), and a child that
 * has exited leaves this list on the very next reap pass. */
#define CHILD_MAX 32

static long children[CHILD_MAX];
static int child_count;

void child_track(long pid) {
    if (pid < 0 || child_count >= CHILD_MAX) {
        return;
    }
    children[child_count++] = pid;
}

void child_reap(void) {
    int out = 0;
    for (int i = 0; i < child_count; i++) {
        /* -2 means "still running" (system_api/include/syscall.h). Anything
         * else - an exit code, or -1 for a pid this kernel no longer knows -
         * means there is nothing left to wait for, so the entry goes. */
        if (sys_wait_nb(children[i]) == -2) {
            children[out++] = children[i];
        }
    }
    child_count = out;
}
