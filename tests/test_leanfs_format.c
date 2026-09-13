#include "check.h"
#include "file_system/leanfs_format.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(leanfs_superblock_t) == 36,
               "the superblock is nine little-endian uint32s");
_Static_assert(offsetof(leanfs_superblock_t, magic) == 0, "magic must be first");
_Static_assert(offsetof(leanfs_superblock_t, inode_table_block) == 4, "");
_Static_assert(offsetof(leanfs_superblock_t, inode_table_blocks) == 8, "");
_Static_assert(offsetof(leanfs_superblock_t, bitmap_block) == 12, "");
_Static_assert(offsetof(leanfs_superblock_t, bitmap_blocks_field) == 16, "");
_Static_assert(offsetof(leanfs_superblock_t, data_block) == 20, "");
_Static_assert(offsetof(leanfs_superblock_t, data_blocks) == 24, "");
_Static_assert(offsetof(leanfs_superblock_t, state) == 28, "");
_Static_assert(offsetof(leanfs_superblock_t, version) == 32, "");

_Static_assert(sizeof(leanfs_inode_t) == 128, "four inodes per sector, exactly");
_Static_assert(offsetof(leanfs_inode_t, type) == 0, "");
_Static_assert(offsetof(leanfs_inode_t, size) == 4, "");
_Static_assert(offsetof(leanfs_inode_t, direct) == 12, "");

_Static_assert(sizeof(leanfs_dirent_t) == LEANFS_DIRENT_HDR,
               "the directory record header is eight bytes");
_Static_assert(offsetof(leanfs_dirent_t, inode) == 0, "");
_Static_assert(offsetof(leanfs_dirent_t, rec_length) == 4, "");
_Static_assert(offsetof(leanfs_dirent_t, name_len) == 6, "");
_Static_assert(offsetof(leanfs_dirent_t, type) == 7, "");

_Static_assert(LEANFS_BLOCK_SIZE == 4096, "M93's block size");
_Static_assert(LEANFS_SECTORS_PER_BLOCK == 8, "a block is eight 512-byte sectors");
_Static_assert(LEANFS_START_LBA % LEANFS_SECTORS_PER_BLOCK == 0,
               "the filesystem must start on a block boundary");
_Static_assert(LEANFS_START_BLOCK * LEANFS_SECTORS_PER_BLOCK == LEANFS_START_LBA,
               "the two ways of naming the start must agree");
_Static_assert((sizeof(leanfs_inode_t) * LEANFS_MAX_INODES) % LEANFS_BLOCK_SIZE == 0,
               "the inode table must be a whole number of blocks");
_Static_assert(LEANFS_DATA_BLOCKS % (8 * LEANFS_BLOCK_SIZE) == 0,
               "the bitmap must be a whole number of blocks");

TEST(leanfs_format, the_magic_and_version_are_this_build_s) {
    CHECK_EQ(LEANFS_MAGIC, 0x3553464Cu);
    CHECK_EQ(LEANFS_VERSION, 5u);
    CHECK_EQ((LEANFS_MAGIC >> 0) & 0xFF, 'L');
    CHECK_EQ((LEANFS_MAGIC >> 8) & 0xFF, 'F');
    CHECK_EQ((LEANFS_MAGIC >> 16) & 0xFF, 'S');
    CHECK_EQ((LEANFS_MAGIC >> 24) & 0xFF, '5');
}

TEST(leanfs_format, the_dirty_marker_is_not_a_value_a_blank_disk_produces) {
    CHECK_EQ(LEANFS_STATE_CLEAN, 0u);
    CHECK_NE(LEANFS_STATE_DIRTY, 0u);
}

TEST(leanfs_format, the_layout_leaves_no_gaps_and_no_overlaps) {
    uint32_t sb_block = LEANFS_START_BLOCK;
    uint32_t inode_table = sb_block + 1;
    uint32_t bitmap = inode_table + (uint32_t)INODE_TABLE_BLOCKS;
    uint32_t data = bitmap + (uint32_t)BITMAP_BLOCKS;

    CHECK_EQ(INODE_TABLE_BLOCKS,
             (sizeof(leanfs_inode_t) * LEANFS_MAX_INODES) / LEANFS_BLOCK_SIZE);
    CHECK_EQ(BITMAP_BLOCKS, LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE);
    CHECK_EQ((uint64_t)BITMAP_BLOCKS * LEANFS_BLOCK_SIZE * 8, LEANFS_DATA_BLOCKS);
    CHECK(data > bitmap);
}

TEST(leanfs_format, a_directory_record_never_straddles_a_block) {
    CHECK(LEANFS_DIRENT_NEED(LEANFS_MAX_NAME) <= LEANFS_BLOCK_SIZE);
    CHECK_EQ(LEANFS_DIRENT_NEED(0), LEANFS_DIRENT_HDR);
    CHECK_EQ(LEANFS_DIRENT_NEED(1), LEANFS_DIRENT_HDR + LEANFS_DIRENT_ALIGN);
    CHECK_EQ(LEANFS_DIRENT_NEED(4), LEANFS_DIRENT_HDR + LEANFS_DIRENT_ALIGN);
    CHECK_EQ(LEANFS_DIRENT_NEED(5), LEANFS_DIRENT_HDR + 2 * LEANFS_DIRENT_ALIGN);
    for (unsigned n = 0; n <= LEANFS_MAX_NAME; n++) {
        CHECK_EQ(LEANFS_DIRENT_NEED(n) % LEANFS_DIRENT_ALIGN, 0);
        CHECK(LEANFS_DIRENT_NEED(n) >= LEANFS_DIRENT_HDR + n);
    }
}

TEST(leanfs_format, a_file_can_address_every_block_the_size_field_can_name) {
    uint64_t addressable = (uint64_t)LEANFS_MAX_FILE_BLOCKS * LEANFS_BLOCK_SIZE;
    CHECK(addressable >= LEANFS_MAX_FILE_SIZE);
    CHECK(LEANFS_MAX_FILE_SIZE <= 0xFFFFFFFFu);
}

TEST(leanfs_format, the_manifest_hash_is_the_algorithm_it_claims_to_be) {
    CHECK_EQ(leanfs_fnv1a(LEANFS_FNV1A_INIT, "", 0), 0x811C9DC5u);
    CHECK_EQ(leanfs_fnv1a(LEANFS_FNV1A_INIT, "a", 1), 0xE40C292Cu);
    CHECK_EQ(leanfs_fnv1a(LEANFS_FNV1A_INIT, "foobar", 6), 0xBF9CF968u);
}

TEST(leanfs_format, the_manifest_hash_does_not_depend_on_the_chunk_size) {
    static uint8_t data[10000];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 7 + (i >> 5));
    }

    uint32_t whole = leanfs_fnv1a(LEANFS_FNV1A_INIT, data, sizeof(data));

    uint32_t in_blocks = LEANFS_FNV1A_INIT;
    for (size_t off = 0; off < sizeof(data); off += LEANFS_BLOCK_SIZE) {
        size_t n = sizeof(data) - off;
        if (n > LEANFS_BLOCK_SIZE) {
            n = LEANFS_BLOCK_SIZE;
        }
        in_blocks = leanfs_fnv1a(in_blocks, data + off, n);
    }
    CHECK_EQ(in_blocks, whole);

    uint32_t byte_at_a_time = LEANFS_FNV1A_INIT;
    for (size_t i = 0; i < sizeof(data); i++) {
        byte_at_a_time = leanfs_fnv1a(byte_at_a_time, data + i, 1);
    }
    CHECK_EQ(byte_at_a_time, whole);

    uint8_t swapped[4] = { data[1], data[0], data[2], data[3] };
    CHECK_NE(leanfs_fnv1a(LEANFS_FNV1A_INIT, swapped, 4),
             leanfs_fnv1a(LEANFS_FNV1A_INIT, data, 4));
}
