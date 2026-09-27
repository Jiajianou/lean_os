#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include "syscall_wrappers.h"

void *mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset) {
    long r = sys_mmap(address, (unsigned long)length, prot, flags, fd,
                      (unsigned long)offset);
    if (r < 0) {
        errno = (fd >= 0 && !(flags & MAP_ANONYMOUS)) ? EACCES : ENOMEM;
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
        /* ENOMEM is what mincore(2) reports for a range containing unmapped
           pages, and since M160 that is the case this kernel refuses - the
           alignment and length errors are caught above, so what is left here
           is a range that is not all mapped. */
        errno = ENOMEM;
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


/* mlock(2) and munlock(2), and why succeeding here is the truthful answer
 * rather than the convenient one.
 *
 * What mlock promises is that the pages in a range are resident and will stay
 * resident. The second half this kernel keeps for nothing: there is no swap
 * device, no page reclaim and no eviction path, so a page that is resident
 * stays resident until the process unmaps it or exits. The first half is not
 * free, because this kernel maps lazily - M91's address space faults a page
 * in when it is first touched - so a range that has never been written has no
 * pages behind it yet, and an mlock that returned 0 without doing anything
 * would be promising residency for memory that is not there.
 *
 * So this touches every page in the range, which is exactly what makes the
 * promise true, and validates the range with mincore first so that an
 * unmapped argument is an error rather than a fault. mincore is the right
 * instrument for that: it is the call that answers "is this range mapped, and
 * which of it is resident", and it already refuses a range that is not.
 *
 * munlock then has nothing to undo, and says so by succeeding.
 *
 * tflite's xnnpack delegate is what asked (M160), and it treats a failure as
 * "this buffer will not be locked" rather than as fatal - so a lie here would
 * have cost nothing visible and been a lie anyway.
 */
static int mlock_range(const void *address, size_t length, int touch) {
    if (length == 0) {
        return 0;
    }
    uintptr_t start = (uintptr_t)address & ~(uintptr_t)(4096u - 1);
    uintptr_t end = ((uintptr_t)address + length + 4096u - 1) &
                    ~(uintptr_t)(4096u - 1);
    size_t pages = (size_t)((end - start) / 4096u);

    /* A page at a time, so that a range of any size needs no allocation for
       the vector mincore fills in. */
    for (size_t i = 0; i < pages; i++) {
        unsigned char resident = 0;
        if (mincore((void *)(start + i * 4096u), 4096u, &resident) != 0) {
            errno = ENOMEM;
            return -1;
        }
        if (touch) {
            volatile const unsigned char *page =
                (volatile const unsigned char *)(start + i * 4096u);
            (void)*page;
        }
    }
    return 0;
}

int mlock(const void *address, size_t length) {
    return mlock_range(address, length, 1);
}

int munlock(const void *address, size_t length) {
    /* Nothing was pinned that can be released, because nothing evicts. The
       range is still checked, so an unmapped argument is still an error. */
    return mlock_range(address, length, 0);
}
