/* user_space/bin/measure.c - M98's fourth box, made into a tool
 *
 * Runs a command and reports what it cost: wall-clock, CPU time split
 * user/system, and the peak resident set of the process it ran.
 *
 * ---- why this exists rather than `time` -------------------------------
 *
 * Toybox has a `time`, and it reports the two CPU numbers this machine
 * has counted since M88. The number M98 and M102 are both waiting on is
 * the third one - how big a compile got - and until this milestone
 * nothing anywhere counted it: <sys/resource.h> zeroed ru_maxrss on
 * purpose and said so. The kernel counts resident pages per address
 * space now (kernel/mm/vmm.c), getrusage fills the field, and this is
 * the program that asks.
 *
 * Written here rather than patched into toybox for the reason
 * third_party/ exists: toybox's source is not this project's to modify,
 * and a measurement instrument that only exists as a local patch is one
 * nobody can rerun.
 *
 * ---- what the peak is a peak OF ---------------------------------------
 *
 * getrusage(RUSAGE_CHILDREN) reports the largest single child, not the
 * sum - so `measure gcc big.c` reports what the compiler needed, and
 * `measure make -j4` reports the largest ONE of the compiles make ran,
 * which is the number that says whether this machine can host a build.
 * The two are different questions and this prints whichever one it was
 * asked; the caller decides by choosing what to wrap.
 *
 * Because RUSAGE_CHILDREN accumulates over the calling process's whole
 * life, this program is careful to be short-lived: one command, one
 * report, exit. A shell that ran `measure` twice gets two independent
 * measurements because they are two processes.
 *
 * Output is one line per run, on stdout, in a shape a script can grep
 * and a person can read:
 *
 *   [measure] <label>: wall 12345 ms  user 210 cs  sys 44 cs  peak-rss 65536 KiB  exit 0
 *
 * Centiseconds because that is the tick this machine counts in
 * (_SC_CLK_TCK is 100); reporting milliseconds would be arithmetic on a
 * number that was never measured, which is the mistake M101's own notes
 * name twice.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>   /* M100: kill(), for the -t deadline */

/* CLOCK_MONOTONIC, and the first version of this used gettimeofday and
 * was wrong. On this machine the seconds come from the CMOS clock and
 * the microseconds from the uptime counter, and the two are not phase
 * locked - <time.h> says so - so a pair taken either side of a second
 * boundary can go BACKWARDS. It did: the first run of this reported an
 * assembler that finished 530 ms before it started. An interval wants
 * the clock that cannot go backwards, which is the one named after that
 * property. */
static long ms_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv) {
    /* ---- M100: -t, and why a measurement tool needs a deadline -------
     *
     * The header above already argues that a measurement which only
     * exists if the whole run finishes is the one you lose exactly when
     * you most want it. That was written about collecting output into a
     * file. The same sentence applies one level up and nobody had
     * applied it: `tests/bootstrap/run.sh`'s step 8 is `make -j4`, which
     * this project's own archive records as having "did not finish
     * inside a 40-minute ceiling" - and because it was measured with no
     * limit, it took the *harness's* ceiling with it. The boot was
     * killed from outside, the three [perf] rows the kernel prints after
     * the script never appeared, and tools/bootstrap-test.sh reported
     * four missing lines. It has never once passed.
     *
     * Four missing lines is a bad description of "one step does not
     * terminate". With -t the step is bounded, the run continues, and
     * the outcome is reported as what it is: a command that was still
     * running at N seconds. That is a measurement. A hang that eats the
     * run is not.
     *
     * The child is put in its own process group and the group is what
     * gets killed, because the thing being bounded here is a `make` with
     * four compiles under it and killing only make would leave them
     * running behind the report. kill(-pgid) has been the way to signal
     * a pipeline since M85. */
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
    char **cmd = &argv[argi + 1];

    long started = ms_now();
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "measure: fork failed\n");
        return 2;
    }
    if (pid == 0) {
        /* Its own group, so a deadline can reach everything it starts -
         * and ONLY when there is a deadline.
         *
         * The first version of this called setpgid unconditionally, on
         * the theory that a process group costs nothing. It is not free:
         * moving a child out of its parent's process group changes which
         * group is the terminal's foreground one, and this machine has
         * had real job control since M85. The unbounded `make -j1` step
         * of the bootstrap fixture - 188 s on the run before - stopped
         * producing output entirely and was still in it ten minutes
         * later. Two single-process steps either side of it were fine,
         * which is what a job-control stop looks like: it takes the
         * process that has children to make it visible.
         *
         * So the group is created for the reason it exists, at the only
         * moment anything will use it, and a `measure` with no -t is
         * byte-for-byte the behaviour it had before M100 touched it.
         * A capability nothing uses is not free; this one cost the step
         * it was added next to. */
        if (limit_s > 0) {
            setpgid(0, 0);
        }
        execvp(cmd[0], cmd);
        /* Only reachable when the exec failed; 127 is what a shell
         * reports for a command it could not run, and a caller reading
         * this line should see that rather than a plausible zero. */
        fprintf(stderr, "measure: cannot run %s\n", cmd[0]);
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
                /* The group, not the pid - see the note above. The
                 * single pid is signalled too, in case setpgid did not
                 * take: a deadline that quietly fails to stop anything
                 * is worse than no deadline, because the run then hangs
                 * *and* claims it did not. */
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                wait4(pid, &status, 0, &ru);
                break;
            }
            /* Coarse on purpose. This loop runs beside a build that is
             * using the whole machine, and a tight poll would be
             * measuring itself. */
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
    /* 124 is what `timeout` reports and what a script will already know
     * how to read. It replaces the -9 the kill produced, which would be
     * indistinguishable from the command being killed by something
     * else. */
    if (timed_out) {
        code = 124;
    }

    /* A zero peak means the kernel had no accounting slot for that
     * address space, not that the program used no memory - vmm.h says
     * so, and printing "not measured" is the difference between a
     * missing number and a wrong one. */
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

    /* And the same two numbers again in the shape this project grades
     * measurements in: `[perf] name value unit`, which tests/budgets.tsv
     * gives a ceiling and tools/qemu-serial-test.sh (and its M98
     * sibling) compares against. Two lines rather than one because the
     * human-readable line above is what a person reads out of a boot log
     * and the machine-readable one is what fails a run - Q6's argument
     * for the [perf] form, applied to a measurement taken in user space
     * rather than by the kernel.
     *
     * The label becomes the row name with dashes turned into
     * underscores, because a budget row name is [a-z0-9_]+ and
     * `cxx-tu` is the name a person wants to read. */
    char row[64];
    size_t li = 0;
    while (label[li] && li < sizeof(row) - 1) {
        row[li] = (label[li] == '-') ? '_' : label[li];
        li++;
    }
    row[li] = '\0';
    /* M100: a run that was cut short has no wall-clock to report. Its
     * elapsed time is the limit it was given, which is a fact about the
     * caller rather than about the command, and feeding that to a
     * ceiling would produce a budget row that passes for the wrong
     * reason and stops moving. The peak IS real - the pages were
     * genuinely resident - so it is still printed. */
    if (!timed_out) {
        printf("[perf] build_%s_wall_ms %ld ms\n", row, wall < 0 ? 0 : wall);
    }
    if (ru.ru_maxrss > 0) {
        printf("[perf] build_%s_peak_rss_kib %ld kib\n", row, ru.ru_maxrss);
    }
    fflush(stdout);
    return code < 0 ? 1 : code;
}
