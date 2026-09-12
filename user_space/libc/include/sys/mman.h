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

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);

/* M91 (second attempt): write a shared file mapping's pages back to the
 * file. A private or anonymous mapping is a success that does nothing -
 * there is no file for its contents to reach. See SYS_msync for why
 * MS_ASYNC and MS_SYNC do the same thing here and why MS_INVALIDATE is
 * refused rather than accepted. */
int msync(void *addr, size_t length, int flags);

/* M91. mprotect's range must lie entirely inside mappings the caller
 * holds - a partially-covered range is -1 rather than half applied.
 * madvise honours MADV_DONTNEED (drop the pages, keep the mapping) and
 * accepts every other advice value as a no-op success. */
int mprotect(void *addr, size_t length, int prot);
int madvise(void *addr, size_t length, int advice);
int posix_madvise(void *addr, size_t length, int advice);

/* ---- M120: anonymous shared memory a descriptor names -----------------
 *
 * `memfd_create` then `ftruncate` then `mmap(MAP_SHARED)`, and the
 * descriptor can be passed to another process over a Unix-domain socket
 * (M118). That is how every program that shares a buffer with a separate
 * process does it - a browser's renderer and its GPU process, a video
 * decoder and its consumer - and it is the third of the three pieces
 * docs/browser.md's measurement named.
 *
 * `read(2)` and `write(2)` on one are refused here, which Linux allows.
 * Nothing that uses shared memory does it, and a second path to the same
 * bytes with different rules is worth less than the refusal. `fstat`
 * reports the size, which is what a receiver checks before mapping.
 *
 * Seals are in <fcntl.h> terms on Linux (F_ADD_SEALS / F_GET_SEALS through
 * fcntl); here they are one call, because the kernel's fcntl has no
 * argument shape for them and inventing one to imitate a spelling is not
 * worth a syscall. `memfd_seals(fd)` asks; `memfd_add_seals(fd, s)`
 * promises. MFD_ALLOW_SEALING is required for the second, as on Linux. */
#define MFD_CLOEXEC       0x0001
#define MFD_ALLOW_SEALING 0x0002

#define F_SEAL_SEAL   0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW   0x0004
#define F_SEAL_WRITE  0x0008

int memfd_create(const char *name, unsigned int flags);
int memfd_add_seals(int fd, unsigned int seals);
int memfd_seals(int fd);

#ifdef __cplusplus
}
#endif
