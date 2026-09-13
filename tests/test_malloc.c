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
        block_header_t *prev = (block_header_t *)0;
        for (block_header_t *b = bins[i]; b; b = LINKS(b)->fnext) {
            CHECK(b->is_free);
            CHECK_EQ(bin_of(b->size), i);
            CHECK_EQ(LINKS(b)->fprev, prev);
            prev = b;
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
