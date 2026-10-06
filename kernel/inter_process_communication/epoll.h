#pragma once

#include <stdint.h>

/* Epoll sets on the machine, and descriptors in one set. 32 of each until
   M187: every Chromium thread with an IO or UI message pump owns a set, in
   every process, and the browser's IO thread watches a descriptor per child
   and more - and an EPOLL_CTL_ADD refused because a set is full is a
   descriptor nobody watches, which is a message that arrives and wakes
   nobody. The watch array is inline and sys_epoll_wait copies up to a set's
   worth of events onto its kernel stack, which is what keeps the second
   number where it is. */
#define EPOLL_MAX       1024
#define EPOLL_MAX_WATCH 128

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
void epoll_reference(struct epoll *ep);
void epoll_unref(struct epoll *ep);

int epoll_control_set(struct epoll *ep, int op, int fd, const void *object,
                  uint32_t events, uint64_t data);

int epoll_watch_count(const struct epoll *ep);
int epoll_objects(struct epoll *ep, int *fds, const void **objects, int max);
int epoll_registered(struct epoll *ep, int *fds, const void **objects, int max);

#define EPOLL_STALE 0xFFFFFFFFu
typedef uint32_t (*epoll_mask_function)(void *context, int fd, const void *object);

int epoll_scan(struct epoll *ep, epoll_mask_function mask_function, void *context,
               epoll_ev_t *out, int max);

int epoll_peek(struct epoll *ep, epoll_mask_function mask_function, void *context);

/* How deep sets may be put inside sets - Linux's EP_MAX_NESTS. A deeper or
   circular registration is refused with ELOOP, as Linux refuses it. */
#define EPOLL_MAX_NESTING 4

int epoll_in_use(void);
