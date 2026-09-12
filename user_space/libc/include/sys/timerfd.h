/* user_space/libc/include/sys/timerfd.h - M119
 *
 * A deadline as a descriptor. **The granularity is 10 ms** - PIT_HZ, the
 * only clock this machine has - and a shorter request is rounded up to one
 * tick rather than down to zero, because a timer that is readable
 * immediately is an event loop that spins. See kernel/ipc/timerfd.h.
 */
#pragma once

#include <time.h> /* struct itimerspec, struct timespec */

#ifdef __cplusplus
extern "C" {
#endif

#define TFD_NONBLOCK     0x800
#define TFD_CLOEXEC      0x80000
#define TFD_TIMER_ABSTIME 0x1

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);

#ifdef __cplusplus
}
#endif
