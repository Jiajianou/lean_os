/* system_api/include/os_poll.h
 *
 * M119: the two structures the readiness calls exchange.
 *
 * A header of their own rather than a corner of os_net.h, for the reason
 * kernel/ipc/unixsock.c is not in kernel/net: an epoll set watches pipes,
 * files, timers and counters, and a socket is one of the things it can
 * watch rather than what it is about.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One event, in and out of the kernel.
 *
 * **Not `struct epoll_event`'s Linux layout**, and that is a decision
 * rather than an accident. Linux packs that struct to 12 bytes so that a
 * 32-bit process and a 64-bit kernel agree; this OS has one word size and
 * no binary compatibility to keep, and a packed field is a misaligned
 * 64-bit load on every access. So `data` is aligned, the struct is 16
 * bytes, and <sys/epoll.h> declares exactly this shape - which means
 * sendmsg's conversion problem does not exist here and `epoll_wait` is a
 * copy rather than a translation.
 *
 * `data` is the caller's cookie. The kernel carries it and never looks
 * inside it, which is what lets a pump put a pointer there. */
typedef struct {
    uint32_t events;
    uint32_t reserved;
    uint64_t data;
} os_epoll_event_t;

/* A timer's setting. Nanoseconds, because `struct itimerspec` is
 * nanoseconds and a coarser ABI would have to be widened the day this
 * machine's clock gets finer than 10 ms - see SYS_timerfd_settime for what
 * the hardware actually provides, which is the number that matters. */
typedef struct {
    uint64_t value_ns;    /* when it next fires; 0 means disarmed */
    uint64_t interval_ns; /* 0 means one-shot */
} os_itimer_t;

/* SYS_epoll_create's flags, SYS_eventfd's and SYS_timerfd_create's. The
 * values are Linux's so that one constant means one thing across the ABI
 * (O_CLOEXEC is 0x80000 and O_NONBLOCK is 0x800 there and in
 * <fcntl.h>). */
#define OS_EFD_SEMAPHORE 0x1
#define OS_TFD_ABSTIME   0x1
#define OS_FD_NONBLOCK   0x800
#define OS_FD_CLOEXEC    0x80000

#define OS_CLOCK_REALTIME  0
#define OS_CLOCK_MONOTONIC 1

#ifdef __cplusplus
}
#endif
