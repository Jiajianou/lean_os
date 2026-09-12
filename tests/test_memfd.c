/* tests/test_memfd.c - M120: shared memory a descriptor names, off the
 * machine.
 *
 * What this tier can reach that a booted machine cannot, in the order the
 * failures would matter:
 *
 *   - **the zeroing.** A frame handed to a second process must not carry
 *     whatever this machine last used it for. On the machine that is
 *     untestable in the way that matters - the frames a fresh boot hands
 *     out are mostly zero already, so a kernel that forgot to clear them
 *     would pass every test and leak on the hundredth allocation. Here the
 *     fake frame allocator can hand out dirty memory on purpose.
 *   - **the generation.** A slot freed and handed out again must not let a
 *     stale mapping name a different object's memory. Reaching that on the
 *     machine means winning a race; here it is four lines.
 *   - **the refusals**: a size past the cap, a shrink, a seal that is
 *     already sealed, an allocation failure part-way through a grow.
 */
#include "check.h"

#include "fakes/fakes.h"
#include "ipc/memfd.h"
#include "proc/proc.h" /* PAGE_SIZE */

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
    /* Empty, which is what memfd_create gives you on every system: a
     * descriptor naming no memory at all until somebody sizes it. */
    CHECK_EQ(memfd_size(m), 0);
    CHECK_EQ(memfd_pages_held(), 0);
    CHECK_EQ(memfd_frame(m, 0), 0);

    CHECK_EQ(memfd_truncate(m, 3 * PAGE_SIZE), 0);
    CHECK_EQ(memfd_size(m), 3 * PAGE_SIZE);
    CHECK_EQ(memfd_pages_held(), 3);
    CHECK_NE(memfd_frame(m, 2), 0);
    /* And page three does not exist, which is what stops a mapping from
     * running off the end of the object. */
    CHECK_EQ(memfd_frame(m, 3), 0);

    /* A byte count that is not a whole number of pages is kept as bytes -
     * fstat reports this number, and a caller that asked for 100 and got
     * 4096 would allocate its own structures wrong. */
    CHECK_EQ(memfd_truncate(m, 3 * PAGE_SIZE + 100), 0);
    CHECK_EQ(memfd_size(m), 3 * PAGE_SIZE + 100);
    CHECK_EQ(memfd_pages_held(), 4);

    memfd_unref(m);
    CHECK_EQ(memfd_in_use(), 0);
    CHECK_EQ(memfd_pages_held(), 0); /* and the frames went with it */
}

TEST(memfd, every_frame_is_zeroed_before_anybody_can_see_it) {
    clean();
    /* tests/fakes/fake_pmm.c hands out frames filled with 0xCD, and it
     * does that on purpose - "the real allocator makes no promise about a
     * fresh frame's contents, so code that depends on one being zero is
     * wrong and should fail here rather than on hardware that happens to
     * oblige". This is the one object in the kernel that must not depend
     * on it and must do it itself.
     *
     * What a missing memset would leak is the worst thing this object
     * could: anonymous memory from whatever ran before, to a process that
     * may hold no capability at all and is about to be handed this
     * descriptor. And it would pass on a freshly booted machine, where
     * most frames are zero anyway. */
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
    /* Growing keeps the frames that were already there, which matters
     * because somebody may already be looking at them - a mapping that
     * moved under a reader would be the worst kind of wrong. */
    CHECK_EQ(memfd_frame(m, 1), second);
    CHECK_EQ(memfd_pages_held(), 8);

    /* Shrinking is refused, and this is a divergence from Linux rather
     * than an omission: Linux answers a mapping of freed pages with
     * SIGBUS, and this kernel has no such machinery - so the conservative
     * half of the same answer is to refuse. */
    CHECK_EQ(memfd_truncate(m, 2 * PAGE_SIZE), -1);
    CHECK_EQ(memfd_size(m), 8 * PAGE_SIZE);
    CHECK_EQ(memfd_pages_held(), 8);

    /* And a size past the cap is refused with nothing allocated. */
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
    /* Three more pages wanted, two available. The object has to come back
     * exactly as it was - not a bit bigger, not with three pages it cannot
     * account for - and the frames it did claim have to go back, because a
     * failed call that keeps memory is a leak on the error path, which is
     * the path nothing exercises. */
    fake_pmm_fail_after((int64_t)fake_pmm_total_allocs() + 2);
    CHECK_EQ(memfd_truncate(m, 5 * PAGE_SIZE), -1);
    fake_pmm_fail_after(-1);
    CHECK_EQ(memfd_size(m), 2 * PAGE_SIZE);
    CHECK_EQ(memfd_pages_held(), 2);
    CHECK_EQ(memfd_frame(m, 0), first);
    CHECK_EQ(fake_pmm_outstanding(), before);
    /* And it still works afterwards - Q9's rule, on a new table. */
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

    /* mmap takes a reference for the region; close(2) drops the
     * descriptor's. This is the sequence every program that shares memory
     * performs, and the object has to survive it - that is POSIX, and it
     * is the whole reason this object is refcounted by two different
     * things. */
    memfd_region_ref(m);
    memfd_unref(m); /* the descriptor goes */
    CHECK_EQ(memfd_in_use(), 1);
    CHECK(memfd_by_tag(slot, gen) == m);
    CHECK_NE(memfd_frame(m, 0), 0);

    /* And when the mapping goes too, so does the memory. */
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

    /* The slot is free and the next caller gets it - which on a machine
     * with 32 slots and a busy browser is not a corner case, it is
     * Tuesday. A stale region naming (slot, old generation) must find
     * NOTHING here. Finding the new object would be a window into another
     * program's memory, which is the one failure in this file that would
     * be silent. */
    struct memfd *second = memfd_create_obj("second");
    REQUIRE(second != NULL);
    CHECK_EQ(memfd_slot(second), slot);
    CHECK_NE(memfd_generation(second), gen);
    CHECK(memfd_by_tag(slot, gen) == NULL);
    CHECK(memfd_by_tag(slot, memfd_generation(second)) == second);
    /* And a slot number that never existed. */
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
    /* The seal that makes read-only shared memory possible: the sender
     * seals writing and passes the descriptor, and the receiver can verify
     * rather than trust. mmap is where this is enforced - memfd_may_write
     * is the question it asks. */
    CHECK_EQ(memfd_may_write(m), 0);

    /* Growing is still allowed: F_SEAL_WRITE is about contents. */
    CHECK_EQ(memfd_truncate(m, 3 * PAGE_SIZE), 0);
    CHECK_EQ(memfd_add_seals(m, MEMFD_SEAL_GROW), 0);
    CHECK_EQ(memfd_truncate(m, 4 * PAGE_SIZE), -1);
    CHECK_EQ(memfd_size(m), 3 * PAGE_SIZE);

    /* A seal this kernel does not implement is refused rather than
     * ignored - a promise nobody keeps is worse than no promise. */
    CHECK_EQ(memfd_add_seals(m, 0x40), -1);
    CHECK_EQ(memfd_get_seals(m), MEMFD_SEAL_WRITE | MEMFD_SEAL_GROW);

    /* And F_SEAL_SEAL stops every further seal, which is how a sender
     * stops a receiver sealing MORE than was agreed. */
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
    /* Not writable rather than writable: a mapping of nothing must not be
     * given permission to write to it. */
    CHECK_EQ(memfd_may_write(NULL), 0);
    CHECK_EQ(memfd_slot(NULL), 0);
    CHECK_EQ(memfd_generation(NULL), 0);
    memfd_ref(NULL);
    memfd_unref(NULL);
    memfd_region_ref(NULL);
    memfd_region_unref(NULL);

    /* And an object whose last holder has gone. The pointer is still a
     * valid address - these live in a fixed table, not on the heap - so
     * every accessor has to answer about a slot that is no longer in use
     * rather than about whatever is left in it. A size of anything but zero
     * here would have a caller mapping a region that does not exist. */
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
    /* memfd_create's name is for diagnostics - the boot self-test prints it
     * when its leak check fails - so a long one is truncated rather than
     * rejected: refusing would break a program for a reason that cannot
     * matter. What must not happen is a copy past the field, which ASan
     * reports here. */
    struct memfd *m = memfd_create_obj(
        "a-name-considerably-longer-than-the-twenty-four-bytes-this-field-has");
    REQUIRE(m != NULL);
    CHECK_EQ(memfd_truncate(m, PAGE_SIZE), 0);
    CHECK_EQ(strncmp(memfd_name(m), "a-name-considerably-lon", 23), 0);
    CHECK_EQ((int)strlen(memfd_name(m)), MEMFD_NAME_MAX - 1);
    CHECK_STREQ(memfd_first_live_name(), memfd_name(m));
    memfd_unref(m);
    /* And a dead object names nothing, rather than whatever is left in the
     * slot - which is what makes the self-test's message trustworthy. */
    CHECK_STREQ(memfd_name(m), "");
    CHECK_STREQ(memfd_first_live_name(), "");
    /* An unnamed one is empty rather than uninitialised. */
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
    /* 100 -> 200 allocates nothing and still has to be recorded: fstat
     * reports bytes, and a receiver that sized its own structures from 100
     * when the sender had written 200 would read half a message. */
    CHECK_EQ(memfd_truncate(m, 200), 0);
    CHECK_EQ(memfd_size(m), 200);
    CHECK_EQ(memfd_pages_held(), 1);
    /* And downwards within the page, which frees no frame and so endangers
     * no mapping - the reason the shrink refusal is about PAGES and not
     * about bytes. */
    CHECK_EQ(memfd_truncate(m, 50), 0);
    CHECK_EQ(memfd_size(m), 50);
    memfd_unref(m);
    clean();
}

/* ---- the half of M120 that is not in memfd.c --------------------------
 *
 * A mapping outlives the descriptor that made it, so an mmap region is a
 * holder of the object in its own right - and that means every path where a
 * region stops describing a mapping has to let go. There are four (munmap
 * removing a slot, execve clearing the table, a task slot being recycled,
 * and fork copying the table, which takes a reference rather than dropping
 * one), and kernel/sched/sched.c has three helpers so that each of those is
 * one line.
 *
 * This is the most dangerous bookkeeping in the milestone: one reference too
 * few frees memory two processes are still reading, and one too many leaks
 * 16 MiB that nothing will ever return. Graded here against the real object
 * rather than a fake, because what is being checked IS the count.
 */
#include "sched/sched.h"

/* Static rather than on the stack: a task_t is several tens of kilobytes,
 * and two of them is more than a host test's stack should be asked for. */
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

    /* What sys_mmap does: record the region and take a reference for it. */
    region_of(&parent_task, 0, m, 2);
    memfd_region_ref(m);
    /* And then the program closes the descriptor. */
    memfd_unref(m);
    CHECK_EQ(memfd_in_use(), 1);
    CHECK_EQ(memfd_pages_held(), 2);

    /* A fork: the child's copy of the table is a second holder, because the
     * memory is shared - which is what the mapping was for. */
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        child_task.mmaps[i] = parent_task.mmaps[i];
    }
    sched_regions_retain_memfds(&child_task);
    CHECK_EQ(memfd_in_use(), 1);

    /* The parent exits. The child is still reading those frames. */
    sched_regions_forget_memfds(&parent_task);
    CHECK_EQ(memfd_in_use(), 1);
    CHECK_EQ(memfd_pages_held(), 2);
    CHECK_EQ(child_task.mmaps[0].memfd_id, (uint8_t)(memfd_slot(m) + 1));
    /* And the tag is cleared on the side that let go, so a second release
     * of the same table cannot drop a reference twice - which is what would
     * happen on a task slot that was recycled after an exec. */
    CHECK_EQ(parent_task.mmaps[0].memfd_id, 0);
    sched_regions_forget_memfds(&parent_task);
    CHECK_EQ(memfd_in_use(), 1);

    /* The child exits too, and only now does the memory go. */
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

    /* What munmap's interior cut and mprotect's split do: the tail becomes
     * a region of its own, naming the same object, and takes a reference of
     * its own. Two regions, two holders - and if the split had forgotten to
     * take one, removing either half would free memory the other half is
     * still mapping. */
    region_of(&parent_task, 1, m, 2);
    memfd_region_ref(m);
    CHECK_EQ(memfd_in_use(), 1);

    sched_region_forget_memfd(&parent_task.mmaps[0]);
    CHECK_EQ(memfd_in_use(), 1);
    CHECK_EQ(memfd_pages_held(), 4);
    sched_region_forget_memfd(&parent_task.mmaps[1]);
    CHECK_EQ(memfd_in_use(), 0);

    /* A region that names no memfd is left alone by all of this, which is
     * every region this kernel made before M120. */
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

    /* What munmap's slot removal leaves behind: it shifts the table down by
     * one, so the vacated slot at the end holds a DUPLICATE of a live
     * region - same tag, same generation - and sched_regions_forget_memfds
     * walks every slot, not only the live ones. A duplicate left there
     * drops a reference nobody took, which frees memory a process is still
     * mapping.
     *
     * Found by reading the asymmetry rather than by a failure:
     * sched_regions_retain_memfds skips empty slots and forget did not.
     * This test is the shape of the bug, written against the fixed code. */
    parent_task.mmaps[1] = parent_task.mmaps[0];
    parent_task.mmaps[1].pages = 0; /* free, but holding the tag - the bug */
    parent_task.mmaps[1].memfd_id = 0; /* ...which the fix clears */
    parent_task.mmaps[1].memfd_gen = 0;

    sched_regions_forget_memfds(&parent_task);
    /* One reference dropped, not two: the descriptor's is still outstanding
     * because this test never closed it. */
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
    /* Deliberately no region reference: this models the window the region
     * reference exists to close, reached here by leaving it out. The object
     * dies with the descriptor. */
    memfd_unref(m);
    CHECK_EQ(memfd_in_use(), 0);
    CHECK(memfd_by_tag(slot, gen) == NULL);

    /* A fork must not take a reference to something that is gone, and must
     * not leave the child naming a slot that may be handed to somebody
     * else - the stale tag is cleared instead, so the child's fault is a
     * SIGSEGV rather than a window into another program's memory. */
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        child_task.mmaps[i] = parent_task.mmaps[i];
    }
    sched_regions_retain_memfds(&child_task);
    CHECK_EQ(child_task.mmaps[0].memfd_id, 0);
    CHECK_EQ(memfd_in_use(), 0);
    /* And a new object reusing that slot is not reachable through the old
     * tag, which is the property the generation is for. */
    struct memfd *next = memfd_create_obj("next");
    REQUIRE(next != NULL);
    CHECK_EQ(memfd_slot(next), slot);
    CHECK(memfd_by_tag(slot, gen) == NULL);
    memfd_unref(next);
    clean();
}
