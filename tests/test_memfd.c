#include "check.h"

#include "fakes/fakes.h"
#include "ipc/memfd.h"
#include "proc/proc.h"

#include <string.h>

static void clean(void) {
    memfd_init();
    CHECK_EQ(memfd_in_use(), 0);
    CHECK_EQ(memfd_pages_held(), 0);
}

TEST(memfd, an_object_starts_empty_and_is_sized_by_truncate) {
    clean();
    struct memfd *m = memfd_create_obj("shared");
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_in_use(), 1);
    CHECK_EQ(memfd_size(m), 0);
    CHECK_EQ(memfd_pages_held(), 0);
    CHECK_EQ(memfd_frame(m, 0), 0);

    CHECK_EQ(memfd_truncate(m, 3 * PAGE_SIZE), 0);
    CHECK_EQ(memfd_size(m), 3 * PAGE_SIZE);
    CHECK_EQ(memfd_pages_held(), 3);
    CHECK_NE(memfd_frame(m, 2), 0);
    CHECK_EQ(memfd_frame(m, 3), 0);

    CHECK_EQ(memfd_truncate(m, 3 * PAGE_SIZE + 100), 0);
    CHECK_EQ(memfd_size(m), 3 * PAGE_SIZE + 100);
    CHECK_EQ(memfd_pages_held(), 4);

    memfd_unref(m);
    CHECK_EQ(memfd_in_use(), 0);
    CHECK_EQ(memfd_pages_held(), 0);
}

TEST(memfd, every_frame_is_zeroed_before_anybody_can_see_it) {
    clean();
    struct memfd *m = memfd_create_obj("zeroed");
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_truncate(m, 2 * PAGE_SIZE), 0);
    for (uint32_t p = 0; p < 2; p++) {
        const unsigned char *bytes = (const unsigned char *)(uintptr_t)memfd_frame(m, p);
        REQUIRE(bytes != NULL);
        int dirty = 0;
        for (uint64_t i = 0; i < PAGE_SIZE; i++) {
            if (bytes[i] != 0) {
                dirty++;
            }
        }
        CHECK_EQ(dirty, 0);
    }
    memfd_unref(m);
    clean();
}

TEST(memfd, growing_is_allowed_and_shrinking_is_refused) {
    clean();
    struct memfd *m = memfd_create_obj(NULL);
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_truncate(m, 4 * PAGE_SIZE), 0);
    uint64_t second = memfd_frame(m, 1);
    CHECK_EQ(memfd_truncate(m, 8 * PAGE_SIZE), 0);
    CHECK_EQ(memfd_frame(m, 1), second);
    CHECK_EQ(memfd_pages_held(), 8);

    CHECK_EQ(memfd_truncate(m, 2 * PAGE_SIZE), -1);
    CHECK_EQ(memfd_size(m), 8 * PAGE_SIZE);
    CHECK_EQ(memfd_pages_held(), 8);

    CHECK_EQ(memfd_truncate(m, (uint64_t)(MEMFD_MAX_PAGES + 1) * PAGE_SIZE), -1);
    CHECK_EQ(memfd_pages_held(), 8);
    memfd_unref(m);
    clean();
}

TEST(memfd, a_grow_that_runs_out_of_memory_changes_nothing) {
    clean();
    struct memfd *m = memfd_create_obj(NULL);
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_truncate(m, 2 * PAGE_SIZE), 0);
    uint64_t first = memfd_frame(m, 0);
    uint64_t before = fake_pmm_outstanding();
    fake_pmm_fail_after((int64_t)fake_pmm_total_allocs() + 2);
    CHECK_EQ(memfd_truncate(m, 5 * PAGE_SIZE), -1);
    fake_pmm_fail_after(-1);
    CHECK_EQ(memfd_size(m), 2 * PAGE_SIZE);
    CHECK_EQ(memfd_pages_held(), 2);
    CHECK_EQ(memfd_frame(m, 0), first);
    CHECK_EQ(fake_pmm_outstanding(), before);
    CHECK_EQ(memfd_truncate(m, 3 * PAGE_SIZE), 0);
    memfd_unref(m);
    clean();
}

TEST(memfd, a_mapping_outlives_the_descriptor_that_made_it) {
    clean();
    struct memfd *m = memfd_create_obj(NULL);
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_truncate(m, PAGE_SIZE), 0);
    uint8_t slot = memfd_slot(m);
    uint16_t gen = memfd_generation(m);

    memfd_region_ref(m);
    memfd_unref(m);
    CHECK_EQ(memfd_in_use(), 1);
    CHECK(memfd_by_tag(slot, gen) == m);
    CHECK_NE(memfd_frame(m, 0), 0);

    memfd_region_unref(m);
    CHECK_EQ(memfd_in_use(), 0);
    CHECK_EQ(memfd_pages_held(), 0);
    clean();
}

TEST(memfd, a_stale_tag_names_nothing_rather_than_somebody_else) {
    clean();
    struct memfd *first = memfd_create_obj("first");
    REQUIRE(first != NULL);
    CHECK_EQ(memfd_truncate(first, PAGE_SIZE), 0);
    uint8_t slot = memfd_slot(first);
    uint16_t gen = memfd_generation(first);
    memfd_unref(first);

    struct memfd *second = memfd_create_obj("second");
    REQUIRE(second != NULL);
    CHECK_EQ(memfd_slot(second), slot);
    CHECK_NE(memfd_generation(second), gen);
    CHECK(memfd_by_tag(slot, gen) == NULL);
    CHECK(memfd_by_tag(slot, memfd_generation(second)) == second);
    CHECK(memfd_by_tag(MEMFD_MAX, 0) == NULL);
    CHECK(memfd_by_tag(MEMFD_MAX + 7, 0) == NULL);
    memfd_unref(second);
    clean();
}

TEST(memfd, seals_are_promises_that_do_not_come_off) {
    clean();
    struct memfd *m = memfd_create_obj(NULL);
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_truncate(m, 2 * PAGE_SIZE), 0);
    CHECK_EQ(memfd_get_seals(m), 0);
    CHECK_EQ(memfd_may_write(m), 1);

    CHECK_EQ(memfd_add_seals(m, MEMFD_SEAL_WRITE), 0);
    CHECK_EQ(memfd_get_seals(m), MEMFD_SEAL_WRITE);
    CHECK_EQ(memfd_may_write(m), 0);

    CHECK_EQ(memfd_truncate(m, 3 * PAGE_SIZE), 0);
    CHECK_EQ(memfd_add_seals(m, MEMFD_SEAL_GROW), 0);
    CHECK_EQ(memfd_truncate(m, 4 * PAGE_SIZE), -1);
    CHECK_EQ(memfd_size(m), 3 * PAGE_SIZE);

    CHECK_EQ(memfd_add_seals(m, 0x40), -1);
    CHECK_EQ(memfd_get_seals(m), MEMFD_SEAL_WRITE | MEMFD_SEAL_GROW);

    CHECK_EQ(memfd_add_seals(m, MEMFD_SEAL_SEAL), 0);
    CHECK_EQ(memfd_add_seals(m, MEMFD_SEAL_SHRINK), -1);
    memfd_unref(m);
    clean();
}

TEST(memfd, running_out_of_objects_refuses_and_recovers) {
    clean();
    struct memfd *all[MEMFD_MAX];
    for (int i = 0; i < MEMFD_MAX; i++) {
        all[i] = memfd_create_obj(NULL);
        REQUIRE(all[i] != NULL);
    }
    CHECK(memfd_create_obj(NULL) == NULL);
    memfd_unref(all[0]);
    all[0] = memfd_create_obj(NULL);
    REQUIRE(all[0] != NULL);
    for (int i = 0; i < MEMFD_MAX; i++) {
        memfd_unref(all[i]);
    }
    clean();
}

TEST(memfd, every_call_refuses_a_null_object) {
    clean();
    CHECK_EQ(memfd_size(NULL), 0);
    CHECK_EQ(memfd_frame(NULL, 0), 0);
    CHECK_EQ(memfd_truncate(NULL, PAGE_SIZE), -1);
    CHECK_EQ(memfd_add_seals(NULL, MEMFD_SEAL_WRITE), -1);
    CHECK_EQ(memfd_get_seals(NULL), 0);
    CHECK_EQ(memfd_may_write(NULL), 0);
    CHECK_EQ(memfd_slot(NULL), 0);
    CHECK_EQ(memfd_generation(NULL), 0);
    memfd_ref(NULL);
    memfd_unref(NULL);
    memfd_region_ref(NULL);
    memfd_region_unref(NULL);

    struct memfd *dead = memfd_create_obj("gone");
    REQUIRE(dead != NULL);
    CHECK_EQ(memfd_truncate(dead, PAGE_SIZE), 0);
    memfd_unref(dead);
    CHECK_EQ(memfd_size(dead), 0);
    CHECK_EQ(memfd_frame(dead, 0), 0);
    CHECK_EQ(memfd_get_seals(dead), 0);
    CHECK_EQ(memfd_may_write(dead), 0);
    CHECK_EQ(memfd_truncate(dead, PAGE_SIZE), -1);
    CHECK_EQ(memfd_add_seals(dead, MEMFD_SEAL_WRITE), -1);
    CHECK_STREQ(memfd_name(dead), "");
    clean();
}

TEST(memfd, a_name_is_kept_truncated_rather_than_refused) {
    clean();
    struct memfd *m = memfd_create_obj(
        "a-name-considerably-longer-than-the-twenty-four-bytes-this-field-has");
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_truncate(m, PAGE_SIZE), 0);
    CHECK_EQ(strncmp(memfd_name(m), "a-name-considerably-lon", 23), 0);
    CHECK_EQ((int)strlen(memfd_name(m)), MEMFD_NAME_MAX - 1);
    CHECK_STREQ(memfd_first_live_name(), memfd_name(m));
    memfd_unref(m);
    CHECK_STREQ(memfd_name(m), "");
    CHECK_STREQ(memfd_first_live_name(), "");
    struct memfd *anon = memfd_create_obj(NULL);
    REQUIRE(anon != NULL);
    CHECK_STREQ(memfd_name(anon), "");
    memfd_unref(anon);
    CHECK_STREQ(memfd_name(NULL), "");
    clean();
}

TEST(memfd, a_size_change_inside_one_page_is_recorded) {
    clean();
    struct memfd *m = memfd_create_obj("bytes");
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_truncate(m, 100), 0);
    CHECK_EQ(memfd_size(m), 100);
    CHECK_EQ(memfd_pages_held(), 1);
    CHECK_EQ(memfd_truncate(m, 200), 0);
    CHECK_EQ(memfd_size(m), 200);
    CHECK_EQ(memfd_pages_held(), 1);
    CHECK_EQ(memfd_truncate(m, 50), 0);
    CHECK_EQ(memfd_size(m), 50);
    memfd_unref(m);
    clean();
}

#include "sched/sched.h"

static task_t parent_task;
static task_t child_task;

static void region_of(task_t *t, int slot, struct memfd *m, uint32_t pages) {
    t->mmaps[slot].base = 0x100000 + (uint64_t)slot * 0x10000;
    t->mmaps[slot].pages = pages;
    t->mmaps[slot].prot = 0;
    t->mmaps[slot].handle = -1;
    t->mmaps[slot].file_page = 0;
    t->mmaps[slot].shared = 1;
    t->mmaps[slot].memfd_id = (uint8_t)(memfd_slot(m) + 1);
    t->mmaps[slot].memfd_gen = memfd_generation(m);
}

TEST(memfd, a_mapping_is_a_holder_and_a_fork_makes_a_second_one) {
    clean();
    memset(&parent_task, 0, sizeof(parent_task));
    memset(&child_task, 0, sizeof(child_task));
    struct memfd *m = memfd_create_obj("mapped");
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_truncate(m, 2 * PAGE_SIZE), 0);

    region_of(&parent_task, 0, m, 2);
    memfd_region_ref(m);
    memfd_unref(m);
    CHECK_EQ(memfd_in_use(), 1);
    CHECK_EQ(memfd_pages_held(), 2);

    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        child_task.mmaps[i] = parent_task.mmaps[i];
    }
    sched_regions_retain_memfds(&child_task);
    CHECK_EQ(memfd_in_use(), 1);

    sched_regions_forget_memfds(&parent_task);
    CHECK_EQ(memfd_in_use(), 1);
    CHECK_EQ(memfd_pages_held(), 2);
    CHECK_EQ(child_task.mmaps[0].memfd_id, (uint8_t)(memfd_slot(m) + 1));
    CHECK_EQ(parent_task.mmaps[0].memfd_id, 0);
    sched_regions_forget_memfds(&parent_task);
    CHECK_EQ(memfd_in_use(), 1);

    sched_regions_forget_memfds(&child_task);
    CHECK_EQ(memfd_in_use(), 0);
    CHECK_EQ(memfd_pages_held(), 0);
    clean();
}

TEST(memfd, a_split_region_is_two_holders) {
    clean();
    memset(&parent_task, 0, sizeof(parent_task));
    struct memfd *m = memfd_create_obj("split");
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_truncate(m, 4 * PAGE_SIZE), 0);
    region_of(&parent_task, 0, m, 4);
    memfd_region_ref(m);
    memfd_unref(m);

    region_of(&parent_task, 1, m, 2);
    memfd_region_ref(m);
    CHECK_EQ(memfd_in_use(), 1);

    sched_region_forget_memfd(&parent_task.mmaps[0]);
    CHECK_EQ(memfd_in_use(), 1);
    CHECK_EQ(memfd_pages_held(), 4);
    sched_region_forget_memfd(&parent_task.mmaps[1]);
    CHECK_EQ(memfd_in_use(), 0);

    memset(&parent_task, 0, sizeof(parent_task));
    parent_task.mmaps[0].pages = 1;
    parent_task.mmaps[0].handle = -1;
    sched_region_forget_memfd(&parent_task.mmaps[0]);
    sched_regions_forget_memfds(&parent_task);
    sched_regions_retain_memfds(&parent_task);
    CHECK_EQ(parent_task.mmaps[0].pages, 1);
    sched_region_forget_memfd(NULL);
    clean();
}

TEST(memfd, a_duplicate_tag_in_a_freed_slot_does_not_drop_a_reference) {
    clean();
    memset(&parent_task, 0, sizeof(parent_task));
    struct memfd *m = memfd_create_obj("duplicate");
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_truncate(m, PAGE_SIZE), 0);
    region_of(&parent_task, 0, m, 1);
    memfd_region_ref(m);

    parent_task.mmaps[1] = parent_task.mmaps[0];
    parent_task.mmaps[1].pages = 0;
    parent_task.mmaps[1].memfd_id = 0;
    parent_task.mmaps[1].memfd_gen = 0;

    sched_regions_forget_memfds(&parent_task);
    CHECK_EQ(memfd_in_use(), 1);
    memfd_unref(m);
    CHECK_EQ(memfd_in_use(), 0);
    clean();
}

TEST(memfd, a_fork_of_a_region_whose_object_died_takes_no_reference) {
    clean();
    memset(&parent_task, 0, sizeof(parent_task));
    memset(&child_task, 0, sizeof(child_task));
    struct memfd *m = memfd_create_obj("vanished");
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_truncate(m, PAGE_SIZE), 0);
    uint8_t slot = memfd_slot(m);
    uint16_t gen = memfd_generation(m);
    region_of(&parent_task, 0, m, 1);
    memfd_unref(m);
    CHECK_EQ(memfd_in_use(), 0);
    CHECK(memfd_by_tag(slot, gen) == NULL);

    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        child_task.mmaps[i] = parent_task.mmaps[i];
    }
    sched_regions_retain_memfds(&child_task);
    CHECK_EQ(child_task.mmaps[0].memfd_id, 0);
    CHECK_EQ(memfd_in_use(), 0);
    struct memfd *next = memfd_create_obj("next");
    REQUIRE(next != NULL);
    CHECK_EQ(memfd_slot(next), slot);
    CHECK(memfd_by_tag(slot, gen) == NULL);
    memfd_unref(next);
    clean();
}
