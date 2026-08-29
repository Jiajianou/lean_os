/* user_space/bin/racetest.c
 *
 * M67's self-test, and it is a user-space program for a reason this
 * project has now given five times (badptr.c, libctest.c, nettest.c,
 * captest.c): what M67 changed is what happens *at the syscall
 * boundary*, so a test that ran inside the kernel would be on the wrong
 * side of it.
 *
 * What M67 changed is that `int 0x80` became a trap gate, so interrupts
 * stay enabled through a syscall. For sixty-six milestones IF=0 was this
 * kernel's only mutual exclusion, never written down and load-bearing
 * everywhere. This program's job is to be the thing that would have
 * noticed.
 *
 * The shape of every check here is the same, and it is the only shape
 * that works for a race: **write something only this process could have
 * written, then demand it back byte for byte.** A test that merely calls
 * a syscall a lot proves nothing - two processes that corrupted each
 * other's state would both still get their syscalls answered. A test
 * that writes a pattern keyed to its own pid and reads back somebody
 * else's has caught the bug, and can say so.
 *
 * Four subsystems, chosen because they are exactly the four that had
 * shared mutable state and no lock before M67:
 *
 *   fs      -> fs_lock       (kernel/fs/vfs.c)      inode table, bitmap
 *   shm     -> shm_lock      (kernel/ipc/shm.c)     the segment table
 *   pipe    -> pipe_lock     (kernel/ipc/pipe.c)    ring buffer, refcounts
 *   socket  -> net_lock      (kernel/net/net.h)     the socket table
 *
 * Run several of these at once (kernel.c spawns four) and the timer tick
 * plus a second core does the rest. Exit 0 for all-passed, 1 otherwise.
 *
 * Deliberately NOT a timing loop with a "ran for N seconds without
 * crashing" verdict. That is the test everybody writes for a race and it
 * is worth nothing: it fails intermittently on a broken kernel and
 * passes intermittently on one, so it can never be believed in either
 * direction. Every assertion below is deterministic - the byte is right
 * or it is not.
 */
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

/* A byte that depends on who wrote it, which round, and where in the
 * buffer. All three matter: without the pid another process's identical
 * write would be indistinguishable from ours, without the round a stale
 * buffer would pass, and without the offset a memcpy that got the length
 * wrong would too. */
static unsigned char stamp(int pid, int round, int offset) {
    return (unsigned char)((pid * 31 + round * 7 + offset * 13) & 0xFF);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int pid = (int)sys_getpid();

    /* The label is this program's own pid rather than an argv index, so a
     * failure message names a task the log and the task manager also
     * name. */
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
        /* ---- fs_lock ---------------------------------------------------
         * A whole-file write is a bitmap update, an inode update and a
         * run of sector writes. Before M67 nothing could interleave with
         * that; now four processes are doing it at once. If two of them
         * are handed the same block, one of these reads comes back with
         * the other's pattern in it. */
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

        /* ---- shm_lock --------------------------------------------------
         * The failure this catches is two callers being handed the same
         * segment id, which before M67 needed only a timer tick between
         * find_free_slot and the store that claims the slot. Two owners
         * of one buffer means whichever writes second wins, so the first
         * one's read comes back wrong. */
        long id = sys_shm_create(SHM_BYTES);
        check(id >= 0, "shm_create failed");
        if (id >= 0) {
            unsigned char *p = (unsigned char *)sys_shm_map(id);
            check(p != (unsigned char *)-1 && p != 0, "shm_map failed");
            if (p && p != (unsigned char *)-1) {
                for (int i = 0; i < SHM_BYTES; i += 64) {
                    p[i] = stamp(pid, round, i);
                }
                /* Give the scheduler a chance to run somebody else
                 * between the write and the read - the window this is
                 * looking for is exactly the one a context switch opens,
                 * so not yielding here would be testing the easy case. */
                sys_yield();
                int shm_ok = 1;
                for (int i = 0; i < SHM_BYTES; i += 64) {
                    if (p[i] != stamp(pid, round, i)) { shm_ok = 0; break; }
                }
                check(shm_ok, "a shared-memory segment came back holding another process's bytes");
                check(sys_shm_free(id, p) == 0, "shm_free failed");
            }
        }

        /* ---- pipe_lock -------------------------------------------------
         * head/tail/count are read-modify-write on both sides. This is a
         * self-pipe (under PIPE_BUF_SIZE, so the write cannot block),
         * which tests the ring arithmetic rather than the rendezvous -
         * and the ring arithmetic is the part that tears. */
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

        /* ---- net_lock --------------------------------------------------
         * Bind to port 0 and the kernel picks an unused one. Two
         * processes handed the *same* ephemeral port is the visible
         * symptom of an unserialised socket table, and it is checkable
         * without a second machine: this process binds two at once and
         * they must differ. Closing them returns both to the table, so a
         * leak here shows up as a bind failure within a few rounds
         * rather than at some unrelated point much later. */
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

    sys_unlink(path);

    if (failures) {
        printf("racetest[");
        printf(label);
        printf("]: ");
        return 1;
    }
    return 0;
}
