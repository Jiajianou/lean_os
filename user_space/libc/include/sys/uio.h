/* user_space/libc/include/sys/uio.h - M89
 *
 * Scattered reads and writes.
 *
 * There is no vectored syscall in this kernel and this does not pretend
 * otherwise: `readv` and `writev` loop over the vector calling read and
 * write. What that costs is atomicity - a real writev delivers the whole
 * vector as one operation, and this one can interleave with another
 * writer between elements. Said here because a program using writev on a
 * pipe for exactly that guarantee is a program this will disappoint, and
 * it should find that out from a header rather than from a garbled log.
 */
#pragma once

#include <sys/types.h>

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

ssize_t readv(int fd, const struct iovec *iov, int count);
ssize_t writev(int fd, const struct iovec *iov, int count);
