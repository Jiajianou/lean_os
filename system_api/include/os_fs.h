/* system_api/include/os_fs.h - M88
 *
 * What SYS_statvfs reports.
 *
 * Its own header rather than a struct bolted onto os_time.h, which is
 * where os_stat_t lives for the historical reason that a file's mtime
 * was the first thing this machine ever learned to report about one.
 * "How full is the disk" is not a question about time and putting it
 * there would make the next person look for it in the wrong file.
 */
#pragma once

#include <stdint.h>

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

/* Blocks and inodes, both, because this filesystem has two ceilings and
 * a caller watching only the first would watch the wrong one:
 * LEANFS_MAX_INODES is fixed at format time, so a source tree of small
 * files runs out of inodes with most of the disk still free.
 *
 * uint32_t throughout, matching leanfs's own on-disk widths. A 4 GiB
 * ceiling on the block count at LEANFS_BLOCK_SIZE is 16 TiB of
 * filesystem, which is not the limit anything here will meet first.
 */
typedef struct {
    uint32_t block_size;   /* bytes in one block - the unit the other counts are in */
    uint32_t total_blocks; /* data blocks the filesystem has, excluding its own metadata */
    uint32_t free_blocks;  /* of those, how many are unallocated right now */
    uint32_t total_inodes; /* files and directories this filesystem can hold at once */
    uint32_t free_inodes;
    uint32_t name_max;     /* longest single path component, not counting the NUL */
} os_statvfs_t;

/* ---- M100: a record lock, as fcntl's three lock commands see it --------
 *
 * The same shape as <fcntl.h>'s struct flock and the same values for
 * the three types (F_RDLCK 0, F_WRLCK 1, F_UNLCK 2 - one encoding, on
 * purpose, so libc copies fields rather than translating them). Fixed
 * widths because this crosses the kernel boundary. `whence` is
 * SEEK_SET/SEEK_CUR/SEEK_END on the way in and resolved by the kernel,
 * which is the only party that knows a descriptor's offset; on the way
 * out of F_GETLK it is always SEEK_SET. `len` 0 means to end of file,
 * and a negative one means the `len` bytes BEFORE `start`, both as
 * POSIX has it. See kernel/fs/flock.h for who asked. */
#define OS_FLOCK_RD    0
#define OS_FLOCK_WR    1
#define OS_FLOCK_UNLCK 2

typedef struct {
    int16_t type;   /* OS_FLOCK_RD / OS_FLOCK_WR / OS_FLOCK_UNLCK */
    int16_t whence;
    int32_t pid;    /* F_GETLK: who holds the conflicting lock */
    int64_t start;
    int64_t len;
} os_flock_t;

#ifdef __cplusplus
}
#endif
