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

#ifdef __cplusplus
}
#endif
