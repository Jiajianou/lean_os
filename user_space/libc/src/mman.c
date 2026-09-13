#include <sys/mman.h>
#include <errno.h>

#include "syscall_wrappers.h"

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    long r = sys_mmap(addr, (unsigned long)length, prot, flags, fd,
                      (unsigned long)offset);
    if (r < 0) {
        errno = (fd >= 0) ? EACCES : ENOMEM;
        return MAP_FAILED;
    }
    return (void *)(unsigned long)r;
}

int munmap(void *addr, size_t length) {
    return (int)sys_munmap(addr, (unsigned long)length);
}

int mprotect(void *addr, size_t length, int prot) {
    return (int)sys_mprotect(addr, (unsigned long)length, prot);
}

int madvise(void *addr, size_t length, int advice) {
    return (int)sys_madvise(addr, (unsigned long)length, advice);
}

int posix_madvise(void *addr, size_t length, int advice) {
    return madvise(addr, length, advice);
}

int msync(void *addr, size_t length, int flags) {
    if (sys_msync(addr, length, flags) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int memfd_create(const char *name, unsigned int flags) {
    long fd = sys_memfd_create(name, (int)flags);
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return (int)fd;
}

int memfd_add_seals(int fd, unsigned int seals) {
    if (seals == 0) {
        return 0;
    }
    long r = sys_memfd_seal(fd, seals);
    if (r < 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int memfd_seals(int fd) {
    long r = sys_memfd_seal(fd, 0);
    if (r < 0) {
        errno = EINVAL;
        return -1;
    }
    return (int)r;
}
