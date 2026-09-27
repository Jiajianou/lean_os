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

/* M179: a table a growth replaced is NOT given back to the heap.

   A page fault reads this table with no lock held, and it cannot take one -
   a fault is how the kernel discovers it needs memory, so the fault path is
   underneath the things a lock would protect. On one core that is harmless
   because nothing else runs while the fault is being handled. On four, a
   sibling thread calling mmap can grow the table while another core is
   walking it, and the walk is then reading a block the heap has already
   handed to somebody else. Every thread of a process shares the owner's
   table, so this needs no unusual program to reach - a browser does it.

   What that looked like on the machine was a thread-stack page that faulted
   not-present, with cr2 exactly rsp-8, and fill_one_page_ex answering "this
   address is not mine" because the region really was not in the bytes it
   read. The process was killed for touching its own stack. */
TEST(mmap_regions, a_table_a_growth_replaced_is_not_handed_back_to_the_heap) {
    fixture();
    REQUIRE(scheduler_regions_reserve(&task) == 0);
    mmap_region_t *first = task.mmaps;
    uint32_t first_capacity = task.mmap_capacity;
    REQUIRE(first != NULL);
    REQUIRE(first_capacity == MMAP_REGIONS_INITIAL);

    for (uint32_t i = 0; i + 1 < first_capacity; i++) {
        fill_region(&task, i);
    }

    REQUIRE(scheduler_regions_reserve(&task) == 0);
    REQUIRE(task.mmaps != first);
    REQUIRE(task.mmap_capacity > first_capacity);

    /* Somebody else asks the heap for exactly what the old table occupied.
       If it had been freed this is what lands on top of it. */
    size_t bytes = sizeof(mmap_region_t) * (size_t)first_capacity;
    void *scratch = kmalloc(bytes);
    REQUIRE(scratch != NULL);
    memset(scratch, 0xEE, bytes);

    /* The replaced table still says what it said, because a fault on another
       core could still be reading it. */
    for (uint32_t i = 0; i + 1 < first_capacity; i++) {
        CHECK_EQ(first[i].base, 0xA000000000ULL + (uint64_t)i * 0x1000);
        CHECK_EQ(first[i].pages, 1u);
    }

    kfree(scratch);
    scheduler_regions_release(&task);
}

/* And they do go back, all of them, when the address space does. */
TEST(mmap_regions, releasing_gives_back_every_table_the_growths_left_behind) {
    fixture();
    size_t used_before = heap_used_bytes();

    for (int grow = 0; grow < 4; grow++) {
        REQUIRE(scheduler_regions_reserve(&task) == 0);
        uint32_t used = regions_in_use(&task);
        while (used + 1 < task.mmap_capacity) {
            fill_region(&task, used);
            used++;
        }
    }
    REQUIRE(task.mmaps_retired_count >= 3);

    scheduler_regions_release(&task);
    CHECK_EQ(task.mmaps_retired_count, 0u);
    CHECK(task.mmaps == NULL);
    /* The heap never gives frames back to the PMM, so what proves these went
       back is the heap's own accounting rather than the frame count. */
    CHECK_EQ(heap_used_bytes(), used_before);
}

/* M187: the ranges fork leaves shared. A MAP_SHARED page made copy-on-write by
   a fork is one the parent's next write copies away from the memfd - and a
   sibling thread on another core can fault one back in between fork's release
   of the shared pages and its copy of the page tables. So the copy is handed
   this list and skips it, and what is graded here is the list: every memfd
   region and every shared FILE mapping, nothing private, sorted by address -
   the page walk visits addresses in order and keeps one cursor, so an
   unsorted list would skip the wrong pages. */
TEST(mmap_regions, fork_is_told_every_shared_region_and_nothing_private) {
    fixture();
    virtual_memory_range_t *ranges = (virtual_memory_range_t *)0;
    CHECK_EQ(scheduler_shared_ranges(&task, &ranges), 0);
    CHECK(ranges == NULL);

    CHECK_EQ(scheduler_regions_reserve(&task), 0);
    /* Out of address order on purpose: private, memfd (high), shared file,
       shared-but-anonymous (no handle, so nothing behind it to share),
       memfd (low). */
    task.mmaps[0] = (mmap_region_t){.base = 0xA000000000ULL, .pages = 2, .prot = 3, .handle = -1};
    task.mmaps[1] = (mmap_region_t){.base = 0xA000900000ULL, .pages = 4, .prot = 3, .handle = -1,
                                    .memfd_id = 7, .memfd_gen = 1};
    task.mmaps[2] = (mmap_region_t){.base = 0xA000400000ULL, .pages = 1, .prot = 1, .handle = 3,
                                    .shared = 1};
    task.mmaps[3] = (mmap_region_t){.base = 0xA000600000ULL, .pages = 1, .prot = 3, .handle = -1,
                                    .shared = 1};
    task.mmaps[4] = (mmap_region_t){.base = 0xA000100000ULL, .pages = 3, .prot = 3, .handle = -1,
                                    .memfd_id = 2, .memfd_gen = 5};

    int n = scheduler_shared_ranges(&task, &ranges);
    REQUIRE(n == 3);
    REQUIRE(ranges != NULL);
    CHECK_EQ(ranges[0].lo, 0xA000100000ULL);
    CHECK_EQ(ranges[0].hi, 0xA000100000ULL + 3 * 4096);
    CHECK_EQ(ranges[1].lo, 0xA000400000ULL);
    CHECK_EQ(ranges[1].hi, 0xA000400000ULL + 4096);
    CHECK_EQ(ranges[2].lo, 0xA000900000ULL);
    CHECK_EQ(ranges[2].hi, 0xA000900000ULL + 4 * 4096);
    kfree(ranges);

    /* Nothing past the terminator is read, however much table there is. */
    task.mmaps[1].pages = 0;
    n = scheduler_shared_ranges(&task, &ranges);
    CHECK_EQ(n, 0);
    CHECK(ranges == NULL);
    task.mmaps[1].pages = 4;
}
