#include "check.h"

#include "drivers/nvme_split.h"

#include <stdint.h>
#include <string.h>

#define MAX_SECTORS 128

static int walk(uint64_t lba, uint32_t count, uint32_t shift, int *saw_partial) {
    uint32_t per_block = 1u << shift;
    uint64_t cursor = lba;
    uint32_t left = count;
    int chunks = 0;
    *saw_partial = 0;

    while (left > 0) {
        nvme_chunk_t c;
        REQUIRE(nvme_split_next(cursor, left, shift, MAX_SECTORS, &c) == 1);
        REQUIRE(c.sectors > 0);
        REQUIRE(c.sectors <= left);

        CHECK_EQ(c.block * per_block + c.offset, cursor);

        if (c.partial) {
            *saw_partial = 1;
            CHECK_EQ(c.blocks, 1u);
            CHECK(c.offset + c.sectors <= per_block);
        } else {
            CHECK_EQ(c.offset, 0u);
            CHECK_EQ(c.sectors, c.blocks * per_block);
            CHECK(c.sectors <= MAX_SECTORS);
            CHECK(c.blocks > 0);
        }

        cursor += c.sectors;
        left -= c.sectors;
        chunks++;
        REQUIRE(chunks < 4096);
    }
    CHECK_EQ(cursor, lba + count);
    return chunks;
}

TEST(nvme_split, an_empty_request_produces_nothing) {
    nvme_chunk_t c;
    CHECK_EQ(nvme_split_next(0, 0, 0, MAX_SECTORS, &c), 0);
    CHECK_EQ(nvme_split_next(12345, 0, 3, MAX_SECTORS, &c), 0);
}

TEST(nvme_split, a_512_byte_namespace_never_reads_to_modify_and_write) {
    for (uint64_t lba = 0; lba < 40; lba++) {
        for (uint32_t count = 1; count <= 300; count++) {
            int partial = 0;
            walk(lba, count, 0, &partial);
            CHECK_EQ(partial, 0);
        }
    }
}

TEST(nvme_split, a_512_byte_namespace_uses_whole_bounce_loads) {
    nvme_chunk_t c;
    REQUIRE(nvme_split_next(0, 1000, 0, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, (uint32_t)MAX_SECTORS);
    CHECK_EQ(c.blocks, (uint32_t)MAX_SECTORS);
    CHECK_EQ(c.partial, 0);
}

TEST(nvme_split, a_4k_namespace_tiles_every_request_exactly) {
    for (uint64_t lba = 0; lba < 40; lba++) {
        for (uint32_t count = 1; count <= 300; count++) {
            int partial = 0;
            walk(lba, count, 3, &partial);
        }
    }
}

TEST(nvme_split, a_4k_namespace_reads_to_modify_and_write_only_at_the_edges) {
    int partial = 1;
    walk(0, 8, 3, &partial);
    CHECK_EQ(partial, 0);
    partial = 1;
    walk(16, 64, 3, &partial);
    CHECK_EQ(partial, 0);

    partial = 0;
    walk(1, 8, 3, &partial);
    CHECK_EQ(partial, 1);

    partial = 0;
    walk(0, 9, 3, &partial);
    CHECK_EQ(partial, 1);

    nvme_chunk_t c;
    REQUIRE(nvme_split_next(5, 1, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.partial, 1);
    CHECK_EQ(c.block, 0u);
    CHECK_EQ(c.offset, 5u);
    CHECK_EQ(c.sectors, 1u);
    CHECK_EQ(c.blocks, 1u);
}

TEST(nvme_split, a_head_a_middle_and_a_tail) {
    int partial = 0;
    int chunks = walk(7, 18, 3, &partial);
    CHECK_EQ(chunks, 3);
    CHECK_EQ(partial, 1);

    nvme_chunk_t c;
    REQUIRE(nvme_split_next(7, 18, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.partial, 1);
    CHECK_EQ(c.sectors, 1u);
    CHECK_EQ(c.offset, 7u);

    REQUIRE(nvme_split_next(8, 17, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.partial, 0);
    CHECK_EQ(c.sectors, 16u);
    CHECK_EQ(c.blocks, 2u);
    CHECK_EQ(c.block, 1u);

    REQUIRE(nvme_split_next(24, 1, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.partial, 1);
    CHECK_EQ(c.block, 3u);
    CHECK_EQ(c.offset, 0u);
    CHECK_EQ(c.sectors, 1u);
}

TEST(nvme_split, the_bounce_bound_is_rounded_down_to_whole_blocks) {
    nvme_chunk_t c;
    REQUIRE(nvme_split_next(0, 1000, 3, 100, &c) == 1);
    CHECK_EQ(c.partial, 0);
    CHECK_EQ(c.sectors, 96u);
    CHECK_EQ(c.blocks, 12u);
    CHECK_EQ(c.sectors % 8, 0u);
}

TEST(nvme_split, a_long_request_is_still_tiled_with_an_awkward_bound) {
    for (uint32_t bound = 8; bound <= 136; bound++) {
        uint64_t lba = 3;
        uint32_t count = 500;
        uint32_t left = count;
        uint64_t cursor = lba;
        int guard = 0;
        while (left > 0) {
            nvme_chunk_t c;
            REQUIRE(nvme_split_next(cursor, left, 3, bound, &c) == 1);
            REQUIRE(c.sectors > 0);
            if (!c.partial) {
                CHECK_EQ(c.sectors % 8, 0u);
                CHECK(c.sectors <= bound);
            }
            cursor += c.sectors;
            left -= c.sectors;
            REQUIRE(++guard < 4096);
        }
        CHECK_EQ(cursor, lba + count);
    }
}

TEST(nvme_split, a_large_lba_keeps_all_its_bits) {
    nvme_chunk_t c;
    uint64_t lba = 0x1FFFFFFF8ULL;
    REQUIRE(nvme_split_next(lba, 8, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.partial, 0);
    CHECK_EQ(c.block, lba >> 3);
    CHECK_EQ(c.block * 8, lba);

    REQUIRE(nvme_split_next(lba + 3, 2, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.partial, 1);
    CHECK_EQ(c.block, lba >> 3);
    CHECK_EQ(c.offset, 3u);
}

TEST(nvme_split, the_other_legal_block_sizes_tile_too) {
    for (uint32_t shift = 1; shift <= 3; shift++) {
        for (uint64_t lba = 0; lba < 20; lba++) {
            for (uint32_t count = 1; count <= 100; count++) {
                int partial = 0;
                walk(lba, count, shift, &partial);
            }
        }
    }
}

TEST(nvme_split, a_partial_chunk_that_ends_exactly_at_the_block_boundary) {
    nvme_chunk_t c;
    REQUIRE(nvme_split_next(5, 3, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.partial, 1);
    CHECK_EQ(c.block, 0u);
    CHECK_EQ(c.offset, 5u);
    CHECK_EQ(c.sectors, 3u);
    CHECK_EQ(c.offset + c.sectors, 8u);

    REQUIRE(nvme_split_next(5, 2, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, 2u);

    REQUIRE(nvme_split_next(5, 4, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, 3u);
    REQUIRE(nvme_split_next(8, 1, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.block, 1u);
    CHECK_EQ(c.offset, 0u);
    CHECK_EQ(c.sectors, 1u);
}

TEST(nvme_split, a_request_of_exactly_the_bounce_size_is_one_command) {
    nvme_chunk_t c;
    REQUIRE(nvme_split_next(0, MAX_SECTORS, 0, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, (uint32_t)MAX_SECTORS);
    CHECK_EQ(c.blocks, (uint32_t)MAX_SECTORS);
    CHECK_EQ(c.partial, 0);

    REQUIRE(nvme_split_next(0, MAX_SECTORS, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, (uint32_t)MAX_SECTORS);
    CHECK_EQ(c.blocks, (uint32_t)MAX_SECTORS / 8);

    REQUIRE(nvme_split_next(0, 100, 3, 100, &c) == 1);
    CHECK_EQ(c.sectors, 96u);
    REQUIRE(nvme_split_next(0, 96, 3, 100, &c) == 1);
    CHECK_EQ(c.sectors, 96u);
    CHECK_EQ(c.blocks, 12u);
}

TEST(nvme_split, the_bounce_bound_is_pinned_from_both_sides) {
    nvme_chunk_t c;
    REQUIRE(nvme_split_next(0, MAX_SECTORS - 1, 0, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, (uint32_t)MAX_SECTORS - 1);
    REQUIRE(nvme_split_next(0, MAX_SECTORS + 1, 0, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, (uint32_t)MAX_SECTORS);

    REQUIRE(nvme_split_next(0, MAX_SECTORS - 8, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, (uint32_t)MAX_SECTORS - 8);
    REQUIRE(nvme_split_next(0, MAX_SECTORS + 8, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, (uint32_t)MAX_SECTORS);
}
