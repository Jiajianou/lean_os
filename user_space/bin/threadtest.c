/* user_space/bin/threadtest.c - M79's fixture
 *
 * The milestone's own statement of proof: "Two threads in one process
 * each increment a shared counter a million times, one thread's
 * increments protected by the M79 mutex; the total is exactly two
 * million. The classic test, chosen because a scheduler that ever runs
 * both halves of an unlocked increment at once fails it visibly instead
 * of 'mostly'."
 *
 * So that is the centrepiece, and everything else here exists to stop it
 * passing for the wrong reason:
 *
 *  - the counter is checked to be genuinely SHARED, by having one thread
 *    write a value the other reads back. Two processes would fail this;
 *    a "thread" that quietly got its own copy of memory would pass the
 *    increment test with a total of exactly one million per copy and
 *    fail here.
 *  - the two threads are checked to have DIFFERENT tids and the same
 *    pid, which is the difference between a thread and a spawn.
 *  - each thread does floating-point work whose result depends on it not
 *    being disturbed. M63 made a task carry its own FXSAVE state; two
 *    tasks that are the same *process* by every other measure are
 *    exactly the case where that could quietly have stopped being true.
 *  - an UNPROTECTED counter is incremented alongside the protected one,
 *    and reported. It is not asserted on: a lost update is a race and a
 *    race is allowed not to happen. But if it never differs across every
 *    boot this test ever runs, the mutex is not being tested by
 *    contention, and that is worth being able to see.
 *
 * Exit codes, so a failure names itself:
 *   0  everything worked
 *   2  a thread could not be created
 *   3  a join failed, or a return value did not come back
 *   4  the protected total was not exactly 2 * ITERATIONS - the mutex
 *      does not exclude
 *   5  memory is not shared between the threads
 *   6  the two threads report the same tid, or different pids
 *   7  floating-point state did not survive being interleaved
 */
#include <errno.h>  /* M100: EPERM, from a thread that unlocks what it does not hold */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "syscall_wrappers.h" /* sys_gettid - there is no getpid()/gettid() split in <unistd.h> here */

/* A million each. Large enough that two threads genuinely overlap inside
 * the critical section on this machine - the point of the test is
 * contention, and a count small enough to finish inside one time slice
 * would never produce any. */
#define ITERATIONS 1000000

static long protected_counter;
static long racy_counter;
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;

/* Written by thread A, read by thread B. The proof that this is one
 * address space and not two. */
static volatile long shared_word;
static volatile int a_wrote;

static int tid_a, tid_b;
static int pid_a, pid_b;
static double fp_a, fp_b;

/* Deliberately not `volatile`, and deliberately summed rather than
 * multiplied: what is being checked is that the x87/SSE register file
 * this loop is using is restored across a task switch. A result that
 * depends on hundreds of intermediate values is one that a single
 * clobbered register changes visibly. */
static double fp_work(double seed) {
    double acc = seed;
    for (int i = 1; i <= 20000; i++) {
        acc = acc + (double)i / (double)(i + 1);
        acc = acc * 1.0000001;
    }
    return acc;
}

static void *worker_a(void *arg) {
    (void)arg;
    tid_a = (int)sys_gettid();
    pid_a = (int)sys_getpid();
    shared_word = 0x5EEDF00D;
    a_wrote = 1;
    for (long i = 0; i < ITERATIONS; i++) {
        pthread_mutex_lock(&counter_lock);
        protected_counter++;
        pthread_mutex_unlock(&counter_lock);
        racy_counter++;
    }
    fp_a = fp_work(1.0);
    return (void *)0xA;
}

static void *worker_b(void *arg) {
    (void)arg;
    tid_b = (int)sys_gettid();
    pid_b = (int)sys_getpid();
    for (long i = 0; i < ITERATIONS; i++) {
        pthread_mutex_lock(&counter_lock);
        protected_counter++;
        pthread_mutex_unlock(&counter_lock);
        racy_counter++;
    }
    fp_b = fp_work(1.0);
    return (void *)0xB;
}

/* ---- M100: the recursive mutex, from two threads ------------------------
 *
 * Three holds per iteration, and inside the innermost a read-modify-
 * write of a plain counter. The mutex is statically initialised as
 * recursive rather than through an attribute, so both spellings are
 * exercised between here and libctest. */
#define RECURSIVE_ITERATIONS 200000
static long recursive_counter;
static pthread_mutex_t recursive_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static void *recursive_worker(void *arg) {
    (void)arg;
    for (long i = 0; i < RECURSIVE_ITERATIONS; i++) {
        if (pthread_mutex_lock(&recursive_lock) != 0 ||
            pthread_mutex_lock(&recursive_lock) != 0 ||
            pthread_mutex_lock(&recursive_lock) != 0) {
            return (void *)1;
        }
        long v = recursive_counter;
        /* A window the other thread must not enter: the value read above
         * has to be the one written below. */
        recursive_counter = v + 1;
        if (pthread_mutex_unlock(&recursive_lock) != 0 ||
            pthread_mutex_unlock(&recursive_lock) != 0 ||
            pthread_mutex_unlock(&recursive_lock) != 0) {
            return (void *)1;
        }
    }
    return (void *)0;
}

/* Runs while main holds recursive_lock: the unlock must be refused. */
static void *foreign_unlocker(void *arg) {
    (void)arg;
    return (void *)(long)(pthread_mutex_unlock(&recursive_lock) == EPERM);
}

/* ---- M99: the stack a thread is entered on ---------------------------
 *
 * SysV requires RSP to be 16-byte aligned at the CALL, so a function's
 * first instruction runs with RSP % 16 == 8 - the return address the
 * call pushed - and GCC lays out every 16-byte-aligned local on that
 * assumption. A thread entered with RSP exactly 16-aligned therefore has
 * every aligned spill slot 8 bytes out, and the first `movaps` to one
 * is a #GP that kills the thread.
 *
 * This is that instruction, on purpose, in a thread. It is written as
 * inline assembly rather than left to the compiler because the whole
 * point is to emit the aligned form: a C expression the optimiser can
 * satisfy with `movups` would test nothing, and which of the two GCC
 * picks is not something this test should be at the mercy of.
 *
 * It cost twenty milestones to find. Nothing that had run on a thread
 * here spilled an XMM register to an aligned slot until CPython's
 * test_io did, and then it did not look like a threading bug at all -
 * it looked like the interpreter crashing in its own text. */
static volatile int sse_ok;

static void *sse_alignment_probe(void *arg) {
    (void)arg;
    double buf[2] __attribute__((aligned(16)));
    __asm__ volatile("movaps %%xmm0, %0" : "=m"(buf) : : "memory");
    /* Reached only if the store above did not fault. */
    sse_ok = 1;
    return (void *)0xC;
}

int main(void) {
    /* The same computation, alone, before either thread exists. Whatever
     * this produces is what both threads must produce while competing
     * with each other - which is the whole claim about per-task FPU
     * state. */
    double alone = fp_work(1.0);

    pthread_t a, b;
    if (pthread_create(&a, 0, worker_a, 0) != 0) {
        return 2;
    }
    if (pthread_create(&b, 0, worker_b, 0) != 0) {
        return 2;
    }

    void *ra = 0, *rb = 0;
    if (pthread_join(a, &ra) != 0 || pthread_join(b, &rb) != 0) {
        return 3;
    }
    if (ra != (void *)0xA || rb != (void *)0xB) {
        return 3;
    }

    if (protected_counter != 2 * (long)ITERATIONS) {
        printf("threadtest: protected total is %ld, wanted %ld\n",
                protected_counter, 2 * (long)ITERATIONS);
        return 4;
    }
    if (!a_wrote || shared_word != 0x5EEDF00D) {
        return 5;
    }
    if (tid_a == tid_b || tid_a == 0 || tid_b == 0) {
        return 6;
    }
    if (pid_a != pid_b || pid_a != (int)sys_getpid()) {
        return 6;
    }
    if (fp_a != alone || fp_b != alone) {
        return 7;
    }

    /* M99: and the alignment of a thread's own stack, which is a
     * different claim from anything above - those threads all ran, and
     * would have run just as well on a stack that was 8 bytes out. */
    pthread_t c;
    if (pthread_create(&c, 0, sse_alignment_probe, 0) != 0) {
        return 8;
    }
    void *rc = 0;
    if (pthread_join(c, &rc) != 0 || rc != (void *)0xC || !sse_ok) {
        printf("threadtest: a thread could not store an XMM register to a "
                "16-byte-aligned local - its stack was entered misaligned\n");
        return 9;
    }

    /* ---- M100: a recursive mutex, under contention ----------------------
     *
     * libctest checks the type contract from one thread. This is the
     * half that needs two: each worker takes the SAME recursive mutex
     * three deep per iteration and bumps a counter inside, so a mutex
     * whose recursion let the other thread in - an owner check against
     * the wrong id, a count that released the word one unlock early -
     * shows as a lost update. And the cross-thread rule: an unlock by a
     * thread that does not hold it is EPERM, not a release. */
    pthread_t d, e;
    if (pthread_create(&d, 0, recursive_worker, 0) != 0 ||
        pthread_create(&e, 0, recursive_worker, 0) != 0) {
        return 10;
    }
    void *rd = 0, *re = 0;
    if (pthread_join(d, &rd) != 0 || pthread_join(e, &re) != 0 ||
        rd != (void *)0 || re != (void *)0) {
        printf("threadtest: a recursive worker saw a hold it should not have\n");
        return 11;
    }
    if (recursive_counter != 2 * (long)RECURSIVE_ITERATIONS) {
        printf("threadtest: recursive-mutex total is %ld, wanted %ld\n",
               recursive_counter, 2 * (long)RECURSIVE_ITERATIONS);
        return 12;
    }
    if (pthread_mutex_lock(&recursive_lock) != 0) {
        return 13;
    }
    pthread_t f;
    void *rf = 0;
    if (pthread_create(&f, 0, foreign_unlocker, 0) != 0 ||
        pthread_join(f, &rf) != 0 || rf != (void *)1) {
        printf("threadtest: a thread unlocked a recursive mutex another thread holds\n");
        return 14;
    }
    if (pthread_mutex_unlock(&recursive_lock) != 0) {
        return 15;
    }

    /* Reported, not asserted - see the header comment. A race is allowed
     * not to happen, but a run in which it never does is a run in which
     * the mutex was never actually contended, and that is worth being
     * able to read in the log. */
    printf("threadtest: all checks passed (protected %ld of %ld; unprotected lost %ld)\n",
            protected_counter, 2 * (long)ITERATIONS,
            2 * (long)ITERATIONS - racy_counter);
    return 0;
}
