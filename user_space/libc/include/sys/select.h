/* user_space/libc/include/sys/select.h - M88's last bullet
 *
 * `select`, over `poll`, over `SYS_waitfds`. Three names for one idea,
 * and this is the outermost.
 *
 * M88's bullet said `poll` "should be the cheapest bullet in the arc"
 * because SYS_waitfds already was poll under a lean_os name. `select` is
 * cheaper still: it is `poll` with the descriptor set written as a
 * bitmap instead of an array, and the whole of this file is that change
 * of shape plus one honest limit.
 *
 * ---- what it can and cannot report ------------------------------------
 *
 * The same two limits <poll.h> already states, and for the same reason -
 * they are properties of this kernel, not of this header.
 *
 *   - **Writability is always reported.** There is no write-readiness
 *     anywhere in this kernel: a pipe write blocks when the pipe is
 *     full, and nothing can be asked in advance whether it would. Saying
 *     "ready" is what a caller acts on anyway, and it beats never
 *     reporting a writable descriptor, which makes a program that waits
 *     for one wait forever.
 *   - **`exceptfds` is always empty.** Out-of-band data is a TCP feature
 *     this stack does not implement (M66 says so), and nothing else here
 *     produces an exceptional condition. A set that is cleared and
 *     stays cleared is the truthful answer; a set that reported
 *     something would have to invent what.
 *
 * ---- FD_SETSIZE, and why it is 128 ------------------------------------
 *
 * MAX_FDS. A descriptor set that could name more descriptors than a
 * process can hold would be a promise about a table that does not exist,
 * and one that could name fewer would silently drop the high end - which
 * is the classic `select` bug and the reason `poll` was invented. This
 * one matches the kernel exactly, and `select` refuses an `nfds` above
 * it rather than reading past the end of the caller's bitmap.
 */
#pragma once

#include <sys/time.h>  /* struct timeval - what select's timeout is */
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MAX_FDS in kernel/sched/sched.h. See the header note. */
#define FD_SETSIZE 128

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define __FD_WORD(fd) ((unsigned)(fd) / (8 * sizeof(unsigned long)))
#define __FD_BIT(fd)  (1UL << ((unsigned)(fd) % (8 * sizeof(unsigned long))))

/* Written as functions rather than as statement macros so they work in
 * an expression and inside an `if` without braces - which is how a
 * ported program will use them, and how the macro versions of these have
 * bitten every project that wrote them the other way. */
static inline void FD_ZERO(fd_set *s) {
    for (unsigned i = 0; i < sizeof(s->fds_bits) / sizeof(s->fds_bits[0]); i++) {
        s->fds_bits[i] = 0;
    }
}
static inline void FD_SET(int fd, fd_set *s) {
    if (fd >= 0 && fd < FD_SETSIZE) {
        s->fds_bits[__FD_WORD(fd)] |= __FD_BIT(fd);
    }
}
static inline void FD_CLR(int fd, fd_set *s) {
    if (fd >= 0 && fd < FD_SETSIZE) {
        s->fds_bits[__FD_WORD(fd)] &= ~__FD_BIT(fd);
    }
}
static inline int FD_ISSET(int fd, const fd_set *s) {
    if (fd < 0 || fd >= FD_SETSIZE) {
        return 0;
    }
    return (s->fds_bits[__FD_WORD(fd)] & __FD_BIT(fd)) != 0;
}

/* Returns the number of descriptors left set across all three sets, 0 on
 * timeout, -1 on error. The sets are modified in place, which is the
 * part of this interface everybody forgets: a caller in a loop has to
 * rebuild them every time round, and that is not a bug in this
 * implementation.
 *
 * A NULL `timeout` waits with no deadline. A timeout of zero polls. The
 * struct is NOT updated with the time remaining - Linux does that and
 * POSIX explicitly permits either, and this kernel's deadline arithmetic
 * is in whole milliseconds. */
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);

#ifdef __cplusplus
}
#endif
