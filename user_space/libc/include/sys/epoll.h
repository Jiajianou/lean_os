#pragma once

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPOLLIN        0x001u
#define EPOLLPRI       0x002u
#define EPOLLOUT       0x004u
#define EPOLLERR       0x008u
#define EPOLLHUP       0x010u
#define EPOLLRDNORM    EPOLLIN
#define EPOLLWRNORM    EPOLLOUT
#define EPOLLRDBAND    0x080u
#define EPOLLWRBAND    0x200u
#define EPOLLMSG       0x400u
#define EPOLLRDHUP     0x2000u
#define EPOLLEXCLUSIVE 0x10000000u
#define EPOLLWAKEUP    0x20000000u
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
    uint32_t     __reserved;
    epoll_data_t data;
};

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

#ifdef __cplusplus
}
#endif
