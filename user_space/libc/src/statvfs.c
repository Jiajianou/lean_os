#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <string.h>

#include "os_fs.h"
#include "syscall_wrappers.h"

int statvfs(const char *path, struct statvfs *buf) {
    if (!path || !buf) {
        errno = EFAULT;
        return -1;
    }
    os_statvfs_t st;
    if (sys_statvfs(path, &st) != 0) {
        errno = ENOENT;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = st.block_size;
    buf->f_frsize = st.block_size;
    buf->f_blocks = st.total_blocks;
    buf->f_bfree = st.free_blocks;
    buf->f_bavail = st.free_blocks;
    buf->f_files = st.total_inodes;
    buf->f_ffree = st.free_inodes;
    buf->f_favail = st.free_inodes;
    buf->f_namemax = st.name_max;
    return 0;
}

int statfs(const char *path, struct statfs *buf) {
    if (!path || !buf) {
        errno = EFAULT;
        return -1;
    }
    os_statvfs_t st;
    if (sys_statvfs(path, &st) != 0) {
        errno = ENOENT;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->f_type = LEANFS_SUPER_MAGIC;
    buf->f_bsize = st.block_size;
    buf->f_frsize = st.block_size;
    buf->f_blocks = st.total_blocks;
    buf->f_bfree = st.free_blocks;
    buf->f_bavail = st.free_blocks;
    buf->f_files = st.total_inodes;
    buf->f_ffree = st.free_inodes;
    buf->f_namelen = st.name_max;
    return 0;
}

int fstatfs(int fd, struct statfs *buf) {
    (void)fd;
    (void)buf;
    errno = ENOSYS;
    return -1;
}

int fstatvfs(int fd, struct statvfs *buf) {
    (void)fd;
    (void)buf;
    errno = ENOSYS;
    return -1;
}
