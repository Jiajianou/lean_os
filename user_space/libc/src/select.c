/* user_space/libc/src/select.c - M88's last bullet. See <sys/select.h>. */
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
        /* exceptfds contributes nothing to the wait. A descriptor named
         * ONLY there is therefore not watched at all, which is correct
         * rather than a shortcut: nothing on this machine can make it
         * ready, so watching it would be waiting for an event that
         * cannot happen. See the header. */
        if (ev == 0) {
            continue;
        }
        pfd[n].fd = fd;
        pfd[n].events = ev;
        pfd[n].revents = 0;
        fds[n] = fd;
        n++;
    }

    /* poll takes milliseconds and a negative value means "no deadline",
     * which is exactly what a NULL timeval means here. The rounding is
     * up rather than down: a caller asking for 1 microsecond is asking
     * not to block indefinitely, and rounding that to 0 would turn its
     * wait into a spin. */
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

    /* The sets are rebuilt from what poll reported rather than cleared
     * bit by bit as they are examined: a descriptor the caller set and
     * this loop never visits - one named only in exceptfds, or one at or
     * above nfds - has to come back clear, and the only way to be sure
     * of that is to start from empty. */
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
        /* POLLNVAL is a bad descriptor, and select reports that as an
         * error for the whole call rather than per descriptor - it has
         * nowhere to put a per-descriptor error. That is what EBADF from
         * select means everywhere and is why poll exists. */
        if (pfd[i].revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        /* A descriptor can be counted twice by select - once for read and
         * once for write - and that is the specified behaviour: the
         * return value is the number of BITS still set, not the number
         * of descriptors. */
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
