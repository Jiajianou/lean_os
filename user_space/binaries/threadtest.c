#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
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
    double buffer[2] __attribute__((aligned(16)));
    __asm__ volatile("movaps %%xmm0, %0" : "=m"(buffer) : : "memory");
    sse_ok = 1;
    return (void *)0xC;
}


/* M165. Two destructor kinds, one hook.

   A C++ thread_local with a non-trivial destructor makes the compiler emit
   the __cxa_thread_atexit call this makes by hand; calling it directly is
   what is being graded, because this program is C and the ABI function is
   the thing the port needed. Nine registrations, so the eighth crosses out
   of the thread's inline storage and onto the heap and BOTH paths run. */
#define THREAD_DESTRUCTORS 9

extern int __cxa_thread_atexit(void (*fn)(void *), void *arg, void *dso);

static int destructor_order[2][THREAD_DESTRUCTORS];
static int destructor_count[2];
static int destructor_thread_of[2];

static void record_destructor(void *arg) {
    long packed = (long)arg;
    int which = (int)(packed >> 8);
    int index = (int)(packed & 0xff);
    if (which < 0 || which > 1) {
        return;
    }
    int n = destructor_count[which];
    if (n < THREAD_DESTRUCTORS) {
        destructor_order[which][n] = index;
    }
    destructor_count[which] = n + 1;
    destructor_thread_of[which] = (int)sys_gettid();
}

static void *destructor_worker(void *arg) {
    long which = (long)arg;
    for (int i = 0; i < THREAD_DESTRUCTORS; i++) {
        if (__cxa_thread_atexit(record_destructor,
                                (void *)((which << 8) | (long)i), 0) != 0) {
            return (void *)1;
        }
    }
    /* Not one of them has run yet, and that is half the check: a destructor
       that ran at registration would pass every test that only counts. */
    if (destructor_count[which] != 0) {
        return (void *)2;
    }
    return (void *)0;
}

/* pthread_key_create's destructor, which this library accepted and discarded
   until M165. POSIX says the value is cleared before the destructor is
   called and the destructor is handed the OLD value, so a destructor that
   reads the key back must see nothing. */
static pthread_key_t sweep_key;
static int sweep_calls;
static void *sweep_value_seen;
static int sweep_key_was_clear;

static void sweep_destructor(void *value) {
    sweep_calls++;
    sweep_value_seen = value;
    sweep_key_was_clear = (pthread_getspecific(sweep_key) == 0);
}

static void *sweep_worker(void *arg) {
    (void)arg;
    if (pthread_setspecific(sweep_key, (void *)0xC0FFEE) != 0) {
        return (void *)1;
    }
    return (void *)0;
}

/* pthread_exit is the other door out of a thread, and a destructor list that
   only ran on the return path would be right about half the threads a
   browser has. */
static int exit_path_destructor_calls;

static void count_exit_path(void *arg) {
    (void)arg;
    exit_path_destructor_calls++;
}

static void *exit_path_worker(void *arg) {
    (void)arg;
    __cxa_thread_atexit(count_exit_path, 0, 0);
    pthread_exit((void *)0);
    return (void *)1;
}

/* The thread-specific storage table held one row per thread that had EVER
   existed rather than one per live thread, so the thirty-fourth thread to
   call pthread_setspecific got ENOMEM and every one after it did too. Forty
   threads, created and joined one at a time, is past that edge with room. */
#define STORAGE_ROUNDS 40

static pthread_key_t churn_key;

static void *churn_worker(void *arg) {
    long round = (long)arg;
    if (pthread_setspecific(churn_key, (void *)(round + 1)) != 0) {
        return (void *)1;
    }
    if (pthread_getspecific(churn_key) != (void *)(round + 1)) {
        return (void *)2;
    }
    return (void *)0;
}


/* M167. A process outlives its first thread: main calls pthread_exit(), the
   process goes on running in a thread it created, and that thread keeps
   using memory the process mapped.

   Until M167 the thread-group leader reset its own record of the address
   space as it terminated, even though its threads were still running in it.
   Everything that reaches the address space through the owner - the
   mappings, the break, the page fault handler's own page table - was then
   handed the KERNEL's, and the shared pages of a memfd stayed mapped into a
   space nothing could release them from. The teardown at the end freed
   frames the memfd still owned, and the machine stopped with a double free
   three layers from the cause.

   The check is what the surviving thread can still do, and then that every
   frame comes back - which is what [m79] measures around this program. */

static unsigned long leader_exit_pages = 64;
#define LEADER_EXIT_BYTE  0x5C

static volatile unsigned char *leader_exit_mapping;

static int leader_exit_bare;

static void *outlives_the_leader(void *unused) {
    (void)unused;
    if (leader_exit_bare) {
        sys_exit(0);
    }

    /* The leader is on its way out while this runs. Touching pages that have
       not been faulted in yet is the point: the fault has to be answered
       against this address space, and the region it needs is the one the
       process owns rather than this thread's own empty table. */
    for (int round = 0; round < 200; round++) {
        sys_yield();
    }
    for (unsigned long i = 0; i < leader_exit_pages; i++) {
        leader_exit_mapping[i * 4096] = (unsigned char)(LEADER_EXIT_BYTE + i);
    }
    for (unsigned long i = 0; i < leader_exit_pages; i++) {
        if (leader_exit_mapping[i * 4096] !=
            (unsigned char)(LEADER_EXIT_BYTE + i)) {
            sys_exit(24);
        }
    }

    /* A mapping made AFTER the leader has gone, because mmap answers out of
       the owner's region table too. */
    void *late = mmap(0, 4096, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (late == MAP_FAILED) {
        sys_exit(25);
    }
    ((volatile unsigned char *)late)[0] = 0x7E;
    if (((volatile unsigned char *)late)[0] != 0x7E) {
        sys_exit(26);
    }
    munmap(late, 4096);

    munmap((void *)leader_exit_mapping, leader_exit_pages * 4096);

    /* This thread ends the process, because it is the only one left and the
       exit code has to be this mode's answer rather than whatever a thread
       returning happens to leave behind. */
    sys_exit(0);
    return 0;
}

static int leader_exit_mode(const char *pages_text) {
    if (pages_text) {
        unsigned long n = 0;
        for (const char *c = pages_text; *c >= '0' && *c <= '9'; c++) {
            n = n * 10 + (unsigned long)(*c - '0');
        }
        if (n) {
            leader_exit_pages = n;
        }
        leader_exit_bare = (pages_text[0] == 'b');
    }
    /* An ordinary private mapping. The bug this reproduces is about which
       address space the OWNER names, which a page fault on any mapping
       exercises - the memfd this used at first proved the same thing twice
       and dragged a second subsystem into the measurement. */
    leader_exit_mapping = (volatile unsigned char *)mmap(
        0, leader_exit_pages * 4096, PROT_READ | PROT_WRITE,
        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (leader_exit_bare) {
        munmap((void *)leader_exit_mapping, leader_exit_pages * 4096);
        leader_exit_mapping = 0;
    }
    if (leader_exit_mapping == (volatile unsigned char *)MAP_FAILED) {
        return 23;
    }
    /* One page touched here, so the mapping is real before the leader goes
       and the rest are faults the surviving thread has to take. */
    if (leader_exit_mapping) {
        leader_exit_mapping[0] = LEADER_EXIT_BYTE;
    }

    pthread_t survivor;
    if (pthread_create(&survivor, 0, outlives_the_leader, 0) != 0) {
        return 23;
    }

    /* The leader leaves. The process does not. */
    pthread_exit(0);
    return 27;
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1] && strcmp(argv[1], "leaderexit") == 0) {
        return leader_exit_mode(argc > 2 ? argv[2] : 0);
    }

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

    /* A destructor that is not there cannot be run later, so refusing is the
       only truthful answer. The C++ runtime never asks this, which is exactly
       why nothing else would find it. */
    if (__cxa_thread_atexit(0, (void *)1, 0) != -1) {
        printf("threadtest: __cxa_thread_atexit accepted a null destructor\n");
        return 16;
    }

    pthread_t g, h;
    void *rg = 0, *rh = 0;
    if (pthread_create(&g, 0, destructor_worker, (void *)0) != 0 ||
        pthread_join(g, &rg) != 0 ||
        pthread_create(&h, 0, destructor_worker, (void *)1) != 0 ||
        pthread_join(h, &rh) != 0) {
        return 16;
    }
    if (rg != (void *)0 || rh != (void *)0) {
        printf("threadtest: a thread_local destructor ran at registration or "
               "could not be registered (%ld, %ld)\n", (long)rg, (long)rh);
        return 16;
    }
    for (int which = 0; which < 2; which++) {
        if (destructor_count[which] != THREAD_DESTRUCTORS) {
            printf("threadtest: thread %d ran %d of %d thread_local "
                   "destructors\n", which, destructor_count[which],
                   THREAD_DESTRUCTORS);
            return 17;
        }
        for (int i = 0; i < THREAD_DESTRUCTORS; i++) {
            int wanted = THREAD_DESTRUCTORS - 1 - i;
            if (destructor_order[which][i] != wanted) {
                printf("threadtest: thread_local destructor %d of thread %d "
                       "was %d, wanted %d - the order is not reversed\n",
                       i, which, destructor_order[which][i], wanted);
                return 18;
            }
        }
    }
    /* Each list is the THREAD's, not the program's. Two threads that shared
       one would still run eighteen destructors in some order and pass every
       check above. */
    if (destructor_thread_of[0] == destructor_thread_of[1] ||
        destructor_thread_of[0] == (int)sys_gettid() ||
        destructor_thread_of[1] == (int)sys_gettid()) {
        printf("threadtest: the two threads' destructors ran on the same "
               "thread (%d, %d; main is %d)\n", destructor_thread_of[0],
               destructor_thread_of[1], (int)sys_gettid());
        return 19;
    }

    if (pthread_key_create(&sweep_key, sweep_destructor) != 0) {
        return 20;
    }
    pthread_t i_thread;
    void *ri = 0;
    if (pthread_create(&i_thread, 0, sweep_worker, 0) != 0 ||
        pthread_join(i_thread, &ri) != 0 || ri != (void *)0) {
        return 20;
    }
    if (sweep_calls != 1 || sweep_value_seen != (void *)0xC0FFEE ||
        !sweep_key_was_clear) {
        printf("threadtest: pthread key destructor ran %d time(s), saw %p, "
               "key clear %d\n", sweep_calls, sweep_value_seen,
               sweep_key_was_clear);
        return 20;
    }
    /* The main thread never touched the key, so nothing is owed for it. */
    if (pthread_key_delete(sweep_key) != 0 || sweep_calls != 1) {
        return 20;
    }

    pthread_t j_thread;
    void *rj = 0;
    if (pthread_create(&j_thread, 0, exit_path_worker, 0) != 0 ||
        pthread_join(j_thread, &rj) != 0 || rj != (void *)0) {
        return 21;
    }
    if (exit_path_destructor_calls != 1) {
        printf("threadtest: pthread_exit ran %d destructors, wanted 1\n",
               exit_path_destructor_calls);
        return 21;
    }

    if (pthread_key_create(&churn_key, 0) != 0) {
        return 22;
    }
    for (long round = 0; round < STORAGE_ROUNDS; round++) {
        pthread_t churn;
        void *rc = 0;
        if (pthread_create(&churn, 0, churn_worker, (void *)round) != 0 ||
            pthread_join(churn, &rc) != 0) {
            return 22;
        }
        if (rc != (void *)0) {
            printf("threadtest: thread %ld could not use thread-specific "
                   "storage (%ld) - the table keeps a row per thread that "
                   "ever existed\n", round, (long)rc);
            return 22;
        }
    }
    if (pthread_key_delete(churn_key) != 0) {
        return 22;
    }

    printf("threadtest: all checks passed (protected %ld of %ld; unprotected lost %ld)\n",
            protected_counter, 2 * (long)ITERATIONS,
            2 * (long)ITERATIONS - racy_counter);
    return 0;
}
