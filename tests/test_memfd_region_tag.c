/* M225: a mapped region's memfd tag, past the 256th memfd.

   A region names its memfd by (slot + 1, generation) - see
   kernel/architecture/x86_64/memfd_region_tag.h - and the memfd table grows
   to 65,536. Two of syscall.c's failure paths (an mmap of a memfd that could
   not get a region-table entry, and the split of a memfd region that could
   not) gave the region's reference back through
   memfd_by_tag((uint8_t)(id - 1), gen): the slot cut to 8 bits. Every memfd
   starts at generation 0, so for memfd 300 that was memfd 44's tag - a live
   memfd in any process that has made more than 256 of them - and memfd 44
   lost a reference it was counting on while memfd 300 kept one nobody held.

   This drives the helpers those paths now call, against the real memfd
   table through the fakes' heap and frames. */
#include "check.h"

#include "fakes/fakes.h"
#include "architecture/x86_64/memfd_region_tag.h"
#include "inter_process_communication/memfd.h"
#include "memory_management/heap.h"

#define MANY 300

static void clean(void) {
    fake_physical_memory_reset();
    fake_virtual_memory_reset();
    heap_init();
    memfd_init();
}

static struct memfd *made[MANY];

static void make_many(void) {
    for (int i = 0; i < MANY; i++) {
        made[i] = memfd_create_object("tagged");
        REQUIRE(made[i] != NULL);
    }
    REQUIRE(memfd_in_use() == MANY);
}

TEST(memfd_region_tag, a_failed_mapping_gives_back_its_own_memfds_reference_past_256) {
    clean();
    make_many();
    struct memfd *last = made[MANY - 1];
    uint32_t id = memfd_slot(last) + 1;
    uint16_t gen = memfd_generation(last);
    REQUIRE(id > 256);
    CHECK(memfd_region_tag_resolve(id, gen) == last);

    /* What sys_mmap does: a region reference, then the insert fails and it
       is given back by tag. */
    memfd_region_tag_reference(id, gen);
    memfd_region_tag_unref(id, gen);

    /* Its own descriptor's reference was the only one left, so letting that
       go frees it; and the memfd 256 below it was never touched. */
    memfd_unref(last);
    CHECK_EQ(memfd_in_use(), MANY - 1);
    CHECK(memfd_region_tag_resolve(id, gen) == NULL);
    struct memfd *alias = made[(MANY - 1) % 256];
    CHECK(memfd_region_tag_resolve(memfd_slot(alias) + 1, memfd_generation(alias)) == alias);

    for (int i = 0; i < MANY - 1; i++) {
        memfd_unref(made[i]);
    }
    CHECK_EQ(memfd_in_use(), 0);
}

TEST(memfd_region_tag, every_tag_resolves_to_its_own_memfd_and_zero_to_none) {
    clean();
    make_many();
    int wrong = 0;
    for (int i = 0; i < MANY; i++) {
        if (memfd_region_tag_resolve(memfd_slot(made[i]) + 1, memfd_generation(made[i])) !=
            made[i]) {
            wrong++;
        }
    }
    CHECK_EQ(wrong, 0);
    CHECK(memfd_region_tag_resolve(0, 0) == NULL);
    memfd_region_tag_unref(0, 0); /* no memfd: nothing to give back */
    memfd_region_tag_reference(0, 0);
    CHECK_EQ(memfd_in_use(), MANY);
    for (int i = 0; i < MANY; i++) {
        memfd_unref(made[i]);
    }
    CHECK_EQ(memfd_in_use(), 0);
}
