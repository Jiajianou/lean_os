#include "check.h"
#include "fakes/fakes.h"

#include <string.h>

#define malloc lean_malloc
#define free lean_free
#define calloc lean_calloc
#define realloc lean_realloc
#define malloc_usable_size lean_malloc_usable_size
#define posix_memalign lean_posix_memalign
#define aligned_alloc lean_aligned_alloc
#include "../user_space/library/malloc.c"
#undef malloc
#undef free
#undef calloc
#undef realloc
#undef malloc_usable_size
#undef posix_memalign
#undef aligned_alloc

static void malloc_reset(void) {
    fake_user_heap_reset();
    heap_head = (block_header_t *)0;
    heap_tail = (block_header_t *)0;
    for (int i = 0; i < NBINS; i++) {
        bins[i] = (block_header_t *)0;
    }
    fake_user_sbrk_refuse(0);
}

static void check_lists(void) {
    int free_on_chain = 0;
    block_header_t *last = (block_header_t *)0;
    for (block_header_t *b = heap_head; b; b = b->next) {
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
        block_header_t *previous = (block_header_t *)0;
        for (block_header_t *b = bins[i]; b; b = LINKS(b)->fnext) {
            CHECK(b->is_free);
            CHECK_EQ(bin_of(b->size), i);
            CHECK_EQ(LINKS(b)->fprev, previous);
            previous = b;
            free_in_bins++;
            CHECK(free_in_bins < 100000);
        }
    }
    CHECK_EQ(free_in_bins, free_on_chain);
}

TEST(malloc, a_block_is_usable_aligned_and_its_own) {
    malloc_reset();
    void *a = lean_malloc(64);
    void *b = lean_malloc(64);
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);
    CHECK(a != b);
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
    CHECK_EQ(b, a);
    lean_free(b);
    check_lists();
}

TEST(malloc, a_small_request_does_not_get_a_huge_block_when_a_small_one_fits) {
    malloc_reset();
    void *big = lean_malloc(4096);
    void *guard = lean_malloc(64);
    void *small = lean_malloc(32);
    REQUIRE(big != NULL);
    REQUIRE(guard != NULL);
    REQUIRE(small != NULL);
    lean_free(small);
    lean_free(big);
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
    void *again = lean_malloc(64);
    CHECK_EQ(again, keep);
    void *huge = lean_malloc(32 * 1024);
    CHECK_EQ(huge, NULL);
    lean_free(again);
    check_lists();
}

TEST(malloc, free_of_null_is_nothing) {
    malloc_reset();
    lean_free(NULL);
    check_lists();
}

TEST(malloc, a_large_allocation_comes_from_its_own_mapping_and_goes_back) {
    malloc_reset();
    size_t before = fake_user_heap_used();
    void *big = lean_malloc(256 * 1024);
    REQUIRE(big != NULL);
    memset(big, 0x11, 256 * 1024);
    CHECK_EQ(fake_user_heap_used(), before);
    lean_free(big);
    check_lists();
}

TEST(malloc, every_size_comes_back_aligned_for_any_type) {
    malloc_reset();
    for (size_t size = 1; size <= 512; size++) {
        void *p = lean_malloc(size);
        REQUIRE(p != NULL);
        CHECK_EQ(((unsigned long)p) % 16, 0u);
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
    malloc_reset();
    void *big = lean_malloc(4000);
    REQUIRE(big != NULL);
    lean_free(big);
    void *previous = NULL;
    for (size_t size = 1; size <= 200; size += 7) {
        void *p = lean_malloc(size);
        REQUIRE(p != NULL);
        CHECK_EQ(((unsigned long)p) % 16, 0u);
        CHECK(p != previous);
        previous = p;
    }
    check_lists();
}

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
    CHECK_EQ(b, a);
    lean_free(b);
    for (int i = 0; i < 10000; i++) {
        void *p = lean_malloc(0);
        REQUIRE(p != NULL);
        lean_free(p);
    }
    check_lists();
}

TEST(malloc, posix_memalign_returns_the_alignment_it_was_asked_for) {
    malloc_reset();
    static const size_t ALIGNMENTS[] = {32, 64, 128, 256, 512, 1024, 4096, 65536};
    void *held[sizeof(ALIGNMENTS) / sizeof(ALIGNMENTS[0])];
    for (unsigned i = 0; i < sizeof(ALIGNMENTS) / sizeof(ALIGNMENTS[0]); i++) {
        void *p = (void *)0;
        CHECK_EQ(lean_posix_memalign(&p, ALIGNMENTS[i], 100), 0);
        REQUIRE(p != NULL);
        CHECK_EQ(((unsigned long)p) % ALIGNMENTS[i], 0u);
        CHECK(lean_malloc_usable_size(p) >= 100);
        memset(p, 0xC3, 100);
        held[i] = p;
        check_lists();
    }
    for (unsigned i = 0; i < sizeof(ALIGNMENTS) / sizeof(ALIGNMENTS[0]); i++) {
        CHECK_EQ(((unsigned char *)held[i])[99], 0xC3);
        lean_free(held[i]);
        check_lists();
    }
}

TEST(malloc, the_front_an_aligned_block_leaves_behind_is_handed_out_again) {
    malloc_reset();
    void *p = (void *)0;
    REQUIRE(lean_posix_memalign(&p, 4096, 64) == 0);
    check_lists();
    void *front = lean_malloc(16);
    REQUIRE(front != NULL);
    CHECK((unsigned char *)front < (unsigned char *)p);
    lean_free(front);
    lean_free(p);
    check_lists();
}

TEST(malloc, an_aligned_block_survives_being_freed_and_asked_for_again) {
    malloc_reset();
    for (int round = 0; round < 500; round++) {
        void *p = (void *)0;
        REQUIRE(lean_posix_memalign(&p, 256, 300) == 0);
        CHECK_EQ(((unsigned long)p) % 256u, 0u);
        memset(p, round & 0xFF, 300);
        lean_free(p);
    }
    check_lists();
}

TEST(malloc, posix_memalign_refuses_an_alignment_that_is_not_a_power_of_two) {
    malloc_reset();
    void *p = (void *)0xDEAD;
    CHECK_EQ(lean_posix_memalign(&p, 24, 16), EINVAL);
    CHECK_EQ(lean_posix_memalign(&p, 1, 16), EINVAL);
    CHECK_EQ(lean_posix_memalign(&p, 0, 16), EINVAL);
    CHECK_EQ(lean_posix_memalign((void **)0, 64, 16), EINVAL);
    check_lists();
}

TEST(malloc, a_small_alignment_is_the_ordinary_allocator) {
    malloc_reset();
    void *p = (void *)0;
    CHECK_EQ(lean_posix_memalign(&p, 8, 40), 0);
    REQUIRE(p != NULL);
    CHECK_EQ(((unsigned long)p) % 16u, 0u);
    lean_free(p);
    void *q = lean_aligned_alloc(16, 40);
    REQUIRE(q != NULL);
    CHECK_EQ(((unsigned long)q) % 16u, 0u);
    lean_free(q);
    check_lists();
}

TEST(malloc, posix_memalign_reports_a_refused_heap_rather_than_returning_null) {
    malloc_reset();
    fake_user_sbrk_refuse(1);
    void *p = (void *)0;
    CHECK_EQ(lean_posix_memalign(&p, 4096, 64), ENOMEM);
    CHECK_EQ(lean_aligned_alloc(4096, 64), NULL);
    CHECK_EQ(lean_posix_memalign(&p, 16, 64), ENOMEM);
    CHECK_EQ(lean_aligned_alloc(8, 64), NULL);
    fake_user_sbrk_refuse(0);
    check_lists();
}

TEST(malloc, a_zero_byte_aligned_request_is_nothing_rather_than_a_block) {
    malloc_reset();
    void *p = (void *)0xDEAD;
    CHECK_EQ(lean_posix_memalign(&p, 4096, 0), 0);
    CHECK_EQ(p, NULL);
    p = (void *)0xDEAD;
    CHECK_EQ(lean_posix_memalign(&p, 16, 0), 0);
    CHECK_EQ(p, NULL);
    CHECK_EQ(lean_aligned_alloc(4096, 0), NULL);
    CHECK_EQ(lean_aligned_alloc(16, 0), NULL);
    CHECK_EQ(lean_posix_memalign(&p, 4096, 1), 0);
    REQUIRE(p != NULL);
    CHECK_EQ(((unsigned long)p) % 4096u, 0u);
    lean_free(p);
    void *q = lean_aligned_alloc(64, 1);
    REQUIRE(q != NULL);
    CHECK_EQ(((unsigned long)q) % 64u, 0u);
    lean_free(q);
    check_lists();
}

TEST(malloc, carving_a_block_that_is_not_the_tail_leaves_the_tail_alone) {
    malloc_reset();
    void *big = lean_malloc(60000);
    void *tail = lean_malloc(64);
    REQUIRE(big != NULL);
    REQUIRE(tail != NULL);
    lean_free(big);
    check_lists();
    block_header_t *was_tail = heap_tail;

    void *p = (void *)0;
    REQUIRE(lean_posix_memalign(&p, 4096, 64) == 0);
    CHECK_EQ(((unsigned long)p) % 4096u, 0u);
    CHECK((unsigned char *)p < (unsigned char *)tail);
    CHECK_EQ(heap_tail, was_tail);
    check_lists();

    memset(p, 0x5A, 64);
    lean_free(p);
    lean_free(tail);
    check_lists();
}
