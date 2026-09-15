#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

#define POLLPRI    0x002
#define POLLRDNORM POLLIN
#define POLLWRNORM POLLOUT
#define POLLRDBAND 0x080
#define POLLWRBAND 0x100

/* The peer closed its half and this end can still write. Reported where the
   kernel knows a stream has two directions - a socket or a pipe - and never
   guessed at for anything else. */
#define POLLRDHUP  0x2000

typedef unsigned int nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
}
#endif
