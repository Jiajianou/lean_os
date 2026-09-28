#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
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
    return descriptor_failures != 0 ? 42 : 0;
}

int main(int argc, char **argv) {
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

    printf("forktest: all checks passed\n");
    return 0;
}
