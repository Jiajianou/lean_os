#include <poll.h>

#include "syscall_wrappers.h"

int poll(struct pollfd *file_descriptors, nfds_t nfds, int timeout) {
    if (!file_descriptors && nfds > 0) {
        return -1;
    }

    int watch[64];
    unsigned int map[64];
    unsigned int n = 0;
    for (unsigned int i = 0; i < nfds; i++) {
        file_descriptors[i].revents = 0;
        if (file_descriptors[i].fd < 0 || n >= 64) {
            continue;
        }
        watch[n] = file_descriptors[i].fd;
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
        if (file_descriptors[map[i]].events & POLLOUT) {
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
            file_descriptors[map[i]].revents |= (short)(file_descriptors[map[i]].events & (POLLIN | POLLOUT));
            if (file_descriptors[map[i]].revents == 0) {
                continue;
            }
            ready++;
            continue;
        }
        if (r == -1) {
            file_descriptors[map[i]].revents = POLLNVAL;
            ready++;
            continue;
        }
        if (file_descriptors[map[i]].events & POLLOUT) {
            file_descriptors[map[i]].revents = POLLOUT;
            ready++;
        }
    }
    return ready;
}
