#pragma once

#include "leanfs.h"

#include <stddef.h>
#include <stdint.h>

#define LEANFS_FNV1A_INIT 0x811C9DC5u

static inline uint32_t leanfs_fnv1a(uint32_t h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

#define LEANFS_MAGIC     0x3553464Cu
#define LEANFS_VERSION   5u

#define LEANFS_TYPE_FREE 0
#define LEANFS_TYPE_FILE 1
#define LEANFS_TYPE_DIR  2
#define LEANFS_TYPE_LINK 3

#define ROOT_INODE 0

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t inode_table_block;
    uint32_t inode_table_blocks;
    uint32_t bitmap_block;
    uint32_t bitmap_blocks_field;
    uint32_t data_block;
    uint32_t data_blocks;
    uint32_t state;
    uint32_t version;
} leanfs_superblock_t;

#define LEANFS_STATE_CLEAN 0u
#define LEANFS_STATE_DIRTY 0x4449525Au

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
    uint32_t mtime;
    uint32_t direct[LEANFS_DIRECT_BLOCKS];
    uint32_t indirect;
    uint32_t dindirect;
    uint32_t nlink;
    uint8_t  reserved[40];
} leanfs_inode_t;

_Static_assert(sizeof(leanfs_inode_t) == 128, "an inode must be 128 bytes so four fit a sector exactly");
_Static_assert(LEANFS_BLOCK_SIZE % sizeof(leanfs_inode_t) == 0, "an inode must not straddle a sector");

#define INODE_TABLE_BLOCKS ((sizeof(leanfs_inode_t) * LEANFS_MAX_INODES + LEANFS_BLOCK_SIZE - 1) / LEANFS_BLOCK_SIZE)
#define BITMAP_BLOCKS       (LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE)

_Static_assert(sizeof(leanfs_dirent_t) == LEANFS_DIRENT_HDR, "leanfs_dirent_t header must stay 8 bytes");
_Static_assert(LEANFS_DIRENT_NEED(LEANFS_MAX_NAME) <= LEANFS_BLOCK_SIZE, "the longest name must still fit in one block");
