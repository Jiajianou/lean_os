#include <stdio.h>
#include <string.h>

#include "os_net.h"
#include "paths.h"
#include "syscall_wrappers.h"

#define ROUNDS      12
#define FILE_BYTES  512
#define SHM_BYTES   4096
#define PIPE_BYTES  256

static int failures;
static char label[32];

static void check(int ok, const char *what) {
    if (!ok) {
        printf("racetest[");
        printf(label);
        printf("]: FAILED - ");
        printf(what);
        printf("\n");
        failures++;
    }
}

static unsigned char stamp(int pid, int round, int offset) {
    return (unsigned char)((pid * 31 + round * 7 + offset * 13) & 0xFF);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int pid = (int)sys_getpid();

    int n = 0;
    int v = pid;
    char digits[12];
    if (v == 0) { digits[n++] = '0'; }
    while (v > 0) { digits[n++] = (char)('0' + (v % 10)); v /= 10; }
    int li = 0;
    while (n > 0) { label[li++] = digits[--n]; }
    label[li] = '\0';

    char path[64];
    strcpy(path, "/tmp/race");
    strcat(path, label);

    static unsigned char out[FILE_BYTES];
    static unsigned char back[FILE_BYTES];

    for (int round = 0; round < ROUNDS; round++) {
        for (int i = 0; i < FILE_BYTES; i++) {
            out[i] = stamp(pid, round, i);
        }
        check(sys_writefile(path, out, FILE_BYTES) == 0, "writefile failed");

        memset(back, 0, FILE_BYTES);
        check(sys_readfile(path, back, FILE_BYTES) == FILE_BYTES,
              "readfile returned the wrong length");

        int fs_ok = 1;
        for (int i = 0; i < FILE_BYTES; i++) {
            if (back[i] != out[i]) { fs_ok = 0; break; }
        }
        check(fs_ok, "a file read back bytes this process did not write");

        long id = sys_shm_create(SHM_BYTES);
        check(id >= 0, "shm_create failed");
        if (id >= 0) {
            unsigned char *p = (unsigned char *)sys_shm_map(id);
            check(p != (unsigned char *)-1 && p != 0, "shm_map failed");
            if (p && p != (unsigned char *)-1) {
                for (int i = 0; i < SHM_BYTES; i += 64) {
                    p[i] = stamp(pid, round, i);
                }
                sys_yield();
                int shm_ok = 1;
                for (int i = 0; i < SHM_BYTES; i += 64) {
                    if (p[i] != stamp(pid, round, i)) { shm_ok = 0; break; }
                }
                check(shm_ok, "a shared-memory segment came back holding another process's bytes");
                check(sys_shm_free(id, p) == 0, "shm_free failed");
            }
        }

        int fds[2];
        check(sys_pipe(fds) == 0, "pipe failed");
        if (fds[0] >= 0 && fds[1] >= 0) {
            for (int i = 0; i < PIPE_BYTES; i++) {
                out[i] = stamp(pid, round, i);
            }
            check(sys_write(fds[1], out, PIPE_BYTES) == PIPE_BYTES, "pipe write was short");
            sys_yield();
            memset(back, 0, PIPE_BYTES);
            long got = 0;
            while (got < PIPE_BYTES) {
                long r = sys_read(fds[0], back + got, (size_t)(PIPE_BYTES - got));
                if (r <= 0) { break; }
                got += r;
            }
            check(got == PIPE_BYTES, "pipe read did not return everything written");
            int pipe_ok = 1;
            for (int i = 0; i < PIPE_BYTES; i++) {
                if (back[i] != out[i]) { pipe_ok = 0; break; }
            }
            check(pipe_ok, "bytes came out of a pipe in the wrong order or from the wrong writer");
            sys_close(fds[0]);
            sys_close(fds[1]);
        }

        int s1 = (int)sys_socket(OS_SOCK_DGRAM);
        int s2 = (int)sys_socket(OS_SOCK_DGRAM);
        check(s1 >= 0 && s2 >= 0, "socket allocation failed");
        if (s1 >= 0 && s2 >= 0) {
            long p1 = sys_bind(s1, 0);
            long p2 = sys_bind(s2, 0);
            check(p1 > 0 && p2 > 0, "ephemeral bind failed");
            check(p1 != p2, "two sockets were bound to the same ephemeral port");
        }
        if (s1 >= 0) { sys_close(s1); }
        if (s2 >= 0) { sys_close(s2); }
    }

    {
        int wfds[2];
        check(sys_pipe(wfds) == 0, "pipe for waitfds failed");
        if (wfds[0] >= 0) {
            long before = sys_uptime_ms();
            long r = sys_waitfds(wfds, 1, 60);
            long waited = sys_uptime_ms() - before;
            check(r == -2, "waitfds on an empty pipe did not report a timeout");
            check(waited >= 40, "waitfds returned far too early - it did not wait");

            check(sys_write(wfds[1], "z", 1) == 1, "write for waitfds failed");
            check(sys_waitfds(wfds, 1, 1000) == 0,
                  "waitfds did not report a pipe with bytes in it as ready");

            {
                long before0 = sys_uptime_ms();
                long r0 = sys_waitfds(wfds, 0, 50);
                long waited0 = sys_uptime_ms() - before0;
                check(r0 == -2, "waitfds with no descriptors did not report a timeout");
                check(waited0 >= 30, "waitfds with no descriptors returned without sleeping");
            }
            check(sys_waitfds(wfds, 100000, 10) == -1,
                  "waitfds accepted a count larger than the fd table");
            sys_close(wfds[0]);
            sys_close(wfds[1]);
        }
    }

    sys_unlink(path);

    if (failures) {
        printf("racetest[");
        printf(label);
        printf("]: ");
        return 1;
    }
    return 0;
}
