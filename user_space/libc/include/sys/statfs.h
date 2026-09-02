/* user_space/libc/include/sys/statfs.h - M89
 *
 * `statfs`, which is Linux's older spelling of <sys/statvfs.h> and is
 * not the same struct.
 *
 * Both exist here because programs use both, and the difference that
 * matters is `f_type`: a magic number identifying the filesystem, which
 * statvfs has no field for. leanfs's is its own superblock magic, so a
 * program printing the filesystem type gets an answer that is true and
 * unique rather than one borrowed from a Linux filesystem this is not.
 *
 * The block counts are the same numbers statvfs reports, from the same
 * syscall - see <sys/statvfs.h> for why f_bfree and f_bavail are equal
 * on a machine with one principal.
 */
#pragma once

#include <stdint.h>
#include <sys/types.h>

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

/* leanfs's own superblock magic (kernel/fs/leanfs_format.h). Not one of
 * Linux's magic numbers, deliberately: claiming to be ext2 would make a
 * program take ext2's path. */
#define LEANFS_SUPER_MAGIC 0x4C45414EU /* "LEAN" */

typedef struct {
    long val[2];
} fsid_t;

struct statfs {
    unsigned long f_type;
    unsigned long f_bsize;
    uint64_t      f_blocks;
    uint64_t      f_bfree;
    uint64_t      f_bavail;
    uint64_t      f_files;
    uint64_t      f_ffree;
    fsid_t        f_fsid;
    unsigned long f_namelen;
    unsigned long f_frsize;
    unsigned long f_flags;
    unsigned long f_spare[4];
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#ifdef __cplusplus
}
#endif
