#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE 0x1
#define EFD_NONBLOCK  0x800
/* O_CLOEXEC and O_NONBLOCK, as Linux defines them. */
#define EFD_CLOEXEC   0x20

int eventfd(unsigned int initval, int flags);

int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#ifdef __cplusplus
}
#endif
