#pragma once

#include <stddef.h>
#include <sys/types.h>

#include <mman.h>

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);

int msync(void *addr, size_t length, int flags);

int mprotect(void *addr, size_t length, int prot);
int madvise(void *addr, size_t length, int advice);
int mincore(void *addr, size_t length, unsigned char *vector);

/* M160. mlock's promise is residency, and this kernel keeps the hard half of
   it for nothing - there is no swap and no reclaim, so nothing is ever
   evicted. See user_space/libc/src/mman.c for why the easy half still costs
   a touch of every page. */
int mlock(const void *address, size_t length);
int munlock(const void *address, size_t length);
int posix_madvise(void *addr, size_t length, int advice);

/* BSD's spelling of MAP_ANONYMOUS, which portable code reaches for first
   and Linux also defines. The same bit, not a second kind of mapping. */
#define MAP_ANON MAP_ANONYMOUS

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
