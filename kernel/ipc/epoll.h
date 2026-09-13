#pragma once

#include <stdint.h>

#define EPOLL_MAX       32
#define EPOLL_MAX_WATCH 32

#define EPOLLIN        0x001u
#define EPOLLPRI       0x002u
#define EPOLLOUT       0x004u
#define EPOLLERR       0x008u
#define EPOLLHUP       0x010u
#define EPOLLRDHUP     0x2000u
#define EPOLLEXCLUSIVE 0x10000000u
#define EPOLLWAKEUP    0x20000000u
#define EPOLLONESHOT   0x40000000u
#define EPOLLET        0x80000000u

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_MOD 2
#define EPOLL_CTL_DEL 3

struct epoll;

typedef struct {
    uint32_t events;
    uint32_t reserved;
    uint64_t data;
} epoll_ev_t;

void epoll_init(void);

struct epoll *epoll_create_set(void);
void epoll_ref(struct epoll *ep);
void epoll_unref(struct epoll *ep);

int epoll_ctl_set(struct epoll *ep, int op, int fd, const void *obj,
                  uint32_t events, uint64_t data);

int epoll_watch_count(const struct epoll *ep);

#define EPOLL_STALE 0xFFFFFFFFu
typedef uint32_t (*epoll_mask_fn)(void *ctx, int fd, const void *obj);

int epoll_scan(struct epoll *ep, epoll_mask_fn mask_fn, void *ctx,
               epoll_ev_t *out, int max);

int epoll_in_use(void);
