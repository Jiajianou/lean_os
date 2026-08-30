/* user_space/libc/src/mman.c - M78 */
#include <sys/mman.h>

#include "syscall_wrappers.h"

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    /* Refused rather than ignored - see the note in <sys/mman.h>. This
     * kernel places every mapping itself (there is no MAP_FIXED), and a
     * program that asked for a specific address and silently got another
     * one would corrupt itself somewhere far from here. */
    if (addr != 0 || fd != -1 || offset != 0) {
        return MAP_FAILED;
    }
    long r = sys_mmap((unsigned long)length, prot, flags);
    if (r < 0) {
        return MAP_FAILED;
    }
    return (void *)(unsigned long)r;
}

int munmap(void *addr, size_t length) {
    return (int)sys_munmap(addr, (unsigned long)length);
}
