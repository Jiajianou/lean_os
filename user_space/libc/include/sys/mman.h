/* user_space/libc/include/sys/mman.h - M78
 *
 * The POSIX spelling of SYS_mmap/SYS_munmap. The flags themselves come
 * from system_api/include/mman.h, which both the kernel and this file
 * include, so there is exactly one definition of what each bit means.
 *
 * M91: `addr` is honoured. It used to be refused, and the reason given
 * was that a program asking for a particular address and silently
 * getting another one corrupts itself far from here - which was the
 * right call while there was no MAP_FIXED. There is one now: a plain
 * `addr` is a hint and the return value says where the mapping actually
 * went; with MAP_FIXED it is a requirement and failure is MAP_FAILED.
 *
 * `fd`/`offset` are still checked rather than ignored - an fd that is not
 * -1 is an error, not a hint quietly dropped, because there are no
 * file-backed mappings here and anonymous zeroes are not a file.
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

/* M91. mprotect's range must lie entirely inside mappings the caller
 * holds - a partially-covered range is -1 rather than half applied.
 * madvise honours MADV_DONTNEED (drop the pages, keep the mapping) and
 * accepts every other advice value as a no-op success. */
int mprotect(void *addr, size_t length, int prot);
int madvise(void *addr, size_t length, int advice);
int posix_madvise(void *addr, size_t length, int advice);
