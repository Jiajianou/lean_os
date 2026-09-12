/* tests/test_malloc.c - M98
 *
 * The user-space allocator, off the machine.
 *
 * ---- why this file exists, which is a measurement -----------------------
 *
 * M98 profiled a C++ compile ON this machine with M101's sampler. 72% of
 * every non-idle sample landed on two adjacent instructions in
 * user_space/lib/malloc.c: the first-fit loop, which walked a list of
 * every block the process had ever allocated. GCC makes hundreds of
 * thousands of small allocations, so that walk was quadratic and the
 * compiler spent most of its life in it.
 *
 * The fix - free blocks on their own size-binned lists, threaded through
 * the free block's own payload - is the kind of change that either works
 * or hands out a pointer into the middle of a live allocation. A bug
 * like that on this machine presents as a compiler that dies somewhere
 * else, twenty minutes into a boot. So the allocator joins the
 * scheduler, the heap and leanfs on the host tier.
 *
 * ---- how it is compiled -------------------------------------------------
 *
 * By including the source, with malloc/free/realloc renamed. The test
 * binary is linked against the HOST's libc and every other test in it
 * calls the host's malloc; linking a second definition of the name would
 * hijack all of them, which is exactly what happened on the first
 * attempt (ASan caught fake_pmm.c's free landing in this allocator). The
 * renaming keeps this allocator addressable and nobody else's.
 */
#include "check.h"
#include "fakes/fakes.h"

#include <string.h>

#define malloc lean_malloc
#define free lean_free
#define calloc lean_calloc
#define realloc lean_realloc
#define malloc_usable_size lean_malloc_usable_size
#include "../user_space/lib/malloc.c"
#undef malloc
#undef free
#undef calloc
#undef realloc
#undef malloc_usable_size

/* The allocator keeps its state in file statics, and each test starts
 * from a fresh heap - which means resetting both sides: the fake sbrk
 * arena, and the lists that point into the arena it just threw away. */
static void malloc_reset(void) {
    fake_user_heap_reset();
    heap_head = (block_header_t *)0;
    heap_tail = (block_header_t *)0;
    for (int i = 0; i < NBINS; i++) {
        bins[i] = (block_header_t *)0;
    }
    fake_user_sbrk_refuse(0);
}

/* Every block on the address-ordered chain, checked for the invariants
 * the bins depend on. Called at the end of the tests that churn, because
 * the failure mode this file exists to catch is a list that is wrong in
 * a way no single allocation notices. */
static void check_lists(void) {
    int free_on_chain = 0;
    block_header_t *last = (block_header_t *)0;
    for (block_header_t *b = heap_head; b; b = b->next) {
        /* Address order, which is what free()'s coalesce assumes. */
        if (last) {
            CHECK((unsigned char *)b > (unsigned char *)last);
        }
        if (b->is_free) {
            free_on_chain++;
        }
        last = b;
    }
    CHECK_EQ(last, heap_tail);

    int free_in_bins = 0;
    for (int i = 0; i < NBINS; i++) {
        block_header_t *prev = (block_header_t *)0;
        for (block_header_t *b = bins[i]; b; b = LINKS(b)->fnext) {
            CHECK(b->is_free);            /* a bin holds free blocks only */
            CHECK_EQ(bin_of(b->size), i); /* and each one in its own class */
            CHECK_EQ(LINKS(b)->fprev, prev);
            prev = b;
            free_in_bins++;
            CHECK(free_in_bins < 100000); /* a cycle would otherwise hang */
        }
    }
    /* Every free block is on exactly one bin, and nothing else is. */
    CHECK_EQ(free_in_bins, free_on_chain);
}

TEST(malloc, a_block_is_usable_aligned_and_its_own) {
    malloc_reset();
    void *a = lean_malloc(64);
    void *b = lean_malloc(64);
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);
    CHECK(a != b);
    /* 16, not 8. This assertion said 8 from M19 to M114 and that is why
     * the allocator returned 8 - an alignment test that agrees with the
     * bug is worse than no alignment test, because it is evidence. */
    CHECK_EQ(((unsigned long)a) % 16, 0u);
    CHECK_EQ(((unsigned long)b) % 16, 0u);
    memset(a, 0xAA, 64);
    memset(b, 0x55, 64);
    CHECK_EQ(((unsigned char *)a)[63], 0xAA);
    CHECK_EQ(((unsigned char *)b)[0], 0x55);
    lean_free(a);
    lean_free(b);
    check_lists();
}

TEST(malloc, a_freed_block_is_handed_out_again) {
    malloc_reset();
    void *a = lean_malloc(128);
    REQUIRE(a != NULL);
    lean_free(a);
    void *b = lean_malloc(128);
    /* The same block, because it is on the free list of exactly the
     * right size class. Before M98 this was true because the search
     * walked everything; the point of the bins is that it is still true
     * without the walk. */
    CHECK_EQ(b, a);
    lean_free(b);
    check_lists();
}

TEST(malloc, a_small_request_does_not_get_a_huge_block_when_a_small_one_fits) {
    malloc_reset();
    void *big = lean_malloc(4096);
    void *guard = lean_malloc(64); /* keeps the two holes from merging */
    void *small = lean_malloc(32);
    REQUIRE(big != NULL);
    REQUIRE(guard != NULL);
    REQUIRE(small != NULL);
    lean_free(small);
    lean_free(big);
    /* Two holes, 4096 and 32, and they cannot coalesce because `guard`
     * sits between them. 32 bytes must come back from the 32-byte hole
     * rather than by splitting the 4 KiB one - which is what searching
     * from the request's own size class upward means, and what one
     * first-fit list in address order cannot promise: it would find the
     * 4 KiB block first, because it is at the lower address. */
    void *again = lean_malloc(32);
    CHECK_EQ(again, small);
    lean_free(again);
    lean_free(guard);
    check_lists();
}

TEST(malloc, two_adjacent_frees_become_one_block) {
    malloc_reset();
    void *a = lean_malloc(200);
    void *b = lean_malloc(200);
    void *c = lean_malloc(200);
    REQUIRE(a && b && c);
    /* b first, then a, and the order is the test: this allocator
     * coalesces FORWARD only (it has no back pointer and no footer, and
     * milestones.md says so), so freeing a while b is already free is
     * what merges them. The merged block has to take b OFF its bin on
     * the way, or the allocator keeps a free-list pointer into the
     * middle of what is now one block and hands it out later. That is
     * the bug this test is really about. */
    lean_free(b);
    lean_free(a);
    void *big = lean_malloc(400);
    CHECK_EQ(big, a);
    lean_free(big);
    lean_free(c);
    check_lists();
}

TEST(malloc, ten_thousand_allocations_stay_consistent) {
    malloc_reset();
    /* The workload that found the original bug, in miniature: many small
     * allocations, most of them kept, some freed and replaced. What is
     * asserted is not speed - a host test cannot claim that honestly -
     * but that the lists survive it, which is what makes the speed
     * possible. */
    static void *live[2000];
    for (int i = 0; i < 2000; i++) {
        live[i] = lean_malloc(16 + (i % 97));
        REQUIRE(live[i] != NULL);
        memset(live[i], i & 0xFF, 16);
    }
    for (int round = 0; round < 4; round++) {
        for (int i = round; i < 2000; i += 4) {
            lean_free(live[i]);
            live[i] = lean_malloc(16 + ((i * 7) % 89));
            REQUIRE(live[i] != NULL);
            memset(live[i], i & 0xFF, 16);
        }
    }
    for (int i = 0; i < 2000; i++) {
        CHECK_EQ(((unsigned char *)live[i])[0], (unsigned char)(i & 0xFF));
    }
    check_lists();
    for (int i = 0; i < 2000; i++) {
        lean_free(live[i]);
    }
    check_lists();
}

TEST(malloc, a_tiny_request_still_makes_a_block_that_can_be_freed) {
    malloc_reset();
    /* Four bytes is smaller than the two links a free block carries in
     * its own payload. Without the minimum size, freeing this block
     * writes those links past the end of it - which is a heap corruption
     * that shows up somewhere else entirely. */
    void *a = lean_malloc(4);
    REQUIRE(a != NULL);
    CHECK(lean_malloc_usable_size(a) >= 2 * sizeof(void *));
    memset(a, 0xFF, 4);
    lean_free(a);
    check_lists();
}

TEST(malloc, a_heap_that_cannot_grow_returns_null_and_still_works) {
    malloc_reset();
    void *keep = lean_malloc(64);
    REQUIRE(keep != NULL);
    lean_free(keep);
    fake_user_sbrk_refuse(1);
    /* The free list still has the block above, so a request that fits it
     * succeeds even with no memory to be had - which is the property
     * that makes an allocator degrade rather than fail. */
    void *again = lean_malloc(64);
    CHECK_EQ(again, keep);
    /* And one that does not fit fails cleanly rather than returning
     * something. Deliberately below MMAP_THRESHOLD: past it an
     * allocation gets its own mapping and never touches the heap at all
     * (M78), so a megabyte here would be testing mmap rather than the
     * heap that has just been told it cannot grow. */
    void *huge = lean_malloc(32 * 1024);
    CHECK_EQ(huge, NULL);
    lean_free(again);
    check_lists();
}

TEST(malloc, free_of_null_is_nothing) {
    /* This used to assert malloc(0) == NULL as well - the M19 answer
     * written down as law, which M117 found NetSurf could not live with.
     * The zero-byte tests at the end of this file say what it does now. */
    malloc_reset();
    lean_free(NULL);
    check_lists();
}

TEST(malloc, a_large_allocation_comes_from_its_own_mapping_and_goes_back) {
    malloc_reset();
    size_t before = fake_user_heap_used();
    /* Past MMAP_THRESHOLD, so this must not come out of the sbrk heap at
     * all - M78's split, still true after the bins. */
    void *big = lean_malloc(256 * 1024);
    REQUIRE(big != NULL);
    memset(big, 0x11, 256 * 1024);
    CHECK_EQ(fake_user_heap_used(), before);
    lean_free(big);
    check_lists();
}

/* ---- M114: max_align_t, on every path that returns a pointer ----------
 *
 * C requires malloc to return memory aligned for any type; on x86-64
 * that is 16, because the compiler will emit `movaps` against a buffer
 * it believes is suitably aligned and `movaps` faults rather than
 * slowing down. This is not theoretical here - it is what killed the
 * browser partway through a real web page, and the whole diagnosis was
 * three bytes at the faulting rip (`0F 29 06`) and an address ending in
 * 8. See user_space/lib/malloc.c's HEAP_ALIGN comment.
 *
 * Every size rather than a representative few: the failure is a
 * *remainder*, so it appears only at sizes whose rounding lands wrong,
 * and picking sizes by hand is picking which bugs to find.
 */
TEST(malloc, every_size_comes_back_aligned_for_any_type) {
    malloc_reset();
    /* Each size is followed by a second, fixed allocation, and it is
     * the SECOND one that catches the bug: a misaligned block is not
     * misaligned itself - malloc rounds every request up, so the first
     * block is fine - it is the block placed immediately *after* a
     * badly-rounded one that lands wrong. Written first without the
     * pair, where it passed against a deliberately broken HEAP_ALIGN of
     * 8 and said nothing. */
    for (size_t size = 1; size <= 512; size++) {
        void *p = lean_malloc(size);
        REQUIRE(p != NULL);
        CHECK_EQ(((unsigned long)p) % 16, 0u);
        /* Written to, so a wrong answer that happens to be aligned is
         * still a block this test used. */
        memset(p, 0x5A, size);
        void *after = lean_malloc(24);
        REQUIRE(after != NULL);
        CHECK_EQ(((unsigned long)after) % 16, 0u);
        memset(after, 0xA5, 24);
        lean_free(p);
        lean_free(after);
    }
    check_lists();
}

TEST(malloc, a_block_that_gets_its_own_mapping_is_aligned) {
    /* The mmap path (>= MMAP_THRESHOLD) reaches the caller through
     * completely different code from the sbrk path, so "the allocator is
     * aligned" is two claims. */
    malloc_reset();
    for (size_t size = MMAP_THRESHOLD; size < MMAP_THRESHOLD + 64; size++) {
        void *p = lean_malloc(size);
        REQUIRE(p != NULL);
        CHECK_EQ(((unsigned long)p) % 16, 0u);
        lean_free(p);
    }
    check_lists();
}

TEST(malloc, a_block_reused_after_a_free_is_still_aligned) {
    /* Splitting a free block puts the remainder's header at
     * payload + size, so a size that is not a multiple of the alignment
     * misaligns every block after it rather than only its own. */
    malloc_reset();
    void *big = lean_malloc(4000);
    REQUIRE(big != NULL);
    lean_free(big);
    void *prev = NULL;
    for (size_t size = 1; size <= 200; size += 7) {
        void *p = lean_malloc(size);
        REQUIRE(p != NULL);
        CHECK_EQ(((unsigned long)p) % 16, 0u);
        CHECK(p != prev);
        prev = p;
    }
    check_lists();
}

/* M117: malloc(0). C allows NULL or a unique pointer; this returned NULL
 * from M19 on, and NetSurf's flex layout - `calloc(count, ...)` for a
 * container with no children - read that as out of memory and failed
 * silently on the first empty `display: flex` box on apple.com. Every
 * libc NetSurf was ever built against hands back a real block here, so
 * this one does now, and these say what "real" means: distinct from every
 * other live block, aligned like any other, and taken back by free. */
TEST(malloc, a_zero_byte_request_is_a_block_and_not_a_refusal) {
    malloc_reset();
    void *a = lean_malloc(0);
    void *b = lean_malloc(0);
    void *c = lean_malloc(1);
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);
    REQUIRE(c != NULL);
    CHECK(a != b);
    CHECK(b != c);
    CHECK(a != c);
    CHECK_EQ(((unsigned long)a) % 16, 0u);
    CHECK(lean_malloc_usable_size(a) >= 1);
    lean_free(a);
    lean_free(b);
    lean_free(c);
    check_lists();
}

TEST(malloc, zero_byte_blocks_are_freed_and_reused_like_any_other) {
    malloc_reset();
    void *a = lean_malloc(0);
    REQUIRE(a != NULL);
    lean_free(a);
    void *b = lean_malloc(0);
    CHECK_EQ(b, a); /* the same minimum block, handed out again */
    lean_free(b);
    /* Ten thousand of them in a row leak nothing: the heap after is one
     * free run, the way the larger test above ends. */
    for (int i = 0; i < 10000; i++) {
        void *p = lean_malloc(0);
        REQUIRE(p != NULL);
        lean_free(p);
    }
    check_lists();
}
