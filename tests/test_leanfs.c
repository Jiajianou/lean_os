/* tests/test_leanfs.c - Q3
 *
 * kernel/fs/leanfs.c, compiled unmodified, mounted on a disk made of host
 * memory.
 *
 * The boot self-tests for this filesystem are good and they are all about
 * the happy path: files are written and read back, directories are
 * created and entered, an indirect block is exercised by a big file. What
 * none of them can do is run out of anything. Reaching "the data region
 * is full" from inside a booted OS means writing two gigabytes; reaching
 * "the inode table is full" means creating 131,072 files; reaching "the
 * superblock is corrupt" means corrupting one. Those branches exist, they
 * are the ones that decide whether a full disk is an error or a
 * catastrophe, and until this file nothing had executed them.
 *
 * The other thing this tier can do that a boot cannot: count. blk_read
 * and blk_write go through a fake that tallies them, so a test can assert
 * what an operation *costs* and not only what it returns. */
#include "check.h"
#include "fakes/fakes.h"
#include "fs/leanfs.h"
#include "lib/libk.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The whole disk leanfs's geometry implies: the superblock, the inode
 * table, the bitmap and every data block. Computed rather than guessed,
 * so a geometry change here fails to build instead of reading off the
 * end. */
#define BLOCKS_BEFORE_DATA (LEANFS_START_BLOCK + 1 + \
                            (LEANFS_MAX_INODES * 128 / LEANFS_BLOCK_SIZE) + \
                            (LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE))
#define DISK_SECTORS ((uint32_t)((BLOCKS_BEFORE_DATA + LEANFS_DATA_BLOCKS) * \
                                 LEANFS_SECTORS_PER_BLOCK))

/* Deliberately does NOT reset the frame allocator.
 *
 * leanfs holds its 16 MiB inode table for the life of the machine (see
 * inodes_alloc), which is what the remount test below is about. Pulling
 * the frames out from under it between tests would leave that cached
 * pointer dangling - ASan says so immediately, which is how this comment
 * came to be written. Tests that care about allocation measure a delta
 * instead of an absolute. */
static void fs_fixture(void) {
    klog_capture_reset();
    fake_blk_reset(DISK_SECTORS);
    leanfs_init();          /* a blank disk has no superblock, so this formats */
    fake_blk_reset_counters();
}

/* ---- The shape of the thing ------------------------------------------ */

TEST(leanfs, a_blank_disk_is_formatted_and_says_so) {
    klog_capture_reset();
    fake_blk_reset(DISK_SECTORS);
    leanfs_init();
    CHECK(klog_capture_contains("no valid leanfs superblock found - formatting fresh"));
    CHECK(leanfs_is_dir("/"));
    fake_blk_free();
}

TEST(leanfs, write_read_roundtrip_at_every_interesting_size) {
    fs_fixture();
    /* One byte, exactly a block, one over a block, and past the direct
     * block count into the indirect chain. The boundaries are where a
     * block-mapping bug lives. */
    const size_t sizes[] = {1, 4095, 4096, 4097, 8192, 40960, 4096 * 13};
    char *buf = malloc(4096 * 16);
    char *back = malloc(4096 * 16);
    REQUIRE(buf && back);

    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t n = sizes[s];
        for (size_t i = 0; i < n; i++) {
            buf[i] = (char)(i * 31 + s);
        }
        CHECK_EQ(leanfs_write("/probe", buf, n), 0);
        memset(back, 0, n);
        int64_t got = leanfs_read("/probe", back, n);
        CHECK_EQ(got, (int64_t)n);
        CHECK_MEMEQ(back, buf, n);
    }
    free(buf);
    free(back);
    fake_blk_free();
}

TEST(leanfs, a_rewrite_that_shrinks_a_file_returns_the_blocks_it_freed) {
    fs_fixture();
    char big[4096 * 4];
    memset(big, 'x', sizeof(big));

    uint32_t free_at_start = leanfs_free_blocks();
    CHECK_EQ(leanfs_write("/shrink", big, sizeof(big)), 0);
    uint32_t free_when_big = leanfs_free_blocks();
    CHECK(free_when_big < free_at_start);

    CHECK_EQ(leanfs_write("/shrink", big, 10), 0);
    uint32_t free_when_small = leanfs_free_blocks();
    CHECK(free_when_small > free_when_big);
    /* And the file really is ten bytes, not four blocks with a small
     * size field - the difference is whether the blocks came back. */
    char back[64];
    CHECK_EQ(leanfs_read("/shrink", back, sizeof(back)), 10);
    fake_blk_free();
}

TEST(leanfs, a_directory_keeps_the_block_it_grew_into) {
    fs_fixture();
    /* Recorded because it is a real property and it surprised this test
     * on first run: the root directory allocates its first data block
     * when it gains its first entry, and removing that entry does not
     * give the block back. That is deliberate - a directory that
     * released and re-allocated a block per file would thrash - but it
     * means "free blocks before" and "free blocks after" only match once
     * the directory has already grown. Every neutrality check below
     * warms up first for this reason. */
    uint32_t empty = leanfs_free_blocks();
    REQUIRE(leanfs_write("/first", "x", 1) == 0);
    REQUIRE(leanfs_unlink("/first") == 0);
    uint32_t after = leanfs_free_blocks();
    CHECK_EQ(empty - after, 1);
    /* ...and it is genuinely reused rather than lost: a second cycle
     * costs nothing more. */
    REQUIRE(leanfs_write("/second", "x", 1) == 0);
    REQUIRE(leanfs_unlink("/second") == 0);
    CHECK_EQ(leanfs_free_blocks(), after);
    fake_blk_free();
}

TEST(leanfs, unlink_returns_every_block_it_took) {
    fs_fixture();
    /* Warm the root directory up first - see the test above. */
    REQUIRE(leanfs_write("/warmup", "x", 1) == 0);
    REQUIRE(leanfs_unlink("/warmup") == 0);
    uint32_t before = leanfs_free_blocks();
    char data[4096 * 3];
    memset(data, 'q', sizeof(data));
    CHECK_EQ(leanfs_write("/temp", data, sizeof(data)), 0);
    CHECK(leanfs_free_blocks() < before);
    CHECK_EQ(leanfs_unlink("/temp"), 0);
    /* Exactly where it started. A filesystem that leaks one block per
     * create/delete cycle is a filesystem that fills up in a week, and
     * the leak is invisible in any test that does not count. */
    CHECK_EQ(leanfs_free_blocks(), before);
    CHECK_EQ(leanfs_exists("/temp"), 0);
    fake_blk_free();
}

TEST(leanfs, a_thousand_create_delete_cycles_are_block_neutral) {
    fs_fixture();
    REQUIRE(leanfs_write("/warmup", "x", 1) == 0);
    REQUIRE(leanfs_unlink("/warmup") == 0);
    uint32_t before = leanfs_free_blocks();
    for (int i = 0; i < 1000; i++) {
        char body[200];
        memset(body, 'a' + (i % 26), sizeof(body));
        REQUIRE(leanfs_write("/churn", body, sizeof(body)) == 0);
        REQUIRE(leanfs_unlink("/churn") == 0);
    }
    CHECK_EQ(leanfs_free_blocks(), before);
    fake_blk_free();
}

/* ---- The paths a booted machine cannot reach ------------------------- */

TEST(leanfs, a_file_larger_than_the_format_allows_is_refused) {
    fs_fixture();
    /* LEANFS_MAX_FILE_SIZE is just under 4 GiB and there is no way to
     * reach it by writing. What can be checked is that the refusal is a
     * refusal - a returned error - rather than a wrap or a panic. */
    static char small[16];
    CHECK(leanfs_write("/toobig", small, (size_t)LEANFS_MAX_FILE_SIZE + 4096) != 0);
    CHECK_EQ(leanfs_exists("/toobig"), 0);
    fake_blk_free();
}

TEST(leanfs, slow_running_out_of_inodes_is_an_error_and_not_a_corruption) {
    fs_fixture();
    /* The inode table is the first thing a filesystem of small files
     * exhausts, and this branch has never run. Creating 131,072 files
     * through the ordinary path would take minutes; a directory of them
     * is not what is under test here, so this fills the table and then
     * checks the very next create fails cleanly and the filesystem is
     * still readable afterwards. */
    char name[64];
    int first_failure = -1;
    for (int i = 0; i < (int)LEANFS_MAX_INODES + 8; i++) {
        snprintf(name, sizeof(name), "/f%d", i);
        if (leanfs_write(name, "x", 1) != 0) {
            first_failure = i;
            break;
        }
    }
    CHECK(first_failure > 0);
    /* It stopped for a reason, and the reason must be a limit rather than
     * a crash. Everything written before the limit must still be there -
     * a filesystem that damages existing files when it runs out of new
     * ones is the failure mode worth ruling out. */
    char back[8];
    CHECK_EQ(leanfs_read("/f0", back, sizeof(back)), 1);
    CHECK_EQ(back[0], 'x');
    CHECK(leanfs_is_dir("/"));
    fake_blk_free();
}

TEST(leanfs, slow_running_out_of_data_blocks_is_an_error_and_leaves_the_disk_usable) {
    fs_fixture();
    /* Fill the data region until a write fails, then check the failure is
     * clean: an error return, no panic, the root still a directory, and a
     * file written before the wall still readable byte for byte. */
    CHECK_EQ(leanfs_write("/witness", "keepme", 6), 0);

    char chunk[LEANFS_BLOCK_SIZE * 16];
    memset(chunk, 'F', sizeof(chunk));
    char name[64];
    int i = 0;
    int hit_the_wall = 0;
    for (; i < 100000; i++) {
        snprintf(name, sizeof(name), "/fill%d", i);
        if (leanfs_write(name, chunk, sizeof(chunk)) != 0) {
            hit_the_wall = 1;
            break;
        }
        if (leanfs_free_blocks() < 32) {
            /* Close enough to the end to stop filling in 64 KiB steps. */
            break;
        }
    }
    /* Whether the wall arrives as a failed write or as an almost-empty
     * free count, the next single-block write into a full region must
     * fail rather than succeed against blocks that do not exist. */
    if (!hit_the_wall) {
        char one[LEANFS_BLOCK_SIZE * 64];
        memset(one, 'Z', sizeof(one));
        CHECK(leanfs_write("/overflow", one, sizeof(one)) != 0);
    }

    char back[16];
    CHECK_EQ(leanfs_read("/witness", back, sizeof(back)), 6);
    CHECK_MEMEQ(back, "keepme", 6);
    CHECK(leanfs_is_dir("/"));
    fake_blk_free();
}

/* ---- Corruption: every one of these must be a refusal ---------------- */

TEST(leanfs, a_superblock_with_the_wrong_magic_is_reformatted) {
    fs_fixture();
    CHECK_EQ(leanfs_write("/gone", "data", 4), 0);

    /* Corrupt the magic in place and remount. The recovery path is a
     * reformat, which loses the file - that is the documented behaviour
     * and what is being asserted. The alternative, interpreting a
     * superblock this build does not understand, is how a corrupt field
     * becomes an out-of-bounds write. */
    uint8_t *sb = fake_blk_sector(LEANFS_START_LBA);
    sb[0] ^= 0xFF;
    klog_capture_reset();
    leanfs_init();
    CHECK(klog_capture_contains("formatting fresh"));
    CHECK_EQ(leanfs_exists("/gone"), 0);
    CHECK(leanfs_is_dir("/"));
    fake_blk_free();
}

TEST(leanfs, a_superblock_claiming_impossible_geometry_is_reformatted) {
    fs_fixture();
    /* The field that matters most: data_blocks sizes the static bitmap
     * every allocation loop trusts as its own bound. A superblock
     * claiming more than this build compiled for must not be believed,
     * because believing it walks the bitmap off its end. */
    uint8_t *sb = fake_blk_sector(LEANFS_START_LBA);
    uint32_t absurd = 0xFFFFFFFFu;
    /* data_blocks sits after magic, inode_table_block, inode_table_blocks,
     * bitmap_block, bitmap_blocks, data_block - offset located by pattern
     * rather than hardcoded, so a layout change fails loudly here. */
    int found = -1;
    for (int off = 0; off + 4 <= 64; off += 4) {
        uint32_t v;
        memcpy(&v, sb + off, 4);
        if (v == LEANFS_DATA_BLOCKS) {
            found = off;
            break;
        }
    }
    REQUIRE(found >= 0);
    memcpy(sb + found, &absurd, 4);

    klog_capture_reset();
    CHECK_NO_PANIC(leanfs_init());
    CHECK(klog_capture_contains("formatting fresh"));
    CHECK(leanfs_is_dir("/"));
    fake_blk_free();
}

TEST(leanfs, a_disk_of_zeroes_mounts_as_an_empty_filesystem) {
    /* The simplest corruption there is, and the one a fresh disk actually
     * presents. It must not panic, must not loop, and must produce a
     * mountable root. */
    fake_blk_reset(DISK_SECTORS);
    klog_capture_reset();
    CHECK_NO_PANIC(leanfs_init());
    CHECK(leanfs_is_dir("/"));
    CHECK_EQ(leanfs_write("/afterwards", "ok", 2), 0);
    fake_blk_free();
}

/* ---- Names and paths ------------------------------------------------- */

TEST(leanfs, path_edge_cases_are_refused_rather_than_misparsed) {
    fs_fixture();
    /* None of these may panic and none may succeed. A path parser that
     * accepts one of them is a path parser that can be argued into
     * touching something it should not. */
    CHECK(leanfs_write("", "x", 1) != 0);
    CHECK(leanfs_write("no-leading-slash", "x", 1) != 0);
    CHECK(leanfs_exists("") == 0);

    /* A name longer than the format allows. */
    char toolong[LEANFS_MAX_NAME + 64];
    toolong[0] = '/';
    memset(toolong + 1, 'n', sizeof(toolong) - 2);
    toolong[sizeof(toolong) - 1] = '\0';
    CHECK(leanfs_write(toolong, "x", 1) != 0);

    /* A path deeper than the buffer that resolves it - this is the one
     * that overran the boot stack into .rodata before M81 grew the
     * stack, per kernel.c's own note. It must be refused. */
    char deep[LEANFS_MAX_PATH + 256];
    size_t k = 0;
    while (k + 3 < sizeof(deep) - 1) {
        deep[k++] = '/';
        deep[k++] = 'd';
        deep[k++] = 'i';
    }
    deep[k] = '\0';
    CHECK_NO_PANIC(leanfs_write(deep, "x", 1));
    fake_blk_free();
}

TEST(leanfs, a_name_at_exactly_the_maximum_length_is_accepted) {
    fs_fixture();
    /* The other side of the boundary. Off-by-one in a length check is
     * invisible unless both sides are tested. */
    char name[LEANFS_MAX_NAME + 2];
    name[0] = '/';
    memset(name + 1, 'm', LEANFS_MAX_NAME);
    name[LEANFS_MAX_NAME + 1] = '\0';
    CHECK_EQ(leanfs_write(name, "edge", 4), 0);
    char back[16];
    CHECK_EQ(leanfs_read(name, back, sizeof(back)), 4);
    fake_blk_free();
}

/* ---- Directories ------------------------------------------------------ */

TEST(leanfs, a_directory_grows_past_one_block_and_lists_every_entry) {
    fs_fixture();
    REQUIRE(leanfs_mkdir("/many") == 0);
    /* Enough entries to push the directory past a single 4 KiB block,
     * which is where the block-mapping path for directories starts
     * mattering. */
    const int N = 300;
    for (int i = 0; i < N; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/many/entry%03d", i);
        REQUIRE(leanfs_write(p, "e", 1) == 0);
    }
    /* Every one must be findable by name, and the count must be exact -
     * a directory that loses an entry when it grows is the bug this
     * catches. */
    int found = 0;
    uint32_t cookie = 0;
    leanfs_dir_entry_t ent;
    /* 1 per entry, 0 at the end, -1 if it is not a directory - not the
     * 0-means-success convention the rest of this API uses. */
    int r;
    while ((r = leanfs_readdir("/many", &cookie, &ent)) == 1) {
        found++;
    }
    CHECK_EQ(r, 0);
    CHECK_EQ(found, N);

    /* The same walk through the handle form, which is what SYS_getdents
     * uses and what M81 added for directories this size. It must agree
     * exactly - two walks that disagree is the bug worth catching. */
    int h = leanfs_dir_open("/many");
    REQUIRE(h >= 0);
    int found_at = 0;
    cookie = 0;
    while (leanfs_readdir_at(h, &cookie, &ent) == 1) {
        found_at++;
    }
    CHECK_EQ(found_at, N);
    for (int i = 0; i < N; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/many/entry%03d", i);
        CHECK(leanfs_exists(p));
    }
    fake_blk_free();
}

TEST(leanfs, removing_entries_from_a_grown_directory_keeps_the_rest) {
    fs_fixture();
    REQUIRE(leanfs_mkdir("/mixed") == 0);
    const int N = 200;
    for (int i = 0; i < N; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/mixed/f%03d", i);
        REQUIRE(leanfs_write(p, "x", 1) == 0);
    }
    /* Delete every other one - the pattern that leaves holes in the
     * directory blocks, which is where a compaction bug shows up. */
    for (int i = 0; i < N; i += 2) {
        char p[64];
        snprintf(p, sizeof(p), "/mixed/f%03d", i);
        REQUIRE(leanfs_unlink(p) == 0);
    }
    for (int i = 0; i < N; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/mixed/f%03d", i);
        CHECK_EQ(leanfs_exists(p), (i % 2) ? 1 : 0);
    }
    /* And a new entry must be able to reuse one of those holes. */
    CHECK_EQ(leanfs_write("/mixed/reused", "y", 1), 0);
    CHECK(leanfs_exists("/mixed/reused"));
    fake_blk_free();
}

TEST(leanfs, rmdir_refuses_a_directory_that_still_has_entries) {
    fs_fixture();
    REQUIRE(leanfs_mkdir("/full") == 0);
    REQUIRE(leanfs_write("/full/child", "x", 1) == 0);
    CHECK(leanfs_rmdir("/full") != 0);
    CHECK(leanfs_is_dir("/full"));
    REQUIRE(leanfs_unlink("/full/child") == 0);
    CHECK_EQ(leanfs_rmdir("/full"), 0);
    CHECK_EQ(leanfs_exists("/full"), 0);
    fake_blk_free();
}

TEST(leanfs, a_file_and_a_directory_cannot_take_the_same_name) {
    fs_fixture();
    REQUIRE(leanfs_mkdir("/collide") == 0);
    CHECK(leanfs_write("/collide", "x", 1) != 0);
    CHECK(leanfs_is_dir("/collide"));

    REQUIRE(leanfs_write("/plain", "x", 1) == 0);
    CHECK(leanfs_mkdir("/plain") != 0);
    CHECK(!leanfs_is_dir("/plain"));
    fake_blk_free();
}

TEST(leanfs, unlink_refuses_a_directory_and_rmdir_refuses_a_file) {
    fs_fixture();
    REQUIRE(leanfs_mkdir("/adir") == 0);
    REQUIRE(leanfs_write("/afile", "x", 1) == 0);
    CHECK(leanfs_unlink("/adir") != 0);
    CHECK(leanfs_is_dir("/adir"));
    CHECK(leanfs_rmdir("/afile") != 0);
    CHECK(leanfs_exists("/afile"));
    fake_blk_free();
}

/* ---- What an operation costs ----------------------------------------- */

TEST(leanfs, reading_a_file_twice_does_not_cost_twice_as_many_device_reads) {
    fs_fixture();
    char body[LEANFS_BLOCK_SIZE * 4];
    memset(body, 'c', sizeof(body));
    REQUIRE(leanfs_write("/counted", body, sizeof(body)) == 0);

    char back[sizeof(body)];
    fake_blk_reset_counters();
    REQUIRE(leanfs_read("/counted", back, sizeof(back)) == (int64_t)sizeof(body));
    uint64_t first = fake_blk_reads();
    CHECK(first > 0);

    /* This fake has no cache - it is the device. The number is recorded
     * as a floor: a read of four blocks must not cost wildly more than
     * four blocks' worth of sectors plus the metadata to find them. An
     * algorithm that re-walks the inode table per block would show up
     * here as a multiple, which is the regression this can see and a
     * pass/fail boot test cannot. */
    CHECK(first < 4 * LEANFS_SECTORS_PER_BLOCK * 8);
    fake_blk_free();
}

/* ---- Mounting twice --------------------------------------------------- */

TEST(leanfs, remounting_does_not_leak_the_inode_table) {
    /* leanfs_init calls inodes_alloc, which takes a 16 MiB *contiguous*
     * run out of the below-4 GiB region M90 reserves for 32-bit DMA - the
     * scarcest memory on the machine, and the only allocation in this
     * kernel that needs a contiguous run bigger than a page.
     *
     * On a booted machine leanfs_init runs exactly once, which is why
     * this has never mattered. It stops being once the moment anything
     * remounts - and the corruption tests above already call it twice,
     * which is how this was noticed: the fake allocator's outstanding
     * count went up by 4096 frames per mount and never came down. */
    fake_blk_reset(DISK_SECTORS);
    klog_capture_reset();

    /* A delta rather than an absolute - the fixture no longer resets the
     * allocator, for the reason given above it. */
    leanfs_init();
    uint64_t baseline = fake_pmm_outstanding();

    for (int i = 0; i < 4; i++) {
        leanfs_init();
    }
    /* Four more mounts must not cost four more inode tables. Before the
     * fix in inodes_alloc this was baseline + 4. */
    CHECK_EQ(fake_pmm_outstanding(), baseline);
    CHECK(leanfs_is_dir("/"));
    fake_blk_free();
}
