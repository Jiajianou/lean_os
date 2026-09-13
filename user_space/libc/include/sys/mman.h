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
int posix_madvise(void *addr, size_t length, int advice);

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
