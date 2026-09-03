/* user_space/libc/src/poll.c - M88
 *
 * See <poll.h> for what this can and cannot honestly report.
 *
 * The implementation is two passes and the reason is in SYS_waitfds's own
 * ABI note: it returns ONE ready index rather than a bitmask, and says
 * that is deliberate because "the callers here service one channel per
 * pass and loop, and a mask would be an API that promises a fairness this
 * scheduler does not implement".
 *
 * `poll` has to report every ready descriptor, not one. So the first pass
 * blocks - that is the part that must not be a spin - and the second asks
 * each descriptor individually with a zero timeout, which SYS_waitfds
 * documents as a poll that returns immediately. That is n+1 syscalls for
 * n descriptors, which is the honest cost of building a mask out of a
 * call that returns an index, and is nothing next to the block it just
 * came out of.
 */
#include <poll.h>

#include "syscall_wrappers.h"

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    if (!fds && nfds > 0) {
        return -1;
    }

    /* The descriptors the caller actually wants watched. A negative fd is
     * skipped with revents 0, which is POSIX's way of letting a program
     * keep a fixed-size array and disable entries in it. */
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
        /* Nothing to wait for.
         *
         * POSIX says an empty set with a timeout is a sleep, and every
         * poll implementation honours it. There is no sleep wrapper in
         * this libc, but SYS_waitfds with a count of zero is exactly one
         * - it has a deadline and nothing that could satisfy it early -
         * so the sleep is expressed as the wait it actually is rather
         * than by reaching for a second mechanism. */
        if (timeout > 0) {
            (void)sys_waitfds(watch, 0, timeout);
        }
        return 0;
    }

    /* ---- M88 (third attempt): a set that asks only for POLLOUT --------
     *
     * POLLOUT is reported ready for any open descriptor here, because
     * there is no write-readiness anywhere in this kernel (see <poll.h>).
     * So a set containing one is a set with something ready in it *before
     * this call blocks*, and blocking on it was a bug with two visible
     * halves: `poll(fd, POLLOUT, 1000)` waited a second and then returned
     * 0, and `poll(fd, POLLOUT, 0)` returned 0 immediately - a poll that
     * says nothing is ready about a descriptor it will call ready one
     * line later.
     *
     * Found by `select`, which is a shim over this function: a
     * select-for-writable is exactly this shape, and nothing had ever
     * written one. SYS_waitfds is not wrong here - it answers about
     * *readability*, which is the only readiness this kernel has - so the
     * fix is that this function must not ask it a question it has
     * already answered. */
    int wants_write = 0;
    for (unsigned int i = 0; i < n; i++) {
        if (fds[map[i]].events & POLLOUT) {
            wants_write = 1;
            break;
        }
    }

    long first = -3; /* nothing known ready yet: probe every descriptor below */
    if (!wants_write) {
        first = sys_waitfds(watch, (int)n, timeout);
        if (first == -1) {
            return -1;
        }
        if (first == -2) {
            return 0; /* the deadline passed with nothing ready */
        }
    }

    /* Now the mask. Each descriptor is asked on its own with a zero
     * timeout, which is a poll rather than a wait - so this cannot block
     * however many of them turn out not to be ready. */
    int ready = 0;
    for (unsigned int i = 0; i < n; i++) {
        int one = watch[i];
        long r = ((long)i == first) ? 0 : sys_waitfds(&one, 1, 0);
        if (r == 0) {
            /* Ready to read. POLLHUP is not distinguished from POLLIN
             * here and cannot be: SYS_waitfds reports end-of-stream as
             * readable on purpose, because a reader that could not be
             * woken by it would hang at exactly the moment it should
             * stop. A caller finds out which it was by reading and
             * getting zero bytes, which is what it would have done
             * anyway. */
            fds[map[i]].revents |= (short)(fds[map[i]].events & POLLIN);
            if (fds[map[i]].revents == 0) {
                /* Readable but the caller did not ask for POLLIN. Nothing
                 * to report, and reporting POLLERR would be wrong - there
                 * is no error, only news the caller did not want. */
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
        /* Not ready to read. Writable is still reported if asked for -
         * see the header note on why that is always true here. */
        if (fds[map[i]].events & POLLOUT) {
            fds[map[i]].revents = POLLOUT;
            ready++;
        }
    }
    return ready;
}
