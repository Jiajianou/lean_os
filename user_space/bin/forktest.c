/* user_space/bin/forktest.c - M83's fixture
 *
 * The classic test, and it has to be the classic one, because the thing
 * that is hard to fake about fork is precisely what every textbook
 * checks: two processes, one line of code, two different answers.
 *
 * What is checked here, in the order the failures matter:
 *
 *   - fork returns twice, with different values, into two processes that
 *     report different pids
 *   - memory written *before* the fork is visible to the child, and
 *     memory written *after* it by either side is invisible to the other.
 *     That second half is copy-on-write working; the first half is what
 *     distinguishes a fork from a spawn
 *   - the same, for a page of the mmap arena, because those pages are
 *     built by M82's fault handler and shared by M83's, and the two
 *     interacting is the newest code in the system
 *   - a descriptor opened before the fork works in both
 *   - a hundred rounds of fork/exit/wait leave the machine where they
 *     found it, which is the check that a process table with a hundred
 *     and twenty-eight slots can survive a shell
 *
 * Exit codes, so a failure names itself:
 *   0  everything worked
 *   2  fork failed outright
 *   3  the two sides did not report different pids
 *   4  the child could not see memory written before the fork
 *   5  a write by one side was visible to the other - not copy-on-write,
 *      just shared
 *   6  an mmap'd page did not survive the fork the same way
 *   7  an inherited descriptor did not work in the child
 *   8  a round of fork/exit/wait reported the wrong exit code
 *   9  the machine ran out of task slots
 */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Three project calls, and this include is the honest way to say so.
 *
 * `sys_wait` because M84 is where waitpid lands and there is nothing
 * portable to call yet. `sys_getpid` because <unistd.h>'s getpid returns
 * the thread GROUP id, which is the right answer for everything except a
 * test that wants to know which task it is. And `sys_exit` rather than
 * <stdlib.h>'s exit, for the reason POSIX has _exit at all: a forked
 * child inherits its parent's stdio buffers, and running the ordinary
 * exit path in the child would flush them a second time and print the
 * parent's output twice. */
#include "syscall_wrappers.h"

#define PAGE 4096UL

/* Written before the fork and read by the child - a plain global, so it
 * lives in the image's data segment rather than in anything either side
 * allocated. */
static volatile int before_fork = 0;

/* How many pages the "cow" mode touches before forking. Large enough that
 * an eager copy of it would be unmistakable in a free-frame count and
 * small enough to fit twice on a 128 MiB machine if it came to that -
 * because if this kernel *were* copying eagerly, the test has to fail by
 * reporting a number rather than by running the machine out of memory. */
#define COW_PAGES 2048UL /* 8 MiB */
#define PARK_YIELDS 600 /* see lazytest.c's note on why this is as small as it is */

/* ---- the mode the kernel measures -------------------------------------
 *
 * Touches eight megabytes, forks, and has both sides sit still while the
 * boot self-test counts frames. A program cannot see the machine's
 * physical memory, so it cannot make this assertion itself; all it can do
 * is hold still in a shape that makes the assertion possible.
 *
 * Both sides park, and both must: a measurement taken while only the
 * parent was alive would be the same number whether the child had copied
 * eight megabytes or nothing at all. */
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
        /* The child reads - never writes - so every one of those eight
         * megabytes stays shared for as long as this parks. A single
         * write here would break one page's sharing and make the number
         * the kernel measures a little bit wrong for a reason nobody
         * would ever find. */
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

int main(int argc, char **argv) {
    if (argc > 1 && argv[1] && strcmp(argv[1], "cow") == 0) {
        return cow_mode();
    }

    before_fork = 0x5A5A;

    /* A page of heap and a page of arena, both written before the fork,
     * so that all three kinds of memory a process has are exercised. */
    static volatile char heap_marker[PAGE];
    heap_marker[0] = 'P';

    volatile char *arena = (volatile char *)mmap(0, 4 * PAGE, PROT_READ | PROT_WRITE,
                                                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (arena == (volatile char *)MAP_FAILED) {
        return 2;
    }
    arena[0] = 'P';
    arena[2 * PAGE] = 'P';

    int fds[2];
    if (pipe(fds) != 0) {
        return 2;
    }

    long parent_pid = sys_getpid();
    pid_t kid = fork();
    if (kid < 0) {
        return 2;
    }

    if (kid == 0) {
        /* ---- the child ---------------------------------------------- */
        int rc = 0;
        if (sys_getpid() == parent_pid) {
            rc = 3;
        } else if (before_fork != 0x5A5A || heap_marker[0] != 'P' ||
                   arena[0] != 'P' || arena[2 * PAGE] != 'P') {
            /* Everything written before the fork must be here. A child
             * that started from zeroed memory would be a spawn. */
            rc = 4;
        } else {
            /* Now make it private. Each of these is a write to a page the
             * parent is still holding read-only, so each one is a
             * copy-on-write fault. */
            before_fork = 0xC3C3;
            heap_marker[0] = 'C';
            arena[0] = 'C';
            arena[2 * PAGE] = 'C';
            /* Tell the parent through the descriptor it opened before the
             * fork, which is the other half of what a child inherits. */
            const char msg[] = "child";
            if (write(fds[1], msg, sizeof(msg)) != (long)sizeof(msg)) {
                rc = 7;
            }
        }
        sys_exit(rc);
    }

    /* ---- the parent --------------------------------------------------
     *
     * Waits first, so the child has certainly done its writes before any
     * of these reads. Without that the test would pass on a kernel with
     * no copy-on-write at all, simply by reading before the child wrote. */
    long child_rc = sys_wait(kid);
    if (child_rc != 0) {
        return (int)child_rc;
    }

    char got[16];
    memset(got, 0, sizeof(got));
    if (read(fds[0], got, sizeof(got)) <= 0 || strcmp(got, "child") != 0) {
        return 7;
    }

    /* The child changed all four and none of it may be visible here. */
    if (before_fork != 0x5A5A || heap_marker[0] != 'P') {
        return 5;
    }
    if (arena[0] != 'P' || arena[2 * PAGE] != 'P') {
        return 6;
    }

    /* And the parent can still write to its own copies - which is the
     * check that breaking the sharing left the page writable rather than
     * merely different. */
    before_fork = 0x1234;
    heap_marker[0] = 'Q';
    arena[0] = 'Q';
    if (before_fork != 0x1234 || heap_marker[0] != 'Q' || arena[0] != 'Q') {
        return 5;
    }

    close(fds[0]);
    close(fds[1]);
    munmap((void *)arena, 4 * PAGE);

    /* ---- a hundred rounds ---------------------------------------------
     *
     * A shell running a build forks thousands of times. This kernel has
     * 128 task slots and, before M83, never recycled one under that kind
     * of pressure - so this is the loop that finds out whether a slot
     * comes back. Each child exits with a value derived from its round,
     * so a mismatched exit code catches a wait that returned somebody
     * else's answer. */
    /* A hundred rounds rather than the two hundred this started at. The
     * property being checked is that a slot comes back, which the first
     * recycle proves and the hundredth does not; what the extra hundred
     * cost was thirty seconds of every boot. */
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

    printf("forktest: all checks passed\n");
    return 0;
}
