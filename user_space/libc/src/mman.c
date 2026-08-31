/* user_space/libc/src/mman.c - M78, M91 */
#include <sys/mman.h>

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
     * `fd`/`offset` stay refused, and for the unchanged reason: a
     * file-backed mapping is not something this kernel can do, and
     * handing back anonymous zeroes for one would be found out much later
     * and somewhere else. */
    if (fd != -1 || offset != 0) {
        return MAP_FAILED;
    }
    long r = sys_mmap(addr, (unsigned long)length, prot, flags, fd,
                      (unsigned long)offset);
    if (r < 0) {
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
