/* user_space/libc/include/sys/mman.h - M78
 *
 * The POSIX spelling of SYS_mmap/SYS_munmap. The flags themselves come
 * from system_api/include/mman.h, which both the kernel and this file
 * include, so there is exactly one definition of what each bit means.
 *
 * `addr` and `fd`/`offset` are in the signature because a program
 * written elsewhere passes them, and they are checked rather than
 * ignored: a non-NULL `addr` or an `fd` that is not -1 is an error, not
 * a hint quietly dropped. A program that asked for a mapping at a
 * particular address and got one somewhere else would corrupt itself in
 * a way that is very hard to trace back to here.
 */
#pragma once

#include <stddef.h>
#include <sys/types.h>

/* Angle brackets, not quotes, and that is load-bearing: a quoted include
 * searches this file's own directory first, where the only mman.h is
 * this one - so `#include "mman.h"` here resolves to itself and defines
 * nothing. Angle brackets skip that step and find
 * system_api/include/mman.h, which is the file that owns these flags. */
#include <mman.h>

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
