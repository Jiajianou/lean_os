#include "check.h"
#include "fakes/fakes.h"
#include "memory_management/heap.h"

#include <stdint.h>
#include <string.h>

#define HEAP_ALIGN 8u
#define PAGE_SIZE 4096u

static void heap_fixture(void) {
    fake_physical_memory_reset();
    fake_virtual_memory_reset();
    kernel_log_capture_reset();
    heap_init();
}

TEST(heap, roundtrip_and_alignment) {
    heap_fixture();
    for (size_t size = 1; size <= 512; size++) {
        void *p = kmalloc(size);
        REQUIRE(p != NULL);
        CHECK_EQ((uintptr_t)p % HEAP_ALIGN, 0);
        memset(p, 0xAB, size);
        kfree(p);
    }
}

TEST(heap, zero_sized_allocation_returns_null) {
    heap_fixture();
    CHECK(kmalloc(0) == NULL);
    CHECK_NO_PANIC(kfree(NULL));
}

TEST(heap, a_freed_block_is_reused_rather_than_growing_the_heap) {
    heap_fixture();
    void *a = kmalloc(128);
    REQUIRE(a != NULL);
    uint64_t pages_after_first = fake_virtual_memory_mapped_pages();
    kfree(a);
    void *b = kmalloc(128);
    CHECK(b == a);
    CHECK_EQ(fake_virtual_memory_mapped_pages(), pages_after_first);
    kfree(b);
}

TEST(heap, a_large_block_splits_and_the_remainder_is_usable) {
    heap_fixture();
    void *big = kmalloc(2048);
    REQUIRE(big != NULL);
    kfree(big);

    uint64_t pages_before = fake_virtual_memory_mapped_pages();
    void *small = kmalloc(64);
    REQUIRE(small != NULL);
    void *second = kmalloc(64);
    REQUIRE(second != NULL);
    CHECK_EQ(fake_virtual_memory_mapped_pages(), pages_before);
    CHECK(small != second);

    memset(small, 1, 64);
    memset(second, 2, 64);
    CHECK_EQ(((unsigned char *)small)[63], 1);
    kfree(small);
    kfree(second);
}

TEST(heap, a_sliver_too_small_to_track_is_not_split_off) {
    heap_fixture();
    void *big = kmalloc(256);
    REQUIRE(big != NULL);
    kfree(big);
    void *nearly = kmalloc(256 - HEAP_ALIGN);
    REQUIRE(nearly != NULL);
    CHECK(nearly == big);
    memset(nearly, 0xEE, 256 - HEAP_ALIGN);
    kfree(nearly);
}

TEST(heap, adjacent_frees_coalesce_into_one_usable_block) {
    heap_fixture();
    void *a = kmalloc(256);
    void *b = kmalloc(256);
    void *c = kmalloc(256);
    REQUIRE(a && b && c);
    void *guard = kmalloc(64);
    REQUIRE(guard != NULL);

    kfree(c);
    kfree(b);
    kfree(a);

    uint64_t pages_before = fake_virtual_memory_mapped_pages();
    void *big = kmalloc(700);
    REQUIRE(big != NULL);
    CHECK(big == a);
    CHECK_EQ(fake_virtual_memory_mapped_pages(), pages_before);
    kfree(big);
    kfree(guard);
}

TEST(heap, fragmentation_pattern_that_first_fit_gets_wrong_if_it_gets_anything_wrong) {
    heap_fixture();
    void *first = kmalloc(64);
    void *pinned = kmalloc(64);
    REQUIRE(first && pinned);
    memset(pinned, 0x77, 64);
    kfree(first);

    void *larger = kmalloc(512);
    REQUIRE(larger != NULL);
    memset(larger, 0x11, 512);

    for (int i = 0; i < 64; i++) {
        CHECK_EQ(((unsigned char *)pinned)[i], 0x77);
    }
    kfree(larger);
    kfree(pinned);
}

TEST(heap, an_allocation_larger_than_a_page_grows_by_enough_pages) {
    heap_fixture();
    size_t size = 4096 * 3 + 100;
    void *p = kmalloc(size);
    REQUIRE(p != NULL);
    memset(p, 0x5A, size);
    CHECK(fake_virtual_memory_mapped_pages() >= 4);
    kfree(p);
}

TEST(heap, a_double_free_panics_rather_than_corrupting_the_list) {
    heap_fixture();
    void *p = kmalloc(64);
    REQUIRE(p != NULL);
    kfree(p);
    CHECK_PANIC(kfree(p), "kfree: double free");
}

TEST(heap, every_allocation_is_backed_by_a_frame_that_came_from_the_physical_memory) {
    heap_fixture();
    CHECK_EQ(fake_physical_memory_outstanding(), 0);
    void *p = kmalloc(4096 * 2);
    REQUIRE(p != NULL);
    CHECK_EQ(fake_physical_memory_outstanding(), fake_virtual_memory_mapped_pages());
    kfree(p);
}

TEST(heap, running_out_of_physical_memory_returns_null_rather_than_halting) {
    heap_fixture();
    fake_physical_memory_fail_after(0);
    void *p = kmalloc(64);
    CHECK(p == NULL);
}

TEST(heap, a_failed_growth_gives_back_every_frame_it_took) {
    heap_fixture();

    fake_physical_memory_fail_after(3);
    void *p = kmalloc(PAGE_SIZE * 8);
    CHECK(p == NULL);
    CHECK_EQ(fake_physical_memory_outstanding(), 0);
    CHECK_EQ(fake_virtual_memory_mapped_pages(), 0);
}

TEST(heap, a_growth_that_cannot_map_gives_back_the_frame_it_had_taken) {
    heap_fixture();

    fake_virtual_memory_fail_map_after(3);
    void *p = kmalloc(PAGE_SIZE * 8);
    CHECK(p == NULL);
    CHECK_EQ(fake_physical_memory_outstanding(), 0);
    CHECK_EQ(fake_virtual_memory_mapped_pages(), 0);

    fake_virtual_memory_fail_map_after(-1);
    void *ok = kmalloc(64);
    REQUIRE(ok != NULL);
    for (int i = 0; i < 64; i++) {
        ((char *)ok)[i] = (char)i;
    }
    for (int i = 0; i < 64; i++) {
        CHECK_EQ(((char *)ok)[i], (char)i);
    }
    kfree(ok);
}

TEST(heap, the_heap_still_works_after_an_allocation_fails) {
    heap_fixture();

    fake_physical_memory_fail_after(2);
    CHECK(kmalloc(PAGE_SIZE * 8) == NULL);
    CHECK_EQ(fake_physical_memory_outstanding(), 0);

    fake_physical_memory_fail_after(-1);

    void *ok = kmalloc(64);
    REQUIRE(ok != NULL);
    for (int i = 0; i < 64; i++) {
        ((volatile uint8_t *)ok)[i] = (uint8_t)i;
    }
    CHECK_EQ(((volatile uint8_t *)ok)[63], 63);
    kfree(ok);
}

TEST(heap, an_allocation_that_fits_an_existing_block_succeeds_with_no_frames_left) {
    heap_fixture();

    void *big = kmalloc(2048);
    REQUIRE(big != NULL);
    kfree(big);

    fake_physical_memory_fail_after(0);
    void *small = kmalloc(512);
    CHECK(small != NULL);
    kfree(small);
}

TEST(heap, a_heap_that_could_never_grow_at_all_fails_cleanly) {
    heap_fixture();
    fake_physical_memory_fail_after(0);
    CHECK(kmalloc(1) == NULL);
    CHECK(kmalloc(PAGE_SIZE * 100) == NULL);
    CHECK_EQ(fake_physical_memory_outstanding(), 0);
    CHECK_EQ(fake_virtual_memory_mapped_pages(), 0);
    fake_physical_memory_fail_after(-1);
    void *p = kmalloc(1);
    CHECK(p != NULL);
    kfree(p);
}

TEST(heap, a_block_of_exactly_the_right_size_is_reused) {
    heap_fixture();
    void *a = kmalloc(128);
    REQUIRE(a != NULL);
    uint64_t pages_before = fake_virtual_memory_mapped_pages();
    kfree(a);
    void *b = kmalloc(128);
    CHECK(b == a);
    CHECK_EQ(fake_virtual_memory_mapped_pages(), pages_before);
    kfree(b);
}

TEST(heap, the_split_threshold_is_exact_in_both_directions) {
    heap_fixture();
    const size_t header = sizeof(void *) * 2 + sizeof(void *);
    (void)header;

    void *big = kmalloc(512);
    REQUIRE(big != NULL);
    kfree(big);
    void *nearly = kmalloc(512 - HEAP_ALIGN);
    REQUIRE(nearly == big);
    kfree(nearly);
    void *again = kmalloc(512);
    CHECK(again == big);
    kfree(again);

    void *host = kmalloc(1024);
    REQUIRE(host != NULL);
    kfree(host);
    void *small = kmalloc(64);
    REQUIRE(small == host);
    uint64_t pages = fake_virtual_memory_mapped_pages();
    void *from_remainder = kmalloc(64);
    REQUIRE(from_remainder != NULL);
    CHECK_EQ(fake_virtual_memory_mapped_pages(), pages);
    CHECK(from_remainder != small);
    kfree(small);
    kfree(from_remainder);
}

TEST(heap, a_split_remainder_has_the_size_it_should) {
    heap_fixture();
    void *host = kmalloc(4000);
    REQUIRE(host != NULL);
    kfree(host);

    void *head = kmalloc(100);
    REQUIRE(head != NULL);
    memset(head, 0x11, 100);

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
    void *a = kmalloc(256);
    void *keep = kmalloc(256);
    void *c = kmalloc(256);
    REQUIRE(a && keep && c);
    memset(keep, 0x99, 256);
    kfree(a);
    kfree(c);

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
    for (size_t over = 1; over <= 3; over++) {
        heap_fixture();
        size_t size = 4096 * over + 1;
        void *p = kmalloc(size);
        REQUIRE(p != NULL);
        memset(p, 0xC7, size);
        CHECK_EQ(((unsigned char *)p)[size - 1], 0xC7);
        CHECK(fake_virtual_memory_mapped_pages() >= over + 1);
        kfree(p);
    }
}

TEST(heap, used_bytes_tracks_what_is_live) {
    heap_fixture();
    size_t before = heap_used_bytes();

    void *a = kmalloc(1000);
    REQUIRE(a != NULL);
    size_t with_one = heap_used_bytes();
    CHECK(with_one >= before + 1000);

    void *b = kmalloc(1000);
    REQUIRE(b != NULL);
    CHECK(heap_used_bytes() >= with_one + 1000);

    kfree(b);
    kfree(a);
    CHECK_EQ(heap_used_bytes(), before);
}

TEST(heap, total_bytes_only_ever_grows_and_used_comes_back) {
    heap_fixture();
    size_t used_before = heap_used_bytes();
    size_t total_before = heap_total_bytes();

    void *held[64];
    for (int i = 0; i < 64; i++) {
        held[i] = kmalloc(4096);
        REQUIRE(held[i] != NULL);
    }
    CHECK(heap_total_bytes() > total_before);
    size_t total_at_peak = heap_total_bytes();

    for (int i = 0; i < 64; i++) {
        kfree(held[i]);
    }
    CHECK_EQ(heap_used_bytes(), used_before);
    CHECK_EQ(heap_total_bytes(), total_at_peak);
}

TEST(heap, a_reused_free_block_is_counted_once) {
    heap_fixture();
    size_t before = heap_used_bytes();
    for (int round = 0; round < 200; round++) {
        void *p = kmalloc(512);
        REQUIRE(p != NULL);
        kfree(p);
    }
    CHECK_EQ(heap_used_bytes(), before);
}
