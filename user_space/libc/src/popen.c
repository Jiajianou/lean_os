/* user_space/libc/src/popen.c - M100: popen, pclose and system.
 *
 * ---- who asked ----------------------------------------------------------
 *
 * sqlite's own shell (shell.c, the sixth library of M100's stack) links
 * popen and pclose for `.import '|command'` and `.output |command`, and
 * system for `.shell`. That is the first program ported here that has
 * needed any of the three, and it is the caller <stdlib.h>'s old note
 * said system() was waiting for: "the version that would get written
 * without a caller to check it against would be wrong in some way nobody
 * would find". Now there is something to check it against - the sqlite
 * transcript in tests/sqlite/cases.sql runs all three, on this machine
 * and on the host, and the two have to agree.
 *
 * ---- what they are made of ---------------------------------------------
 *
 * All three are `/bin/sh -c command` over M83's fork and M84's exec,
 * which is what every libc does and the reason none of them needed a
 * syscall. The one decision worth writing down: the child's end of the
 * pipe is put on fd 0 or 1 with dup2, which on this kernel RELEASES the
 * slot it overwrites (M59) - so the child holds exactly one reference to
 * each pipe end, and when it exits the parent's read sees EOF rather
 * than a reader that waits for a writer that is still counted.
 *
 * A popen'ed stream is an ordinary FILE from fdopen; what makes it a
 * popen'ed stream is the row below that remembers its child. pclose on
 * anything not in that table is -1, because "close this stream and wait
 * for a process that does not exist" has no honest answer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

#define POPEN_MAX 16 /* FOPEN_MAX: there cannot be more streams than that */

static struct {
    FILE *f;
    pid_t pid;
} pipes[POPEN_MAX];

/* fork, and in the child: put `child_in`/`child_out` (either may be -1)
 * on stdin/stdout, drop the descriptors the parent keeps, and exec the
 * shell. Returns the child's pid to the parent, -1 if fork refused. */
static pid_t run_shell(const char *command, int child_in, int child_out,
                       int drop_a, int drop_b) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        if (child_in >= 0) {
            dup2(child_in, 0);
        }
        if (child_out >= 0) {
            dup2(child_out, 1);
        }
        if (drop_a >= 0) {
            close(drop_a);
        }
        if (drop_b >= 0) {
            close(drop_b);
        }
        char *argv[] = {"sh", "-c", (char *)command, (char *)0};
        execv("/bin/sh", argv);
        /* 127 is what every shell reports for a command that could not
         * be run at all, and it is what a caller checking WEXITSTATUS
         * will recognise. */
        _exit(127);
    }
    return pid;
}

FILE *popen(const char *command, const char *mode) {
    if (!command || !mode || (mode[0] != 'r' && mode[0] != 'w')) {
        errno = EINVAL;
        return (FILE *)0;
    }
    int slot = -1;
    for (int i = 0; i < POPEN_MAX; i++) {
        if (!pipes[i].f) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        errno = EMFILE;
        return (FILE *)0;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        errno = EMFILE;
        return (FILE *)0;
    }
    int reading = mode[0] == 'r';
    /* "r": the child writes fds[1], the parent reads fds[0]. "w": the
     * other way round. The child drops both of the parent's descriptors
     * - the one it does not use, and the one it just dup2'd onto 0/1. */
    pid_t pid = reading ? run_shell(command, -1, fds[1], fds[0], fds[1])
                        : run_shell(command, fds[0], -1, fds[0], fds[1]);
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        errno = EAGAIN;
        return (FILE *)0;
    }
    int keep = reading ? fds[0] : fds[1];
    close(reading ? fds[1] : fds[0]);
    FILE *f = fdopen(keep, reading ? "r" : "w");
    if (!f) {
        /* No FILE to wrap it in. The child is already running; reap it
         * rather than leave it, and report the limit that was hit. */
        close(keep);
        int st;
        waitpid(pid, &st, 0);
        errno = EMFILE;
        return (FILE *)0;
    }
    pipes[slot].f = f;
    pipes[slot].pid = pid;
    return f;
}

int pclose(FILE *f) {
    for (int i = 0; i < POPEN_MAX; i++) {
        if (pipes[i].f && pipes[i].f == f) {
            pid_t pid = pipes[i].pid;
            pipes[i].f = (FILE *)0;
            /* Flush and close FIRST: a child reading its stdin from a
             * "w" stream is waiting for EOF, and waiting for the child
             * before giving it one is a deadlock with two participants. */
            fclose(f);
            int st = 0;
            if (waitpid(pid, &st, 0) != pid) {
                return -1;
            }
            return st;
        }
    }
    errno = EINVAL; /* not a stream popen opened */
    return -1;
}

int system(const char *command) {
    if (!command) {
        return 1; /* "is there a command processor" - yes, /bin/sh */
    }
    pid_t pid = run_shell(command, -1, -1, -1, -1);
    if (pid < 0) {
        return -1;
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) {
        return -1;
    }
    return st;
}
