#pragma once

#include <sys/time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FD_SETSIZE 128

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define __FD_WORD(fd) ((unsigned)(fd) / (8 * sizeof(unsigned long)))
#define __FD_BIT(fd)  (1UL << ((unsigned)(fd) % (8 * sizeof(unsigned long))))

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

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);

#ifdef __cplusplus
}
#endif
