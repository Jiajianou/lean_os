#include "check.h"
#include "fakes/fakes.h"
#include "file_system/leanfs.h"
#include "library/kernel_library.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BLOCKS_BEFORE_DATA (LEANFS_START_BLOCK + 1 + \
                            (LEANFS_MAX_INODES * 128 / LEANFS_BLOCK_SIZE) + \
                            (LEANFS_DATA_BLOCKS / 8 / LEANFS_BLOCK_SIZE))
#define DISK_SECTORS ((uint32_t)((BLOCKS_BEFORE_DATA + LEANFS_DATA_BLOCKS) * \
                                 LEANFS_SECTORS_PER_BLOCK))

static void fs_fixture(void) {
    kernel_log_capture_reset();
    fake_block_device_reset(DISK_SECTORS);
    leanfs_init();
    fake_block_device_reset_counters();
}

TEST(leanfs, a_blank_disk_is_formatted_and_says_so) {
    kernel_log_capture_reset();
    fake_block_device_reset(DISK_SECTORS);
    leanfs_init();
    CHECK(kernel_log_capture_contains("no valid leanfs superblock found - formatting fresh"));
    CHECK(leanfs_is_directory("/"));
    fake_block_device_free();
}

TEST(leanfs, write_read_roundtrip_at_every_interesting_size) {
    fs_fixture();
    const size_t sizes[] = {1, 4095, 4096, 4097, 8192, 40960, 4096 * 13};
    char *buffer = malloc(4096 * 16);
    char *back = malloc(4096 * 16);
    REQUIRE(buffer && back);

    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t n = sizes[s];
        for (size_t i = 0; i < n; i++) {
            buffer[i] = (char)(i * 31 + s);
        }
        CHECK_EQ(leanfs_write("/probe", buffer, n), 0);
        memset(back, 0, n);
        int64_t got = leanfs_read("/probe", back, n);
        CHECK_EQ(got, (int64_t)n);
        CHECK_MEMEQ(back, buffer, n);
    }
    free(buffer);
    free(back);
    fake_block_device_free();
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
    char back[64];
    CHECK_EQ(leanfs_read("/shrink", back, sizeof(back)), 10);
    fake_block_device_free();
}

TEST(leanfs, a_directory_keeps_the_block_it_grew_into) {
    fs_fixture();
    uint32_t empty = leanfs_free_blocks();
    REQUIRE(leanfs_write("/first", "x", 1) == 0);
    REQUIRE(leanfs_unlink("/first") == 0);
    uint32_t after = leanfs_free_blocks();
    CHECK_EQ(empty - after, 1);
    REQUIRE(leanfs_write("/second", "x", 1) == 0);
    REQUIRE(leanfs_unlink("/second") == 0);
    CHECK_EQ(leanfs_free_blocks(), after);
    fake_block_device_free();
}

TEST(leanfs, unlink_returns_every_block_it_took) {
    fs_fixture();
    REQUIRE(leanfs_write("/warmup", "x", 1) == 0);
    REQUIRE(leanfs_unlink("/warmup") == 0);
    uint32_t before = leanfs_free_blocks();
    char data[4096 * 3];
    memset(data, 'q', sizeof(data));
    CHECK_EQ(leanfs_write("/temp", data, sizeof(data)), 0);
    CHECK(leanfs_free_blocks() < before);
    CHECK_EQ(leanfs_unlink("/temp"), 0);
    CHECK_EQ(leanfs_free_blocks(), before);
    CHECK_EQ(leanfs_exists("/temp"), 0);
    fake_block_device_free();
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
    fake_block_device_free();
}

TEST(leanfs, a_file_larger_than_the_format_allows_is_refused) {
    fs_fixture();
    static char small[16];
    CHECK(leanfs_write("/toobig", small, (size_t)LEANFS_MAX_FILE_SIZE + 4096) != 0);
    CHECK_EQ(leanfs_exists("/toobig"), 0);
    fake_block_device_free();
}

TEST(leanfs, slow_running_out_of_inodes_is_an_error_and_not_a_corruption) {
    fs_fixture();
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
    char back[8];
    CHECK_EQ(leanfs_read("/f0", back, sizeof(back)), 1);
    CHECK_EQ(back[0], 'x');
    CHECK(leanfs_is_directory("/"));
    fake_block_device_free();
}

TEST(leanfs, slow_running_out_of_data_blocks_is_an_error_and_leaves_the_disk_usable) {
    fs_fixture();
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
            break;
        }
    }
    if (!hit_the_wall) {
        char one[LEANFS_BLOCK_SIZE * 64];
        memset(one, 'Z', sizeof(one));
        CHECK(leanfs_write("/overflow", one, sizeof(one)) != 0);
    }

    char back[16];
    CHECK_EQ(leanfs_read("/witness", back, sizeof(back)), 6);
    CHECK_MEMEQ(back, "keepme", 6);
    CHECK(leanfs_is_directory("/"));
    fake_block_device_free();
}

TEST(leanfs, a_superblock_with_the_wrong_magic_is_reformatted) {
    fs_fixture();
    CHECK_EQ(leanfs_write("/gone", "data", 4), 0);

    uint8_t *sb = fake_block_device_sector(LEANFS_START_LBA);
    sb[0] ^= 0xFF;
    kernel_log_capture_reset();
    leanfs_init();
    CHECK(kernel_log_capture_contains("formatting fresh"));
    CHECK_EQ(leanfs_exists("/gone"), 0);
    CHECK(leanfs_is_directory("/"));
    fake_block_device_free();
}

TEST(leanfs, a_superblock_claiming_impossible_geometry_is_reformatted) {
    fs_fixture();
    uint8_t *sb = fake_block_device_sector(LEANFS_START_LBA);
    uint32_t absurd = 0xFFFFFFFFu;
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

    kernel_log_capture_reset();
    CHECK_NO_PANIC(leanfs_init());
    CHECK(kernel_log_capture_contains("formatting fresh"));
    CHECK(leanfs_is_directory("/"));
    fake_block_device_free();
}

TEST(leanfs, a_disk_of_zeroes_mounts_as_an_empty_filesystem) {
    fake_block_device_reset(DISK_SECTORS);
    kernel_log_capture_reset();
    CHECK_NO_PANIC(leanfs_init());
    CHECK(leanfs_is_directory("/"));
    CHECK_EQ(leanfs_write("/afterwards", "ok", 2), 0);
    fake_block_device_free();
}

TEST(leanfs, path_edge_cases_are_refused_rather_than_misparsed) {
    fs_fixture();
    CHECK(leanfs_write("", "x", 1) != 0);
    CHECK(leanfs_write("no-leading-slash", "x", 1) != 0);
    CHECK(leanfs_exists("") == 0);

    char toolong[LEANFS_MAX_NAME + 64];
    toolong[0] = '/';
    memset(toolong + 1, 'n', sizeof(toolong) - 2);
    toolong[sizeof(toolong) - 1] = '\0';
    CHECK(leanfs_write(toolong, "x", 1) != 0);

    char deep[LEANFS_MAX_PATH + 256];
    size_t k = 0;
    while (k + 3 < sizeof(deep) - 1) {
        deep[k++] = '/';
        deep[k++] = 'd';
        deep[k++] = 'i';
    }
    deep[k] = '\0';
    CHECK_NO_PANIC(leanfs_write(deep, "x", 1));
    fake_block_device_free();
}

TEST(leanfs, a_name_at_exactly_the_maximum_length_is_accepted) {
    fs_fixture();
    char name[LEANFS_MAX_NAME + 2];
    name[0] = '/';
    memset(name + 1, 'm', LEANFS_MAX_NAME);
    name[LEANFS_MAX_NAME + 1] = '\0';
    CHECK_EQ(leanfs_write(name, "edge", 4), 0);
    char back[16];
    CHECK_EQ(leanfs_read(name, back, sizeof(back)), 4);
    fake_block_device_free();
}

TEST(leanfs, a_directory_grows_past_one_block_and_lists_every_entry) {
    fs_fixture();
    REQUIRE(leanfs_mkdir("/many") == 0);
    const int N = 300;
    for (int i = 0; i < N; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/many/entry%03d", i);
        REQUIRE(leanfs_write(p, "e", 1) == 0);
    }
    int found = 0;
    uint32_t cookie = 0;
    leanfs_directory_entry_t entry;
    int r;
    while ((r = leanfs_readdir("/many", &cookie, &entry)) == 1) {
        found++;
    }
    CHECK_EQ(r, 0);
    CHECK_EQ(found, N);

    int h = leanfs_directory_open("/many");
    REQUIRE(h >= 0);
    int found_at = 0;
    cookie = 0;
    while (leanfs_readdir_at(h, &cookie, &entry) == 1) {
        found_at++;
    }
    CHECK_EQ(found_at, N);
    for (int i = 0; i < N; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/many/entry%03d", i);
        CHECK(leanfs_exists(p));
    }
    fake_block_device_free();
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
    CHECK_EQ(leanfs_write("/mixed/reused", "y", 1), 0);
    CHECK(leanfs_exists("/mixed/reused"));
    fake_block_device_free();
}

TEST(leanfs, rmdir_refuses_a_directory_that_still_has_entries) {
    fs_fixture();
    REQUIRE(leanfs_mkdir("/full") == 0);
    REQUIRE(leanfs_write("/full/child", "x", 1) == 0);
    CHECK(leanfs_rmdir("/full") != 0);
    CHECK(leanfs_is_directory("/full"));
    REQUIRE(leanfs_unlink("/full/child") == 0);
    CHECK_EQ(leanfs_rmdir("/full"), 0);
    CHECK_EQ(leanfs_exists("/full"), 0);
    fake_block_device_free();
}

TEST(leanfs, a_file_and_a_directory_cannot_take_the_same_name) {
    fs_fixture();
    REQUIRE(leanfs_mkdir("/collide") == 0);
    CHECK(leanfs_write("/collide", "x", 1) != 0);
    CHECK(leanfs_is_directory("/collide"));

    REQUIRE(leanfs_write("/plain", "x", 1) == 0);
    CHECK(leanfs_mkdir("/plain") != 0);
    CHECK(!leanfs_is_directory("/plain"));
    fake_block_device_free();
}

TEST(leanfs, unlink_refuses_a_directory_and_rmdir_refuses_a_file) {
    fs_fixture();
    REQUIRE(leanfs_mkdir("/adir") == 0);
    REQUIRE(leanfs_write("/afile", "x", 1) == 0);
    CHECK(leanfs_unlink("/adir") != 0);
    CHECK(leanfs_is_directory("/adir"));
    CHECK(leanfs_rmdir("/afile") != 0);
    CHECK(leanfs_exists("/afile"));
    fake_block_device_free();
}

TEST(leanfs, reading_a_file_twice_does_not_cost_twice_as_many_device_reads) {
    fs_fixture();
    char body[LEANFS_BLOCK_SIZE * 4];
    memset(body, 'c', sizeof(body));
    REQUIRE(leanfs_write("/counted", body, sizeof(body)) == 0);

    char back[sizeof(body)];
    fake_block_device_reset_counters();
    REQUIRE(leanfs_read("/counted", back, sizeof(back)) == (int64_t)sizeof(body));
    uint64_t first = fake_block_device_reads();
    CHECK(first > 0);

    CHECK(first < 4 * LEANFS_SECTORS_PER_BLOCK * 8);
    fake_block_device_free();
}

TEST(leanfs, remounting_does_not_leak_the_inode_table) {
    fake_block_device_reset(DISK_SECTORS);
    kernel_log_capture_reset();

    leanfs_init();
    uint64_t baseline = fake_physical_memory_outstanding();

    for (int i = 0; i < 4; i++) {
        leanfs_init();
    }
    CHECK_EQ(fake_physical_memory_outstanding(), baseline);
    CHECK(leanfs_is_directory("/"));
    fake_block_device_free();
}

TEST(leanfs, a_write_to_a_disk_that_refuses_every_write_is_reported) {
    fs_fixture();
    static uint8_t data[8192];
    memset(data, 'a', sizeof(data));

    CHECK_EQ(leanfs_write("/before", data, sizeof(data)), 0);

    fake_block_device_fail_writes_after(0);
    memset(data, 'b', sizeof(data));
    CHECK_EQ(leanfs_write("/during", data, sizeof(data)), -1);

    fake_block_device_fail_writes_after(-1);
    memset(data, 'c', sizeof(data));
    CHECK_EQ(leanfs_write("/after", data, sizeof(data)), 0);

    static uint8_t got[8192];
    CHECK_EQ(leanfs_read("/after", got, sizeof(got)), (int64_t)sizeof(got));
    CHECK_EQ(got[0], 'c');
    fake_block_device_free();
}

TEST(leanfs, a_read_from_a_disk_that_refuses_every_read_is_reported) {
    fs_fixture();
    static uint8_t data[8192];
    memset(data, 'a', sizeof(data));
    CHECK_EQ(leanfs_write("/f", data, sizeof(data)), 0);

    static uint8_t got[8192];
    memset(got, 'z', sizeof(got));
    fake_block_device_fail_reads_after(0);
    CHECK_EQ(leanfs_read("/f", got, sizeof(got)), -1);
    fake_block_device_fail_reads_after(-1);
    CHECK_EQ(leanfs_read("/f", got, sizeof(got)), (int64_t)sizeof(got));
    CHECK_EQ(got[0], 'a');
    fake_block_device_free();
}

TEST(leanfs, a_file_written_before_the_disk_failed_survives_it) {
    fs_fixture();
    static uint8_t data[4096];
    memset(data, 'k', sizeof(data));
    CHECK_EQ(leanfs_write("/keep", data, sizeof(data)), 0);

    fake_block_device_reset_counters();
    fake_block_device_fail_writes_after(10);
    static uint8_t big[64 * 1024];
    memset(big, 'x', sizeof(big));
    CHECK_EQ(leanfs_write("/doomed", big, sizeof(big)), -1);
    fake_block_device_fail_writes_after(-1);

    static uint8_t got[4096];
    memset(got, 0, sizeof(got));
    CHECK_EQ(leanfs_read("/keep", got, sizeof(got)), (int64_t)sizeof(got));
    CHECK_EQ(got[0], 'k');
    CHECK_EQ(got[sizeof(got) - 1], 'k');
    fake_block_device_free();
}

TEST(leanfs, the_filesystem_is_consistent_after_a_disk_that_failed_mid_write) {
    fs_fixture();
    static uint8_t data[4096];
    memset(data, 'a', sizeof(data));
    for (int i = 0; i < 4; i++) {
        char name[16];
        name[0] = '/';
        name[1] = (char)('a' + i);
        name[2] = '\0';
        CHECK_EQ(leanfs_write(name, data, sizeof(data)), 0);
    }

    fake_block_device_reset_counters();
    fake_block_device_fail_writes_silently_after(6);
    static uint8_t big[128 * 1024];
    memset(big, 'x', sizeof(big));
    (void)leanfs_write("/torn", big, sizeof(big));
    fake_block_device_fail_writes_after(-1);

    leanfs_init();
    uint32_t reclaimed = leanfs_check();
    (void)reclaimed;

    for (int i = 0; i < 4; i++) {
        char name[16];
        name[0] = '/';
        name[1] = (char)('a' + i);
        name[2] = '\0';
        static uint8_t got[4096];
        memset(got, 0, sizeof(got));
        int64_t n = leanfs_read(name, got, sizeof(got));
        if (n != (int64_t)sizeof(got)) {
            test_fail(__FILE__, __LINE__, "%s read %lld bytes after a torn write, expected %lld",
                      name, (long long)n, (long long)sizeof(got));
        } else {
            CHECK_EQ(got[0], 'a');
            CHECK_EQ(got[sizeof(got) - 1], 'a');
        }
    }
    fake_block_device_free();
}

TEST(leanfs, a_directory_created_on_a_failing_disk_is_reported) {
    fs_fixture();
    fake_block_device_fail_writes_after(0);
    CHECK_EQ(leanfs_mkdir("/nope"), -1);
    fake_block_device_fail_writes_after(-1);

    leanfs_init();
    CHECK(!leanfs_exists("/nope"));
    CHECK_EQ(leanfs_mkdir("/yes"), 0);
    CHECK(leanfs_is_directory("/yes"));
    fake_block_device_free();
}

/* M176. Block 16 is where the indirect table takes over and block 1040 where
   the double-indirect one does, so 1200 blocks is the smallest file with bytes
   on all three paths. Nothing in this suite had ever read a file past the
   sixteen direct blocks - the largest was thirteen - which is how a cache in
   front of the two table reads every double-indirect block performs could have
   gone in with the suite still green. */
#define M176_SPAN_BLOCKS 1200

TEST(leanfs, a_file_spanning_direct_indirect_and_double_indirect_reads_back) {
    fs_fixture();
    const size_t n = (size_t)M176_SPAN_BLOCKS * LEANFS_BLOCK_SIZE;
    uint8_t *buffer = (uint8_t *)malloc(n);
    uint8_t *back = (uint8_t *)malloc(n);
    REQUIRE(buffer && back);

    for (size_t i = 0; i < n; i++) {
        buffer[i] = (uint8_t)(i * 31u + (i >> 12));
    }
    CHECK_EQ(leanfs_write("/spans", buffer, n), 0);

    memset(back, 0xAA, n);
    CHECK_EQ(leanfs_read("/spans", back, n), (int64_t)n);
    CHECK_EQ(memcmp(buffer, back, n), 0);

    /* Again, because the second read is the one that runs entirely out of a
       warm table cache and is where a stale entry would show. */
    memset(back, 0x55, n);
    CHECK_EQ(leanfs_read("/spans", back, n), (int64_t)n);
    CHECK_EQ(memcmp(buffer, back, n), 0);

    free(buffer);
    free(back);
    fake_block_device_free();
}

TEST(leanfs, two_large_files_read_alternately_do_not_borrow_each_others_tables) {
    fs_fixture();
    const size_t n = (size_t)M176_SPAN_BLOCKS * LEANFS_BLOCK_SIZE;
    uint8_t *one = (uint8_t *)malloc(n);
    uint8_t *two = (uint8_t *)malloc(n);
    uint8_t *back = (uint8_t *)malloc(n);
    REQUIRE(one && two && back);

    for (size_t i = 0; i < n; i++) {
        one[i] = (uint8_t)(i * 7u + 1u);
        two[i] = (uint8_t)(i * 13u + 2u);
    }
    CHECK_EQ(leanfs_write("/one", one, n), 0);
    CHECK_EQ(leanfs_write("/two", two, n), 0);

    for (int round = 0; round < 3; round++) {
        memset(back, 0, n);
        CHECK_EQ(leanfs_read("/one", back, n), (int64_t)n);
        CHECK_EQ(memcmp(one, back, n), 0);
        memset(back, 0, n);
        CHECK_EQ(leanfs_read("/two", back, n), (int64_t)n);
        CHECK_EQ(memcmp(two, back, n), 0);
    }

    free(one);
    free(two);
    free(back);
    fake_block_device_free();
}

/* And the optimisation itself, which correctness cannot see. Every
   double-indirect block used to cost two extra table reads - the root, which
   is the same block for the whole file, and an inner table that changes once
   every LEANFS_INDIRECT_POINTERS blocks - so a read of N blocks cost about 3N.
   The ceiling here is 1.5N, which a regression to the old behaviour fails and
   which leaves room for the inner table legitimately changing. */
TEST(leanfs, a_large_read_does_not_fetch_the_same_tables_over_and_over) {
    fs_fixture();
    const size_t n = (size_t)M176_SPAN_BLOCKS * LEANFS_BLOCK_SIZE;
    uint8_t *buffer = (uint8_t *)malloc(n);
    uint8_t *back = (uint8_t *)malloc(n);
    REQUIRE(buffer && back);
    for (size_t i = 0; i < n; i++) {
        buffer[i] = (uint8_t)i;
    }
    CHECK_EQ(leanfs_write("/counted", buffer, n), 0);

    fake_block_device_reset_counters();
    CHECK_EQ(leanfs_read("/counted", back, n), (int64_t)n);
    CHECK_EQ(memcmp(buffer, back, n), 0);

    uint64_t sectors = fake_block_device_reads();
    uint64_t blocks_read = sectors / LEANFS_SECTORS_PER_BLOCK;
    CHECK(blocks_read >= M176_SPAN_BLOCKS);
    CHECK(blocks_read <= (uint64_t)(M176_SPAN_BLOCKS * 3 / 2));

    free(buffer);
    free(back);
    fake_block_device_free();
}

/* And the other half of M176, which the sector count cannot see. The block
   cache's line is one leanfs block, so one call per block used to mean one
   DEVICE read per block - 54,528 of them for the 213 MiB program this
   milestone was measuring, at 4 KiB each, where the disk budget in
   tests/budgets.tsv measures a MiB at a time and reaches four times the rate.
   Neighbouring blocks go in one request now, so what changes is the number of
   CALLS rather than the number of sectors. */
TEST(leanfs, neighbouring_blocks_are_asked_for_in_one_request) {
    fs_fixture();
    const size_t n = (size_t)M176_SPAN_BLOCKS * LEANFS_BLOCK_SIZE;
    uint8_t *buffer = (uint8_t *)malloc(n);
    uint8_t *back = (uint8_t *)malloc(n);
    REQUIRE(buffer && back);
    for (size_t i = 0; i < n; i++) {
        buffer[i] = (uint8_t)(i * 17u);
    }
    CHECK_EQ(leanfs_write("/runs", buffer, n), 0);

    fake_block_device_reset_counters();
    CHECK_EQ(leanfs_read("/runs", back, n), (int64_t)n);
    CHECK_EQ(memcmp(buffer, back, n), 0);

    uint64_t calls = fake_block_device_read_calls();
    uint64_t sectors = fake_block_device_reads();
    /* Every block still arrives - the sectors prove that - but it takes far
       fewer requests than there are blocks. One call per block would be 1200. */
    CHECK(sectors >= (uint64_t)M176_SPAN_BLOCKS * LEANFS_SECTORS_PER_BLOCK);
    CHECK(calls >= 1);
    CHECK(calls <= (uint64_t)M176_SPAN_BLOCKS / 4);

    free(buffer);
    free(back);
    fake_block_device_free();
}
