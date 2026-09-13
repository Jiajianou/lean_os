#include <stdio.h>
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

int main(int argc, char **argv) {
    if (argc > 1 && argv[1] && strcmp(argv[1], "cow") == 0) {
        return cow_mode();
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
            const char msg[] = "child";
            if (write(fds[1], msg, sizeof(msg)) != (long)sizeof(msg)) {
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
    if (read(fds[0], got, sizeof(got)) <= 0 || strcmp(got, "child") != 0) {
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

    close(fds[0]);
    close(fds[1]);
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
