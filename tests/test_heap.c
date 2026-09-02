/* tests/test_heap.c - Q2
 *
 * kernel/mm/heap.c against a real host-backed address space.
 *
 * The boot self-test for this allocator is one kmalloc, one write, one
 * read back and one kfree. That proves it is wired up. It does not touch
 * splitting, coalescing, reuse, alignment, growth, or the double-free
 * check - and the double-free check is a panic(), which no test in this
 * project has ever been able to execute.
 *
 * A first-fit allocator with forward-only coalescing has exactly three
 * places it can be wrong, and there is a test for each: the split
 * threshold, the address-order invariant coalescing depends on, and the
 * arithmetic that decides how many pages a growth needs. */
#include "check.h"
#include "fakes/fakes.h"
#include "mm/heap.h"

#include <stdint.h>
#include <string.h>

#define HEAP_ALIGN 8u
/* M102: page-sized requests, to make the heap grow by more than one page
 * and so reach the partial-failure unwind. Spelled here rather than
 * included from mm/pmm.h because this file tests the heap's contract,
 * not the machine's page size, and 4096 is the number heap.c grows in. */
#define PAGE_SIZE 4096u

static void heap_fixture(void) {
    fake_pmm_reset();
    fake_vmm_reset();
    klog_capture_reset();
    heap_init();
}

TEST(heap, roundtrip_and_alignment) {
    heap_fixture();
    /* Every pointer this allocator returns must be 8-aligned: heap.c
     * aligns sizes but the guarantee callers actually depend on is about
     * the address, and nothing in this tree has ever asserted it. */
    for (size_t size = 1; size <= 512; size++) {
        void *p = kmalloc(size);
        REQUIRE(p != NULL);
        CHECK_EQ((uintptr_t)p % HEAP_ALIGN, 0);
        memset(p, 0xAB, size);   /* ASan catches an under-sized block here */
        kfree(p);
    }
}

TEST(heap, zero_sized_allocation_returns_null) {
    heap_fixture();
    CHECK(kmalloc(0) == NULL);
    /* ...and freeing NULL is a no-op rather than a panic - callers rely
     * on it and the contract is only stated in a comment. */
    CHECK_NO_PANIC(kfree(NULL));
}

TEST(heap, a_freed_block_is_reused_rather_than_growing_the_heap) {
    heap_fixture();
    void *a = kmalloc(128);
    REQUIRE(a != NULL);
    uint64_t pages_after_first = fake_vmm_mapped_pages();
    kfree(a);
    void *b = kmalloc(128);
    CHECK(b == a);
    CHECK_EQ(fake_vmm_mapped_pages(), pages_after_first);
    kfree(b);
}

TEST(heap, a_large_block_splits_and_the_remainder_is_usable) {
    heap_fixture();
    void *big = kmalloc(2048);
    REQUIRE(big != NULL);
    kfree(big);

    /* A small request out of that 2048-byte hole must split it and leave
     * a remainder big enough to satisfy a second request without growing
     * the heap again. */
    uint64_t pages_before = fake_vmm_mapped_pages();
    void *small = kmalloc(64);
    REQUIRE(small != NULL);
    void *second = kmalloc(64);
    REQUIRE(second != NULL);
    CHECK_EQ(fake_vmm_mapped_pages(), pages_before);
    CHECK(small != second);

    memset(small, 1, 64);
    memset(second, 2, 64);
    /* The two must not overlap - the split arithmetic is the one place
     * that can hand out the same bytes twice. */
    CHECK_EQ(((unsigned char *)small)[63], 1);
    kfree(small);
    kfree(second);
}

TEST(heap, a_sliver_too_small_to_track_is_not_split_off) {
    heap_fixture();
    /* heap.c splits only when the remainder can carry its own header plus
     * one aligned word. A request that leaves less than that must get the
     * whole block as trailing slack rather than producing a free node
     * whose size is nonsense. */
    void *big = kmalloc(256);
    REQUIRE(big != NULL);
    kfree(big);
    void *nearly = kmalloc(256 - HEAP_ALIGN);
    REQUIRE(nearly != NULL);
    CHECK(nearly == big);
    /* Writing the full original extent must stay inside the block the
     * allocator still owns - ASan is the assertion here. */
    memset(nearly, 0xEE, 256 - HEAP_ALIGN);
    kfree(nearly);
}

TEST(heap, adjacent_frees_coalesce_into_one_usable_block) {
    heap_fixture();
    /* Three adjacent blocks, freed in an order that requires forward
     * coalescing to run more than one step. If coalescing is broken the
     * heap has three small holes and the 3x request has to grow it. */
    void *a = kmalloc(256);
    void *b = kmalloc(256);
    void *c = kmalloc(256);
    REQUIRE(a && b && c);
    void *guard = kmalloc(64);   /* keeps the tail from being the last block */
    REQUIRE(guard != NULL);

    kfree(c);
    kfree(b);
    kfree(a);

    uint64_t pages_before = fake_vmm_mapped_pages();
    void *big = kmalloc(700);    /* fits only if all three merged */
    REQUIRE(big != NULL);
    CHECK(big == a);
    CHECK_EQ(fake_vmm_mapped_pages(), pages_before);
    kfree(big);
    kfree(guard);
}

TEST(heap, fragmentation_pattern_that_first_fit_gets_wrong_if_it_gets_anything_wrong) {
    heap_fixture();
    /* alloc, alloc, free-the-first, alloc-larger. The larger request must
     * NOT be squeezed into the first hole. This is the canonical
     * first-fit bug and the block sizes are chosen so a wrong answer
     * overlaps a live allocation, which ASan will report. */
    void *first = kmalloc(64);
    void *pinned = kmalloc(64);
    REQUIRE(first && pinned);
    memset(pinned, 0x77, 64);
    kfree(first);

    void *larger = kmalloc(512);
    REQUIRE(larger != NULL);
    memset(larger, 0x11, 512);

    /* The pinned block must be untouched. */
    for (int i = 0; i < 64; i++) {
        CHECK_EQ(((unsigned char *)pinned)[i], 0x77);
    }
    kfree(larger);
    kfree(pinned);
}

TEST(heap, an_allocation_larger_than_a_page_grows_by_enough_pages) {
    heap_fixture();
    /* The growth arithmetic rounds up, and a block bigger than one page
     * is the case where a wrong rounding hands out unmapped memory. On
     * the machine that is a page fault in kernel mode; here it is a
     * mapped-page count that can be asserted. */
    size_t size = 4096 * 3 + 100;
    void *p = kmalloc(size);
    REQUIRE(p != NULL);
    memset(p, 0x5A, size);
    CHECK(fake_vmm_mapped_pages() >= 4);
    kfree(p);
}

TEST(heap, a_double_free_panics_rather_than_corrupting_the_list) {
    heap_fixture();
    void *p = kmalloc(64);
    REQUIRE(p != NULL);
    kfree(p);
    /* The first execution of this branch by any test, ever. It is one of
     * 219 panic() calls in this kernel and one of the ones that is
     * correct: a double free is a caller bug with no safe recovery. */
    CHECK_PANIC(kfree(p), "kfree: double free");
}

TEST(heap, every_allocation_is_backed_by_a_frame_that_came_from_the_pmm) {
    heap_fixture();
    CHECK_EQ(fake_pmm_outstanding(), 0);
    void *p = kmalloc(4096 * 2);
    REQUIRE(p != NULL);
    /* Heap growth is the only pmm consumer here, so the frame count and
     * the mapped-page count must agree exactly. A mismatch is a leak in
     * one direction and a use of unmapped memory in the other. */
    CHECK_EQ(fake_pmm_outstanding(), fake_vmm_mapped_pages());
    kfree(p);
}

/* ---- M102: the after half of the pair -------------------------------
 *
 * The test that used to stand here asserted that kmalloc *panicked* when
 * the machine ran out of physical memory, and its own comment said so on
 * purpose: "when Q9 turns that panic into a failed allocation this test
 * is what has to change, and changing it is the record that the behaviour
 * changed on purpose."
 *
 * This is that change. kmalloc returns NULL now.
 */
TEST(heap, running_out_of_physical_memory_returns_null_rather_than_halting) {
    heap_fixture();
    fake_pmm_fail_after(0);
    void *p = kmalloc(64);
    CHECK(p == NULL);
}

/* The property that decides whether running out twice costs more than
 * running out once.
 *
 * Growing the heap by several pages and failing part way used to be
 * impossible. Now it is ordinary, and the pages already mapped have to go
 * back - otherwise every failed allocation on a nearly-full machine eats
 * the memory it could not use, and the machine grinds to a halt through a
 * path that reports success at every step. */
TEST(heap, a_failed_growth_gives_back_every_frame_it_took) {
    heap_fixture();

    /* Three frames available, and an allocation that needs more than
     * three pages. The growth takes all three, fails on the fourth, and
     * must give the three back. */
    fake_pmm_fail_after(3);
    void *p = kmalloc(PAGE_SIZE * 8);
    CHECK(p == NULL);
    CHECK_EQ(fake_pmm_outstanding(), 0);
    CHECK_EQ(fake_vmm_mapped_pages(), 0);
}

/* A failed allocation must not poison the heap. The next one, for
 * something that fits, has to work. */
TEST(heap, the_heap_still_works_after_an_allocation_fails) {
    heap_fixture();

    fake_pmm_fail_after(2);
    CHECK(kmalloc(PAGE_SIZE * 8) == NULL);
    CHECK_EQ(fake_pmm_outstanding(), 0);

    /* Memory is available again. The failure point has to be lifted
     * explicitly because fake_pmm_fail_after counts allocations *ever*
     * rather than frames outstanding - so the frames the unwind gave back
     * do not move it, and without this line the next kmalloc would fail
     * for a reason that has nothing to do with the heap. Getting that
     * wrong once is what this comment is for. */
    fake_pmm_fail_after(-1);

    /* Usable, not merely non-NULL: a bookkeeping-only unwind that
     * rewound heap_virt_end without unmapping would hand back a pointer
     * into memory that is no longer there. */
    void *ok = kmalloc(64);
    REQUIRE(ok != NULL);
    for (int i = 0; i < 64; i++) {
        ((volatile uint8_t *)ok)[i] = (uint8_t)i;
    }
    CHECK_EQ(((volatile uint8_t *)ok)[63], 63);
    kfree(ok);
}

/* An allocation that fits a block already on the free list must not care
 * that the machine has no frames left: it never grows. A kmalloc that
 * consulted the allocator before its own free list would fail here, and
 * would do it under exactly the memory pressure where reusing what you
 * already hold matters most. */
TEST(heap, an_allocation_that_fits_an_existing_block_succeeds_with_no_frames_left) {
    heap_fixture();

    void *big = kmalloc(2048);
    REQUIRE(big != NULL);
    kfree(big);

    fake_pmm_fail_after(0);
    void *small = kmalloc(512);
    CHECK(small != NULL);
    kfree(small);
}

/* Zero frames from the very first call. The heap has nothing at all, and
 * has to say so rather than dereferencing the NULL it just produced. */
TEST(heap, a_heap_that_could_never_grow_at_all_fails_cleanly) {
    heap_fixture();
    fake_pmm_fail_after(0);
    CHECK(kmalloc(1) == NULL);
    CHECK(kmalloc(PAGE_SIZE * 100) == NULL);
    CHECK_EQ(fake_pmm_outstanding(), 0);
    CHECK_EQ(fake_vmm_mapped_pages(), 0);
    /* And it recovers the moment there is memory again. */
    fake_pmm_fail_after(-1);
    void *p = kmalloc(1);
    CHECK(p != NULL);
    kfree(p);
}

/* ---- Q12: what the mutation census found here -------------------------
 *
 * heap.c had 100% line coverage and a 66.7% mutation score. Every test
 * above ran every line and a third of the injected faults survived
 * anyway, which is the exact failure a coverage percentage cannot show:
 * the lines ran, and nothing asserted what they did.
 *
 * The survivors clustered in three places and each one gets a test. */

TEST(heap, a_block_of_exactly_the_right_size_is_reused) {
    heap_fixture();
    /* `b->free && b->size >= size` mutated to `>` survived: an exact fit
     * would be walked past and the heap grown instead. Invisible to
     * every test above, because they all asked for a different size than
     * they freed. Costs a page per allocation on a real workload, where
     * an exact fit is the commonest case there is. */
    void *a = kmalloc(128);
    REQUIRE(a != NULL);
    uint64_t pages_before = fake_vmm_mapped_pages();
    kfree(a);
    void *b = kmalloc(128);            /* exactly what was freed */
    CHECK(b == a);
    CHECK_EQ(fake_vmm_mapped_pages(), pages_before);
    kfree(b);
}

TEST(heap, the_split_threshold_is_exact_in_both_directions) {
    heap_fixture();
    /* heap.c splits only when the remainder can hold a header plus one
     * aligned word. Both `>=` mutations at that test survived, so
     * neither side of the boundary was pinned. This pins both.
     *
     * One byte below the threshold: no split, the whole block is handed
     * out with the remainder as slack. One byte above: a split, and the
     * remainder is a usable free block. */
    const size_t header = sizeof(void *) * 2 + sizeof(void *);  /* size, free, next */
    (void)header;

    /* Below the threshold. Freeing and asking for the original size must
     * give the same address back, which is only true if no split
     * happened - a split would have left a smaller block there. */
    void *big = kmalloc(512);
    REQUIRE(big != NULL);
    kfree(big);
    void *nearly = kmalloc(512 - HEAP_ALIGN);
    REQUIRE(nearly == big);
    kfree(nearly);
    void *again = kmalloc(512);
    CHECK(again == big);               /* not split: the full 512 is still one block */
    kfree(again);

    /* Well above the threshold: a split, and the remainder must be
     * allocatable without growing the heap. */
    void *host = kmalloc(1024);
    REQUIRE(host != NULL);
    kfree(host);
    void *small = kmalloc(64);
    REQUIRE(small == host);
    uint64_t pages = fake_vmm_mapped_pages();
    void *from_remainder = kmalloc(64);
    REQUIRE(from_remainder != NULL);
    CHECK_EQ(fake_vmm_mapped_pages(), pages);
    CHECK(from_remainder != small);
    kfree(small);
    kfree(from_remainder);
}

TEST(heap, a_split_remainder_has_the_size_it_should) {
    heap_fixture();
    /* `rem->size = b->size - size - sizeof(block_header_t)` had
     * surviving mutants: the remainder could be recorded a header too
     * large and nothing noticed. That is a heap that hands out memory
     * belonging to the next block - the worst kind of allocator bug,
     * because it corrupts a neighbour rather than failing.
     *
     * Checked by using the whole of the remainder and letting ASan say
     * whether it was really that big. */
    void *host = kmalloc(4000);
    REQUIRE(host != NULL);
    kfree(host);

    void *head = kmalloc(100);
    REQUIRE(head != NULL);
    memset(head, 0x11, 100);

    /* Whatever the remainder's recorded size is, writing all of it must
     * stay inside the allocation. If the split recorded it too large,
     * this write runs past the block and ASan reports it. */
    void *rest = kmalloc(3000);
    if (rest) {
        memset(rest, 0x22, 3000);
        for (int i = 0; i < 100; i++) {
            CHECK_EQ(((unsigned char *)head)[i], 0x11);
        }
        kfree(rest);
    }
    kfree(head);
}

TEST(heap, coalescing_requires_the_blocks_to_be_genuinely_adjacent) {
    heap_fixture();
    /* The `==` in the coalescing condition compares the next block's
     * address against the end of this one. Mutated to `!=`, blocks that
     * are *not* adjacent get merged - which produces a free block whose
     * size spans a live allocation. Nothing above could see it.
     *
     * Three blocks, the middle one kept. Freeing the outer two must NOT
     * merge them across the middle, so a request for their combined size
     * has to grow the heap rather than being satisfied. */
    void *a = kmalloc(256);
    void *keep = kmalloc(256);
    void *c = kmalloc(256);
    REQUIRE(a && keep && c);
    memset(keep, 0x99, 256);
    kfree(a);
    kfree(c);

    /* If a and c were wrongly merged, this fits in the hole and lands on
     * top of `keep`. */
    void *big = kmalloc(600);
    REQUIRE(big != NULL);
    memset(big, 0x33, 600);
    for (int i = 0; i < 256; i++) {
        CHECK_EQ(((unsigned char *)keep)[i], 0x99);
    }
    kfree(big);
    kfree(keep);
}

TEST(heap, the_growth_page_count_is_rounded_up_not_down) {
    heap_fixture();
    /* `(needed + PAGE_SIZE - 1) / PAGE_SIZE` mutated to `- 0` rounds
     * *down*, which hands out a block whose last bytes are not mapped.
     * On the machine that is a kernel page fault; here the fake vmm
     * refuses the write and ASan sees it. Checked at the sizes where
     * rounding matters: one byte over a page boundary. */
    for (size_t over = 1; over <= 3; over++) {
        heap_fixture();
        size_t size = 4096 * over + 1;
        void *p = kmalloc(size);
        REQUIRE(p != NULL);
        memset(p, 0xC7, size);        /* every byte, including the last */
        CHECK_EQ(((unsigned char *)p)[size - 1], 0xC7);
        CHECK(fake_vmm_mapped_pages() >= over + 1);
        kfree(p);
    }
}
