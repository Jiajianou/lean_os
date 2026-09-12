/* tests/test_nvme_split.c - M107
 *
 * The one path in the NVMe driver that QEMU cannot reach.
 *
 * QEMU's emulated namespace has 512-byte logical blocks, so `shift` is
 * zero on the only machine this project has ever booted, every request is
 * already aligned, and the read-modify-write branch below is dead code.
 * A 4Kn drive - one whose logical block IS 4096 bytes - is an ordinary
 * thing to find in a laptop, and the first time that branch would run is
 * on hardware, on somebody's filesystem, with no serial log of what it
 * did to it.
 *
 * So this is a property test rather than a set of examples: it walks
 * every request length up to a few blocks at every alignment, for both
 * block sizes, and checks that the chunks TILE the request - in order,
 * no gap, no overlap, nothing outside it. A splitter that drops a sector
 * silently corrupts a file; one that overlaps writes a sector twice and
 * looks fine until two of them are on either side of a block boundary.
 *
 * It also asserts the thing that would be a silent performance halving
 * rather than a correctness bug: on a 512-byte namespace there must be no
 * partial chunks at all, because a read-modify-write per sector would
 * turn every write on the machine into three commands.
 */
#include "check.h"

#include "drivers/nvme_split.h"

#include <stdint.h>
#include <string.h>

#define MAX_SECTORS 128 /* what nvme.c's bounce buffer holds */

/* Walks one request to completion, asserting every property on the way.
 * Returns the number of chunks it took. */
static int walk(uint64_t lba, uint32_t count, uint32_t shift, int *saw_partial) {
    uint32_t per_block = 1u << shift;
    uint64_t cursor = lba;
    uint32_t left = count;
    int chunks = 0;
    *saw_partial = 0;

    while (left > 0) {
        nvme_chunk_t c;
        REQUIRE(nvme_split_next(cursor, left, shift, MAX_SECTORS, &c) == 1);
        REQUIRE(c.sectors > 0);          /* or the loop never ends */
        REQUIRE(c.sectors <= left);      /* never past the end of the request */

        /* The chunk must start exactly where the last one stopped: the
         * device block it names, plus the offset into it, is the caller's
         * current LBA. */
        CHECK_EQ(c.block * per_block + c.offset, cursor);

        if (c.partial) {
            *saw_partial = 1;
            CHECK_EQ(c.blocks, 1u);
            /* A partial chunk must stay inside its one block. */
            CHECK(c.offset + c.sectors <= per_block);
        } else {
            CHECK_EQ(c.offset, 0u);
            CHECK_EQ(c.sectors, c.blocks * per_block); /* whole blocks only */
            CHECK(c.sectors <= MAX_SECTORS);
            CHECK(c.blocks > 0);
        }

        cursor += c.sectors;
        left -= c.sectors;
        chunks++;
        REQUIRE(chunks < 4096); /* a splitter that stalls must fail, not hang */
    }
    CHECK_EQ(cursor, lba + count);
    return chunks;
}

TEST(nvme_split, an_empty_request_produces_nothing) {
    nvme_chunk_t c;
    CHECK_EQ(nvme_split_next(0, 0, 0, MAX_SECTORS, &c), 0);
    CHECK_EQ(nvme_split_next(12345, 0, 3, MAX_SECTORS, &c), 0);
}

/* The 512-byte case: every request must go straight through, whatever its
 * alignment, and nothing may be partial. */
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

/* The 4Kn case, exhaustively over alignments and lengths. This is the
 * body of the file. */
TEST(nvme_split, a_4k_namespace_tiles_every_request_exactly) {
    for (uint64_t lba = 0; lba < 40; lba++) {
        for (uint32_t count = 1; count <= 300; count++) {
            int partial = 0;
            walk(lba, count, 3, &partial);
        }
    }
}

TEST(nvme_split, a_4k_namespace_reads_to_modify_and_write_only_at_the_edges) {
    /* Aligned at both ends: no partial chunk anywhere. */
    int partial = 1;
    walk(0, 8, 3, &partial);
    CHECK_EQ(partial, 0);
    partial = 1;
    walk(16, 64, 3, &partial);
    CHECK_EQ(partial, 0);

    /* A head that starts inside a block. */
    partial = 0;
    walk(1, 8, 3, &partial);
    CHECK_EQ(partial, 1);

    /* A tail shorter than a block. */
    partial = 0;
    walk(0, 9, 3, &partial);
    CHECK_EQ(partial, 1);

    /* A single sector in the middle of a block is one partial chunk and
     * nothing else. */
    nvme_chunk_t c;
    REQUIRE(nvme_split_next(5, 1, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.partial, 1);
    CHECK_EQ(c.block, 0u);
    CHECK_EQ(c.offset, 5u);
    CHECK_EQ(c.sectors, 1u);
    CHECK_EQ(c.blocks, 1u);
}

/* A request that crosses one block boundary by a sector at each end is
 * the shape that catches an off-by-one in either direction: head, middle,
 * tail, and the middle must be exactly the whole blocks between them. */
TEST(nvme_split, a_head_a_middle_and_a_tail) {
    int partial = 0;
    /* LBA 7 for 18 sectors on a 4K namespace: 1 sector to finish block 0,
     * 16 sectors (2 whole blocks), then 1 sector of block 3. */
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

/* The bounce-buffer bound must be rounded DOWN to a whole number of
 * device blocks. A bound that split a block would hand the controller a
 * command for part of a block, which it refuses - and the refusal would
 * only ever happen on a 4Kn drive with a max_sectors that is not a
 * multiple of eight. */
TEST(nvme_split, the_bounce_bound_is_rounded_down_to_whole_blocks) {
    nvme_chunk_t c;
    /* 100 sectors of bounce on a 4K namespace is 12.5 blocks. */
    REQUIRE(nvme_split_next(0, 1000, 3, 100, &c) == 1);
    CHECK_EQ(c.partial, 0);
    CHECK_EQ(c.sectors, 96u); /* 12 whole blocks */
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

/* A 64-bit LBA must not be truncated on the way to a block number. A
 * 32-bit intermediate here would put a write on a 2 TiB disk in the wrong
 * place, silently. */
TEST(nvme_split, a_large_lba_keeps_all_its_bits) {
    nvme_chunk_t c;
    uint64_t lba = 0x1FFFFFFF8ULL; /* past 2^32, block-aligned on a 4K namespace */
    REQUIRE(nvme_split_next(lba, 8, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.partial, 0);
    CHECK_EQ(c.block, lba >> 3);
    CHECK_EQ(c.block * 8, lba);

    REQUIRE(nvme_split_next(lba + 3, 2, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.partial, 1);
    CHECK_EQ(c.block, lba >> 3);
    CHECK_EQ(c.offset, 3u);
}

/* 1024- and 2048-byte logical blocks are legal formats (LBADS 10 and 11)
 * and the driver accepts them, so the splitter has to be right for them
 * too rather than only for the two sizes anybody expects. */
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

/* ---- What the mutation harness found ---------------------------------
 *
 * Two survivors, both boundaries: `if (n > count)` and
 * `if (sectors > max_sectors)`. Every test above walks past those
 * boundaries rather than landing on them, so `>` and `>=` were
 * indistinguishable - and the difference between them is one sector at
 * the end of a request, which is the sector a filesystem keeps its
 * metadata in about as often as any other.
 */

/* Kills: `if (n > count)` -> `n >= count`.
 *
 * A partial chunk that runs to exactly the end of its device block, with
 * the request ending at exactly the same place, is the case where the
 * clamp is and is not needed at once. `n` and `count` are equal here, so
 * a `>=` takes the branch and clamps to a value that is already right -
 * harmless - while the mirror case below is the one that is not. */
TEST(nvme_split, a_partial_chunk_that_ends_exactly_at_the_block_boundary) {
    nvme_chunk_t c;
    /* LBA 5 for 3 sectors on a 4K namespace: sectors 5,6,7 of block 0 -
     * the request ends exactly where the block does. */
    REQUIRE(nvme_split_next(5, 3, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.partial, 1);
    CHECK_EQ(c.block, 0u);
    CHECK_EQ(c.offset, 5u);
    CHECK_EQ(c.sectors, 3u);   /* not clamped to 2, and not extended to 8 */
    CHECK_EQ(c.offset + c.sectors, 8u);

    /* One sector shorter: the clamp must NOT fire, and the chunk must
     * stop where the request does rather than at the block boundary. */
    REQUIRE(nvme_split_next(5, 2, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, 2u);

    /* One sector longer: the chunk must stop at the boundary, and the
     * next call picks up the rest. */
    REQUIRE(nvme_split_next(5, 4, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, 3u);
    REQUIRE(nvme_split_next(8, 1, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.block, 1u);
    CHECK_EQ(c.offset, 0u);
    CHECK_EQ(c.sectors, 1u);
}

/* Kills: `if (sectors > max_sectors)` -> `sectors >= max_sectors`.
 *
 * A request of exactly the bounce buffer's size must go in ONE command.
 * With `>=`, the clamp fires on the equal case and - because it rounds
 * down to whole blocks, which it already is - returns the same number,
 * so on a 4K namespace this mutant is invisible. On a 512-byte one it is
 * also invisible. What makes it visible is a max_sectors that is not a
 * multiple of the block size, where the clamp CHANGES the answer: then
 * taking the branch one case too early shortens a request that did not
 * need shortening. */
TEST(nvme_split, a_request_of_exactly_the_bounce_size_is_one_command) {
    nvme_chunk_t c;
    /* 512-byte namespace, exactly MAX_SECTORS asked for. */
    REQUIRE(nvme_split_next(0, MAX_SECTORS, 0, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, (uint32_t)MAX_SECTORS);
    CHECK_EQ(c.blocks, (uint32_t)MAX_SECTORS);
    CHECK_EQ(c.partial, 0);

    /* 4K namespace, exactly MAX_SECTORS (which is 16 whole blocks). */
    REQUIRE(nvme_split_next(0, MAX_SECTORS, 3, MAX_SECTORS, &c) == 1);
    CHECK_EQ(c.sectors, (uint32_t)MAX_SECTORS);
    CHECK_EQ(c.blocks, (uint32_t)MAX_SECTORS / 8);

    /* And the case that separates > from >=: a bound of 100 sectors on a
     * 4K namespace rounds to 96, so asking for exactly 100 must come
     * back as 96 and asking for exactly 96 must come back as 96 in one
     * piece rather than being re-clamped. */
    REQUIRE(nvme_split_next(0, 100, 3, 100, &c) == 1);
    CHECK_EQ(c.sectors, 96u);
    REQUIRE(nvme_split_next(0, 96, 3, 100, &c) == 1);
    CHECK_EQ(c.sectors, 96u);
    CHECK_EQ(c.blocks, 12u);
}

/* One sector short of the bound, and one over, on both block sizes - so
 * the comparison is pinned from both sides rather than from one. */
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
