#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "syscall_wrappers.h"

#define PAGE 4096UL

static volatile int before_fork = 0;

#define COW_PAGES 2048UL
#define PARK_YIELDS 600

static int cow_mode(void) {
    volatile char *big = (volatile char *)mmap(0, COW_PAGES * PAGE,
                                                PROT_READ | PROT_WRITE,
                                                MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (big == (volatile char *)MAP_FAILED) {
        return 2;
    }
    for (unsigned long i = 0; i < COW_PAGES; i++) {
        big[i * PAGE] = (char)(i & 0x7F);
    }

    pid_t k = fork();
    if (k < 0) {
        return 2;
    }
    if (k == 0) {
        volatile char sink = 0;
        for (unsigned long i = 0; i < COW_PAGES; i++) {
            sink = (char)(sink + big[i * PAGE]);
        }
        (void)sink;
        for (int i = 0; i < PARK_YIELDS; i++) {
            sys_yield();
        }
        sys_exit(0);
    }
    for (int i = 0; i < PARK_YIELDS; i++) {
        sys_yield();
    }
    long rc = sys_wait(k);
    munmap((void *)big, COW_PAGES * PAGE);
    return rc == 0 ? 0 : 8;
}


/* Fork from a process that has other threads running, which this kernel
   refused until M165. The codes here are 20 and up so that nothing already
   printed against a code below 20 changes meaning.

   What makes this worth booting rather than reasoning about is the last
   check. A sibling thread writing in a tight loop on another core holds a
   writable translation for every page it touches. fork clears the writable
   bit in the page table, but a page table is not a TLB, so without a
   shootdown the sibling keeps writing into the frame the child was just
   promised is its own - and the child reads a value from after the fork. One
   page would almost never catch it; a thousand, checked immediately, does. */

#define THREAD_PAGES  512UL
#define SIBLINGS      3
#define THREAD_ROUNDS 8
#define HOT_WRITES    4096UL
#define BEFORE_FORK   0x41
#define AFTER_FORK    0x42
#define CHILD_WROTE   0x43

static volatile char *shared_pages;
static volatile int siblings_should_stop;
static volatile unsigned long sibling_laps[SIBLINGS];
static volatile int sibling_write_value = BEFORE_FORK;

static void *sibling_main(void *argument) {
    unsigned long which = (unsigned long)argument;
    while (!siblings_should_stop) {
        /* A page of this sibling's own, written over and over with whatever
           the value is RIGHT NOW rather than with what it was at the top of
           the lap. That is what makes the window catchable: the parent can
           change the value after fork has returned and this thread is
           writing the new one a handful of instructions later, with no lap
           boundary in between to wait for. */
        for (unsigned long spin = 0; spin < HOT_WRITES; spin++) {
            shared_pages[which * PAGE] = (char)sibling_write_value;
            /* And a lap tag beside it, so that a frame this thread is
               writing can be told from one it is not: M172 read a stale
               value and a fresh tag out of the SAME frame, which is what
               named the bug as a stale read of the value, not a lost write. */
            shared_pages[which * PAGE + 8] = (char)(0x80 | (sibling_laps[which] & 0x7f));
        }
        char value = (char)sibling_write_value;
        for (unsigned long i = SIBLINGS; i < THREAD_PAGES; i++) {
            shared_pages[i * PAGE] = value;
        }
        sibling_laps[which]++;
    }
    return 0;
}

/* Hand the siblings a new value and do not come back until every page holds
   it. A sibling reads the value once at the top of a lap, so a lap already
   under way finishes writing the old one: two full laps each is what makes
   "every page was written with the new value last" true rather than likely. */
static int siblings_settle_on(int value) {
    sibling_write_value = value;
    unsigned long target[SIBLINGS];
    for (int i = 0; i < SIBLINGS; i++) {
        target[i] = sibling_laps[i] + 2;
    }
    for (int spin = 0; spin < 2000000; spin++) {
        int settled = 1;
        for (int i = 0; i < SIBLINGS; i++) {
            if (sibling_laps[i] < target[i]) {
                settled = 0;
            }
        }
        if (settled) {
            return 0;
        }
        sys_yield();
    }
    return -1;
}

static int threads_mode(void) {
    shared_pages = (volatile char *)mmap(0, THREAD_PAGES * PAGE,
                                         PROT_READ | PROT_WRITE,
                                         MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (shared_pages == (volatile char *)MAP_FAILED) {
        return 20;
    }
    for (unsigned long i = 0; i < THREAD_PAGES; i++) {
        shared_pages[i * PAGE] = BEFORE_FORK;
    }

    pthread_t siblings[SIBLINGS];
    for (int i = 0; i < SIBLINGS; i++) {
        if (pthread_create(&siblings[i], 0, sibling_main,
                           (void *)(unsigned long)i) != 0) {
            return 21;
        }
    }

    /* Every sibling has to be round its loop, or "there were other threads"
       is a claim about pthread_create rather than about this process. */
    if (siblings_settle_on(BEFORE_FORK) != 0) {
        return 22;
    }

    /* Rounds, because one fork is one sample of a race. A sibling whose core
       happened to reload CR3 between the page table change and the child's
       read would make a kernel with no shootdown look right exactly once. */
    for (int round = 0; round < THREAD_ROUNDS; round++) {
        if (siblings_settle_on(BEFORE_FORK) != 0) {
            return 22;
        }

        pid_t kid = fork();
        if (kid < 0) {
            return 23;
        }

        if (kid == 0) {
            /* The child is one thread: the one that called fork. Nothing else
               in here runs, so every lap counter stands still across a stretch
               in which the parent's are climbing. */
            unsigned long laps_at_entry[SIBLINGS];
            for (int i = 0; i < SIBLINGS; i++) {
                laps_at_entry[i] = sibling_laps[i];
            }

            /* Read before yielding to anything. The parent's siblings are
               writing AFTER_FORK into what is now the parent's own copy, and
               not one of those writes may be visible here. */
            int from_after_the_fork = 0;
            for (unsigned long i = 0; i < THREAD_PAGES; i++) {
                if (shared_pages[i * PAGE] != BEFORE_FORK) {
                    from_after_the_fork++;
                }
            }
            if (from_after_the_fork) {
                printf("forktest: round %d: %d of %lu pages carried a write "
                       "from after the fork - a sibling was still writing "
                       "through a translation nobody discarded\n", round,
                       from_after_the_fork, THREAD_PAGES);
                sys_exit(24);
            }

            for (int i = 0; i < 100; i++) {
                sys_yield();
            }
            for (int i = 0; i < SIBLINGS; i++) {
                if (sibling_laps[i] != laps_at_entry[i]) {
                    printf("forktest: round %d: sibling %d ran in the child - "
                           "fork gave the child more than the calling "
                           "thread\n", round, i);
                    sys_exit(25);
                }
            }

            /* Writing here breaks copy-on-write rather than reaching the
               parent, over every page rather than one. */
            for (unsigned long i = 0; i < THREAD_PAGES; i++) {
                shared_pages[i * PAGE] = CHILD_WROTE;
            }
            for (unsigned long i = 0; i < THREAD_PAGES; i++) {
                if (shared_pages[i * PAGE] != CHILD_WROTE) {
                    sys_exit(26);
                }
            }
            sys_exit(0);
        }

        /* Before anything else, and deliberately without settling: the child
           is reading its pages on another core at this moment, and the whole
           question is whether a sibling's writes can still reach them. The
           siblings pick this up within a write or two. */
        sibling_write_value = AFTER_FORK;

        /* The siblings keep writing and what they write changes; if one of
           them died - on a copy-on-write fault two took at once, which a
           single-threaded fork could not reach - its laps stop and settling
           never finishes. */
        unsigned long laps_before[SIBLINGS];
        for (int i = 0; i < SIBLINGS; i++) {
            laps_before[i] = sibling_laps[i];
        }
        if (siblings_settle_on(AFTER_FORK) != 0) {
            printf("forktest: round %d: a sibling stopped running across the "
                   "fork\n", round);
            return 27;
        }

        long child_rc = sys_wait(kid);
        if (child_rc != 0) {
            return (int)child_rc;
        }

        for (int i = 0; i < SIBLINGS; i++) {
            if (sibling_laps[i] <= laps_before[i]) {
                printf("forktest: round %d: sibling %d never ran again\n",
                       round, i);
                return 27;
            }
        }

        /* The parent's pages are the parent's: the child wrote over every one
           of them and none of that may be here. */
        for (unsigned long i = 0; i < THREAD_PAGES; i++) {
            if (shared_pages[i * PAGE] != AFTER_FORK) {
                printf("forktest: round %d: page %lu is 0x%02x rather than "
                       "what this process last wrote - a sibling was still "
                       "reading the frame fork took away from it (M172), or "
                       "the child's writes reached the parent\n", round, i,
                       (unsigned)(unsigned char)shared_pages[i * PAGE]);
                return 28;
            }
        }
    }

    siblings_should_stop = 1;
    for (int i = 0; i < SIBLINGS; i++) {
        pthread_join(siblings[i], 0);
    }
    munmap((void *)shared_pages, THREAD_PAGES * PAGE);
    return 0;
}


/* fork called by a thread that is not the one the process started on. The
   mappings, the break and the working directory belong to the address space
   rather than to the caller, and a thread's own copies of them are empty - so
   a child built from the caller's had no mappings at all. */

static volatile int thread_fork_result;

static void *forking_thread_main(void *argument) {
    (void)argument;
    volatile char *arena = (volatile char *)mmap(0, 4 * PAGE,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_ANONYMOUS | MAP_PRIVATE,
                                                 -1, 0);
    if (arena == (volatile char *)MAP_FAILED) {
        thread_fork_result = 30;
        return 0;
    }
    arena[0] = 'T';
    arena[3 * PAGE] = 'T';

    pid_t kid = fork();
    if (kid < 0) {
        thread_fork_result = 31;
        return 0;
    }
    if (kid == 0) {
        /* Touching the far end proves the whole region came across rather
           than one page the fault handler would have filled anyway. */
        if (arena[0] != 'T' || arena[3 * PAGE] != 'T') {
            sys_exit(32);
        }
        arena[0] = 'U';
        void *more = mmap(0, PAGE, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (more == MAP_FAILED) {
            sys_exit(33);
        }
        char *heap = (char *)malloc(64 * 1024);
        if (!heap) {
            sys_exit(34);
        }
        heap[0] = 'h';
        heap[64 * 1024 - 1] = 'h';
        sys_exit(0);
    }

    long rc = sys_wait(kid);
    if (rc != 0) {
        thread_fork_result = (int)rc;
        return 0;
    }
    if (arena[0] != 'T') {
        thread_fork_result = 35;
        return 0;
    }
    munmap((void *)arena, 4 * PAGE);
    thread_fork_result = 0;
    return 0;
}

static int thread_fork_mode(void) {
    pthread_t forker;
    if (pthread_create(&forker, 0, forking_thread_main, 0) != 0) {
        return 29;
    }
    pthread_join(forker, 0);
    return thread_fork_result;
}

/* M192. Threads of one process opening and closing at once, on as many
   processors as the machine has. Each registers the number it was given
   before using it and clears the registration before closing; finding the
   number already registered means two live descriptors were handed the same
   number - the bug that stopped Chromium on four cores. Pipes and open(2)
   both, because they claimed slots by different code. */
#define DESCRIPTOR_RACERS 4
#define DESCRIPTOR_ROUNDS 3000
#define DESCRIPTOR_CEILING 1024

static volatile int descriptor_owner[DESCRIPTOR_CEILING];
static volatile int descriptor_collisions;
static volatile int descriptor_failures;

static void *descriptor_racer(void *arg) {
    int me = (int)(long)arg + 1;
    for (int round = 0; round < DESCRIPTOR_ROUNDS; round++) {
        int fds[2];
        int count = 0;
        if (round & 1) {
            if (pipe(fds) != 0) {
                __sync_fetch_and_add(&descriptor_failures, 1);
                continue;
            }
            count = 2;
        } else {
            fds[0] = open("/dev/null", O_RDONLY);
            if (fds[0] < 0) {
                __sync_fetch_and_add(&descriptor_failures, 1);
                continue;
            }
            count = 1;
        }
        for (int i = 0; i < count; i++) {
            if (fds[i] >= DESCRIPTOR_CEILING ||
                !__sync_bool_compare_and_swap(&descriptor_owner[fds[i]], 0, me)) {
                __sync_fetch_and_add(&descriptor_collisions, 1);
            }
        }
        for (int i = 0; i < count; i++) {
            if (fds[i] < DESCRIPTOR_CEILING && descriptor_owner[fds[i]] == me) {
                descriptor_owner[fds[i]] = 0;
            }
            close(fds[i]);
        }
    }
    return 0;
}

static int fdslots_mode(void);
static int fduse_mode(void);
static int execenv_stage(int stage);

static int descriptors_mode(void) {
    pthread_t racers[DESCRIPTOR_RACERS];
    for (long i = 0; i < DESCRIPTOR_RACERS; i++) {
        if (pthread_create(&racers[i], 0, descriptor_racer, (void *)i) != 0) {
            return 40;
        }
    }
    for (int i = 0; i < DESCRIPTOR_RACERS; i++) {
        pthread_join(racers[i], 0);
    }
    printf("forktest: %d racers x %d rounds, %d descriptor(s) handed out twice, %d failure(s)\n",
           DESCRIPTOR_RACERS, DESCRIPTOR_ROUNDS, descriptor_collisions, descriptor_failures);
    if (descriptor_collisions != 0) {
        return 41;
    }
    if (descriptor_failures != 0) {
        return 42;
    }
    /* M225: and then the other half of a descriptor's life - letting go of
       one while a sibling is copying it - in the same [forksmp] stage, which
       runs this mode on every core the machine has. */
    int slots = fdslots_mode();
    if (slots != 0) {
        return slots;
    }
    /* M225 (fd-use-holds): and the third - USING one while a sibling closes
       it. */
    return fduse_mode();
}

/* M193. Two threads handing a word back and forth through futexes, on as
   many processors as the machine has. A wait carries a two-second timeout it
   should never need: the other side changes the word and wakes it at once. A
   wait that times out and finds the word already handed over is a wake that
   was lost - the waker ran between the waiter's check and its sleep - which on
   the old kernel hung the forksmp stage for its whole half hour. */
#define FUTEX_ROUNDS 20000
#define FUTEX_PATIENCE_MS 2000

static volatile unsigned int futex_turn;
static volatile int futex_lost;

static void futex_player(unsigned int me) {
    for (int round = 0; round < FUTEX_ROUNDS; round++) {
        while (futex_turn != me) {
            unsigned int seen = futex_turn;
            if (seen == me) {
                break;
            }
            long waited = sys_futex(&futex_turn, FUTEX_WAIT, seen, FUTEX_PATIENCE_MS);
            if (waited == -2 && futex_turn == me) {
                __sync_fetch_and_add(&futex_lost, 1);
            }
        }
        futex_turn = me ^ 1u;
        sys_futex(&futex_turn, FUTEX_WAKE, 1, 0);
    }
}

static void *futex_partner(void *arg) {
    (void)arg;
    futex_player(1);
    return 0;
}

static int futex_mode(void) {
    futex_turn = 0;
    pthread_t partner;
    if (pthread_create(&partner, 0, futex_partner, 0) != 0) {
        return 50;
    }
    futex_player(0);
    pthread_join(partner, 0);
    printf("forktest: %d futex hand-overs each way, %d wake(s) lost\n", FUTEX_ROUNDS,
           futex_lost);
    return futex_lost != 0 ? 51 : 0;
}

/* M197: a page of this program's own data that nothing touches before the
   fork, so that after it the page is copy-on-write and has never been
   written - the state a kernel that writes a result into it has to cope
   with. Chromium's static pipe descriptors were exactly this. */
static int written_by_the_kernel_after_fork[PAGE / sizeof(int)] __attribute__((aligned(PAGE)));

static int stuck_pipe[2];

static void *block_forever(void *arg) {
    char byte;
    (void)read(stuck_pipe[0], &byte, 1);
    return arg;
}

static void *sweep_trigger(void *arg) {
    return arg;
}

/* M225: every thread of a process exiting at once, on every processor,
   while the next batch of threads is being made. A process's descriptor
   table is shared by all its threads and counted: each new thread takes a
   reference, each exit drops one, and the last drop closes every descriptor
   in it. The count was a plain read-modify-write, so two processors at once
   lost one of the two writes - an eight-processor battery panicked on the
   double free that follows a lost reference ("[heap] second kfree of ...
   from descriptor_table_release"), and a lost release is a table nobody
   ever closes.

   What is graded is what a program can see. Each round forks a child that
   holds the only write end of a pipe; the child runs its batches, writes one
   byte (a table closed early would have taken the write end with it), and
   exits. When wait() says it has ended, its last thread has gone - and its
   table must be closed by then: the parent must read that one byte and then
   end-of-file at once. A table that was never closed still holds the write
   end, and the read says EAGAIN. A table closed twice panics the machine. */
#define STORM_ROUNDS 24
#define STORM_BATCHES 12
#define STORM_THREADS 8

static void *storm_thread(void *arg) {
    volatile int *go = (volatile int *)arg;
    while (!__atomic_load_n(go, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
    return 0;
}

static int storm_child(int write_end) {
    static int go[STORM_BATCHES];
    static pthread_t threads[STORM_BATCHES][STORM_THREADS];
    for (int batch = 0; batch < STORM_BATCHES; batch++) {
        for (int i = 0; i < STORM_THREADS; i++) {
            if (pthread_create(&threads[batch][i], 0, storm_thread, &go[batch]) != 0) {
                return 70;
            }
        }
        /* This batch leaves together; the previous one is still leaving as
           this one was made, and is joined only now. */
        __atomic_store_n(&go[batch], 1, __ATOMIC_RELEASE);
        if (batch > 0) {
            for (int i = 0; i < STORM_THREADS; i++) {
                pthread_join(threads[batch - 1][i], 0);
            }
        }
    }
    for (int i = 0; i < STORM_THREADS; i++) {
        pthread_join(threads[STORM_BATCHES - 1][i], 0);
    }
    return write(write_end, "x", 1) == 1 ? 0 : 71;
}

static int exitstorm_mode(void) {
    for (int round = 0; round < STORM_ROUNDS; round++) {
        int ends[2];
        if (pipe(ends) != 0) {
            return 72;
        }
        pid_t child = fork();
        if (child < 0) {
            return 73;
        }
        if (child == 0) {
            close(ends[0]);
            _exit(storm_child(ends[1]));
        }
        close(ends[1]);
        int status = 0;
        if (waitpid(child, &status, 0) != child) {
            printf("forktest: exitstorm round %d - waitpid failed (errno %d)\n", round, errno);
            return 74;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("forktest: exitstorm round %d - the child ended with status 0x%x "
                   "(71: its own write end was already closed)\n", round, status);
            return 75;
        }
        int flags = fcntl(ends[0], F_GETFL);
        if (flags < 0 || fcntl(ends[0], F_SETFL, flags | O_NONBLOCK) != 0) {
            return 76;
        }
        char byte = 0;
        ssize_t first = read(ends[0], &byte, 1);
        ssize_t second = read(ends[0], &byte, 1);
        int second_errno = errno;
        close(ends[0]);
        if (first != 1) {
            printf("forktest: exitstorm round %d - the child's byte never arrived (%ld)\n",
                   round, (long)first);
            return 77;
        }
        if (second != 0) {
            printf("forktest: exitstorm round %d - the child has ended and its write end "
                   "is still open (read %ld, errno %d): its descriptor table was never "
                   "closed\n", round, (long)second, second_errno);
            return 78;
        }
    }
    printf("forktest: exitstorm - %d processes x %d batches of %d threads leaving at once, "
           "every descriptor table closed exactly when its process ended\n",
           STORM_ROUNDS, STORM_BATCHES, STORM_THREADS);
    return 0;
}

/* M225: record locks (fcntl F_SETLKW) between processes on every processor.
   The kernel's lock table had no lock of its own: an unlock wakes every
   waiter at once, they retry together on different processors, and two of
   them could both find the range free and both be told they hold the write
   lock. Each worker here takes the write lock, reads a counter out of the
   file, adds one and writes it back - a lost update is two holders at once. */
#define LOCK_WORKERS 4
#define LOCK_ROUNDS 150
#define LOCK_FILE "/tmp/forktest.locks"

static int lock_range(int fd, short type) {
    struct flock request;
    memset(&request, 0, sizeof(request));
    request.l_type = type;
    request.l_whence = SEEK_SET;
    request.l_start = 0;
    request.l_len = 1;
    return fcntl(fd, type == F_UNLCK ? F_SETLK : F_SETLKW, &request);
}

static int lock_worker(void) {
    int fd = open(LOCK_FILE, O_RDWR);
    if (fd < 0) {
        return 80;
    }
    for (int i = 0; i < LOCK_ROUNDS; i++) {
        if (lock_range(fd, F_WRLCK) != 0) {
            return 81;
        }
        long value = 0;
        if (pread(fd, &value, sizeof(value), 0) != (ssize_t)sizeof(value)) {
            return 82;
        }
        value++;
        sched_yield();
        if (pwrite(fd, &value, sizeof(value), 0) != (ssize_t)sizeof(value)) {
            return 83;
        }
        if (lock_range(fd, F_UNLCK) != 0) {
            return 84;
        }
    }
    close(fd);
    return 0;
}

static int recordlocks_mode(void) {
    int fd = open(LOCK_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return 85;
    }
    long zero = 0;
    if (pwrite(fd, &zero, sizeof(zero), 0) != (ssize_t)sizeof(zero)) {
        return 86;
    }
    pid_t workers[LOCK_WORKERS];
    for (int w = 0; w < LOCK_WORKERS; w++) {
        workers[w] = fork();
        if (workers[w] < 0) {
            return 87;
        }
        if (workers[w] == 0) {
            _exit(lock_worker());
        }
    }
    int failed = 0;
    for (int w = 0; w < LOCK_WORKERS; w++) {
        int status = 0;
        if (waitpid(workers[w], &status, 0) != workers[w] || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            printf("forktest: recordlocks - worker %d ended with status 0x%x\n", w, status);
            failed = 1;
        }
    }
    long total = 0;
    if (pread(fd, &total, sizeof(total), 0) != (ssize_t)sizeof(total)) {
        return 88;
    }
    close(fd);
    unlink(LOCK_FILE);
    printf("forktest: recordlocks - %d processes x %d increments under F_SETLKW, total %ld "
           "of %d\n", LOCK_WORKERS, LOCK_ROUNDS, total, LOCK_WORKERS * LOCK_ROUNDS);
    if (failed) {
        return 89;
    }
    return total == (long)LOCK_WORKERS * LOCK_ROUNDS ? 0 : 90;
}

/* Several programs writing whole lines to the console at once, one write()
   per line, for the kernel's [logwrite] self-test to read back out of the log
   and require every one of intact. Until sys_write staged a write and sent it
   out under one hold of the log's lock, the lock was taken per CHARACTER, and
   on eight cores a battery marker was printed and never found because its
   '[' had been spliced into another program's line.

   The children wait on a pipe until all of them exist, so they write at the
   same moment rather than one after another as they are forked - on one core
   they take turns, on several they do not. What a line says is a function of
   (writer, line) that kernel.c's grader computes as well; change both or
   neither. */
#define LOGLINE_PAYLOAD 48

static int logline_format(char *out, int writer, int line) {
    static const char head[] = "[logwrite] writer ";
    int n = 0;
    for (int i = 0; head[i]; i++) {
        out[n++] = head[i];
    }
    out[n++] = (char)('0' + writer % 10);
    static const char mid[] = " line ";
    for (int i = 0; mid[i]; i++) {
        out[n++] = mid[i];
    }
    out[n++] = (char)('0' + (line / 100) % 10);
    out[n++] = (char)('0' + (line / 10) % 10);
    out[n++] = (char)('0' + line % 10);
    out[n++] = ' ';
    for (int j = 0; j < LOGLINE_PAYLOAD; j++) {
        out[n++] = (char)('a' + (writer * 7 + line * 3 + j) % 26);
    }
    out[n++] = '\n';
    return n;
}

static int loglines_mode(int writers, int lines) {
    if (writers < 1 || writers > 9 || lines < 1 || lines > 999) {
        return 2;
    }
    int start[2];
    if (pipe(start) != 0) {
        return 2;
    }
    pid_t kids[9];
    for (int w = 0; w < writers; w++) {
        kids[w] = fork();
        if (kids[w] < 0) {
            return 2;
        }
        if (kids[w] == 0) {
            close(start[1]);
            char go;
            if (read(start[0], &go, 1) != 1) {
                sys_exit(3);
            }
            char text[96];
            for (int l = 0; l < lines; l++) {
                int n = logline_format(text, w, l);
                if (sys_write(1, text, (size_t)n) != n) {
                    sys_exit(4);
                }
            }
            sys_exit(0);
        }
    }
    close(start[0]);
    char go[9] = {0};
    if (write(start[1], go, (size_t)writers) != writers) {
        return 2;
    }
    int failed = 0;
    for (int w = 0; w < writers; w++) {
        if (sys_wait(kids[w]) != 0) {
            failed = 1;
        }
    }
    close(start[1]);
    return failed ? 5 : 0;
}

/* M225: one descriptor, closed, dup'd, dup2'd and forked by sibling threads
   at the same instant, on every processor. A process's descriptor table is
   shared by its threads, and the kernel's close looked at a slot and
   released what was in it with no claim on the slot: two threads closing
   one descriptor at once both released it, so a pipe end, a unix socket or
   an eventfd lost a reference another descriptor still counted on - the
   pipe clamps at zero and its reader sees end-of-file early; the socket and
   the eventfd are freed twice. dup, dup2 and fork copied a slot and took
   their reference afterwards, which a sibling's close could get in front of.

   Each round makes one object - a pipe, a socketpair or an eventfd, in turn
   - and four racers, released together, do two close()s, one dup() and one
   dup2() of the same descriptor (every fourth round the dup2 is a fork whose
   child exits at once). Then the program grades what it can see from the
   OUTSIDE: exactly one of the two closes may succeed; the object must be
   alive exactly as long as some descriptor still names it - each survivor
   writes a byte the other end must read back, and the other end must see
   end-of-file after the last survivor closes and not one close before it. A
   reference freed twice takes the machine down, which the harness sees.

   Then the memfd half of the milestone: an mmap of a memfd that the kernel
   refuses must give back the reference it took, or the memfd and every page
   of it outlives its last descriptor - measured in free frames, with
   eight 4 MiB memfds refused two ways each. */
#define SLOT_RACERS 4
#define SLOT_ROUNDS 1500
#define SLOT_DUP2_BASE 900

static volatile int slot_go;      /* generation of the start line */
static volatile int slot_arrived;
static volatile int slot_done;
static volatile int slot_over;
static volatile int slot_target;
static volatile int slot_round;
static volatile int slot_result[SLOT_RACERS];
static volatile int slot_forked[SLOT_RACERS];

static void slot_wait_for(volatile int *word, int value) {
    for (unsigned spins = 0; __atomic_load_n(word, __ATOMIC_ACQUIRE) != value; spins++) {
        if (spins > 2000) {
            sched_yield();
        }
    }
}

enum { SLOT_CLOSE, SLOT_DUP, SLOT_DUP2, SLOT_FORK };

static int slot_op(int me, int round) {
    int op = (me + round) % 4; /* two closers, one dup, one dup2 - by turns */
    if (op == 3) {
        return (round % 4 == 3) ? SLOT_FORK : SLOT_DUP2;
    }
    return op == 2 ? SLOT_DUP : SLOT_CLOSE;
}

static void *slot_racer(void *arg) {
    int me = (int)(long)arg;
    int seen = 0;
    for (;;) {
        slot_wait_for(&slot_go, seen + 1);
        seen++;
        if (__atomic_load_n(&slot_over, __ATOMIC_ACQUIRE)) {
            break;
        }
        int fd = slot_target;
        int result = -1;
        switch (slot_op(me, slot_round)) {
        case SLOT_CLOSE:
            result = close(fd);
            break;
        case SLOT_DUP:
            result = dup(fd);
            break;
        case SLOT_DUP2:
            result = dup2(fd, SLOT_DUP2_BASE + me);
            break;
        default: {
            pid_t child = fork();
            if (child == 0) {
                _exit(0);
            }
            slot_forked[me] = (int)child;
            break;
        }
        }
        slot_result[me] = result;
        __atomic_add_fetch(&slot_done, 1, __ATOMIC_ACQ_REL);
    }
    return 0;
}

/* One byte through `fd`, which must come out of `observer`. */
static int slot_still_this_object(int fd, int observer, int kind, int tag) {
    if (kind == 2) {
        eventfd_t v = (eventfd_t)tag;
        eventfd_t got = 0;
        return eventfd_write(fd, v) == 0 && eventfd_read(fd, &got) == 0 && got == v;
    }
    char byte = (char)tag;
    char back = 0;
    return write(fd, &byte, 1) == 1 && read(observer, &back, 1) == 1 && back == byte;
}

/* 1 when the other end has seen end-of-file, 0 when it is open and empty. */
static int slot_ended(int observer) {
    char byte;
    errno = 0;
    ssize_t n = read(observer, &byte, 1);
    if (n == 0) {
        return 1;
    }
    if (n < 0 && errno == EAGAIN) {
        return 0;
    }
    return -1;
}

static int slot_memfd_half(void) {
    os_meminfo_t before, after;
    if (sys_meminfo(&before) != 0) {
        return 120;
    }
    for (int i = 0; i < 8; i++) {
        int fd = memfd_create("fdslots", 0);
        if (fd < 0 || ftruncate(fd, 4L * 1024 * 1024) != 0) {
            return 121;
        }
        /* An address that is not a page boundary, MAP_FIXED - refused after
           the reference is taken; and a protection bit that does not exist -
           refused after it too. Both used to keep it. */
        if (mmap((void *)0x60000123UL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                 fd, 0) != MAP_FAILED ||
            mmap(0, PAGE, PROT_READ | 0x40, MAP_SHARED, fd, 0) != MAP_FAILED) {
            return 122;
        }
        close(fd);
    }
    if (sys_meminfo(&after) != 0) {
        return 120;
    }
    long lost = (long)before.free_frames - (long)after.free_frames;
    printf("forktest: fdslots - 8 memfds of 4 MiB, each refused an mmap twice and closed: "
           "%ld frame(s) not given back (a memfd kept alive is 1024)\n", lost);
    /* Half of one memfd: other tasks may allocate a little meanwhile, and a
       memfd one kept reference holds alive is 1024 frames. On the kernel
       before this fix, 8192 (all eight) on one core. */
    return lost > 512 ? 123 : 0;
}

static int fdslots_mode(void) {
    pthread_t racers[SLOT_RACERS];
    for (long i = 0; i < SLOT_RACERS; i++) {
        if (pthread_create(&racers[i], 0, slot_racer, (void *)i) != 0) {
            return 100;
        }
    }
    int failure = 0;
    int both_closed = 0, early_end = 0, late_end = 0, wrong_object = 0;
    for (int round = 0; round < SLOT_ROUNDS && !failure; round++) {
        int kind = round % 3; /* 0 pipe, 1 unix socket, 2 eventfd */
        int ends[2] = {-1, -1};
        if (kind == 0 && pipe(ends) == 0) {
            int t = ends[0];
            ends[0] = ends[1]; /* the raced descriptor is the WRITE end */
            ends[1] = t;
        } else if (kind == 1 && socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) {
            ends[0] = -1;
        } else if (kind == 2) {
            ends[0] = eventfd(0, 0);
        }
        if (ends[0] < 0) {
            failure = 101;
            break;
        }
        if (ends[1] >= 0) {
            int flags = fcntl(ends[1], F_GETFL);
            if (flags < 0 || fcntl(ends[1], F_SETFL, flags | O_NONBLOCK) != 0) {
                failure = 102;
                break;
            }
        }
        slot_target = ends[0];
        slot_round = round;
        for (int i = 0; i < SLOT_RACERS; i++) {
            slot_result[i] = -9;
            slot_forked[i] = 0;
        }
        __atomic_store_n(&slot_done, 0, __ATOMIC_RELEASE);
        __atomic_add_fetch(&slot_go, 1, __ATOMIC_ACQ_REL);
        slot_wait_for(&slot_done, SLOT_RACERS);

        int closes = 0;
        int survivors[SLOT_RACERS + 1];
        int nsurvivors = 0;
        for (int i = 0; i < SLOT_RACERS; i++) {
            int op = slot_op(i, round);
            if (op == SLOT_CLOSE && slot_result[i] == 0) {
                closes++;
            } else if ((op == SLOT_DUP || op == SLOT_DUP2) && slot_result[i] >= 0) {
                survivors[nsurvivors++] = slot_result[i];
            } else if (op == SLOT_FORK && slot_forked[i] > 0) {
                int status = 0;
                waitpid(slot_forked[i], &status, 0);
            }
        }
        if (closes != 1) {
            /* Nobody else closes it, so one of the two closes succeeds. */
            both_closed = closes;
            failure = 103;
            printf("forktest: fdslots round %d - %d of 2 simultaneous close(%d)s succeeded\n",
                   round, closes, ends[0]);
            break;
        }
        for (int s = 0; s < nsurvivors && !failure; s++) {
            if (kind != 2 && slot_ended(ends[1]) != 0) {
                early_end = 1;
                failure = 104;
                printf("forktest: fdslots round %d - the %s ended while %d descriptor(s) "
                       "still named it\n", round, kind == 0 ? "pipe" : "socket",
                       nsurvivors - s);
                break;
            }
            if (!slot_still_this_object(survivors[s], ends[1], kind, 'a' + s)) {
                wrong_object = 1;
                failure = 105;
                printf("forktest: fdslots round %d - descriptor %d does not reach the object "
                       "it was duplicated from (errno %d)\n", round, survivors[s], errno);
                break;
            }
            close(survivors[s]);
        }
        if (!failure && kind != 2 && slot_ended(ends[1]) != 1) {
            late_end = 1;
            failure = 106;
            printf("forktest: fdslots round %d - every descriptor of the %s is closed and "
                   "the other end has not seen it end\n", round,
                   kind == 0 ? "pipe" : "socket");
        }
        if (ends[1] >= 0) {
            close(ends[1]);
        }
    }
    __atomic_store_n(&slot_over, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&slot_go, 1, __ATOMIC_ACQ_REL);
    for (int i = 0; i < SLOT_RACERS; i++) {
        pthread_join(racers[i], 0);
    }
    if (failure) {
        (void)both_closed; (void)early_end; (void)late_end; (void)wrong_object;
        return failure;
    }
    printf("forktest: fdslots - %d rounds of %d threads closing, dup'ing, dup2'ing and forking "
           "one descriptor at once (pipes, unix sockets, eventfds): one close each, and every "
           "object alive exactly until its last descriptor closed\n", SLOT_ROUNDS, SLOT_RACERS);
    return slot_memfd_half();
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1] && strcmp(argv[1], "fdslots") == 0) {
        return fdslots_mode();
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "fduse") == 0) {
        return fduse_mode();
    }
    if (argc > 2 && argv[1] && strcmp(argv[1], "execenv") == 0) {
        return execenv_stage(atoi(argv[2]));
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "exitstorm") == 0) {
        return exitstorm_mode();
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "recordlocks") == 0) {
        return recordlocks_mode();
    }
    if (argc > 3 && argv[1] && strcmp(argv[1], "loglines") == 0) {
        return loglines_mode(atoi(argv[2]), atoi(argv[3]));
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "futex") == 0) {
        return futex_mode();
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "descriptors") == 0) {
        return descriptors_mode();
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "cow") == 0) {
        return cow_mode();
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "threads") == 0) {
        return threads_mode();
    }
    if (argc > 1 && argv[1] && strcmp(argv[1], "threadfork") == 0) {
        return thread_fork_mode();
    }

    before_fork = 0x5A5A;

    static volatile char heap_marker[PAGE];
    heap_marker[0] = 'P';

    volatile char *arena = (volatile char *)mmap(0, 4 * PAGE, PROT_READ | PROT_WRITE,
                                                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (arena == (volatile char *)MAP_FAILED) {
        return 2;
    }
    arena[0] = 'P';
    arena[2 * PAGE] = 'P';

    int file_descriptors[2];
    if (pipe(file_descriptors) != 0) {
        return 2;
    }

    long parent_pid = sys_getpid();
    pid_t kid = fork();
    if (kid < 0) {
        return 2;
    }

    if (kid == 0) {
        int rc = 0;
        if (sys_getpid() == parent_pid) {
            rc = 3;
        } else if (before_fork != 0x5A5A || heap_marker[0] != 'P' ||
                   arena[0] != 'P' || arena[2 * PAGE] != 'P') {
            rc = 4;
        } else {
            before_fork = 0xC3C3;
            heap_marker[0] = 'C';
            arena[0] = 'C';
            arena[2 * PAGE] = 'C';
            const char message[] = "child";
            if (write(file_descriptors[1], message, sizeof(message)) != (long)sizeof(message)) {
                rc = 7;
            }
        }
        sys_exit(rc);
    }

    long child_rc = sys_wait(kid);
    if (child_rc != 0) {
        return (int)child_rc;
    }

    char got[16];
    memset(got, 0, sizeof(got));
    if (read(file_descriptors[0], got, sizeof(got)) <= 0 || strcmp(got, "child") != 0) {
        return 7;
    }

    if (before_fork != 0x5A5A || heap_marker[0] != 'P') {
        return 5;
    }
    if (pipe(written_by_the_kernel_after_fork) != 0) {
        printf("forktest: pipe() refused to write its descriptors into a copy-on-write page "
               "of this program's own data\n");
        return 12;
    }
    close(written_by_the_kernel_after_fork[0]);
    close(written_by_the_kernel_after_fork[1]);
    if (arena[0] != 'P' || arena[2 * PAGE] != 'P') {
        return 6;
    }

    before_fork = 0x1234;
    heap_marker[0] = 'Q';
    arena[0] = 'Q';
    if (before_fork != 0x1234 || heap_marker[0] != 'Q' || arena[0] != 'Q') {
        return 5;
    }

    close(file_descriptors[0]);
    close(file_descriptors[1]);
    munmap((void *)arena, 4 * PAGE);

    for (int round = 0; round < 100; round++) {
        pid_t k = fork();
        if (k < 0) {
            printf("forktest: fork failed on round %d\n", round);
            return 9;
        }
        if (k == 0) {
            sys_exit((round % 100) + 1);
        }
        long rc = sys_wait(k);
        if (rc != (round % 100) + 1) {
            printf("forktest: round %d waited for %d and got %ld\n",
                   round, (round % 100) + 1, rc);
            return 8;
        }
    }

    {
        pid_t a_pid = fork();
        if (a_pid < 0) {
            return 9;
        }
        if (a_pid == 0) {
            sys_setpgid(0, 0);
            for (volatile int i = 0; i < 200000; i++) { }
            sys_exit(41);
        }
        sys_setpgid(a_pid, a_pid);

        pid_t b_pid = fork();
        if (b_pid < 0) {
            return 9;
        }
        if (b_pid == 0) {
            sys_setpgid(0, a_pid);
            for (volatile int i = 0; i < 400000; i++) { }
            sys_exit(42);
        }
        sys_setpgid(b_pid, a_pid);

        pid_t mine = fork();
        if (mine < 0) {
            return 9;
        }
        if (mine == 0) {
            sys_exit(43);
        }

        int seen_a = 0, seen_b = 0;
        for (int i = 0; i < 2; i++) {
            int st = 0;
            pid_t got = waitpid(-a_pid, &st, 0);
            if (got < 0) {
                printf("forktest: waitpid(-%d) found nothing\n", (int)a_pid);
                return 10;
            }
            if (got == mine) {
                printf("forktest: waitpid(-%d) returned %d, which is in our "
                       "own group\n", (int)a_pid, (int)got);
                return 11;
            }
            if (got == a_pid) {
                seen_a = 1;
            } else if (got == b_pid) {
                seen_b = 1;
            } else {
                return 11;
            }
        }
        if (!seen_a || !seen_b) {
            return 10;
        }

        if (waitpid(-a_pid, NULL, 0) >= 0) {
            return 13;
        }

        int st = 0;
        pid_t got = waitpid(0, &st, 0);
        if (got != mine) {
            printf("forktest: waitpid(0) returned %d, wanted %d\n",
                   (int)got, (int)mine);
            return 12;
        }
    }

    {
        pid_t first = fork();
        if (first == 0) {
            _exit(7);
        }
        if (first < 0) {
            return 14;
        }
        int st = 0;
        while (kill(first, 0) == 0) {
            sched_yield();
        }
        pthread_t sweeper;
        if (pthread_create(&sweeper, NULL, sweep_trigger, NULL) != 0 ||
            pthread_join(sweeper, NULL) != 0) {
            return 14;
        }
        if (waitpid(first, &st, 0) != first || !WIFEXITED(st) || WEXITSTATUS(st) != 7) {
            printf("forktest: a child that exited before a thread was started "
                   "could not be waited for - the spawn's sweep took its status\n");
            return 14;
        }
        errno = 0;
        if (kill(first, 0) != -1 || errno != ESRCH) {
            printf("forktest: kill of a reaped child said errno %d, not ESRCH\n", errno);
            return 15;
        }
    }

    {
        int link[2];
        if (pipe(link) != 0) {
            return 16;
        }
        pid_t middle = fork();
        if (middle == 0) {
            pid_t spinner = fork();
            if (spinner == 0) {
                for (;;) {
                    sched_yield();
                }
            }
            (void)write(link[1], &spinner, sizeof(spinner));
            _exit(0);
        }
        pid_t spinner = -1;
        if (middle < 0 || read(link[0], &spinner, sizeof(spinner)) != sizeof(spinner) ||
            waitpid(middle, NULL, 0) != middle) {
            return 16;
        }
        close(link[0]);
        close(link[1]);
        if (kill(spinner, SIGKILL) != 0) {
            printf("forktest: a grandchild whose parent had been reaped could not be "
                   "killed by the process that started it (errno %d)\n", errno);
            return 16;
        }
    }

    {
        int held[2];
        if (pipe(held) != 0 || pipe(stuck_pipe) != 0) {
            return 17;
        }
        pid_t victim = fork();
        if (victim == 0) {
            close(held[0]);
            pthread_t sleeper;
            if (pthread_create(&sleeper, NULL, block_forever, NULL) != 0) {
                _exit(3);
            }
            for (;;) {
                pause();
            }
        }
        close(held[1]);
        if (victim < 0) {
            return 17;
        }
        struct timespec settle = {0, 200000000};
        nanosleep(&settle, NULL);
        kill(victim, SIGTERM);
        int st = 0;
        if (waitpid(victim, &st, 0) != victim || !WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM) {
            return 17;
        }
        struct pollfd hangup = {held[0], POLLIN, 0};
        char byte;
        if (poll(&hangup, 1, 5000) != 1 || read(held[0], &byte, 1) != 0) {
            printf("forktest: SIGTERM ended a process's main thread and left its "
                   "other thread holding the process's descriptors\n");
            return 17;
        }
        close(held[0]);
        close(stuck_pipe[0]);
        close(stuck_pipe[1]);
    }

    printf("forktest: all checks passed\n");
    return 0;
}

/* M225 (fd-use-holds): a descriptor closed by one thread while another is
   blocked USING it - in read() on a unix socket, a pipe, an eventfd or a
   timerfd, or in epoll_wait() on a set. The kernel's read looked the slot up
   at the start and again after every wait, and took no reference of its own:
   the close freed the socket, the timerfd or the epoll set under the
   sleeping reader (which woke into freed memory, or found the slot empty and
   used a null object), and dropped the pipe's last reader so the writer was
   told EPIPE while a reader was still in read().

   What Linux does, and what is graded here: the call that is already using
   the object goes on with it until it returns - close() does not wake it -
   and returns what arrives on the object; the object is let go of when that
   call returns. So for each kind:
     - unix socket, pipe: the close succeeds; a write from the other end
       still succeeds and the blocked read returns that byte; once the read
       has returned, the next write is refused (nothing holds the far end any
       more - the reference the read took was given back);
     - eventfd: another descriptor for it (a dup) writes; the read returns
       that value;
     - timerfd: no other descriptor at all; the timer still expires and the
       read returns its count;
     - epoll: the set's descriptor is closed; an event on a watched eventfd
       still ends the wait with that event.
   A round in which the reader had not yet entered the kernel when the close
   came is told EBADF, which is right too, and is counted as "late" rather
   than graded; each kind must have been caught blocked at least once.

   Then dup2 onto a number another thread has claimed and not yet filled
   (socketpair claims two numbers, copies them out and only then fills
   them): Linux answers EBUSY for that window, and every dup2 that fails
   here must say so. It used to answer a bare -1 with errno untouched. */
#define USE_ROUNDS 40
#define USE_KINDS 5
#define USE_SETTLE_US 4000
#define USE_WAIT_MS 3000

enum { USE_UNIX, USE_PIPE, USE_EVENT, USE_TIMER, USE_EPOLL };
static const char *const use_kind_name[USE_KINDS] = {"unix", "pipe", "eventfd", "timerfd",
                                                     "epoll"};

typedef struct {
    int kind;
    int fd;
    volatile int entering;
    volatile int done;
    long result;
    int error;
    uint64_t value;
    char byte;
    struct epoll_event event;
} use_reader_t;

static void *use_reader(void *arg) {
    use_reader_t *r = (use_reader_t *)arg;
    __atomic_store_n(&r->entering, 1, __ATOMIC_RELEASE);
    errno = 0;
    switch (r->kind) {
    case USE_UNIX:
    case USE_PIPE:
        r->result = read(r->fd, &r->byte, 1);
        break;
    case USE_EVENT:
    case USE_TIMER:
        r->result = read(r->fd, &r->value, sizeof(r->value));
        break;
    default:
        r->result = epoll_wait(r->fd, &r->event, 1, USE_WAIT_MS);
        break;
    }
    r->error = errno;
    __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
    return 0;
}

static int use_wait_done(use_reader_t *r) {
    for (int ms = 0; ms < USE_WAIT_MS + 1000; ms++) {
        if (__atomic_load_n(&r->done, __ATOMIC_ACQUIRE)) {
            return 1;
        }
        usleep(1000);
    }
    return 0;
}

/* One round: 1 held, 0 late, or a failure code (and a line saying why). */
static int use_round(int kind, int round) {
    int ends[2] = {-1, -1};
    int other = -1;
    use_reader_t r;
    memset(&r, 0, sizeof(r));
    r.kind = kind;
    switch (kind) {
    case USE_UNIX:
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) {
            return 130;
        }
        r.fd = ends[0];
        other = ends[1];
        break;
    case USE_PIPE:
        if (pipe(ends) != 0) {
            return 130;
        }
        r.fd = ends[0];
        other = ends[1];
        break;
    case USE_EVENT:
        r.fd = eventfd(0, 0);
        other = r.fd >= 0 ? dup(r.fd) : -1;
        if (other < 0) {
            return 130;
        }
        break;
    case USE_TIMER: {
        r.fd = timerfd_create(CLOCK_MONOTONIC, 0);
        struct itimerspec in;
        memset(&in, 0, sizeof(in));
        in.it_value.tv_nsec = 60L * 1000 * 1000;
        if (r.fd < 0 || timerfd_settime(r.fd, 0, &in, NULL) != 0) {
            return 130;
        }
        break;
    }
    default: {
        r.fd = epoll_create1(0);
        other = eventfd(0, 0);
        struct epoll_event want;
        memset(&want, 0, sizeof(want));
        want.events = EPOLLIN;
        want.data.fd = other;
        if (r.fd < 0 || other < 0 || epoll_ctl(r.fd, EPOLL_CTL_ADD, other, &want) != 0) {
            return 130;
        }
        break;
    }
    }

    pthread_t reader;
    if (pthread_create(&reader, 0, use_reader, &r) != 0) {
        return 131;
    }
    while (!__atomic_load_n(&r.entering, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
    usleep(USE_SETTLE_US);
    int closed = close(r.fd);
    long wrote = 0;
    if (kind == USE_UNIX || kind == USE_PIPE) {
        wrote = write(other, "u", 1);
    } else if (kind == USE_EVENT) {
        wrote = eventfd_write(other, 7) == 0 ? 1 : -1;
    } else if (kind == USE_EPOLL) {
        wrote = eventfd_write(other, 1) == 0 ? 1 : -1;
    }
    if (!use_wait_done(&r)) {
        printf("forktest: fduse round %d - a %s read blocked on a descriptor its sibling "
               "closed never returned\n", round, use_kind_name[kind]);
        return 132;
    }
    pthread_join(reader, 0);

    int outcome = -1;
    if (closed != 0) {
        printf("forktest: fduse round %d - closing the %s a sibling was reading failed "
               "(errno %d)\n", round, use_kind_name[kind], errno);
        outcome = 133;
    } else if (r.result < 0 && r.error == EBADF) {
        outcome = 0; /* the reader came in after the close: not open, as on Linux */
    } else {
        int right = 0;
        switch (kind) {
        case USE_UNIX:
        case USE_PIPE:
            right = wrote == 1 && r.result == 1 && r.byte == 'u';
            break;
        case USE_EVENT:
            right = wrote == 1 && r.result == 8 && r.value == 7;
            break;
        case USE_TIMER:
            right = r.result == 8 && r.value >= 1;
            break;
        default:
            right = r.result == 1 && r.event.data.fd == other;
            break;
        }
        if (!right) {
            printf("forktest: fduse round %d - a %s read whose descriptor a sibling closed "
                   "returned %ld (errno %d, value %lu); the other end's write returned %ld\n",
                   round, use_kind_name[kind], r.result, r.error, (unsigned long)r.value,
                   wrote);
            outcome = 134;
        } else {
            outcome = 1;
        }
    }
    /* The reference the reader's call held is given back when it returns:
       nothing names this end now, so the far end's next write is refused. */
    if (outcome == 1 && (kind == USE_UNIX || kind == USE_PIPE)) {
        if (write(other, "v", 1) >= 0) {
            printf("forktest: fduse round %d - the %s's far end was still writable after "
                   "the last reader returned: its call kept the reference it took\n",
                   round, use_kind_name[kind]);
            outcome = 135;
        }
    }
    if (outcome == 0 && (kind == USE_UNIX || kind == USE_PIPE) && wrote >= 0) {
        printf("forktest: fduse round %d - nothing was reading the %s when its last "
               "descriptor closed, and the far end could still write\n",
               round, use_kind_name[kind]);
        outcome = 136;
    }
    if (other >= 0) {
        close(other);
    }
    return outcome;
}

#define BUSY_ROUNDS 20000

static volatile int busy_over;
static int busy_target;

static void *busy_claimer(void *arg) {
    (void)arg;
    for (int i = 0; i < BUSY_ROUNDS; i++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            close(sv[0]);
            close(sv[1]);
        }
    }
    __atomic_store_n(&busy_over, 1, __ATOMIC_RELEASE);
    return 0;
}

static int dup2_onto_a_slot_being_filled(int *busy_out, long *tries_out) {
    int keep = eventfd(0, 0);
    int probe[2];
    if (keep < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, probe) != 0) {
        return 140;
    }
    busy_target = probe[0]; /* the lowest number the claimer will be given */
    close(probe[0]);
    close(probe[1]);
    busy_over = 0;
    pthread_t claimer;
    if (pthread_create(&claimer, 0, busy_claimer, 0) != 0) {
        return 141;
    }
    int busy = 0;
    int other = 0;
    int other_errno = 0;
    long tries = 0;
    while (!__atomic_load_n(&busy_over, __ATOMIC_ACQUIRE)) {
        errno = 0;
        int r = dup2(keep, busy_target);
        tries++;
        if (r == busy_target) {
            close(busy_target);
        } else if (errno == EBUSY) {
            busy++;
        } else {
            other++;
            other_errno = errno;
        }
    }
    pthread_join(claimer, 0);
    close(keep);
    *busy_out = busy;
    *tries_out = tries;
    if (other != 0) {
        printf("forktest: fduse - %d dup2(s) onto a number another thread was filling "
               "failed with errno %d, not EBUSY\n", other, other_errno);
        return 142;
    }
    return 0;
}

/* What holding a descriptor for a call costs, measured where it is paid:
   one byte written into a pipe and read back, alone (a table only this task
   names: the copy and nothing counted) and with a sibling thread parked in
   the process (the shared table's path: the table's lock and the pipe's
   reference count, both ways). getpid is the trap alone, for scale. Best of
   five batches, in TSC cycles; printed, not graded - tests/budgets.tsv has
   no row for it, and the number to compare is this line before and after. */
#define COST_BATCH 4000
#define COST_BATCHES 5

static uint64_t cost_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t cost_pipe_pair(const int p[2]) {
    uint64_t best = ~0ull;
    char byte = 'c';
    for (int b = 0; b < COST_BATCHES; b++) {
        uint64_t start = cost_tsc();
        for (int i = 0; i < COST_BATCH; i++) {
            if (write(p[1], &byte, 1) != 1 || read(p[0], &byte, 1) != 1) {
                return 0;
            }
        }
        uint64_t per = (cost_tsc() - start) / COST_BATCH;
        if (per < best) {
            best = per;
        }
    }
    return best;
}

static uint64_t cost_getpid(void) {
    uint64_t best = ~0ull;
    for (int b = 0; b < COST_BATCHES; b++) {
        uint64_t start = cost_tsc();
        for (int i = 0; i < COST_BATCH; i++) {
            sys_getpid();
        }
        uint64_t per = (cost_tsc() - start) / COST_BATCH;
        if (per < best) {
            best = per;
        }
    }
    return best;
}

static int cost_park[2];

static void *cost_parked(void *arg) {
    (void)arg;
    char byte;
    return read(cost_park[0], &byte, 1) == 1 ? (void *)0 : (void *)1;
}

static void fduse_cost(void) {
    int p[2];
    if (pipe(p) != 0 || pipe(cost_park) != 0) {
        printf("forktest: fduse cost - no pipe\n");
        return;
    }
    uint64_t trap = cost_getpid();
    uint64_t alone = cost_pipe_pair(p);
    pthread_t sibling;
    uint64_t shared = 0;
    if (pthread_create(&sibling, 0, cost_parked, 0) == 0) {
        usleep(2000);
        shared = cost_pipe_pair(p);
        if (write(cost_park[1], "x", 1) == 1) {
            pthread_join(sibling, 0);
        }
    }
    printf("forktest: fduse cost - one byte through a pipe and back: %lu cycles alone, %lu "
           "with a sibling thread (getpid: %lu)\n",
           (unsigned long)alone, (unsigned long)shared, (unsigned long)trap);
    close(p[0]);
    close(p[1]);
    close(cost_park[0]);
    close(cost_park[1]);
}

/* An exec handed no environment (envp NULL) inherits its process's record,
   which the kernel now copies out under the scheduler lock rather than
   reading the record in place. Stage 1 is exec'd WITH an environment, which
   becomes the process's record; it execs stage 2 with none; stage 2 must
   find what stage 1 was given. Nothing in the battery execs without an
   environment - the C library always passes environ - so this is the
   path's only caller here. */
#define EXECENV_SELF "/bin/forktest"

static int execenv_stage(int stage) {
    if (stage == 1) {
        char *const next[] = {"forktest", "execenv", "2", NULL};
        sys_execve(EXECENV_SELF, next, NULL);
        return 2;
    }
    const char *value = getenv("FDUSE_ENV");
    return value && strcmp(value, "inherited") == 0 ? 0 : 3;
}

static int exec_inherits_its_environment(void) {
    pid_t child = fork();
    if (child < 0) {
        return 150;
    }
    if (child == 0) {
        char *const first[] = {"forktest", "execenv", "1", NULL};
        char *const env[] = {"FDUSE_ENV=inherited", NULL};
        execve(EXECENV_SELF, first, env);
        _exit(4);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("forktest: fduse - an exec given no environment did not inherit its process's "
               "(status 0x%x)\n", status);
        return 151;
    }
    return 0;
}

/* A threaded process waiting on an idle NAMED pipe (sys_pipe_open, which
   is what every window's event pipe is) has to sleep. Counting each use of
   a descriptor made poll's and epoll_wait's own scan wake the poller: a
   named pipe's let-go wakes everybody watching it, the poller was watching
   it, so scheduler_watch_block never slept - a core spun until the timeout,
   and for ever with none, in every idle threaded client watching its
   window's events (Chromium's UI thread). Graded on the waiting thread's
   own processor time, which the kernel counts a tick at a time: an idle
   wait costs a tick or two, the spin all of it. The sibling thread is there
   so the table is shared and a use is the counted kind. */
#define IDLE_WAIT_MS 400
#define IDLE_PIPE_NAME "forktest-idle"

static int idle_park[2];

static void *idle_parked(void *arg) {
    (void)arg;
    char byte;
    return read(idle_park[0], &byte, 1) == 1 ? (void *)0 : (void *)1;
}

static long idle_clock_ms(clockid_t clock) {
    struct timespec ts;
    if (clock_gettime(clock, &ts) != 0) {
        return -1;
    }
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One wait: 0, or a failure code (and a line saying why). */
static int idle_wait_graded(const char *what, int epoll_fd, int fd, long *cpu_out,
                            long *wall_out) {
    long cpu0 = idle_clock_ms(CLOCK_THREAD_CPUTIME_ID);
    long wall0 = idle_clock_ms(CLOCK_MONOTONIC);
    int r;
    if (epoll_fd >= 0) {
        struct epoll_event got;
        r = epoll_wait(epoll_fd, &got, 1, IDLE_WAIT_MS);
    } else {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        r = poll(&pfd, 1, IDLE_WAIT_MS);
    }
    long cpu = idle_clock_ms(CLOCK_THREAD_CPUTIME_ID) - cpu0;
    long wall = idle_clock_ms(CLOCK_MONOTONIC) - wall0;
    *cpu_out = cpu;
    *wall_out = wall;
    if (cpu0 < 0 || wall0 < 0) {
        printf("forktest: fduse idle - no thread processor clock\n");
        return 162;
    }
    if (r != 0) {
        printf("forktest: fduse idle - %s on an idle named pipe returned %d (errno %d), "
               "not a timeout\n", what, r, errno);
        return 163;
    }
    if (wall < IDLE_WAIT_MS - 20) {
        printf("forktest: fduse idle - %s on an idle named pipe came back after %ld ms of "
               "%d\n", what, wall, IDLE_WAIT_MS);
        return 164;
    }
    if (cpu * 4 > wall) {
        printf("forktest: fduse idle - %s on an idle named pipe in a threaded process used "
               "%ld ms of processor time in %ld ms: it never slept\n", what, cpu, wall);
        return 165;
    }
    return 0;
}

static int idle_waits_sleep(long cpu[2], long wall[2]) {
    int named[2];
    if (sys_pipe_open(IDLE_PIPE_NAME, named) != 0) {
        printf("forktest: fduse idle - no named pipe\n");
        return 160;
    }
    int ep = epoll_create1(0);
    struct epoll_event want;
    memset(&want, 0, sizeof(want));
    want.events = EPOLLIN;
    want.data.fd = named[0];
    if (pipe(idle_park) != 0 || ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, named[0], &want) != 0) {
        printf("forktest: fduse idle - no epoll set or parking pipe\n");
        return 160;
    }
    pthread_t sibling;
    if (pthread_create(&sibling, 0, idle_parked, 0) != 0) {
        return 161;
    }
    usleep(2000);
    int r = idle_wait_graded("poll", -1, named[0], &cpu[0], &wall[0]);
    if (r == 0) {
        r = idle_wait_graded("epoll_wait", ep, named[0], &cpu[1], &wall[1]);
    }
    if (write(idle_park[1], "x", 1) == 1) {
        pthread_join(sibling, 0);
    }
    close(ep);
    close(named[0]);
    close(named[1]);
    close(idle_park[0]);
    close(idle_park[1]);
    return r;
}

static int fduse_mode(void) {
    fduse_cost();
    int held[USE_KINDS] = {0};
    int late[USE_KINDS] = {0};
    for (int round = 0; round < USE_ROUNDS; round++) {
        for (int kind = 0; kind < USE_KINDS; kind++) {
            int outcome = use_round(kind, round);
            if (outcome == 1) {
                held[kind]++;
            } else if (outcome == 0) {
                late[kind]++;
            } else {
                return outcome;
            }
        }
    }
    for (int kind = 0; kind < USE_KINDS; kind++) {
        if (held[kind] == 0) {
            printf("forktest: fduse - the %s reader was never caught blocked in %d rounds; "
                   "nothing was graded\n", use_kind_name[kind], USE_ROUNDS);
            return 137;
        }
    }
    int busy = 0;
    long tries = 0;
    int r = dup2_onto_a_slot_being_filled(&busy, &tries);
    if (r != 0) {
        return r;
    }
    /* Probabilistic, but not by much: 4,400 to 13,000 hits a run on 4 and 8
       processors. On one, the claimer would have to be preempted inside its
       claim, so a miss there is not a failure. */
    if (busy == 0 && sysconf(_SC_NPROCESSORS_ONLN) > 1) {
        printf("forktest: fduse - dup2 never found a number another thread was filling in "
               "%ld tries; EBUSY was not graded\n", tries);
        return 143;
    }
    r = exec_inherits_its_environment();
    if (r != 0) {
        return r;
    }
    long idle_cpu[2] = {0, 0};
    long idle_wall[2] = {0, 0};
    r = idle_waits_sleep(idle_cpu, idle_wall);
    if (r != 0) {
        return r;
    }
    printf("forktest: fduse - %d rounds of each: a read or epoll_wait blocked on a descriptor "
           "its sibling closed went on with the object it started with (caught blocked/late: "
           "unix %d/%d, pipe %d/%d, eventfd %d/%d, timerfd %d/%d, epoll %d/%d); dup2 onto a "
           "number being filled: %d EBUSY in %ld tries, nothing else; an exec given no "
           "environment inherited its process's; idle named pipe slept through poll %ld/%ld "
           "ms and epoll_wait %ld/%ld ms (processor/wall)\n",
           USE_ROUNDS, held[USE_UNIX], late[USE_UNIX], held[USE_PIPE], late[USE_PIPE],
           held[USE_EVENT], late[USE_EVENT], held[USE_TIMER], late[USE_TIMER],
           held[USE_EPOLL], late[USE_EPOLL], busy, tries, idle_cpu[0], idle_wall[0],
           idle_cpu[1], idle_wall[1]);
    return 0;
}
