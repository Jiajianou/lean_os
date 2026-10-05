/* M225 (fd-use-holds): USING a descriptor under real threads.

   tests/test_descriptor_slots_threads.c grades taking a descriptor OUT of a
   slot (close, dup2) and copying one with a reference of its own (dup, fork,
   SCM_RIGHTS). This grades the third thing a call does with a slot: use
   what is in it - read, write, ioctl, poll's readiness, epoll's mask - for
   the length of the call. Every one of those read the slot and used its
   object with no reference of its own, so a sibling's close() in the middle
   of the call dropped the last reference under it: on the machine a unix
   socket, eventfd, timerfd, epoll set or memfd freed while a read was still
   using it, and a pipe end or open file - whose counts clamp - a reference
   somebody else still counted, gone.

   The workers are tasks of one process: each has a task_t of its own naming
   the one shared table, as threads do, so file_descriptor_get takes the
   counted path. A user gets the descriptor, uses the object twice with a
   yield between (fake_objects_pipe_write_use, which says whether the object
   was alive - its count above "nobody" - at that moment), and puts it back.
   Closers and a dup2 that replaces the slot let go of it meanwhile. Judged
   on the main thread after each round: no use of a dead object, exactly one
   close, every reference given back (the count returns to "nobody" and no
   task has a use left on its list).

   Deterministic in the direction that matters, as its sibling files are:
   on the fixed code a use holds a reference, so no interleaving can make one
   dead; how quickly the old code fails is what varies, and that is measured
   in the commit message. Fixed work, no clocks. */
#include "check.h"
#include "fakes/fakes.h"

#include "memory_management/heap.h"
#include "scheduler/scheduler.h"

#include <pthread.h>
#include <sched.h>

#define SLOT 5
#define DUP2_SOURCE 9
#define USERS 3
#define USE_THREADS (USERS + 2) /* two closers, then one dup2 over the slot */
#define USE_ROUNDS 3000

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

static int tasks_used;

static void clean(void) {
    tasks_used = 0;
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

static void close_everything(file_descriptor_table_t *table) {
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
        file_descriptor_slot_t gone;
        if (file_descriptor_detach(table, i, &gone) == 0) {
            file_descriptor_release(&gone);
        }
    }
}

/* A task of the process: its own task_t, the process's table. (Static
   storage: <stdlib.h> and the kernel's signal headers do not mix.) */
#define TASKS_NEEDED (USE_THREADS + 2)
static task_t task_storage[TASKS_NEEDED];

static task_t *task_with(file_descriptor_table_t *table, int counted) {
    REQUIRE(tasks_used < TASKS_NEEDED);
    task_t *t = &task_storage[tasks_used++];
    memset(t, 0, sizeof(*t));
    t->descriptor_table = table;
    if (counted) {
        descriptor_table_reference(table);
    }
    return t;
}

static task_t *thread_of(file_descriptor_table_t *table) {
    return task_with(table, 1);
}

static void thread_gone(task_t *t) {
    descriptor_table_release(t->descriptor_table);
    t->descriptor_table = NULL;
}

/* ---- calls using one descriptor while siblings close and replace it ---- */

static spin_barrier_t use_start, use_end;
static file_descriptor_table_t *use_table;
static task_t *use_tasks[USE_THREADS];
static int use_over;
static int use_got[USERS];
/* Two closers and a dup2 race for the slot, so zero, one or two closes
   succeed (a closer can take the socket the dup2 put there); what must hold
   is that the pipe end is let go of exactly once, by whichever got it. */
static int use_close_won[2];
static int use_dup2_got;

static void *use_worker(void *arg) {
    int me = (int)(long)arg;
    task_t *self = use_tasks[me];
    for (;;) {
        barrier_wait(&use_start, USE_THREADS + 1);
        if (__atomic_load_n(&use_over, __ATOMIC_ACQUIRE)) {
            break;
        }
        if (me < USERS) {
            /* What sys_read, sys_write, sys_ioctl and poll do: get, use,
               block or think for a moment, use again, put. */
            file_descriptor_use_t use;
            use_got[me] = file_descriptor_get(self, SLOT, &use) == 0;
            if (use_got[me]) {
                if (use.slot.type == FILE_DESCRIPTOR_PIPE_WRITE) {
                    fake_objects_pipe_write_use();
                    sched_yield();
                    fake_objects_pipe_write_use();
                }
                file_descriptor_put(self, &use);
            }
        } else if (me < USERS + 2) {
            file_descriptor_slot_t gone;
            int c = me - USERS;
            use_close_won[c] = file_descriptor_detach(use_table, SLOT, &gone) == 0;
            if (use_close_won[c]) {
                file_descriptor_release(&gone);
            }
        } else {
            file_descriptor_slot_t displaced;
            use_dup2_got = descriptor_table_duplicate(use_table, DUP2_SOURCE, SLOT, 0, &displaced);
            file_descriptor_release(&displaced);
        }
        barrier_wait(&use_end, USE_THREADS + 1);
    }
    return 0;
}

TEST(descriptor_use_threads, a_call_using_a_descriptor_keeps_its_object_while_siblings_close_it) {
    clean();
    fake_spinlock_threaded(1);
    use_table = descriptor_table_new();
    REQUIRE(use_table != NULL);
    for (int i = 0; i < USE_THREADS; i++) {
        use_tasks[i] = thread_of(use_table);
    }
    use_over = 0;
    use_start.arrived = use_end.arrived = 0;
    /* The dup2's source: a socket, whose count the fakes keep separately,
       so the pipe's count says only what happened to the raced object. */
    {
        file_descriptor_slot_t source;
        memset(&source, 0, sizeof(source));
        source.type = FILE_DESCRIPTOR_SOCKET;
        source.sock = (struct socket *)0x3000;
        REQUIRE(file_descriptor_claim(use_table, DUP2_SOURCE) == DUP2_SOURCE);
        file_descriptor_install(use_table, DUP2_SOURCE, &source);
    }

    pthread_t workers[USE_THREADS];
    for (long i = 0; i < USE_THREADS; i++) {
        REQUIRE(pthread_create(&workers[i], 0, use_worker, (void *)i) == 0);
    }
    int first_wrong = -1;
    int uses_seen = 0;
    char why[200] = "";
    for (int round = 0; round < USE_ROUNDS && first_wrong < 0; round++) {
        /* A write end at SLOT that nothing else counts, so the object is
           gone the moment the count is one below where it starts. */
        file_descriptor_slot_t gone;
        if (file_descriptor_detach(use_table, SLOT, &gone) == 0) {
            file_descriptor_release(&gone); /* last round's dup2 left a socket */
        }
        open_write_end(use_table, SLOT);
        int start = fake_objects_pipe_write_refs();
        fake_objects_pipe_write_floor(start - 1);
        int dead_before = fake_objects_pipe_write_dead_uses();
        for (int i = 0; i < USERS; i++) {
            use_got[i] = 0;
        }
        use_close_won[0] = use_close_won[1] = 0;
        use_dup2_got = -9;
        barrier_wait(&use_start, USE_THREADS + 1);
        barrier_wait(&use_end, USE_THREADS + 1);
        fake_objects_pipe_write_floor(-2147483647 - 1);

        for (int i = 0; i < USERS; i++) {
            uses_seen += use_got[i];
        }
        int dead = fake_objects_pipe_write_dead_uses() - dead_before;
        int left_on_lists = 0;
        for (int i = 0; i < USE_THREADS; i++) {
            left_on_lists += use_tasks[i]->descriptor_uses != NULL;
        }
        if (dead != 0) {
            snprintf(why, sizeof(why),
                     "%d use(s) of the object after its last reference was let go - a "
                     "read or write on freed memory, on the machine",
                     dead);
        } else if (fake_objects_pipe_write_refs() != start - 1) {
            snprintf(why, sizeof(why),
                     "the object's count is %d after the round, not %d: a use's reference "
                     "was %s",
                     fake_objects_pipe_write_refs(), start - 1,
                     fake_objects_pipe_write_refs() > start - 1 ? "never given back"
                                                                : "given back twice");
        } else if (left_on_lists != 0) {
            snprintf(why, sizeof(why), "%d task(s) still list a use after every call put it",
                     left_on_lists);
        }
        if (why[0]) {
            first_wrong = round;
        }
    }
    __atomic_store_n(&use_over, 1, __ATOMIC_RELEASE);
    barrier_wait(&use_start, USE_THREADS + 1);
    for (int i = 0; i < USE_THREADS; i++) {
        pthread_join(workers[i], 0);
    }
    fake_spinlock_threaded(0);
    CHECK_MSG(first_wrong < 0, "round %d of %d: %s", first_wrong, USE_ROUNDS, why);
    /* Not vacuous: the users did get the descriptor, round after round. */
    CHECK(first_wrong >= 0 || uses_seen > USE_ROUNDS / 4);
    close_everything(use_table);
    for (int i = 0; i < USE_THREADS; i++) {
        thread_gone(use_tasks[i]);
    }
    descriptor_table_release(use_table);
}

/* ---- the slot's own order: a reader that re-reads the type ---- */

/* slot_publish writes the object and then the type, so a reader that sees a
   live type sees its object. slot_clear did it the other way round from the
   rule - object first, type last - so for a moment a slot said "pipe" and
   named nothing. The reader here is the check every lock-free reader of a
   slot would have to make: type, object, type again; if the type did not
   change, the object read between belongs to it. */
#define ORDER_ROUNDS 400000

static file_descriptor_table_t *order_table;
static int order_over;
/* Bumped between one round's close and the next round's open, so a reader
   whose three loads straddle a round - a type from one round and the same
   type again from a later one - knows to throw them away. */
static int order_round;
static long order_torn;
static long order_live_reads;

static void *order_reader(void *arg) {
    (void)arg;
    file_descriptor_slot_t *slot = &order_table->slots[SLOT];
    while (!__atomic_load_n(&order_over, __ATOMIC_ACQUIRE)) {
        int round = __atomic_load_n(&order_round, __ATOMIC_ACQUIRE);
        file_descriptor_type_t before = __atomic_load_n(&slot->type, __ATOMIC_ACQUIRE);
        struct pipe *object = __atomic_load_n(&slot->pipe, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        file_descriptor_type_t after = __atomic_load_n(&slot->type, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&order_round, __ATOMIC_ACQUIRE) != round) {
            continue;
        }
        if (before == FILE_DESCRIPTOR_PIPE_WRITE && after == before) {
            order_live_reads++;
            if (!object) {
                order_torn++;
            }
        }
    }
    return 0;
}

TEST(descriptor_use_threads, a_slot_being_cleared_never_names_nothing_under_a_live_type) {
    clean();
    fake_spinlock_threaded(1);
    order_table = descriptor_table_new();
    REQUIRE(order_table != NULL);
    order_over = 0;
    order_torn = 0;
    order_live_reads = 0;
    pthread_t reader;
    REQUIRE(pthread_create(&reader, 0, order_reader, 0) == 0);
    for (int round = 0; round < ORDER_ROUNDS; round++) {
        __atomic_add_fetch(&order_round, 1, __ATOMIC_ACQ_REL);
        open_write_end(order_table, SLOT);
        file_descriptor_slot_t gone;
        REQUIRE(file_descriptor_detach(order_table, SLOT, &gone) == 0);
    }
    __atomic_store_n(&order_over, 1, __ATOMIC_RELEASE);
    pthread_join(reader, 0);
    fake_spinlock_threaded(0);
    CHECK_MSG(order_torn == 0,
              "%ld of %ld reads saw a pipe write end, its object null, and the same type "
              "again - a slot cleared object-first",
              order_torn, order_live_reads);
    descriptor_table_release(order_table);
}

/* ---- the two paths, and an exit in the middle of a call ---- */

TEST(descriptor_use, a_table_only_its_task_names_is_used_without_counting) {
    clean();
    file_descriptor_table_t *table = descriptor_table_new();
    REQUIRE(table != NULL);
    task_t *self = task_with(table, 0);
    open_write_end(table, 3);

    file_descriptor_use_t use;
    CHECK_EQ(file_descriptor_get(self, 3, &use), 0);
    CHECK_EQ(use.slot.type, FILE_DESCRIPTOR_PIPE_WRITE);
    CHECK_EQ(use.counted, 0);
    CHECK_EQ(fake_objects_pipe_write_refs(), 0);
    CHECK(self->descriptor_uses == NULL);
    file_descriptor_put(self, &use);
    CHECK_EQ(fake_objects_pipe_write_refs(), 0);

    /* Nothing open, a reserved slot, out of range: not open, and nothing
       held to give back. */
    CHECK_EQ(file_descriptor_get(self, 4, &use), DESCRIPTOR_TABLE_BAD);
    REQUIRE(file_descriptor_claim(table, 4) == 4);
    CHECK_EQ(file_descriptor_get(self, 4, &use), DESCRIPTOR_TABLE_BAD);
    CHECK_EQ(file_descriptor_get(self, -1, &use), DESCRIPTOR_TABLE_BAD);
    CHECK_EQ(file_descriptor_get(self, MAX_FILE_DESCRIPTORS, &use), DESCRIPTOR_TABLE_BAD);
    CHECK_EQ(use.counted, 0);
    file_descriptor_unclaim(table, 4);

    /* A second thread: from now on a use is counted, listed, given back. */
    task_t *sibling = task_with(table, 1);
    file_descriptor_use_t outer, inner;
    CHECK_EQ(file_descriptor_get(self, 3, &outer), 0);
    CHECK_EQ(file_descriptor_get(self, 3, &inner), 0);
    CHECK_EQ(outer.counted, 1);
    CHECK_EQ(fake_objects_pipe_write_refs(), 2);
    CHECK(self->descriptor_uses == &inner);
    CHECK(inner.next == &outer);
    /* The sibling closes it: the calls still hold it. */
    file_descriptor_slot_t gone;
    CHECK_EQ(file_descriptor_detach(table, 3, &gone), 0);
    file_descriptor_release(&gone);
    CHECK_EQ(fake_objects_pipe_write_refs(), 1);
    CHECK_EQ(file_descriptor_get(sibling, 3, &use), DESCRIPTOR_TABLE_BAD);
    /* Given back out of order too. */
    file_descriptor_put(self, &outer);
    CHECK(self->descriptor_uses == &inner);
    file_descriptor_put(self, &inner);
    CHECK(self->descriptor_uses == NULL);
    CHECK_EQ(fake_objects_pipe_write_refs(), -1); /* the object's last let-go */

    /* The console names no object: nothing to count or list. */
    file_descriptor_slot_t console;
    memset(&console, 0, sizeof(console));
    console.type = FILE_DESCRIPTOR_STDOUT;
    REQUIRE(file_descriptor_claim(table, 1) == 1);
    file_descriptor_install(table, 1, &console);
    CHECK_EQ(file_descriptor_get(sibling, 1, &use), 0);
    CHECK_EQ(use.counted, 0);
    CHECK(sibling->descriptor_uses == NULL);
    file_descriptor_put(sibling, &use);

    descriptor_table_release(table);
    descriptor_table_release(table);
}

TEST(descriptor_use, an_exit_in_the_middle_of_a_call_gives_back_what_the_call_held) {
    clean();
    file_descriptor_table_t *table = descriptor_table_new();
    REQUIRE(table != NULL);
    task_t *self = task_with(table, 1); /* and a sibling thread's reference */
    open_write_end(table, 3);
    open_write_end(table, 7);

    file_descriptor_use_t a, b;
    CHECK_EQ(file_descriptor_get(self, 3, &a), 0);
    CHECK_EQ(file_descriptor_get(self, 7, &b), 0);
    CHECK_EQ(fake_objects_pipe_write_refs(), 2);
    file_descriptor_put_all(self);
    CHECK_EQ(fake_objects_pipe_write_refs(), 0);
    CHECK(self->descriptor_uses == NULL);
    CHECK_EQ(a.counted, 0);
    /* And nothing is given back twice by a put that comes after anyway. */
    file_descriptor_put(self, &a);
    CHECK_EQ(fake_objects_pipe_write_refs(), 0);

    close_everything(table);
    descriptor_table_release(table);
    descriptor_table_release(table);
}

/* A named pipe (pipe_named: every window's event pipe) is never freed and
   never counted down - its pipe_unref_* only wakes everybody watching it.
   So a use of one takes nothing: counting it made every poll and epoll_wait
   of a threaded process wake ITSELF with the scan's put, and an idle
   Chromium UI thread watching its window's events spun a core. Here the
   fake's counts stand in for the machine's: a persistent end used through a
   shared table must not be retained, released or listed; an ordinary end
   beside it still is. */
TEST(descriptor_use, a_named_pipe_is_used_without_counting_even_through_a_shared_table) {
    clean();
    file_descriptor_table_t *table = descriptor_table_new();
    REQUIRE(table != NULL);
    task_t *self = task_with(table, 1); /* a sibling thread's reference too */
    struct pipe *named = (struct pipe *)0x3000;
    fake_objects_pipe_persistent(named);

    file_descriptor_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.type = FILE_DESCRIPTOR_PIPE_READ;
    slot.pipe = named;
    REQUIRE(file_descriptor_claim(table, 4) == 4);
    file_descriptor_install(table, 4, &slot);
    slot.type = FILE_DESCRIPTOR_PIPE_WRITE;
    REQUIRE(file_descriptor_claim(table, 5) == 5);
    file_descriptor_install(table, 5, &slot);
    open_write_end(table, 6); /* an ordinary pipe */

    file_descriptor_use_t read_end, write_end, ordinary;
    CHECK_EQ(file_descriptor_get(self, 4, &read_end), 0);
    CHECK_EQ(file_descriptor_get(self, 5, &write_end), 0);
    CHECK_EQ(read_end.slot.type, FILE_DESCRIPTOR_PIPE_READ);
    CHECK(read_end.slot.pipe == named);
    CHECK_EQ(read_end.counted, 0);
    CHECK_EQ(write_end.counted, 0);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    CHECK_EQ(fake_objects_pipe_write_refs(), 0);
    CHECK(self->descriptor_uses == NULL);
    file_descriptor_put(self, &read_end);
    file_descriptor_put(self, &write_end);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    CHECK_EQ(fake_objects_pipe_write_refs(), 0);

    CHECK_EQ(file_descriptor_get(self, 6, &ordinary), 0);
    CHECK_EQ(ordinary.counted, 1);
    CHECK_EQ(fake_objects_pipe_write_refs(), 1);
    CHECK(self->descriptor_uses == &ordinary);
    file_descriptor_put(self, &ordinary);
    CHECK_EQ(fake_objects_pipe_write_refs(), 0);
    CHECK(self->descriptor_uses == NULL);

    close_everything(table);
    descriptor_table_release(table);
    descriptor_table_release(table);
}

/* Every get is put before its system call returns; the return path says so
   (file_descriptor_uses_settled) rather than leave a record from a returned
   stack frame on the list for the next get and an exit's put_all. */
TEST(descriptor_use, a_call_that_returns_still_holding_a_use_is_caught) {
    clean();
    file_descriptor_table_t *table = descriptor_table_new();
    REQUIRE(table != NULL);
    task_t *self = task_with(table, 1);
    open_write_end(table, 3);

    file_descriptor_uses_settled(self); /* nothing got: nothing said */
    file_descriptor_use_t use;
    CHECK_EQ(file_descriptor_get(self, 3, &use), 0);
    CHECK_PANIC(file_descriptor_uses_settled(self), "without putting");
    file_descriptor_put(self, &use);
    file_descriptor_uses_settled(self);
    CHECK(self->descriptor_uses == NULL);

    close_everything(table);
    descriptor_table_release(table);
    descriptor_table_release(table);
}
