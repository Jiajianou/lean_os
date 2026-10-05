/* M225: the descriptor table's reference count under REAL threads.

   Every other host test is one thread, and the fakes are built for that (a
   contended fake spinlock is a panic). This one is not, because the bug it
   grades cannot exist on one thread: a table is shared by every thread of a
   process (M146), a thread takes a reference when it is made and drops one
   when it exits, and the exit side takes no lock. Two processors dropping or
   taking at once on a plain `references++` / `--references` lose one of the
   two writes - and an 8-processor hvf battery panicked on exactly that,
   "[heap] second kfree of ... from descriptor_table_release".

   It runs only descriptor_table_reference/descriptor_table_release on the
   worker threads. Those take no lock, and the one heap call - the teardown's
   kfree - happens once, on one thread, while the others are only waiting:
   that is the property under test, and if it fails the heap's own double-free
   check says so.

   How it stays deterministic: on the fixed code every assertion below holds
   for EVERY interleaving - an atomic count has no schedule that loses an
   update - so the test cannot flake green-to-red. What is probabilistic is
   only how fast the OLD code fails, and that is measured in the milestone's
   commit message rather than relied on here. The work is fixed (no clocks,
   no "run until"): eight threads, a few hundred thousand operations each,
   well under a second with the sanitizers on, on either kind of Mac. Eight
   rather than the host's core count, so both Macs run the same test; on a
   host with fewer cores the threads still interleave through preemption,
   just less often. */
#include "check.h"
#include "fakes/fakes.h"

#include "memory_management/heap.h"
#include "scheduler/scheduler.h"

#include <pthread.h>
#include <sched.h>

#define TABLE_THREADS 8
#define HAMMER_PAIRS 100000
#define LETGO_ROUNDS 2000
/* What the main thread holds while the workers hammer, so that a count
   that loses updates is wrong at the end rather than reaching zero on a
   worker in the middle - a teardown there would free the table under the
   others and turn a clean failed check into a heap panic. */
#define CUSHION (1 << 24)

/* A sense-reversing spin barrier. macOS has no pthread_barrier_t, and this
   one yields while it waits so a host with fewer cores than threads still
   makes progress. */
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

static spin_barrier_t start_line;
static file_descriptor_table_t *shared_table;

static void *hammer(void *unused) {
    (void)unused;
    barrier_wait(&start_line, TABLE_THREADS);
    for (int i = 0; i < HAMMER_PAIRS; i++) {
        descriptor_table_reference(shared_table);
        descriptor_table_release(shared_table);
    }
    return 0;
}

static void clean(void) {
    fake_physical_memory_reset();
    fake_virtual_memory_reset();
    heap_init();
    fake_objects_reset();
}

/* A slot the teardown closes, so a teardown can be counted: the fakes keep
   a pipe-read reference count that file_descriptor_release decrements. */
static file_descriptor_table_t *table_with_a_pipe(void) {
    file_descriptor_table_t *table = descriptor_table_new();
    if (table) {
        table->slots[3].type = FILE_DESCRIPTOR_PIPE_READ;
        table->slots[3].pipe = (struct pipe *)0x1000;
    }
    return table;
}

TEST(descriptor_table_threads, no_reference_is_lost_when_threads_take_and_drop_at_once) {
    clean();
    shared_table = table_with_a_pipe();
    REQUIRE(shared_table != NULL);
    shared_table->references += CUSHION;
    start_line.arrived = 0;

    pthread_t workers[TABLE_THREADS];
    for (int i = 0; i < TABLE_THREADS; i++) {
        REQUIRE(pthread_create(&workers[i], 0, hammer, 0) == 0);
    }
    for (int i = 0; i < TABLE_THREADS; i++) {
        pthread_join(workers[i], 0);
    }

    /* Eight hundred thousand takes and as many drops: the count is where it
       started, exactly, and nothing was closed on the way. */
    CHECK_EQ(__atomic_load_n(&shared_table->references, __ATOMIC_ACQUIRE), 1 + CUSHION);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    if (__atomic_load_n(&shared_table->references, __ATOMIC_ACQUIRE) != 1 + CUSHION) {
        return; /* a table this wrong is leaked rather than freed twice */
    }
    shared_table->references -= CUSHION;
    descriptor_table_release(shared_table);
    CHECK_EQ(fake_objects_pipe_read_refs(), -1);
}

static spin_barrier_t round_start, round_end;
static file_descriptor_table_t *round_table;
static int rounds_over;

static void *let_go(void *unused) {
    (void)unused;
    /* Until the main thread says the rounds are over, which it does through
       the same start line - so it never waits there for a thread that has
       already gone. */
    for (;;) {
        barrier_wait(&round_start, TABLE_THREADS + 1);
        if (__atomic_load_n(&rounds_over, __ATOMIC_ACQUIRE)) {
            break;
        }
        descriptor_table_release(round_table);
        barrier_wait(&round_end, TABLE_THREADS + 1);
    }
    return 0;
}

/* Every thread of a process exiting at the same moment - exit()'s SIGKILL
   to the group makes that the ordinary case - each dropping the one
   reference it holds. Exactly one of them closes the descriptors, every
   round: none is the leak a lost decrement makes, and two is the double
   free. */
TEST(descriptor_table_threads, the_last_of_many_threads_letting_go_at_once_closes_it_once) {
    clean();
    round_start.arrived = round_end.arrived = 0;
    rounds_over = 0;

    pthread_t workers[TABLE_THREADS];
    for (int i = 0; i < TABLE_THREADS; i++) {
        REQUIRE(pthread_create(&workers[i], 0, let_go, 0) == 0);
    }
    int rounds_wrong = 0;
    int first_wrong = -1;
    int closes_then = 0;
    for (int round = 0; round < LETGO_ROUNDS; round++) {
        round_table = table_with_a_pipe();
        if (!round_table) {
            rounds_wrong++;
            break;
        }
        round_table->references = TABLE_THREADS;
        int before = fake_objects_pipe_read_refs();
        barrier_wait(&round_start, TABLE_THREADS + 1);
        barrier_wait(&round_end, TABLE_THREADS + 1);
        int closes = before - fake_objects_pipe_read_refs();
        if (closes != 1) {
            if (first_wrong < 0) {
                first_wrong = round;
                closes_then = closes;
            }
            rounds_wrong++;
            /* A table nobody closed is leaked, and two thousand of them
               would run the fake heap out before the check below could
               say why. One wrong round is the whole verdict. */
            break;
        }
    }
    __atomic_store_n(&rounds_over, 1, __ATOMIC_RELEASE);
    barrier_wait(&round_start, TABLE_THREADS + 1);
    for (int i = 0; i < TABLE_THREADS; i++) {
        pthread_join(workers[i], 0);
    }
    CHECK_MSG(rounds_wrong == 0,
              "round %d of %d: %d threads letting go of one table at once closed "
              "it %d time(s), not once",
              first_wrong, LETGO_ROUNDS, TABLE_THREADS, closes_then);
}
