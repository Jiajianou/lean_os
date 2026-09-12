/* user_space/libc/include/sys/eventfd.h - M119
 *
 * A counter you can wait on. See kernel/ipc/eventfd.h for why it exists
 * beside SYS_pipe, which could carry the same wake-up: a pipe costs two
 * descriptors and 4 KiB to carry one bit, and a wake-up written more often
 * than it is read fills that buffer and then blocks the waker.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE 0x1
#define EFD_NONBLOCK  0x800
#define EFD_CLOEXEC   0x80000

int eventfd(unsigned int initval, int flags);

/* The two helpers glibc added, and the reason they exist is worth knowing:
 * an eventfd is read and written in units of eight bytes, so every caller
 * was writing the same `read(fd, &v, 8) != 8` by hand. */
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#ifdef __cplusplus
}
#endif
