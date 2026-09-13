#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <string.h>

#include "os_file_system.h"
#include "syscall_wrappers.h"

int statvfs(const char *path, struct statvfs *buffer) {
    if (!path || !buffer) {
        errno = EFAULT;
        return -1;
    }
    os_statvfs_t st;
    if (sys_statvfs(path, &st) != 0) {
        errno = ENOENT;
        return -1;
    }
    memset(buffer, 0, sizeof(*buffer));
    buffer->f_bsize = st.block_size;
    buffer->f_frsize = st.block_size;
    buffer->f_blocks = st.total_blocks;
    buffer->f_bfree = st.free_blocks;
    buffer->f_bavail = st.free_blocks;
    buffer->f_files = st.total_inodes;
    buffer->f_ffree = st.free_inodes;
    buffer->f_favail = st.free_inodes;
    buffer->f_namemax = st.name_max;
    return 0;
}

int statfs(const char *path, struct statfs *buffer) {
    if (!path || !buffer) {
        errno = EFAULT;
        return -1;
    }
    os_statvfs_t st;
    if (sys_statvfs(path, &st) != 0) {
        errno = ENOENT;
        return -1;
    }
    memset(buffer, 0, sizeof(*buffer));
    buffer->f_type = LEANFS_SUPER_MAGIC;
    buffer->f_bsize = st.block_size;
    buffer->f_frsize = st.block_size;
    buffer->f_blocks = st.total_blocks;
    buffer->f_bfree = st.free_blocks;
    buffer->f_bavail = st.free_blocks;
    buffer->f_files = st.total_inodes;
    buffer->f_ffree = st.free_inodes;
    buffer->f_namelen = st.name_max;
    return 0;
}

int fstatfs(int fd, struct statfs *buffer) {
    (void)fd;
    (void)buffer;
    errno = ENOSYS;
    return -1;
}

int fstatvfs(int fd, struct statvfs *buffer) {
    (void)fd;
    (void)buffer;
    errno = ENOSYS;
    return -1;
}
