/* tests/test_leanfs_format.c - Q3
 *
 * The check that would have caught the drift.
 *
 * Two programs write a leanfs image: kernel/fs/leanfs.c and
 * tools/leanfs-put.c. Until Q3 the second carried its own copy of the
 * on-disk structs, kept in agreement with the first by a comment. They
 * had been out of agreement since M93 - the tool was writing "LFS4" with
 * 512-byte blocks at a kernel that had moved to "LFS5" with 4096-byte
 * ones - and every `make preseed` since then produced an image the next
 * boot silently reformatted.
 *
 * kernel/fs/leanfs_format.h is now the one definition and both include
 * it, so that specific bug cannot recur. This file is the belt to that
 * braces: it pins the on-disk layout to the byte offsets it actually has,
 * so that a future edit to a struct which *both* programs would agree on
 * and which no existing disk would survive shows up as a failing test
 * rather than as a filesystem.
 *
 * A format is a contract with data already written. It is worth an
 * assertion per field. */
#include "check.h"
#include "fs/leanfs_format.h"

#include <stddef.h>
#include <stdint.h>

/* ---- Compile-time: sizes and offsets --------------------------------- */

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
_Static_assert(offsetof(leanfs_dirent_t, rec_len) == 4, "");
_Static_assert(offsetof(leanfs_dirent_t, name_len) == 6, "");
_Static_assert(offsetof(leanfs_dirent_t, type) == 7, "");

/* ---- Compile-time: the geometry every buffer is sized from ----------- */

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
    /* Spelled out rather than compared to themselves. A magic that
     * changes is a disk that gets reformatted, so it should take a
     * deliberate edit here to change one - not a silent inherit. */
    CHECK_EQ(LEANFS_MAGIC, 0x3553464Cu);   /* "LFS5" */
    CHECK_EQ(LEANFS_VERSION, 5u);
    /* And the magic really does spell what its comment claims. */
    CHECK_EQ((LEANFS_MAGIC >> 0) & 0xFF, 'L');
    CHECK_EQ((LEANFS_MAGIC >> 8) & 0xFF, 'F');
    CHECK_EQ((LEANFS_MAGIC >> 16) & 0xFF, 'S');
    CHECK_EQ((LEANFS_MAGIC >> 24) & 0xFF, '5');
}

TEST(leanfs_format, the_dirty_marker_is_not_a_value_a_blank_disk_produces) {
    /* M71 reuses a formerly-reserved word for the mount state, on the
     * argument that an old disk reads 0 there and 0 means CLEAN. That
     * only holds if DIRTY is not 0, and it is the kind of invariant a
     * later edit breaks without noticing. */
    CHECK_EQ(LEANFS_STATE_CLEAN, 0u);
    CHECK_NE(LEANFS_STATE_DIRTY, 0u);
}

TEST(leanfs_format, the_layout_leaves_no_gaps_and_no_overlaps) {
    /* The four regions must tile the disk in order, each starting exactly
     * where the last ended. An off-by-one here is a bitmap that overlaps
     * the inode table. */
    uint32_t sb_block = LEANFS_START_BLOCK;
    uint32_t inode_table = sb_block + 1;
    uint32_t bitmap = inode_table + (uint32_t)INODE_TABLE_BLOCKS;
    uint32_t data = bitmap + (uint32_t)BITMAP_BLOCKS;

    CHECK_EQ(INODE_TABLE_BLOCKS,
             (sizeof(leanfs_inode_t) * LEANFS_MAX_INODES) / LEANFS_BLOCK_SIZE);
    CHECK_EQ(BITMAP_BLOCKS, LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE);
    /* Every bit of the bitmap has a data block, and every data block a
     * bit - a bitmap that is one block short means the top of the disk is
     * unallocatable, and one block long means it hands out blocks that do
     * not exist. */
    CHECK_EQ((uint64_t)BITMAP_BLOCKS * LEANFS_BLOCK_SIZE * 8, LEANFS_DATA_BLOCKS);
    CHECK(data > bitmap);
}

TEST(leanfs_format, a_directory_record_never_straddles_a_block) {
    /* The one rule the variable-length record format rests on. */
    CHECK(LEANFS_DIRENT_NEED(LEANFS_MAX_NAME) <= LEANFS_BLOCK_SIZE);
    /* Rounding is up, to the alignment, always. */
    CHECK_EQ(LEANFS_DIRENT_NEED(0), LEANFS_DIRENT_HDR);
    CHECK_EQ(LEANFS_DIRENT_NEED(1), LEANFS_DIRENT_HDR + LEANFS_DIRENT_ALIGN);
    CHECK_EQ(LEANFS_DIRENT_NEED(4), LEANFS_DIRENT_HDR + LEANFS_DIRENT_ALIGN);
    CHECK_EQ(LEANFS_DIRENT_NEED(5), LEANFS_DIRENT_HDR + 2 * LEANFS_DIRENT_ALIGN);
    /* And a rounded record length is always a multiple of the alignment,
     * which is what lets a walker step through a block by rec_len. */
    for (unsigned n = 0; n <= LEANFS_MAX_NAME; n++) {
        CHECK_EQ(LEANFS_DIRENT_NEED(n) % LEANFS_DIRENT_ALIGN, 0);
        CHECK(LEANFS_DIRENT_NEED(n) >= LEANFS_DIRENT_HDR + n);
    }
}

TEST(leanfs_format, a_file_can_address_every_block_the_size_field_can_name) {
    /* LEANFS_MAX_FILE_SIZE is a promise about what fits. The block
     * pointers - direct, indirect, double-indirect - have to be able to
     * reach that far, or the promise is bigger than the format. */
    uint64_t addressable = (uint64_t)LEANFS_MAX_FILE_BLOCKS * LEANFS_BLOCK_SIZE;
    CHECK(addressable >= LEANFS_MAX_FILE_SIZE);
    /* And the size field itself must be able to hold it. */
    CHECK(LEANFS_MAX_FILE_SIZE <= 0xFFFFFFFFu);
}
