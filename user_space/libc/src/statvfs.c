/* user_space/libc/src/statvfs.c - M88
 *
 * `statvfs`, over SYS_statvfs. See <sys/statvfs.h> for why f_files and
 * f_ffree are real numbers here rather than the zeros they usually are.
 */
#include <sys/statvfs.h>
#include <errno.h>
#include <string.h>

#include "os_fs.h" /* system_api/include/os_fs.h - os_statvfs_t */
#include "syscall_wrappers.h"

int statvfs(const char *path, struct statvfs *buf) {
    if (!path || !buf) {
        errno = EFAULT;
        return -1;
    }
    os_statvfs_t st;
    if (sys_statvfs(path, &st) != 0) {
        /* Two causes and one errno, which is the honest limit of what the
         * syscall reports: the path does not exist, or it is under a
         * synthetic mount that has no blocks to count. ENOENT is the
         * overwhelmingly common one and the one a caller acts on. */
        errno = ENOENT;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = st.block_size;
    buf->f_frsize = st.block_size; /* leanfs has no fragments - one size */
    buf->f_blocks = st.total_blocks;
    buf->f_bfree = st.free_blocks;
    buf->f_bavail = st.free_blocks; /* no reserved pool: there is no second principal to reserve it for */
    buf->f_files = st.total_inodes;
    buf->f_ffree = st.free_inodes;
    buf->f_favail = st.free_inodes;
    buf->f_namemax = st.name_max;
    return 0;
}

/* Refused rather than guessed.
 *
 * SYS_statvfs takes a path because that is what selects a mount, and
 * this libc does not remember the path a descriptor was opened with -
 * SYS_fstat answers about the open file itself, which is a different
 * question. Answering with the root filesystem's numbers would be right
 * for every descriptor today and wrong for the first one opened under
 * /proc, and a call that is right by coincidence is the kind this
 * project has been bitten by before. ENOSYS says what is true: this
 * function is not implemented here.
 *
 * If something ports that needs it, the fix is a descriptor-to-mount
 * lookup in the kernel rather than an inference in libc. */
int fstatvfs(int fd, struct statvfs *buf) {
    (void)fd;
    (void)buf;
    errno = ENOSYS;
    return -1;
}
