#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t name_max;
} os_statvfs_t;

#define OS_FLOCK_RD    0
#define OS_FLOCK_WR    1
#define OS_FLOCK_UNLCK 2

typedef struct {
    int16_t type;
    int16_t whence;
    int32_t pid;
    int64_t start;
    int64_t len;
} os_flock_t;

#ifdef __cplusplus
}
#endif
