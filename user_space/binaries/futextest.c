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

static __thread long mine;
static long reported[THREADS];
static int report_slot;
static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;

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

/* M225: the count is atomic because readers share the lock - that is what a
   read lock is for. readers_in++ from four readers inside it at once on four
   processors lost updates, the count drifted below zero, and the writer saw
   a reader that was not there: "[m96] the fixture exited 0x00000008" at
   QEMU_CPUS=4, on kernels with and without the scheduler fix alike. One
   processor almost never preempted a reader between the load and the store,
   which is why it passed for a hundred and thirty milestones. */
static void *rw_reader(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        pthread_rwlock_rdlock(&rw);
        __atomic_add_fetch(&readers_in, 1, __ATOMIC_SEQ_CST);
        spin_for(200);
        __atomic_sub_fetch(&readers_in, 1, __ATOMIC_SEQ_CST);
        pthread_rwlock_unlock(&rw);
    }
    return 0;
}

static void *rw_writer(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        pthread_rwlock_wrlock(&rw);
        if (__atomic_load_n(&readers_in, __ATOMIC_SEQ_CST) != 0) {
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

    long wall_ticks = slow_wall / 10;
    if (slow_cpu > wall_ticks + wall_ticks / 2 + 20) {
        printf("futextest: a blocked waiter is burning CPU - %ld ticks used "
               "against %ld ticks of wall clock. The wait is still a spin.\n",
               slow_cpu, wall_ticks);
        return 5;
    }
    printf("futextest: a waiting thread costs nothing - %ld cpu ticks against "
           "%ld wall ticks across %d threads\n", slow_cpu, wall_ticks, THREADS);

    pthread_t cw[4];
    predicate = 0;
    woke = 0;
    for (int i = 0; i < 4; i++) {
        if (pthread_create(&cw[i], 0, cond_waiter, 0) != 0) {
            return 2;
        }
    }
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
