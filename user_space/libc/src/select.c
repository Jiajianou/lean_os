#include <sys/select.h>

#include <errno.h>
#include <poll.h>
#include <stddef.h>

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout) {
    struct pollfd pfd[FD_SETSIZE];
    int fds[FD_SETSIZE];
    int n = 0;

    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (readfds && FD_ISSET(fd, readfds)) {
            ev |= POLLIN;
        }
        if (writefds && FD_ISSET(fd, writefds)) {
            ev |= POLLOUT;
        }
        if (ev == 0) {
            continue;
        }
        pfd[n].fd = fd;
        pfd[n].events = ev;
        pfd[n].revents = 0;
        fds[n] = fd;
        n++;
    }

    int ms = -1;
    if (timeout) {
        long long us = (long long)timeout->tv_sec * 1000000 + timeout->tv_usec;
        if (us < 0) {
            errno = EINVAL;
            return -1;
        }
        ms = (int)((us + 999) / 1000);
        if (us > 0 && ms == 0) {
            ms = 1;
        }
    }

    int r = poll(pfd, (nfds_t)n, ms);
    if (r < 0) {
        return -1;
    }

    fd_set rout, wout;
    FD_ZERO(&rout);
    FD_ZERO(&wout);
    int ready = 0;
    for (int i = 0; i < n; i++) {
        int counted = 0;
        if (readfds && (pfd[i].revents & POLLIN)) {
            FD_SET(fds[i], &rout);
            counted = 1;
        }
        if (writefds && (pfd[i].revents & POLLOUT)) {
            FD_SET(fds[i], &wout);
            counted = 1;
        }
        if (pfd[i].revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        if (counted) {
            ready += (readfds && FD_ISSET(fds[i], &rout)) ? 1 : 0;
            ready += (writefds && FD_ISSET(fds[i], &wout)) ? 1 : 0;
        }
    }

    if (readfds) {
        *readfds = rout;
    }
    if (writefds) {
        *writefds = wout;
    }
    if (exceptfds) {
        FD_ZERO(exceptfds);
    }
    return ready;
}
