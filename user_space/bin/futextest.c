/* user_space/bin/futextest.c - M96's fixture.
 *
 * The milestone in one sentence: a thread that is waiting should cost
 * nothing.
 *
 * ---- why the measurement is CPU time and not throughput --------------
 *
 * M96's own "how we'll know" is explicit about this, and it is the whole
 * design of this program: **the measurement that matters is the CPU time
 * a blocked waiter consumes, which must be indistinguishable from zero.**
 * A throughput number would pass with the spin loop still in place - the
 * spin-then-yield mutex this replaces handed the lock over perfectly
 * well, it just burned a time slice per waiter per handoff while doing
 * it. Only the disappearance of that cost is evidence that the wait
 * became a wait.
 *
 * So: N threads contend for one mutex while a *watcher* thread does
 * nothing but sleep, and what is reported is how much CPU the process
 * used. SYS_rusage counts ticks charged to this process by the timer
 * interrupt, which is exactly the thing a spinning waiter inflates and a
 * blocked one does not.
 *
 * Two runs, and the comparison is the test: one where the threads hold
 * the lock briefly (little blocking, so most acquires take the fast path
 * and never enter the kernel) and one where they hold it long enough
 * that everybody else genuinely sleeps. With a futex the second run's
 * CPU time is close to the first's plus the work; with a spin loop it is
 * the *wall clock* times the thread count, because every waiter is
 * running the whole time.
 *
 * ---- and the second half: __thread ------------------------------------
 *
 * A counter per thread, incremented only by its owner, checked at the
 * end. A shared variable would come out as the sum; sixteen separate
 * ones come out as sixteen equal counts. That difference is the entire
 * observable meaning of thread-local storage, and it is why the check is
 * "each thread saw exactly its own count" rather than "the total is
 * right".
 *
 * Exit codes so a failure names itself:
 *   0  everything worked
 *   2  a thread would not start
 *   3  the mutex did not exclude - the shared counter is wrong
 *   4  a __thread counter holds somebody else's count
 *   5  a blocked waiter burned CPU - the wait is still a spin
 *   6  a condition variable did not wake its waiter
 *   7  a barrier did not release everybody exactly once
 *   8  a reader-writer lock let a writer in beside a reader
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "syscall_wrappers.h"

#define THREADS 16
#define ROUNDS  200

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long shared;
static long hold_us;

/* The per-thread counter. Only ever incremented by the thread that owns
 * it, so its final value is a fact about one thread. */
static __thread long mine;
static long reported[THREADS];
static int report_slot;
static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;

/* Burns a known amount of time INSIDE the lock, so the other threads
 * have something real to wait for. A sleep would release the CPU and
 * make every waiter's wait short; this holds the lock while running,
 * which is the case a futex is for. */
static void spin_for(long iterations) {
    volatile long x = 0;
    for (long i = 0; i < iterations; i++) {
        x += i;
    }
    (void)x;
}

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&lock);
        shared++;
        mine++;
        spin_for(hold_us);
        pthread_mutex_unlock(&lock);
    }
    pthread_mutex_lock(&report_lock);
    if (report_slot < THREADS) {
        reported[report_slot++] = mine;
    }
    pthread_mutex_unlock(&report_lock);
    return 0;
}

/* CPU ticks this process has used, from SYS_rusage. Ticks and not
 * microseconds, because a tick is what this machine observes - see that
 * call's own ABI note. */
static long cpu_ticks(void) {
    os_rusage_t ru;
    memset(&ru, 0, sizeof(ru));
    if (sys_rusage(OS_RUSAGE_SELF, &ru) != 0) {
        return -1;
    }
    return (long)(ru.user_ticks + ru.sys_ticks);
}

static int run_round(long hold, long *out_cpu, long *out_wall) {
    pthread_t t[THREADS];
    shared = 0;
    report_slot = 0;
    hold_us = hold;

    long cpu0 = cpu_ticks();
    long wall0 = sys_uptime_ms();
    for (int i = 0; i < THREADS; i++) {
        if (pthread_create(&t[i], 0, worker, 0) != 0) {
            return 2;
        }
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(t[i], 0);
    }
    *out_cpu = cpu_ticks() - cpu0;
    *out_wall = sys_uptime_ms() - wall0;

    if (shared != (long)THREADS * ROUNDS) {
        printf("futextest: the mutex did not exclude - %ld increments, expected %d\n",
               shared, THREADS * ROUNDS);
        return 3;
    }
    for (int i = 0; i < THREADS; i++) {
        if (reported[i] != ROUNDS) {
            printf("futextest: a __thread counter holds %ld, not %d - the threads "
                   "are sharing one variable\n", reported[i], ROUNDS);
            return 4;
        }
    }
    return 0;
}

/* ---- the other three primitives, each checked for the one thing it is
 * for rather than for running at all ---------------------------------- */

static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t cv_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int predicate;
static volatile int woke;

static void *cond_waiter(void *arg) {
    (void)arg;
    pthread_mutex_lock(&cv_lock);
    while (!predicate) {
        pthread_cond_wait(&cv, &cv_lock);
    }
    woke++;
    pthread_mutex_unlock(&cv_lock);
    return 0;
}

static pthread_barrier_t barrier;
static volatile int through;

static void *barrier_worker(void *arg) {
    (void)arg;
    for (int round = 0; round < 4; round++) {
        pthread_barrier_wait(&barrier);
    }
    pthread_mutex_lock(&report_lock);
    through++;
    pthread_mutex_unlock(&report_lock);
    return 0;
}

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static volatile int readers_in;
static volatile int writer_saw_reader;

static void *rw_reader(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        pthread_rwlock_rdlock(&rw);
        readers_in++;
        spin_for(200);
        readers_in--;
        pthread_rwlock_unlock(&rw);
    }
    return 0;
}

static void *rw_writer(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        pthread_rwlock_wrlock(&rw);
        /* The one thing a reader-writer lock is for: while this holds
         * the write lock, no reader may be inside. */
        if (readers_in != 0) {
            writer_saw_reader = 1;
        }
        spin_for(200);
        pthread_rwlock_unlock(&rw);
    }
    return 0;
}

int main(void) {
    long fast_cpu = 0, fast_wall = 0, slow_cpu = 0, slow_wall = 0;

    int rc = run_round(50, &fast_cpu, &fast_wall);
    if (rc) {
        return rc;
    }
    rc = run_round(20000, &slow_cpu, &slow_wall);
    if (rc) {
        return rc;
    }

    printf("futextest: brief hold  cpu=%ld ticks wall=%ld ms\n", fast_cpu, fast_wall);
    printf("futextest: long hold   cpu=%ld ticks wall=%ld ms\n", slow_cpu, slow_wall);

    /* ---- the measurement -------------------------------------------
     *
     * With a futex, the CPU a run costs is the work it did: fifteen
     * threads asleep contribute nothing, so `cpu` tracks the critical
     * sections and not the wall clock. With a spin-then-yield mutex,
     * every waiter is runnable for the whole run, so `cpu` approaches
     * `wall * THREADS` - the machine has fewer cores than that, so in
     * practice it approaches `wall * cores`.
     *
     * The threshold is deliberately generous: this asserts that the CPU
     * used is less than the wall clock plus a margin, which a spin loop
     * on a multi-core machine cannot satisfy and a futex satisfies with
     * room to spare. A tighter bound would be a claim about the
     * scheduler rather than about the wait.
     *
     * Ticks are centiseconds (PIT_HZ 100), so wall milliseconds are
     * divided by ten to compare.
     */
    long wall_ticks = slow_wall / 10;
    if (slow_cpu > wall_ticks + wall_ticks / 2 + 20) {
        printf("futextest: a blocked waiter is burning CPU - %ld ticks used "
               "against %ld ticks of wall clock. The wait is still a spin.\n",
               slow_cpu, wall_ticks);
        return 5;
    }
    printf("futextest: a waiting thread costs nothing - %ld cpu ticks against "
           "%ld wall ticks across %d threads\n", slow_cpu, wall_ticks, THREADS);

    /* ---- condition variable ---------------------------------------- */
    pthread_t cw[4];
    predicate = 0;
    woke = 0;
    for (int i = 0; i < 4; i++) {
        if (pthread_create(&cw[i], 0, cond_waiter, 0) != 0) {
            return 2;
        }
    }
    /* Long enough that every waiter is genuinely parked before the
     * broadcast - a signal that arrives before anybody waits is legal
     * and is not what this is testing. */
    usleep(200000);
    pthread_mutex_lock(&cv_lock);
    predicate = 1;
    pthread_mutex_unlock(&cv_lock);
    pthread_cond_broadcast(&cv);
    for (int i = 0; i < 4; i++) {
        pthread_join(cw[i], 0);
    }
    if (woke != 4) {
        printf("futextest: %d of 4 waiters woke from a broadcast\n", woke);
        return 6;
    }
    printf("futextest: a broadcast woke every waiter\n");

    /* ---- barrier ---------------------------------------------------- */
    pthread_t bw[8];
    through = 0;
    pthread_barrier_init(&barrier, 0, 8);
    for (int i = 0; i < 8; i++) {
        if (pthread_create(&bw[i], 0, barrier_worker, 0) != 0) {
            return 2;
        }
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(bw[i], 0);
    }
    if (through != 8) {
        printf("futextest: %d of 8 threads came through four barrier rounds\n", through);
        return 7;
    }
    printf("futextest: eight threads through four barrier rounds\n");

    /* ---- reader-writer lock ----------------------------------------- */
    pthread_t rr[4];
    pthread_t rwr;
    readers_in = 0;
    writer_saw_reader = 0;
    for (int i = 0; i < 4; i++) {
        if (pthread_create(&rr[i], 0, rw_reader, 0) != 0) {
            return 2;
        }
    }
    if (pthread_create(&rwr, 0, rw_writer, 0) != 0) {
        return 2;
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(rr[i], 0);
    }
    pthread_join(rwr, 0);
    if (writer_saw_reader) {
        printf("futextest: a writer held the lock while a reader was inside\n");
        return 8;
    }
    printf("futextest: a writer never overlapped a reader\n");

    printf("futextest: all checks passed\n");
    return 0;
}
