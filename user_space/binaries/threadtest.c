#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "syscall_wrappers.h"

#define ITERATIONS 1000000

static long protected_counter;
static long racy_counter;
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile long shared_word;
static volatile int a_wrote;

static int tid_a, tid_b;
static int pid_a, pid_b;
static double fp_a, fp_b;

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
        recursive_counter = v + 1;
        if (pthread_mutex_unlock(&recursive_lock) != 0 ||
            pthread_mutex_unlock(&recursive_lock) != 0 ||
            pthread_mutex_unlock(&recursive_lock) != 0) {
            return (void *)1;
        }
    }
    return (void *)0;
}

static void *foreign_unlocker(void *arg) {
    (void)arg;
    return (void *)(long)(pthread_mutex_unlock(&recursive_lock) == EPERM);
}

static volatile int sse_ok;

static void *sse_alignment_probe(void *arg) {
    (void)arg;
    double buf[2] __attribute__((aligned(16)));
    __asm__ volatile("movaps %%xmm0, %0" : "=m"(buf) : : "memory");
    sse_ok = 1;
    return (void *)0xC;
}

int main(void) {
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

    printf("threadtest: all checks passed (protected %ld of %ld; unprotected lost %ld)\n",
            protected_counter, 2 * (long)ITERATIONS,
            2 * (long)ITERATIONS - racy_counter);
    return 0;
}
