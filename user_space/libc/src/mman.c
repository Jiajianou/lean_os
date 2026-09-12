/* user_space/libc/src/mman.c - M78, M91 */
#include <sys/mman.h>
#include <errno.h>

#include "syscall_wrappers.h"

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    /* M91: `addr` is passed through rather than refused. It used to be an
     * error here, and the header explained why - "this kernel places
     * every mapping itself (there is no MAP_FIXED), and a program that
     * asked for a specific address and silently got another one would
     * corrupt itself somewhere far from here." Both halves of that are
     * now false: there is a MAP_FIXED, and a plain hint is honoured when
     * it can be and reported honestly when it cannot, because the address
     * actually mapped is the return value.
     *
     * M91 (second attempt): `fd` and `offset` are passed through too.
     * The paragraph here said "a file-backed mapping is not something
     * this kernel can do, and handing back anonymous zeroes for one
     * would be found out much later and somewhere else" - the second
     * half is why it was a refusal rather than a fiction, and the first
     * half stopped being true. The kernel does the checking now: a
     * descriptor that is not an open file, a sub-page offset, or a
     * shared writable mapping of a read-only descriptor are all refused
     * there, where the fd table is. */
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

/* POSIX spells the advice call posix_madvise and gives it the same
 * meaning; a program written against either finds it here. */
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

/* ---- M120: memfd_create ----------------------------------------------
 *
 * Thin, like the rest of this file. The one thing worth a line: a failure
 * here is EMFILE or EINVAL and not ENOMEM - the object is empty when it is
 * created, so nothing about this call can run out of memory. The
 * ftruncate that sizes it is where ENOMEM lives.
 */
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
        return 0; /* adding nothing succeeds, and must not be read as a query */
    }
    long r = sys_memfd_seal(fd, seals);
    if (r < 0) {
        /* EPERM on Linux: the descriptor is sealed against sealing, or was
         * not created with MFD_ALLOW_SEALING. EINVAL for a seal this kernel
         * does not implement. One return value, and the commoner cause is
         * the permission one. */
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
