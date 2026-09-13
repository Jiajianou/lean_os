#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

static long ms_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv) {
    long limit_s = 0;
    int argi = 1;
    if (argc >= 3 && strcmp(argv[1], "-t") == 0) {
        limit_s = atol(argv[2]);
        argi = 3;
    }
    if (argc - argi < 2) {
        fprintf(stderr, "usage: measure [-t seconds] <label> <command> [args...]\n");
        return 2;
    }
    const char *label = argv[argi];
    char **command = &argv[argi + 1];

    long started = ms_now();
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "measure: fork failed\n");
        return 2;
    }
    if (pid == 0) {
        if (limit_s > 0) {
            setpgid(0, 0);
        }
        execvp(command[0], command);
        fprintf(stderr, "measure: cannot run %s\n", command[0]);
        _exit(127);
    }

    int status = 0;
    int timed_out = 0;
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));
    if (limit_s > 0) {
        long deadline = started + limit_s * 1000L;
        for (;;) {
            pid_t r = wait4(pid, &status, WNOHANG, &ru);
            if (r == pid) {
                break;
            }
            if (r < 0) {
                fprintf(stderr, "measure: wait failed\n");
                return 2;
            }
            if (ms_now() >= deadline) {
                timed_out = 1;
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                wait4(pid, &status, 0, &ru);
                break;
            }
            usleep(200000);
        }
    } else if (wait4(pid, &status, 0, &ru) < 0) {
        fprintf(stderr, "measure: wait failed\n");
        return 2;
    }
    long wall = ms_now() - started;

    long user_cs = (long)ru.ru_utime.tv_sec * 100L + ru.ru_utime.tv_usec / 10000L;
    long sys_cs = (long)ru.ru_stime.tv_sec * 100L + ru.ru_stime.tv_usec / 10000L;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
    if (timed_out) {
        code = 124;
    }

    const char *cut = timed_out ? "  UNFINISHED at the -t limit" : "";
    if (ru.ru_maxrss > 0) {
        printf("[measure] %s: wall %ld ms  user %ld cs  sys %ld cs  "
               "peak-rss %ld KiB  exit %d%s\n",
               label, wall, user_cs, sys_cs, ru.ru_maxrss, code, cut);
    } else {
        printf("[measure] %s: wall %ld ms  user %ld cs  sys %ld cs  "
               "peak-rss not measured  exit %d%s\n",
               label, wall, user_cs, sys_cs, code, cut);
    }

    char row[64];
    size_t li = 0;
    while (label[li] && li < sizeof(row) - 1) {
        row[li] = (label[li] == '-') ? '_' : label[li];
        li++;
    }
    row[li] = '\0';
    if (!timed_out) {
        printf("[perf] build_%s_wall_ms %ld ms\n", row, wall < 0 ? 0 : wall);
    }
    if (ru.ru_maxrss > 0) {
        printf("[perf] build_%s_peak_rss_kib %ld kib\n", row, ru.ru_maxrss);
    }
    fflush(stdout);
    return code < 0 ? 1 : code;
}
