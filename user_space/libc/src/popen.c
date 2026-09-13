#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

#define POPEN_MAX 16

static struct {
    FILE *f;
    pid_t pid;
} pipes[POPEN_MAX];

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
            fclose(f);
            int st = 0;
            if (waitpid(pid, &st, 0) != pid) {
                return -1;
            }
            return st;
        }
    }
    errno = EINVAL;
    return -1;
}

int system(const char *command) {
    if (!command) {
        return 1;
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
