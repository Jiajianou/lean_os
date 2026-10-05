/* M225: the descriptor SLOT under real threads.

   tests/test_descriptor_table_threads.c grades the table's reference count -
   how many threads point at one table. This grades what is in it: one slot,
   on which sibling threads close, dup, dup2, F_DUPFD, hold (what SCM_RIGHTS
   and /proc/self/fd reopen do) and copy the whole table (what fork and spawn
   do) at the same instant. The object counts were locked; the slot was not.
   sys_close looked at the type and released, so two threads closing one
   descriptor both released it - a socket, eventfd or memfd freed with a
   descriptor still counting on it - and every copy-then-retain could take
   its reference after a sibling's close had dropped the last one.

   The workers call only the slot protocol in scheduler.c (file_descriptor_*,
   descriptor_table_*), with fake_spinlock_threaded on so the table's lock is
   a real one, and the objects are the fakes' pipe write ends, whose count is
   atomic and which can say when a reference was taken on an object nobody
   held any more (fake_objects_pipe_write_floor). Nothing on a worker touches
   the heap or the check macros; outcomes are written down and judged on the
   main thread after the join.

   Deterministic in the direction that matters, as its sibling file is: on
   the fixed protocol every check holds for every interleaving (the lock
   makes each operation one step), so it cannot flake red; how quickly the
   OLD protocol fails is what varies, and that is measured in the commit
   message. Fixed work, no clocks, the same thread count on either Mac. */
#include "check.h"
#include "fakes/fakes.h"

#include "memory_management/heap.h"
#include "scheduler/scheduler.h"

#include <pthread.h>
#include <sched.h>

#define SLOT 5
#define DUP2_TARGET 20
#define DUPFD_FROM 30
#define CLOSE_ROUNDS 4000
#define CLOSERS 4
#define MIXED_ROUNDS 3000
#define MIXED_THREADS 5

typedef struct {
    int arrived;
    int generation;
} spin_barrier_t;

static void barrier_wait(spin_barrier_t *b, int parties) {
    int generation = __atomic_load_n(&b->generation, __ATOMIC_ACQUIRE);
    if (__atomic_add_fetch(&b->arrived, 1, __ATOMIC_ACQ_REL) == parties) {
        __atomic_store_n(&b->arrived, 0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&b->generation, 1, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&b->generation, __ATOMIC_ACQUIRE) == generation) {
        sched_yield();
    }
}

static void clean(void) {
    fake_physical_memory_reset();
    fake_virtual_memory_reset();
    heap_init();
    fake_objects_reset();
}

static void open_write_end(file_descriptor_table_t *table, int fd) {
    file_descriptor_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.type = FILE_DESCRIPTOR_PIPE_WRITE;
    slot.pipe = (struct pipe *)0x2000;
    REQUIRE(file_descriptor_claim(table, fd) == fd);
    file_descriptor_install(table, fd, &slot);
}

static int open_write_ends(const file_descriptor_table_t *table) {
    int n = 0;
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
        if (table->slots[i].type == FILE_DESCRIPTOR_PIPE_WRITE) {
            n++;
        }
    }
    return n;
}

static void close_everything(file_descriptor_table_t *table) {
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
        file_descriptor_slot_t gone;
        if (file_descriptor_detach(table, i, &gone) == 0) {
            file_descriptor_release(&gone);
        }
    }
}

/* ---- every thread closing the same descriptor at once ---- */

static spin_barrier_t close_start, close_end;
static file_descriptor_table_t *close_table;
static int close_over;
static int close_won[CLOSERS];

static void *closer(void *arg) {
    int me = (int)(long)arg;
    for (;;) {
        barrier_wait(&close_start, CLOSERS + 1);
        if (__atomic_load_n(&close_over, __ATOMIC_ACQUIRE)) {
            break;
        }
        file_descriptor_slot_t gone;
        close_won[me] = file_descriptor_detach(close_table, SLOT, &gone) == 0;
        if (close_won[me]) {
            file_descriptor_release(&gone);
        }
        barrier_wait(&close_end, CLOSERS + 1);
    }
    return 0;
}

TEST(descriptor_slots_threads, threads_closing_one_descriptor_at_once_release_it_once) {
    clean();
    fake_spinlock_threaded(1);
    close_table = descriptor_table_new();
    REQUIRE(close_table != NULL);
    close_over = 0;
    close_start.arrived = close_end.arrived = 0;

    pthread_t workers[CLOSERS];
    for (long i = 0; i < CLOSERS; i++) {
        REQUIRE(pthread_create(&workers[i], 0, closer, (void *)i) == 0);
    }
    int first_wrong = -1, wins_then = 0, released_then = 0;
    for (int round = 0; round < CLOSE_ROUNDS && first_wrong < 0; round++) {
        open_write_end(close_table, SLOT);
        int before = fake_objects_pipe_write_refs();
        barrier_wait(&close_start, CLOSERS + 1);
        barrier_wait(&close_end, CLOSERS + 1);
        int wins = 0;
        for (int i = 0; i < CLOSERS; i++) {
            wins += close_won[i];
        }
        int released = before - fake_objects_pipe_write_refs();
        if (wins != 1 || released != 1 || close_table->slots[SLOT].type != FILE_DESCRIPTOR_NONE) {
            first_wrong = round;
            wins_then = wins;
            released_then = released;
        }
    }
    __atomic_store_n(&close_over, 1, __ATOMIC_RELEASE);
    barrier_wait(&close_start, CLOSERS + 1);
    for (int i = 0; i < CLOSERS; i++) {
        pthread_join(workers[i], 0);
    }
    fake_spinlock_threaded(0);
    CHECK_MSG(first_wrong < 0,
              "round %d of %d: %d threads closed descriptor %d at once - %d of them were "
              "told they closed it and its object was released %d time(s); one and once "
              "is the only right answer",
              first_wrong, CLOSE_ROUNDS, CLOSERS, SLOT, wins_then, released_then);
    close_everything(close_table);
    descriptor_table_release(close_table);
}

/* ---- close, dup2, F_DUPFD, hold and fork's copy of one slot at once ---- */

static spin_barrier_t mixed_start, mixed_end;
static file_descriptor_table_t *mixed_table;
static file_descriptor_table_t *mixed_child; /* fork's copy, made by a worker */
static int mixed_over;
static int mixed_round;
static int mixed_close_won[2];
static int mixed_dup2_got;
static int mixed_dupfd_got;
static int mixed_held;
static int mixed_child_ends;

static void *mixed_worker(void *arg) {
    int me = (int)(long)arg;
    for (;;) {
        barrier_wait(&mixed_start, MIXED_THREADS + 1);
        if (__atomic_load_n(&mixed_over, __ATOMIC_ACQUIRE)) {
            break;
        }
        file_descriptor_slot_t slot;
        switch (me) {
        case 0:
        case 1:
            mixed_close_won[me] = file_descriptor_detach(mixed_table, SLOT, &slot) == 0;
            if (mixed_close_won[me]) {
                file_descriptor_release(&slot);
            }
            break;
        case 2:
            mixed_dup2_got = descriptor_table_duplicate(mixed_table, SLOT, DUP2_TARGET, 0, &slot);
            file_descriptor_release(&slot);
            break;
        case 3:
            mixed_dupfd_got = descriptor_table_duplicate_lowest(mixed_table, SLOT, DUPFD_FROM, 0);
            break;
        default:
            /* What SCM_RIGHTS and /proc/self/fd do on even rounds - hold,
               use, let go - and what fork does on odd ones. */
            if (mixed_round & 1) {
                descriptor_table_copy(mixed_child, mixed_table, 0, MAX_FILE_DESCRIPTORS);
                mixed_child_ends = open_write_ends(mixed_child);
                for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
                    file_descriptor_release(&mixed_child->slots[i]);
                }
            } else {
                mixed_held = file_descriptor_hold(mixed_table, SLOT, &slot) == 0;
                file_descriptor_release(&slot);
            }
            break;
        }
        barrier_wait(&mixed_end, MIXED_THREADS + 1);
    }
    return 0;
}

TEST(descriptor_slots_threads, no_copy_of_a_slot_takes_a_reference_its_close_already_let_go) {
    clean();
    fake_spinlock_threaded(1);
    mixed_table = descriptor_table_new();
    mixed_child = descriptor_table_new();
    REQUIRE(mixed_table != NULL);
    REQUIRE(mixed_child != NULL);
    mixed_over = 0;
    mixed_start.arrived = mixed_end.arrived = 0;

    pthread_t workers[MIXED_THREADS];
    for (long i = 0; i < MIXED_THREADS; i++) {
        REQUIRE(pthread_create(&workers[i], 0, mixed_worker, (void *)i) == 0);
    }
    int first_wrong = -1;
    char why[160] = "";
    for (int round = 0; round < MIXED_ROUNDS && first_wrong < 0; round++) {
        mixed_round = round;
        open_write_end(mixed_table, SLOT);
        /* The slot just opened is one reference the fakes do not count, so
           the object is gone when the count is one below where it starts. */
        int start = fake_objects_pipe_write_refs();
        fake_objects_pipe_write_floor(start - 1);
        int revivals_before = fake_objects_pipe_write_revivals();
        mixed_close_won[0] = mixed_close_won[1] = 0;
        mixed_dup2_got = mixed_dupfd_got = -9;
        barrier_wait(&mixed_start, MIXED_THREADS + 1);
        barrier_wait(&mixed_end, MIXED_THREADS + 1);
        fake_objects_pipe_write_floor(-2147483647 - 1);

        int closes = mixed_close_won[0] + mixed_close_won[1];
        int open_now = open_write_ends(mixed_table);
        int counted = fake_objects_pipe_write_refs() - (start - 1);
        int revived = fake_objects_pipe_write_revivals() - revivals_before;
        if (closes != 1) {
            snprintf(why, sizeof(why), "%d closes of descriptor %d succeeded", closes, SLOT);
        } else if (revived != 0) {
            snprintf(why, sizeof(why),
                     "%d reference(s) taken on the object after its last one was let go",
                     revived);
        } else if (counted != open_now) {
            snprintf(why, sizeof(why),
                     "%d descriptor(s) open on the object and %d reference(s) counted",
                     open_now, counted);
        } else if ((mixed_dup2_got != DUP2_TARGET) !=
                   (mixed_table->slots[DUP2_TARGET].type != FILE_DESCRIPTOR_PIPE_WRITE)) {
            snprintf(why, sizeof(why), "dup2 answered %d and left %d %s", mixed_dup2_got,
                     DUP2_TARGET,
                     mixed_table->slots[DUP2_TARGET].type == FILE_DESCRIPTOR_PIPE_WRITE
                         ? "open"
                         : "closed");
        } else if (mixed_dupfd_got >= 0 && mixed_dupfd_got != DUPFD_FROM) {
            snprintf(why, sizeof(why), "F_DUPFD from %d answered %d", DUPFD_FROM,
                     mixed_dupfd_got);
        }
        if (why[0]) {
            first_wrong = round;
        }
        close_everything(mixed_table);
        if (fake_objects_pipe_write_refs() != start - 1 && !why[0]) {
            first_wrong = round;
            snprintf(why, sizeof(why), "closing every descriptor left %d reference(s)",
                     fake_objects_pipe_write_refs() - (start - 1));
        }
    }
    __atomic_store_n(&mixed_over, 1, __ATOMIC_RELEASE);
    barrier_wait(&mixed_start, MIXED_THREADS + 1);
    for (int i = 0; i < MIXED_THREADS; i++) {
        pthread_join(workers[i], 0);
    }
    fake_spinlock_threaded(0);
    CHECK_MSG(first_wrong < 0, "round %d of %d: %s", first_wrong, MIXED_ROUNDS, why);
    descriptor_table_release(mixed_table);
    descriptor_table_release(mixed_child);
}

/* ---- the protocol on one thread: a reserved slot is its claimant's ---- */

TEST(descriptor_slots, a_slot_being_filled_in_is_nobody_elses_to_close_copy_or_replace) {
    clean();
    file_descriptor_table_t *table = descriptor_table_new();
    REQUIRE(table != NULL);
    open_write_end(table, 3);
    REQUIRE(file_descriptor_claim(table, 4) == 4); /* RESERVED, not filled yet */

    file_descriptor_slot_t out;
    CHECK_EQ(file_descriptor_detach(table, 4, &out), DESCRIPTOR_TABLE_BAD);
    CHECK_EQ(file_descriptor_hold(table, 4, &out), DESCRIPTOR_TABLE_BAD);
    CHECK_EQ(descriptor_table_duplicate_lowest(table, 4, 10, 0), DESCRIPTOR_TABLE_BAD);
    CHECK_EQ(descriptor_table_duplicate(table, 4, 9, 0, &out), DESCRIPTOR_TABLE_BAD);
    /* dup2 ONTO it is refused too - its claimant is about to install there
       and one of the two objects would be lost. */
    CHECK_EQ(descriptor_table_duplicate(table, 3, 4, 0, &out), DESCRIPTOR_TABLE_BUSY);
    CHECK_EQ(out.type, FILE_DESCRIPTOR_NONE);
    CHECK_EQ(table->slots[4].type, FILE_DESCRIPTOR_RESERVED);
    CHECK_EQ(file_descriptor_set_flags(table, 4, 1, -1), DESCRIPTOR_TABLE_BAD);

    /* A fork's copy leaves it behind, and takes the open one with it. */
    file_descriptor_table_t *child = descriptor_table_new();
    REQUIRE(child != NULL);
    descriptor_table_copy(child, table, 0, MAX_FILE_DESCRIPTORS);
    CHECK_EQ(child->slots[3].type, FILE_DESCRIPTOR_PIPE_WRITE);
    CHECK_EQ(child->slots[4].type, FILE_DESCRIPTOR_NONE);
    CHECK_EQ(fake_objects_pipe_write_refs(), 1);
    descriptor_table_release(child);
    CHECK_EQ(fake_objects_pipe_write_refs(), 0);

    file_descriptor_unclaim(table, 4);
    CHECK_EQ(table->slots[4].type, FILE_DESCRIPTOR_NONE);
    descriptor_table_release(table);
    CHECK_EQ(fake_objects_pipe_write_refs(), -1);
}

TEST(descriptor_slots, installing_into_a_slot_nobody_claimed_panics) {
    clean();
    file_descriptor_table_t *table = descriptor_table_new();
    REQUIRE(table != NULL);
    file_descriptor_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.type = FILE_DESCRIPTOR_PIPE_WRITE;
    CHECK_PANIC(file_descriptor_install(table, 6, &slot), "a slot nobody claimed");
    fake_spinlock_release_all();
    descriptor_table_release(table);
}

TEST(descriptor_slots, close_on_exec_detaches_only_what_asks_for_it_and_spawn_copies_the_rest) {
    clean();
    file_descriptor_table_t *table = descriptor_table_new();
    REQUIRE(table != NULL);
    open_write_end(table, 3);
    open_write_end(table, 4);
    open_write_end(table, 7);
    CHECK_EQ(file_descriptor_set_flags(table, 4, 1, -1), 0);
    CHECK_EQ(file_descriptor_set_flags(table, 7, 1, 1), 0);
    CHECK_EQ(table->slots[7].nonblock, 1);

    /* spawn: an exec, so the close-on-exec ones stay behind; and a program
       the kernel starts keeps only 0-2 (keep_below 3). */
    file_descriptor_table_t *spawned = descriptor_table_new();
    REQUIRE(spawned != NULL);
    descriptor_table_copy(spawned, table, 1, MAX_FILE_DESCRIPTORS);
    CHECK_EQ(open_write_ends(spawned), 1);
    CHECK_EQ(spawned->slots[3].type, FILE_DESCRIPTOR_PIPE_WRITE);
    descriptor_table_release(spawned);
    file_descriptor_table_t *from_kernel = descriptor_table_new();
    REQUIRE(from_kernel != NULL);
    descriptor_table_copy(from_kernel, table, 1, 3);
    CHECK_EQ(open_write_ends(from_kernel), 0);
    descriptor_table_release(from_kernel);
    CHECK_EQ(fake_objects_pipe_write_refs(), 0);

    file_descriptor_slot_t gone;
    CHECK_EQ(descriptor_table_detach_cloexec(table, 0, &gone), 4);
    CHECK_EQ(gone.type, FILE_DESCRIPTOR_PIPE_WRITE);
    file_descriptor_release(&gone);
    CHECK_EQ(descriptor_table_detach_cloexec(table, 5, &gone), 7);
    file_descriptor_release(&gone);
    CHECK_EQ(descriptor_table_detach_cloexec(table, 8, &gone), DESCRIPTOR_TABLE_BAD);
    CHECK_EQ(open_write_ends(table), 1);
    CHECK_EQ(fake_objects_pipe_write_refs(), -2);
    descriptor_table_release(table);
    CHECK_EQ(fake_objects_pipe_write_refs(), -3);
}

TEST(descriptor_slots, dup2_hands_back_what_it_replaced_and_duplicates_count_once_each) {
    clean();
    file_descriptor_table_t *table = descriptor_table_new();
    REQUIRE(table != NULL);
    open_write_end(table, 3);
    open_write_end(table, 8);
    CHECK_EQ(file_descriptor_set_flags(table, 3, 1, -1), 0);

    file_descriptor_slot_t displaced;
    CHECK_EQ(descriptor_table_duplicate(table, 3, 8, 0, &displaced), 8);
    CHECK_EQ(displaced.type, FILE_DESCRIPTOR_PIPE_WRITE); /* what 8 held */
    CHECK_EQ(table->slots[8].cloexec, 0);                 /* POSIX: cleared */
    file_descriptor_release(&displaced);
    CHECK_EQ(descriptor_table_duplicate(table, 3, 3, 0, &displaced), 3);
    CHECK_EQ(displaced.type, FILE_DESCRIPTOR_NONE);
    CHECK_EQ(descriptor_table_duplicate(table, 3, MAX_FILE_DESCRIPTORS, 0, &displaced),
             DESCRIPTOR_TABLE_BAD);

    CHECK_EQ(descriptor_table_duplicate_lowest(table, 3, 0, 1), 0);
    CHECK_EQ(table->slots[0].cloexec, 1);
    CHECK_EQ(descriptor_table_duplicate_lowest(table, 3, 4, 0), 4);
    CHECK_EQ(descriptor_table_duplicate_lowest(table, 6, 4, 0), DESCRIPTOR_TABLE_BAD);
    CHECK_EQ(open_write_ends(table), 4);
    /* The two opened above are uncounted; dup2 counted one into 8 and let
       the old 8 go (-1); 0 and 4 are one each. */
    CHECK_EQ(fake_objects_pipe_write_refs(), 2);

    file_descriptor_slot_t held;
    CHECK_EQ(file_descriptor_hold(table, 4, &held), 0);
    CHECK_EQ(fake_objects_pipe_write_refs(), 3);
    file_descriptor_release(&held);
    CHECK_EQ(fake_objects_pipe_write_refs(), 2);
    close_everything(table);
    CHECK_EQ(fake_objects_pipe_write_refs(), -2);
    descriptor_table_release(table);
}
