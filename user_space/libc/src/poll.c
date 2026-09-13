#include <poll.h>

#include "syscall_wrappers.h"

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    if (!fds && nfds > 0) {
        return -1;
    }

    int watch[64];
    unsigned int map[64];
    unsigned int n = 0;
    for (unsigned int i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0 || n >= 64) {
            continue;
        }
        watch[n] = fds[i].fd;
        map[n] = i;
        n++;
    }
    if (n == 0) {
        if (timeout > 0) {
            (void)sys_waitfds(watch, 0, timeout);
        }
        return 0;
    }

    int wants_write = 0;
    for (unsigned int i = 0; i < n; i++) {
        if (fds[map[i]].events & POLLOUT) {
            wants_write = 1;
            break;
        }
    }

    long first = -3;
    if (!wants_write) {
        first = sys_waitfds(watch, (int)n, timeout);
        if (first == -1) {
            return -1;
        }
        if (first == -2) {
            return 0;
        }
    }

    int ready = 0;
    for (unsigned int i = 0; i < n; i++) {
        int one = watch[i];
        long r = ((long)i == first) ? 0 : sys_waitfds(&one, 1, 0);
        if (r == 0) {
            fds[map[i]].revents |= (short)(fds[map[i]].events & (POLLIN | POLLOUT));
            if (fds[map[i]].revents == 0) {
                continue;
            }
            ready++;
            continue;
        }
        if (r == -1) {
            fds[map[i]].revents = POLLNVAL;
            ready++;
            continue;
        }
        if (fds[map[i]].events & POLLOUT) {
            fds[map[i]].revents = POLLOUT;
            ready++;
        }
    }
    return ready;
}
