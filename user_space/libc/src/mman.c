#include <sys/mman.h>
#include <errno.h>

#include "syscall_wrappers.h"

void *mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset) {
    long r = sys_mmap(address, (unsigned long)length, prot, flags, fd,
                      (unsigned long)offset);
    if (r < 0) {
        errno = (fd >= 0) ? EACCES : ENOMEM;
        return MAP_FAILED;
    }
    return (void *)(unsigned long)r;
}

int munmap(void *address, size_t length) {
    return (int)sys_munmap(address, (unsigned long)length);
}

int mprotect(void *address, size_t length, int prot) {
    return (int)sys_mprotect(address, (unsigned long)length, prot);
}

int mincore(void *address, size_t length, unsigned char *vector) {
    if (!vector) {
        errno = EFAULT;
        return -1;
    }
    if (sys_mincore(address, length, vector) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int madvise(void *address, size_t length, int advice) {
    return (int)sys_madvise(address, (unsigned long)length, advice);
}

int posix_madvise(void *address, size_t length, int advice) {
    return madvise(address, length, advice);
}

int msync(void *address, size_t length, int flags) {
    if (sys_msync(address, length, flags) != 0) {
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
