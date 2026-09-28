#pragma once

#include "leanfs.h"

#include <stddef.h>
#include <stdint.h>

#define LEANFS_FNV1A_INIT 0x811C9DC5u

static inline uint32_t leanfs_fnv1a(uint32_t h, const void *data, size_t length) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < length; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

#define LEANFS_DISK_LABEL_OFFSET 0x1A0u
#define LEANFS_DISK_LABEL_LENGTH 8u

static inline int leanfs_disk_carries_this_os(const uint8_t *first_block) {
    static const char label[LEANFS_DISK_LABEL_LENGTH] = {'L', 'E', 'A', 'N', '_', 'O', 'S', '1'};
    for (uint32_t i = 0; i < LEANFS_DISK_LABEL_LENGTH; i++) {
        if (first_block[LEANFS_DISK_LABEL_OFFSET + i] != (uint8_t)label[i]) {
            return 0;
        }
    }
    return first_block[510] == 0x55 && first_block[511] == 0xAA;
}

#define LEANFS_MAGIC     0x3553464Cu
#define LEANFS_VERSION   5u

#define LEANFS_TYPE_FREE 0
#define LEANFS_TYPE_FILE 1
#define LEANFS_TYPE_DIRECTORY  2
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

_Static_assert(sizeof(leanfs_dirent_t) == LEANFS_DIRENT_HEADER, "leanfs_dirent_t header must stay 8 bytes");
_Static_assert(LEANFS_DIRENT_NEED(LEANFS_MAX_NAME) <= LEANFS_BLOCK_SIZE, "the longest name must still fit in one block");

/* M194: the journal. It sits on the disk directly after the filesystem, and
   the ESP moved out by its size. Everything leanfs writes goes into a
   transaction in memory; a commit writes the transaction here as one
   sequential run - descriptor blocks naming where each block belongs, the
   blocks, then a commit block whose checksum covers them - and only later
   does a checkpoint write the blocks home. A mount replays every committed
   transaction the header's start points at, so what reaches the disk is
   always a state that existed between two operations, whatever the power
   does. A journal area of zeros is an empty journal, which is what every
   image the build writes starts with. */
#define LEANFS_JOURNAL_BLOCKS 8192u
#define LEANFS_JOURNAL_VERSION 1u
#define LEANFS_JOURNAL_HEADER_MAGIC 0x4C4E524Au
#define LEANFS_JOURNAL_DESCRIPTOR_MAGIC 0x5344524Au
#define LEANFS_JOURNAL_COMMIT_MAGIC 0x4D43524Au
#define LEANFS_JOURNAL_TARGETS_PER_DESCRIPTOR 1018u

#define LEANFS_TOTAL_BLOCKS (1u + INODE_TABLE_BLOCKS + BITMAP_BLOCKS + LEANFS_DATA_BLOCKS)
#define LEANFS_JOURNAL_START_LBA (LEANFS_START_LBA + LEANFS_TOTAL_BLOCKS * LEANFS_SECTORS_PER_BLOCK)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t start_block;
    uint32_t journal_blocks;
    uint64_t start_sequence;
    uint32_t checksum;
} leanfs_journal_header_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t index;
    uint64_t sequence;
    uint32_t count;
    uint32_t reserved;
    uint32_t targets[LEANFS_JOURNAL_TARGETS_PER_DESCRIPTOR];
} leanfs_journal_descriptor_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t count;
    uint64_t sequence;
    uint32_t checksum;
} leanfs_journal_commit_t;

_Static_assert(sizeof(leanfs_journal_descriptor_t) == LEANFS_BLOCK_SIZE,
               "a journal descriptor is exactly one block");

static inline uint32_t leanfs_journal_descriptor_blocks(uint32_t count) {
    return (count + LEANFS_JOURNAL_TARGETS_PER_DESCRIPTOR - 1) / LEANFS_JOURNAL_TARGETS_PER_DESCRIPTOR;
}

static inline uint32_t leanfs_journal_header_checksum(const leanfs_journal_header_t *h) {
    return leanfs_fnv1a(LEANFS_FNV1A_INIT, h, offsetof(leanfs_journal_header_t, checksum));
}
