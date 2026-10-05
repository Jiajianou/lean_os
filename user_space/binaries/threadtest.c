#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdlib.h>
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

static unsigned long count_argument(const char *text, unsigned long fallback) {
    unsigned long n = 0;
    for (const char *c = text ? text : ""; *c >= '0' && *c <= '9'; c++) {
        n = n * 10 + (unsigned long)(*c - '0');
    }
    return n ? n : fallback;
}

static volatile int retired;

static void *retire(void *argument) {
    (void)argument;
    __atomic_store_n(&retired, 1, __ATOMIC_RELEASE);
    return 0;
}

/* M205: what a thread pool does all day - start a worker nobody will join,
   let it finish, start another. Every one of them used to keep its slot in
   the kernel's task table, and this process alone would run out of them
   before the 230th. */
static int detached_mode(const char *count_text) {
    unsigned long count = count_argument(count_text, 600);
    for (unsigned long i = 0; i < count; i++) {
        __atomic_store_n(&retired, 0, __ATOMIC_RELEASE);
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        pthread_t worker;
        int rc = pthread_create(&worker, &attributes, retire, 0);
        pthread_attr_destroy(&attributes);
        if (rc != 0) {
            printf("threadtest: detached thread %lu of %lu could not be created (%d)\n",
                   i + 1, count, rc);
            return 40;
        }
        while (!__atomic_load_n(&retired, __ATOMIC_ACQUIRE)) {
            sched_yield();
        }
    }
    return 0;
}

static volatile int abandoned_finished;

static void *abandoned(void *argument) {
    (void)argument;
    __atomic_add_fetch(&abandoned_finished, 1, __ATOMIC_ACQ_REL);
    return 0;
}

/* M205: joinable threads that finish and are never joined, in a process that
   then exits. Nobody is left who could join them. */
static int abandon_mode(const char *count_text) {
    unsigned long count = count_argument(count_text, 40);
    for (unsigned long i = 0; i < count; i++) {
        pthread_t worker;
        if (pthread_create(&worker, 0, abandoned, 0) != 0) {
            return 41;
        }
    }
    while (__atomic_load_n(&abandoned_finished, __ATOMIC_ACQUIRE) < (int)count) {
        sched_yield();
    }
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


/* M169. As many threads as this library says it supports.

   sysconf(_SC_THREAD_THREADS_MAX) answered 128 while the registry behind
   pthread_create held 32, so the thirty-third thread was refused with EAGAIN
   by a library that had just said it would take a hundred and twenty-eight.
   Nothing on this machine had ever wanted more than a handful; Chromium in
   one process wants more than thirty-two before it has finished starting.

   The count is taken from sysconf rather than written here, because what is
   being graded is that the answer and the behaviour are the same answer. */
static void *counting_worker(void *argument) {
    __atomic_fetch_add((int *)argument, 1, __ATOMIC_RELAXED);
    return (void *)7;
}

static int as_many_threads_as_promised(void) {
    long promised = sysconf(_SC_THREAD_THREADS_MAX);
    if (promised < 2) {
        printf("threadtest: sysconf says %ld threads\n", promised);
        return 40;
    }
    /* Half, because the kernel's task table is the whole machine's and the
       rest of the machine is running too. Half of the promise is still four
       times what the old registry held, which is what this is about. */
    int want = (int)(promised / 2);
    if (want > 64) {
        want = 64;
    }
    pthread_t *threads = (pthread_t *)calloc((size_t)want, sizeof(pthread_t));
    if (!threads) {
        return 41;
    }
    int ran = 0;
    int made = 0;
    for (int i = 0; i < want; i++) {
        if (pthread_create(&threads[i], 0, counting_worker, &ran) != 0) {
            break;
        }
        made++;
    }
    for (int i = 0; i < made; i++) {
        void *value = 0;
        if (pthread_join(threads[i], &value) != 0 || value != (void *)7) {
            free(threads);
            return 42;
        }
    }
    free(threads);
    if (made != want) {
        printf("threadtest: sysconf promises %ld threads and the %d'th was "
               "refused\n", promised, made + 1);
        return 43;
    }
    if (ran != want) {
        printf("threadtest: %d of %d threads ran\n", ran, want);
        return 44;
    }
    return 0;
}

/* The region table, read by one thread's page fault while another thread is
   changing it.

   A page fault answers "is this address mine" from the process's table of
   mmap regions WITHOUT the table's lock (M181 measured why), and every
   mmap, munmap and mprotect by a sibling thread rewrites that table under
   the lock: a split shrinks a region and then inserts its tail, an unmap
   shifts every entry above it down by one, a merge moves a region's base
   before it grows its length. A fault that read the table half way through
   one of those was told its own fresh page was nobody's, and the thread
   was killed for touching memory mmap had just handed it. [m79] met it on
   four processors as threadtest's TLS block faulting in two new threads at
   once (error code 6, cr2 inside the block __lean_tls_setup had mapped a
   moment before).

   So: several threads at once, each mapping three pages next to whatever
   the others mapped, touching each one for the first time (three faults
   that have to find their region), splitting the middle page off with
   mprotect and unmapping the lot. On one processor nothing can interleave
   and this passes on any kernel; on more it is the race. A lost fault ends
   the whole process with SIGSEGV, which is the exit code [m79] reports. */
#define REGION_THREADS 4

static unsigned long region_rounds = 1500;
static volatile int region_go;

static void *region_churn(void *argument) {
    unsigned char mark = (unsigned char)(0x40 + (long)argument);
    while (!__atomic_load_n(&region_go, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
    for (unsigned long round = 0; round < region_rounds; round++) {
        volatile unsigned char *p = (volatile unsigned char *)mmap(
            0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == (volatile unsigned char *)MAP_FAILED) {
            return (void *)1;
        }
        p[0] = mark;
        p[4096] = (unsigned char)(mark + 1);
        p[8192] = (unsigned char)(mark + 2);
        if (mprotect((void *)(p + 4096), 4096, PROT_READ) != 0) {
            return (void *)2;
        }
        if (p[0] != mark || p[4096] != (unsigned char)(mark + 1) ||
            p[8192] != (unsigned char)(mark + 2)) {
            return (void *)3;
        }
        if (munmap((void *)p, 3 * 4096) != 0) {
            return (void *)4;
        }
    }
    return (void *)0;
}

static int region_table_under_threads(void) {
    pthread_t threads[REGION_THREADS];
    int made = 0;
    __atomic_store_n(&region_go, 0, __ATOMIC_RELEASE);
    for (long i = 0; i < REGION_THREADS; i++) {
        if (pthread_create(&threads[i], 0, region_churn, (void *)i) != 0) {
            break;
        }
        made++;
    }
    __atomic_store_n(&region_go, 1, __ATOMIC_RELEASE);
    int failed = made != REGION_THREADS;
    for (int i = 0; i < made; i++) {
        void *value = 0;
        if (pthread_join(threads[i], &value) != 0 || value != (void *)0) {
            printf("threadtest: region thread %d answered %ld - mmap, mprotect "
                   "or munmap refused a range it had just been given\n",
                   i, (long)value);
            failed = 1;
        }
    }
    return failed ? 45 : 0;
}

/* M225. A process whose main thread has left is still a process.

   Since M225 wait() says so - it reports the process when its LAST thread
   ends - but kill(2) answered ESRCH the moment the main thread had gone, so
   a parent had a child it could neither signal nor stop waiting for: the
   compositor's SIGTERM-then-SIGKILL to a client, the Task Manager's End,
   Node's subprocess.kill, all of them ESRCH and then a wait with no end.
   Linux delivers a signal for a pid to the thread group. The child here
   leaves through pthread_exit with a thread still running; the parent waits
   until /proc says the main thread has terminated, and then asks: is it
   there (kill 0), and can it be ended (SIGKILL, then a wait that returns). */
static void *lingers(void *argument) {
    (void)argument;
    for (;;) {
        usleep(20000);
    }
    return 0;
}

static int leader_state_is_terminated(int pid) {
    char path[48];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    char text[512];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return strstr(text, "State:\tterminated") != 0;
}

static int leaderless_mode(void) {
    pid_t child = fork();
    if (child < 0) {
        return 50;
    }
    if (child == 0) {
        pthread_t thread;
        if (pthread_create(&thread, 0, lingers, 0) != 0) {
            _exit(41);
        }
        pthread_exit(0);
        _exit(42);
    }
    int left = 0;
    for (int i = 0; i < 3000 && !left; i++) {
        left = leader_state_is_terminated((int)child);
        if (!left) {
            usleep(10000);
        }
    }
    if (!left) {
        kill(child, SIGKILL);
        printf("threadtest: leaderless - the child's main thread never left\n");
        return 51;
    }
    int status = 0;
    if (waitpid(child, &status, WNOHANG) != 0) {
        printf("threadtest: leaderless - wait reported a process still running\n");
        return 52;
    }
    if (kill(child, 0) != 0) {
        printf("threadtest: leaderless - kill(pid, 0) said %d (errno %d) of a running process\n",
               -1, errno);
        kill(child, SIGKILL);
        return 53;
    }
    if (kill(child, SIGKILL) != 0) {
        printf("threadtest: leaderless - kill(pid, SIGKILL) refused (errno %d)\n", errno);
        return 54;
    }
    for (int i = 0; i < 3000; i++) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            if (kill(child, 0) == 0 || errno != ESRCH) {
                printf("threadtest: leaderless - a reaped process still answers kill(pid, 0)\n");
                return 56;
            }
            printf("threadtest: leaderless - a process whose main thread had left was "
                   "there for kill(pid, 0), ended by kill(pid, SIGKILL) and reaped\n");
            return 0;
        }
        if (done < 0) {
            printf("threadtest: leaderless - waitpid failed (errno %d)\n", errno);
            return 55;
        }
        usleep(10000);
    }
    printf("threadtest: leaderless - SIGKILL was accepted and the process never ended\n");
    return 57;
}

/* M225 (process-lifetimes): fcntl record locks across the threads of ONE
   process. POSIX makes them the process's: a second thread asking for a
   range its process holds is not in conflict, any thread may unlock what
   another took, F_GETLK from outside names the process (getpid()), a lock
   outlives the thread that took it, and closing ANY descriptor for the file
   gives back all of the process's locks on it. The kernel keyed them by the
   task, so each of those was the opposite. What another process sees is
   asked of a forked child, which answers in its exit status. */
/* M225 (fd-use-holds): one file per PROCESS. It was one fixed path opened
   with O_TRUNC, so two threadtests at once - the battery's [m79] and a
   harness's, or two harnesses - truncated, locked and unlinked each other's
   file, and each saw the other's locks as its own process's. */
static char record_lock_file[48];

static int record_lock_fd;

static int record_lock(int fd, int command, short type, off_t start, off_t length,
                       struct flock *answer) {
    struct flock request;
    memset(&request, 0, sizeof(request));
    request.l_type = type;
    request.l_whence = SEEK_SET;
    request.l_start = start;
    request.l_len = length;
    int rc = fcntl(fd, command, &request);
    if (answer) {
        *answer = request;
    }
    return rc;
}

#define SEEN_UNLOCKED 0
#define SEEN_HELD_BY_US 1
#define SEEN_SOMETHING_ELSE 2

/* What a different process is told about [start, start+length). */
static int seen_from_another_process(off_t start, off_t length) {
    pid_t us = getpid();
    pid_t child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        struct flock seen;
        if (record_lock(record_lock_fd, F_GETLK, F_WRLCK, start, length, &seen) != 0) {
            _exit(3);
        }
        if (seen.l_type == F_UNLCK) {
            _exit(SEEN_UNLOCKED);
        }
        if (seen.l_type == F_WRLCK && seen.l_pid == us) {
            _exit(SEEN_HELD_BY_US);
        }
        printf("threadtest: recordlocks - another process was told type %d pid %d "
               "(we are %d)\n", seen.l_type, (int)seen.l_pid, (int)us);
        _exit(SEEN_SOMETHING_ELSE);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

static void *takes_its_own_process_lock_again(void *unused) {
    (void)unused;
    /* [0,10) is held by the main thread - by this process. */
    if (record_lock(record_lock_fd, F_SETLK, F_WRLCK, 0, 10, 0) != 0) {
        return (void *)1;
    }
    struct flock seen;
    if (record_lock(record_lock_fd, F_GETLK, F_WRLCK, 0, 10, &seen) != 0 ||
        seen.l_type != F_UNLCK) {
        return (void *)2;
    }
    /* And one of its own, which it leaves holding. */
    if (record_lock(record_lock_fd, F_SETLK, F_WRLCK, 20, 10, 0) != 0) {
        return (void *)3;
    }
    return (void *)0;
}

static void *unlocks_what_main_took(void *unused) {
    (void)unused;
    return record_lock(record_lock_fd, F_SETLK, F_UNLCK, 0, 10, 0) == 0 ? (void *)0 : (void *)1;
}

static int record_locks_across_threads(void) {
    snprintf(record_lock_file, sizeof(record_lock_file), "/tmp/threadtest.%d.locks",
             (int)getpid());
    record_lock_fd = open(record_lock_file, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (record_lock_fd < 0) {
        return 60;
    }
    int code = 0;
    pthread_t t;
    void *r = (void *)1;
    if (record_lock(record_lock_fd, F_SETLK, F_WRLCK, 0, 10, 0) != 0) {
        code = 60;
    } else if (pthread_create(&t, 0, takes_its_own_process_lock_again, 0) != 0 ||
               pthread_join(t, &r) != 0 || r != (void *)0) {
        printf("threadtest: recordlocks - a second thread was refused (or not told it "
               "held) a range its own process holds (%ld)\n", (long)r);
        code = 61;
    } else if (seen_from_another_process(0, 10) != SEEN_HELD_BY_US) {
        printf("threadtest: recordlocks - another process was not told this process "
               "holds [0,10)\n");
        code = 62;
    } else if (seen_from_another_process(20, 10) != SEEN_HELD_BY_US) {
        printf("threadtest: recordlocks - the lock a thread took went when the thread "
               "did; it is its process's\n");
        code = 63;
    } else if (pthread_create(&t, 0, unlocks_what_main_took, 0) != 0 ||
               pthread_join(t, &r) != 0 || r != (void *)0 ||
               seen_from_another_process(0, 10) != SEEN_UNLOCKED) {
        printf("threadtest: recordlocks - a thread could not unlock what another "
               "thread of its process took\n");
        code = 64;
    } else {
        /* POSIX's close rule: closing any descriptor for the file gives back
           every lock this process has on it - here the one the first thread
           took, through another descriptor. */
        int other = open(record_lock_file, O_RDONLY);
        if (other < 0 || close(other) != 0 ||
            seen_from_another_process(20, 10) != SEEN_UNLOCKED) {
            printf("threadtest: recordlocks - closing another descriptor for the "
                   "file left the process's lock in place\n");
            code = 65;
        }
    }
    close(record_lock_fd);
    unlink(record_lock_file);
    if (code == 0) {
        printf("threadtest: recordlocks - one process's threads share its record "
               "locks: no conflict, any may unlock, the lock outlives its thread, "
               "F_GETLK names the process, a close gives them all back\n");
    }
    return code;
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1] && strcmp(argv[1], "recordlocks") == 0) {
        return record_locks_across_threads();
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "leaderless") == 0) {
        return leaderless_mode();
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "regions") == 0) {
        region_rounds = count_argument(argc > 2 ? argv[2] : 0, region_rounds);
        int rc = region_table_under_threads();
        if (rc == 0) {
            printf("threadtest: regions - %d threads, %lu rounds each of map, "
                   "first touch, split and unmap, no fault lost\n",
                   REGION_THREADS, region_rounds);
        }
        return rc;
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "leaderexit") == 0) {
        return leader_exit_mode(argc > 2 ? argv[2] : 0);
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "detached") == 0) {
        return detached_mode(argc > 2 ? argv[2] : 0);
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "abandon") == 0) {
        return abandon_mode(argc > 2 ? argv[2] : 0);
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

    int many = as_many_threads_as_promised();
    if (many != 0) {
        return many;
    }

    int regions = region_table_under_threads();
    if (regions != 0) {
        return regions;
    }

    int locks = record_locks_across_threads();
    if (locks != 0) {
        return locks;
    }

    printf("threadtest: all checks passed (protected %ld of %ld; unprotected lost %ld)\n",
            protected_counter, 2 * (long)ITERATIONS,
            2 * (long)ITERATIONS - racy_counter);
    return 0;
}
