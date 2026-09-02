/* user_space/libc/include/poll.h - M88
 *
 * `poll`, over SYS_waitfds (M68) - which is genuinely the same idea under
 * a lean_os name, so this is a header and a shim rather than a kernel
 * feature. That is worth stating because it is unusual for this project:
 * most of what a ported program asks for here had to be built. This one
 * existed already and was spelled differently.
 *
 * Two honest limits, both about things this machine does not have:
 *
 *   - POLLOUT is reported ready for any descriptor that is open. There
 *     is no write-readiness anywhere in this kernel: a pipe write blocks
 *     when the pipe is full and nothing can be asked in advance whether
 *     it would. Saying "ready" is what every caller then does anyway -
 *     it writes, and blocks if it must - and it is a better answer than
 *     never reporting POLLOUT, which would make a program that waits for
 *     it wait forever.
 *   - POLLPRI, POLLRDHUP and the rest of the out-of-band family are not
 *     defined. There is no out-of-band data on this machine to report,
 *     and a constant a program could test but never see set is the
 *     failure mode <fcntl.h> spent a header comment avoiding.
 *
 * POLLHUP and POLLERR are real: a pipe whose last writer has gone is
 * exactly the end-of-stream condition SYS_waitfds already reports as
 * readable, and a caller that could not tell that from data would spin.
 */
#pragma once

#include <stddef.h>

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

#define POLLIN   0x001 /* there is something to read */
#define POLLOUT  0x004 /* writable - see the header note on why this is always set */
#define POLLERR  0x008 /* only ever set in revents */
#define POLLHUP  0x010 /* only ever set in revents */
#define POLLNVAL 0x020 /* the fd was not open - only ever set in revents */

typedef unsigned int nfds_t;

struct pollfd {
    int fd;         /* negative to skip this entry, as POSIX specifies */
    short events;   /* what the caller is waiting for */
    short revents;  /* what actually happened */
};

/* Waits until one of `fds` is ready, `timeout` milliseconds pass, or -
 * with a negative timeout - forever. Returns the number of entries with
 * a nonzero revents, 0 on timeout, or -1.
 *
 * The blocking is SYS_waitfds's, so a task inside this is TASK_BLOCKED
 * rather than spinning - which is the whole reason M68 exists and the
 * reason a program should use this rather than a loop of non-blocking
 * reads. */
int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
}
#endif
