/* user_space/libc/include/sys/epoll.h - M119
 *
 * epoll, over SYS_epoll_create/_ctl/_wait. See
 * [docs/readiness.md](../../../../docs/readiness.md) for what this can and
 * cannot honestly report, and kernel/ipc/epoll.h for what epoll buys on a
 * machine where `poll` over 128 descriptors is already cheap: a set the
 * kernel remembers, a cookie that comes back with the event, and edge and
 * one-shot modes, which a stateless interface cannot express at all.
 *
 * **`struct epoll_event` is NOT glibc's layout and that is deliberate.**
 * Linux packs it to 12 bytes so a 32-bit process and a 64-bit kernel
 * agree; this OS has one word size, no binary compatibility to keep, and a
 * packed 64-bit field is a misaligned load on every access. The struct
 * here is 16 bytes with `data` aligned, which is exactly
 * system_api/include/os_poll.h's os_epoll_event_t - so `epoll_wait` is a
 * copy rather than a translation. Nothing that uses the macros and the
 * struct can tell; something that hard-codes `sizeof(struct epoll_event)
 * == 12` can, and is wrong on every other platform too.
 */
#pragma once

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPOLLIN        0x001u
#define EPOLLPRI       0x002u /* defined, never set - see <poll.h> on POLLPRI for why an unraised flag beats an absent one */
#define EPOLLOUT       0x004u
#define EPOLLERR       0x008u
#define EPOLLHUP       0x010u
#define EPOLLRDNORM    EPOLLIN
#define EPOLLWRNORM    EPOLLOUT
#define EPOLLRDBAND    0x080u
#define EPOLLWRBAND    0x200u
#define EPOLLMSG       0x400u
#define EPOLLRDHUP     0x2000u
#define EPOLLEXCLUSIVE 0x10000000u /* accepted and ignored: one waiter per set here */
#define EPOLLWAKEUP    0x20000000u /* accepted and ignored: nothing here suspends */
#define EPOLLONESHOT   0x40000000u
#define EPOLLET        0x80000000u

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_MOD 2
#define EPOLL_CTL_DEL 3

#define EPOLL_CLOEXEC 0x80000

typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    uint32_t     __reserved; /* named rather than anonymous padding: os_poll.h's struct has it, and a field a program can see is better than a hole it cannot */
    epoll_data_t data;
};

/* `size` is ignored, as it has been on Linux since 2.6.8 - it was a hint
 * about the expected set size and the kernel has sized its own table since.
 * Kept because ported code calls this spelling. */
int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

#ifdef __cplusplus
}
#endif
