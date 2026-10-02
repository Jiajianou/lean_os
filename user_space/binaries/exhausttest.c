#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "syscall_wrappers.h"

/* Past every ceiling it is asked to find: M187 made a process's descriptor
   table 1024 and the machine's open files 4096, and a test that stops trying
   before the ceiling reports a resource that never ran out. */
#define TRY_HARD 8192

#define AT_LEAST_ONE(n) do { if ((n) == 0) { return 11; } } while (0)

static int wanted(int argc, char **argv, const char *section) {
    return argc < 2 || strcmp(argv[1], section) == 0;
}

int main(int argc, char **argv) {
    if (wanted(argc, argv, "descriptors")) {
        int fd = open("/tmp/exhaust.probe", O_RDWR | O_CREAT | O_TRUNC);
        if (fd < 0) {
            return 10;
        }
        close(fd);

        static int held[TRY_HARD];
        int n = 0;
        for (; n < TRY_HARD; n++) {
            held[n] = open("/tmp/exhaust.probe", O_RDONLY);
            if (held[n] < 0) {
                break;
            }
        }
        if (n >= TRY_HARD) {
            for (int i = 0; i < n; i++) {
                close(held[i]);
            }
            return 2;
        }
        AT_LEAST_ONE(n);
        printf("exhausttest: descriptors ran out after %d opens\n", n);
        for (int i = 0; i < n; i++) {
            close(held[i]);
        }
        int again = open("/tmp/exhaust.probe", O_RDONLY);
        if (again < 0) {
            return 3;
        }
        close(again);
        unlink("/tmp/exhaust.probe");
    }

    if (wanted(argc, argv, "pipes")) {
        static int rd[TRY_HARD], wr[TRY_HARD];
        int n = 0;
        for (; n < TRY_HARD; n++) {
            int p[2];
            if (pipe(p) != 0) {
                break;
            }
            rd[n] = p[0];
            wr[n] = p[1];
        }
        if (n >= TRY_HARD) {
            for (int i = 0; i < n; i++) {
                close(rd[i]);
                close(wr[i]);
            }
            return 4;
        }
        AT_LEAST_ONE(n);
        printf("exhausttest: pipes ran out after %d pairs\n", n);
        for (int i = 0; i < n; i++) {
            close(rd[i]);
            close(wr[i]);
        }
        int p[2];
        if (pipe(p) != 0) {
            return 5;
        }
        close(p[0]);
        close(p[1]);
    }

    if (wanted(argc, argv, "shm")) {
        static long ids[TRY_HARD];
        static void *addrs[TRY_HARD];
        int n = 0;
        for (; n < TRY_HARD; n++) {
            ids[n] = sys_shared_memory_create(4096);
            if (ids[n] < 0) {
                break;
            }
            long va = sys_shared_memory_map(ids[n]);
            if (va == -1) {
                break;
            }
            addrs[n] = (void *)va;
        }
        if (n >= TRY_HARD) {
            for (int i = 0; i < n; i++) {
                sys_shared_memory_free(ids[i], addrs[i]);
            }
            return 6;
        }
        AT_LEAST_ONE(n);
        printf("exhausttest: shm segments ran out after %d\n", n);
        for (int i = 0; i < n; i++) {
            sys_shared_memory_free(ids[i], addrs[i]);
        }
        long again = sys_shared_memory_create(4096);
        if (again < 0) {
            return 7;
        }
        long va = sys_shared_memory_map(again);
        if (va != -1) {
            sys_shared_memory_free(again, (void *)va);
        }
    }

    if (wanted(argc, argv, "sockets")) {
        static int socks[TRY_HARD];
        int n = 0;
        for (; n < TRY_HARD; n++) {
            socks[n] = (int)sys_socket(0);
            if (socks[n] < 0) {
                break;
            }
        }
        if (n >= TRY_HARD) {
            for (int i = 0; i < n; i++) {
                close(socks[i]);
            }
            return 8;
        }
        AT_LEAST_ONE(n);
        printf("exhausttest: sockets ran out after %d\n", n);
        for (int i = 0; i < n; i++) {
            close(socks[i]);
        }
        int again = (int)sys_socket(0);
        if (again < 0) {
            return 9;
        }
        close(again);
    }

    printf("exhausttest: every resource refused, recovered and worked again\n");
    return 0;
}
