#include "check.h"

#include "fakes/fakes.h"
#include "memory_management/heap.h"
#include "scheduler/scheduler.h"

#include <string.h>

/* M156: the per-process table of mmap regions. It was a fixed array of 128
   entries inside task_t until V8 ran out of them - PartitionAlloc reserves
   one range and then commits, decommits and protects sub-ranges of it, and
   every one of those splits an entry. It is a heap allocation that doubles
   now, and what is graded here is the invariant every reader depends on:
   the array is terminated by an entry whose `pages` is zero, so there must
   always be one of those past the last region in use. */

static task_t task;

static void fixture(void) {
    scheduler_regions_release(&task);
    fake_physical_memory_reset();
    fake_virtual_memory_reset();
    heap_init();
    memset(&task, 0, sizeof(task));
}

static uint32_t regions_in_use(const task_t *t) {
    uint32_t used = 0;
    while (used < t->mmap_capacity && t->mmaps[used].pages != 0) {
        used++;
    }
    return used;
}

static void fill_region(task_t *t, uint32_t index) {
    t->mmaps[index].base = 0xA000000000ULL + (uint64_t)index * 0x1000;
    t->mmaps[index].pages = 1;
    t->mmaps[index].prot = 3;
    t->mmaps[index].handle = -1;
}

TEST(mmap_regions, a_task_that_maps_nothing_carries_no_table) {
    fixture();
    CHECK(task.mmaps == NULL);
    CHECK_EQ(task.mmap_capacity, 0u);

    /* Every loop over the table is bounded by the capacity, so all of these
       are asked about a task with no table at all. */
    CHECK_NO_PANIC(scheduler_regions_forget_memfds(&task));
    CHECK_NO_PANIC(scheduler_regions_retain_memfds(&task));
    CHECK_EQ(scheduler_release_shared_range(&task, 0xA000000000ULL, 0xA000001000ULL), 0);
    CHECK_NO_PANIC(scheduler_regions_release(&task));
}

TEST(mmap_regions, the_first_reservation_allocates_and_the_table_is_empty) {
    fixture();
    CHECK_EQ(scheduler_regions_reserve(&task), 0);
    REQUIRE(task.mmaps != NULL);
    CHECK_EQ(task.mmap_capacity, (uint32_t)MMAP_REGIONS_INITIAL);
    CHECK_EQ(regions_in_use(&task), 0u);
    for (uint32_t i = 0; i < task.mmap_capacity; i++) {
        CHECK_EQ(task.mmaps[i].pages, 0u);
        CHECK_EQ(task.mmaps[i].base, 0ull);
        CHECK_EQ(task.mmaps[i].memfd_id, 0);
    }
    scheduler_regions_release(&task);
}

TEST(mmap_regions, a_reservation_that_still_fits_allocates_nothing) {
    fixture();
    REQUIRE(scheduler_regions_reserve(&task) == 0);
    mmap_region_t *first = task.mmaps;
    for (uint32_t i = 0; i + 2 < (uint32_t)MMAP_REGIONS_INITIAL; i++) {
        CHECK_EQ(scheduler_regions_reserve(&task), 0);
        CHECK(task.mmaps == first);
        CHECK_EQ(task.mmap_capacity, (uint32_t)MMAP_REGIONS_INITIAL);
        fill_region(&task, i);
    }
    scheduler_regions_release(&task);
}

TEST(mmap_regions, the_table_doubles_and_keeps_what_was_in_it) {
    fixture();
    /* Four doublings, filling every entry the reservation hands back, so the
       capacity is driven by use rather than asked for. */
    uint32_t expected[] = {32, 64, 128, 256, 512};
    uint32_t doublings = 0;
    uint32_t previous = 0;
    for (uint32_t written = 0; written < 400; written++) {
        REQUIRE(scheduler_regions_reserve(&task) == 0);
        if (task.mmap_capacity != previous) {
            REQUIRE(doublings < sizeof(expected) / sizeof(expected[0]));
            CHECK_EQ(task.mmap_capacity, expected[doublings]);
            previous = task.mmap_capacity;
            doublings++;
        }
        /* The invariant: the entry being filled is not the last one, so the
           zero that terminates the array is still there afterwards. */
        CHECK(written + 1 < task.mmap_capacity);
        fill_region(&task, written);
        CHECK_EQ(regions_in_use(&task), written + 1);
    }
    CHECK_EQ(doublings, 5u);
    CHECK_EQ(task.mmap_capacity, 512u);
    for (uint32_t i = 0; i < 400; i++) {
        CHECK_EQ(task.mmaps[i].base, 0xA000000000ULL + (uint64_t)i * 0x1000);
        CHECK_EQ(task.mmaps[i].pages, 1u);
        CHECK_EQ(task.mmaps[i].handle, -1);
    }
    for (uint32_t i = 400; i < task.mmap_capacity; i++) {
        CHECK_EQ(task.mmaps[i].pages, 0u);
    }
    scheduler_regions_release(&task);
}

TEST(mmap_regions, a_grow_the_heap_refuses_changes_nothing) {
    fixture();
    REQUIRE(scheduler_regions_reserve(&task) == 0);
    for (uint32_t i = 0; i + 1 < (uint32_t)MMAP_REGIONS_INITIAL; i++) {
        fill_region(&task, i);
    }
    mmap_region_t *before = task.mmaps;
    uint32_t capacity = task.mmap_capacity;

    fake_physical_memory_fail_after(0);
    CHECK_EQ(scheduler_regions_reserve(&task), -1);
    CHECK(task.mmaps == before);
    CHECK_EQ(task.mmap_capacity, capacity);
    CHECK_EQ(regions_in_use(&task), capacity - 1);
    CHECK_EQ(task.mmaps[0].base, 0xA000000000ULL);

    fake_physical_memory_fail_after(-1);
    CHECK_EQ(scheduler_regions_reserve(&task), 0);
    CHECK_EQ(task.mmap_capacity, capacity * 2);
    scheduler_regions_release(&task);
}

TEST(mmap_regions, the_ceiling_refuses_rather_than_growing_past_it) {
    fixture();
    /* Reaching MAX_MMAP_REGIONS by doubling is 65535 reservations, each of
       which scans the table - so the ceiling is set up directly and what is
       graded is the refusal, which is the part with a decision in it. */
    task.mmaps = (mmap_region_t *)kmalloc(sizeof(mmap_region_t) * MAX_MMAP_REGIONS);
    REQUIRE(task.mmaps != NULL);
    memset(task.mmaps, 0, sizeof(mmap_region_t) * MAX_MMAP_REGIONS);
    task.mmap_capacity = MAX_MMAP_REGIONS;

    for (uint32_t i = 0; i + 2 < (uint32_t)MAX_MMAP_REGIONS; i++) {
        task.mmaps[i].pages = 1;
    }
    /* One entry free and the terminator after it: this still fits. */
    CHECK_EQ(scheduler_regions_reserve(&task), 0);
    CHECK_EQ(task.mmap_capacity, (uint32_t)MAX_MMAP_REGIONS);

    task.mmaps[MAX_MMAP_REGIONS - 2].pages = 1;
    CHECK_EQ(scheduler_regions_reserve(&task), -1);
    CHECK_EQ(task.mmap_capacity, (uint32_t)MAX_MMAP_REGIONS);
    CHECK_EQ(regions_in_use(&task), (uint32_t)MAX_MMAP_REGIONS - 1);

    scheduler_regions_release(&task);
    CHECK(task.mmaps == NULL);
    CHECK_EQ(task.mmap_capacity, 0u);
}

TEST(mmap_regions, releasing_twice_is_not_a_double_free) {
    fixture();
    REQUIRE(scheduler_regions_reserve(&task) == 0);
    scheduler_regions_release(&task);
    CHECK(task.mmaps == NULL);
    CHECK_NO_PANIC(scheduler_regions_release(&task));
    CHECK(task.mmaps == NULL);
    CHECK_EQ(task.mmap_capacity, 0u);
}
