/* The physical memory manager's own init, run on the host over firmware maps
   shaped like the ones QEMU and a laptop hand over.

   What it grades is the difference between the memory a machine HAS and the
   span its frame bitmap has to cover. They were one number until this test,
   and a 16 GiB guest reported 17408 MiB, because QEMU puts 3 GiB of it below
   the PCI hole at 3-4 GiB and the other 13 GiB above 4 GiB.

   The real file is compiled here, renamed so it does not collide with
   tests/fakes/fake_pmm.c (which every other host test allocates through),
   with its three machine facts supplied: physical memory is a buffer this
   test owns, the kernel image ends at 4 MiB, and the firmware map sits in
   the first page after the kernel, inside that buffer. The bitmap and
   reference counts are placed by the code under test, and the arena below
   is exactly where it puts them - any other address fails the test rather
   than writing through a host pointer nobody owns.

   The map is in the arena, not on this test's stack, because init reserves
   the map's own pages by their address. A stack address is the host's: on
   an x86_64 Mac it is far above every map here and was clipped away, on an
   arm64 Mac it is about 6 GiB, inside the 16 GiB map's RAM, and the count
   below would have been a frame or two off there and nowhere else.

   Two things keep that from coming back, on either Mac. Init must have
   asked test_physical_address_of() for the map's address - the calls are
   counted and checked after every init - so a version of the file that took
   a host pointer for a physical address again fails here, wherever the
   pointer is. And the arena is asked for at a host address INSIDE the
   16 GiB map's RAM (TEST_ARENA_HOST_HINT, 14 GiB, clear of an arm64 Mac's
   stack and shared cache and of everything on an x86_64 Mac), so the
   situation the arm64 stack made by accident is the one this test runs in
   on both: a host pointer taken for a physical address would land on a
   frame the map owns, on an Intel Mac too. A page past the arena is mapped
   with no access, so metadata that overran it faults instead of writing
   into somebody else's memory. */

#include "check.h"

#include <stdint.h>
#include <sys/mman.h>

#define TEST_KERNEL_END        0x400000ULL
#define TEST_ARENA_BASE        TEST_KERNEL_END
#define TEST_ARENA_BYTES       (16ULL * 1024 * 1024)
#define TEST_ARENA_HOST_HINT   0x380000000ULL
#define TEST_GUARD_BYTES       4096ULL

static uint8_t *test_arena;
static unsigned test_address_of_calls;

static uint8_t *test_physical_pointer(uint64_t physical) {
    if (physical < TEST_ARENA_BASE || physical >= TEST_ARENA_BASE + TEST_ARENA_BYTES) {
        test_fail(__FILE__, __LINE__, "the frame bitmap was placed at 0x%llx, outside "
                  "the memory this test gave it", (unsigned long long)physical);
        test_abandon();
    }
    return test_arena + (physical - TEST_ARENA_BASE);
}

static uint64_t test_physical_address_of(const void *pointer) {
    const uint8_t *p = pointer;
    test_address_of_calls++;
    if (!test_arena || p < test_arena || p >= test_arena + TEST_ARENA_BYTES) {
        test_fail(__FILE__, __LINE__, "init was handed a firmware map at host address %p, "
                  "outside the memory this test gave it", pointer);
        test_abandon();
    }
    return TEST_ARENA_BASE + (uint64_t)(p - test_arena);
}

#define PHYSICAL_MEMORY_POINTER(physical) test_physical_pointer(physical)
#define PHYSICAL_MEMORY_ADDRESS_OF(pointer) test_physical_address_of(pointer)
#define PHYSICAL_MEMORY_KERNEL_END TEST_KERNEL_END

#define physical_memory_init                         pmm_under_test_init
#define physical_memory_alloc_frame                  pmm_under_test_alloc_frame
#define physical_memory_try_alloc_frame              pmm_under_test_try_alloc_frame
#define physical_memory_alloc_frame_dma              pmm_under_test_alloc_frame_dma
#define physical_memory_free_frame                   pmm_under_test_free_frame
#define physical_memory_free_frame_count             pmm_under_test_free_frame_count
#define physical_memory_total_frame_count            pmm_under_test_total_frame_count
#define physical_memory_tracked_limit                pmm_under_test_tracked_limit
#define physical_memory_alloc_frame_above            pmm_under_test_alloc_frame_above
#define physical_memory_metadata_end                 pmm_under_test_metadata_end
#define physical_memory_alloc_contiguous             pmm_under_test_alloc_contiguous
#define physical_memory_try_alloc_contiguous         pmm_under_test_try_alloc_contiguous
#define physical_memory_try_alloc_contiguous_anywhere pmm_under_test_try_alloc_contiguous_anywhere
#define physical_memory_free_contiguous              pmm_under_test_free_contiguous
#define physical_memory_frame_reference              pmm_under_test_frame_reference
#define physical_memory_frame_refs                   pmm_under_test_frame_refs
#define pmm_free_site                                pmm_under_test_free_site
#define pmm_free_virt                                pmm_under_test_free_virt

#include "../kernel/memory_management/physical_memory.c"

#define MIB (1024ULL * 1024)
#define GIB (1024ULL * MIB)

typedef struct {
    uint32_t count;
    uint32_t pad;
    e820_entry_t entries[16];
} test_map_t;

/* A hint, not MAP_FIXED: a host that has something there already gets the
   arena somewhere else, and the address-of check above still holds. */
static uint8_t *the_arena(void) {
    if (!test_arena) {
        void *p = mmap((void *)(uintptr_t)TEST_ARENA_HOST_HINT,
                       TEST_ARENA_BYTES + TEST_GUARD_BYTES, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
        REQUIRE(p != MAP_FAILED);
        REQUIRE(mprotect((uint8_t *)p + TEST_ARENA_BYTES, TEST_GUARD_BYTES, PROT_NONE) == 0);
        test_arena = p;
    }
    return test_arena;
}

static test_map_t *new_map_at(uint64_t physical) {
    REQUIRE(physical >= TEST_ARENA_BASE &&
            physical + sizeof(test_map_t) <= TEST_ARENA_BASE + TEST_ARENA_BYTES);
    test_map_t *map = (test_map_t *)(the_arena() + (physical - TEST_ARENA_BASE));
    map->count = 0;
    map->pad = 0;
    return map;
}

/* The firmware map, at physical TEST_ARENA_BASE: the page straight after the
   kernel, which init has to step over to place the bitmap. */
static test_map_t *new_map(void) {
    return new_map_at(TEST_ARENA_BASE);
}

static void add(test_map_t *map, uint64_t base, uint64_t length, uint32_t type) {
    e820_entry_t *e = &map->entries[map->count++];
    e->base = base;
    e->length = length;
    e->type = type;
    e->acpi_ext = 1;
}

static void init_over(test_map_t *map) {
    unsigned calls = test_address_of_calls;
    volatile int returned = 0;
    /* A panic is a failure of this test, not the end of the run: a map
       reserved at the wrong address lets the bitmap be placed over it, and
       init then reads its own metadata as the map and finds no memory. */
    CHECK_NO_PANIC((pmm_under_test_init((const uint32_t *)map), returned = 1));
    /* The map's pages were reserved at the address this test says the map
       is at, not wherever this host keeps it. */
    CHECK_MSG(test_address_of_calls > calls,
              "init reserved the firmware map without asking for its physical "
              "address - a host pointer was taken for one");
    if (!returned) {
        test_abandon();
    }
    /* And all init wrote is inside the memory this test gave it. */
    CHECK(pmm_under_test_metadata_end() <= TEST_ARENA_BASE + TEST_ARENA_BYTES);
}

/* Shaped like what OVMF hands this boot loader for `-m 16384` on QEMU's pc
   machine: the legacy hole at 640K-1M, firmware-reserved pieces inside the
   low 3 GiB, the PCI window from 3 GiB to 4 GiB, and 13 GiB above it. */
static test_map_t *qemu_16_gib(void) {
    test_map_t *map = new_map();
    add(map, 0x0, 0x9F000, E820_TYPE_USABLE);
    add(map, 0x9F000, 0x61000, E820_TYPE_RESERVED);
    add(map, 0x100000, 0x7F700000, E820_TYPE_USABLE);
    add(map, 0x7F800000, 0x400000, E820_TYPE_ACPI_NVS);
    add(map, 0x7FC00000, 0x40400000, E820_TYPE_USABLE);
    add(map, 0xFEC00000, 0x1000, E820_TYPE_MMIO);
    add(map, 0x100000000ULL, 13 * GIB, E820_TYPE_USABLE);
    return map;
}

static uint64_t usable_bytes(const test_map_t *map) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < map->count; i++) {
        if (map->entries[i].type == E820_TYPE_USABLE) {
            sum += map->entries[i].length;
        }
    }
    return sum;
}

TEST(physical_memory, a_16_gib_machine_reports_the_ram_it_has_not_the_span_it_tracks) {
    test_map_t *map = qemu_16_gib();
    init_over(map);

    /* The span: the bitmap reaches the top of the highest usable region. */
    CHECK_EQ(pmm_under_test_tracked_limit(), 17 * GIB);
    /* The memory: every usable byte, and no more - not the hole. */
    CHECK_EQ(pmm_under_test_total_frame_count() * 4096, usable_bytes(map));
    CHECK(pmm_under_test_total_frame_count() * 4096 <= 16 * GIB);
    CHECK(pmm_under_test_total_frame_count() * 4096 < pmm_under_test_tracked_limit());
    /* Free is a part of the memory, never more than it. */
    CHECK(pmm_under_test_free_frame_count() < pmm_under_test_total_frame_count());
}

TEST(physical_memory, a_4_gib_machine_has_no_gigabyte_it_was_never_given) {
    test_map_t *map = new_map();
    add(map, 0x0, 0x9F000, E820_TYPE_USABLE);
    add(map, 0x100000, 3 * GIB - 0x100000, E820_TYPE_USABLE);
    add(map, 0x100000000ULL, 1 * GIB, E820_TYPE_USABLE);
    init_over(map);

    CHECK_EQ(pmm_under_test_tracked_limit(), 5 * GIB);
    CHECK_EQ(pmm_under_test_total_frame_count() * 4096, usable_bytes(map));
    CHECK(pmm_under_test_total_frame_count() * 4096 <= 4 * GIB);
}

TEST(physical_memory, overlapping_firmware_entries_are_one_frame_of_memory_not_two) {
    test_map_t *map = new_map();
    add(map, 0x100000, 255 * MIB, E820_TYPE_USABLE);
    add(map, 0x100000, 64 * MIB, E820_TYPE_USABLE);
    add(map, 200 * MIB, 56 * MIB, E820_TYPE_USABLE);
    init_over(map);

    CHECK_EQ(pmm_under_test_tracked_limit(), 256 * MIB);
    CHECK_EQ(pmm_under_test_total_frame_count() * 4096, 255 * MIB);
}

TEST(physical_memory, a_part_page_at_an_entry_edge_is_not_counted_as_memory) {
    test_map_t *map = new_map();
    add(map, 0x100000, 128 * MIB, E820_TYPE_USABLE);
    add(map, 0x10000800ULL, 0x2000, E820_TYPE_USABLE);
    init_over(map);

    /* 0x10000800-0x10002800 covers one whole frame (0x10001000) and two
       halves the allocator could never hand out. */
    CHECK_EQ(pmm_under_test_total_frame_count(), 128 * MIB / 4096 + 1);
}

TEST(physical_memory, the_kernel_the_map_and_the_bitmap_are_memory_but_not_free_memory) {
    test_map_t *map = qemu_16_gib();
    init_over(map);

    uint64_t ram = pmm_under_test_total_frame_count();
    uint64_t free_now = pmm_under_test_free_frame_count();
    /* Taken, and every one of them named: the usable frames below 1 MiB,
       the 3 MiB of kernel this test claims, the firmware map's page right
       after it, and the metadata for a 17 GiB span (a bit and a byte a
       frame: 557056 + 4456448 bytes, 1224 pages) placed after the map. */
    uint64_t low = 0x9F000 / 4096;
    uint64_t kernel = (TEST_KERNEL_END - 0x100000) / 4096;
    uint64_t map_page = 1;
    uint64_t metadata = (17 * GIB / 4096 / 8 + 17 * GIB / 4096) / 4096;
    CHECK_EQ(metadata, 1224);
    CHECK_EQ(pmm_under_test_metadata_end(), TEST_KERNEL_END + (map_page + metadata) * 4096);
    CHECK_EQ(ram - free_now, low + kernel + map_page + metadata);

    uint64_t frame = pmm_under_test_alloc_frame();
    CHECK(frame != 0);
    /* Everything below the metadata's end is taken, the map's page too. */
    CHECK(frame >= pmm_under_test_metadata_end());
    CHECK_EQ(pmm_under_test_total_frame_count(), ram);
    CHECK_EQ(pmm_under_test_free_frame_count(), free_now - 1);
    pmm_under_test_free_frame(frame);
    CHECK_EQ(pmm_under_test_free_frame_count(), free_now);
}

TEST(physical_memory, the_firmware_map_is_reserved_where_it_is_and_nowhere_else) {
    /* The map 8 MiB further into the arena, at physical 0xC00000, which
       leaves the metadata free to start straight after the kernel. The map's
       page is taken and the pages either side of it are not, and what is
       taken adds up the same way as with the map anywhere else. */
    uint64_t map_physical = TEST_ARENA_BASE + 8 * MIB;
    test_map_t *map = new_map_at(map_physical);
    add(map, 0x0, 0x9F000, E820_TYPE_USABLE);
    add(map, 0x100000, 3 * GIB - 0x100000, E820_TYPE_USABLE);
    add(map, 0x100000000ULL, 1 * GIB, E820_TYPE_USABLE);
    init_over(map);

    /* A 5 GiB span: a bit and a byte a frame, 163840 + 1310720 bytes. */
    uint64_t metadata = (5 * GIB / 4096 / 8 + 5 * GIB / 4096) / 4096;
    CHECK_EQ(metadata, 360);
    CHECK_EQ(pmm_under_test_metadata_end(), TEST_KERNEL_END + metadata * 4096);
    CHECK(pmm_under_test_metadata_end() < map_physical);

    uint64_t low = 0x9F000 / 4096;
    uint64_t kernel = (TEST_KERNEL_END - 0x100000) / 4096;
    uint64_t map_page = 1;
    CHECK_EQ(pmm_under_test_total_frame_count() - pmm_under_test_free_frame_count(),
             low + kernel + map_page + metadata);

    CHECK_EQ(pmm_under_test_alloc_frame_above(pmm_under_test_metadata_end()),
             pmm_under_test_metadata_end());
    CHECK_EQ(pmm_under_test_alloc_frame_above(map_physical), map_physical + 4096);
    CHECK_EQ(pmm_under_test_alloc_frame_above(map_physical - 4096), map_physical - 4096);
}
