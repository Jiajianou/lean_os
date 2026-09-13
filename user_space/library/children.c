#include "children.h"

#include "syscall_wrappers.h"

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
        if (sys_wait_nb(children[i]) == -2) {
            children[out++] = children[i];
        }
    }
    child_count = out;
}
