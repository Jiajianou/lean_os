/* user_space/bin/profile.c - M101
 *
 * Turns the kernel's sample histogram into a report a person can read.
 *
 *   profile start                 begin sampling
 *   profile stop                  stop
 *   profile reset                 throw the histogram away
 *   profile report [n]            the top n addresses (default 20)
 *   profile syscalls [n]          the top n syscalls by call count
 *   profile timing on|off         time syscalls as well as counting them
 *   profile run <secs> <cmd>...   reset, start, spawn, wait, stop, report
 *
 * ---- Why the last one exists ------------------------------------------
 *
 * Because a profile of "whatever the machine happened to be doing" is
 * the profile people accidentally take, and it is dominated by the
 * compositor's idle loop. `profile run` brackets one workload, which is
 * the only shape of measurement this milestone's own question - where
 * does a build's time go - can be asked in.
 *
 * ---- Symbols ----------------------------------------------------------
 *
 * Resolved against /etc/kernel.syms, written by tools/gen-kernel-syms.sh
 * and put on the disk by `make syms`. If it is absent the report prints
 * raw addresses and says why in one line, rather than failing: an
 * unresolved profile is still a profile, and the failure to say so is
 * what would make it look like a broken one.
 */
#include "symtab.h"
#include "syscall_wrappers.h"

#include <profile.h> /* system_api - the ops and the structs */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PROF_BUCKETS' worth, which is the most the kernel can ever hand back.
 * Static rather than malloc'd: this program's whole job is to run beside
 * a workload being measured, and a 48 KiB heap request at exactly that
 * moment is the profiler perturbing the thing it profiles. */
#define MAX_SAMPLES 2048
static prof_sample_t samples[MAX_SAMPLES];

#define SYMS_PATH "/etc/kernel.syms"

/* Sized against the real file with headroom, and both limits are
 * *checked* rather than trusted - see load_symbols. The kernel this was
 * written against has 763 text symbols in 23 KiB; these are about three
 * times that.
 *
 * The reason the check matters more than the size: a symbol file read
 * only part way through parses cleanly, and every lookup past the
 * truncation point returns the last symbol that did fit. That is a
 * confident wrong name on every line of the report, which is worse than
 * no name at all - so overflowing either limit refuses the whole table
 * rather than using part of it. */
#define MAX_SYMS 2048
#define SYMS_TEXT_MAX (64 * 1024)
static char syms_text[SYMS_TEXT_MAX];
static symtab_entry_t syms_storage[MAX_SYMS];
static symtab_t syms;
static int syms_loaded;

/* Reads the symbol file if it is there. Silent about its absence - the
 * report says so once, in context, rather than here where it would be a
 * warning about something the caller may not care about. */
static void load_symbols(void) {
    int fd = (int)sys_open(SYMS_PATH, 0);
    if (fd < 0) {
        return;
    }
    size_t total = 0;
    int truncated = 0;
    for (;;) {
        if (total >= sizeof(syms_text)) {
            /* The buffer filled. Either the file is exactly this long -
             * which one more read will tell us - or there is more of it
             * than fits, and a partial symbol table is the thing this
             * must not use. */
            char probe;
            truncated = sys_read(fd, &probe, 1) > 0;
            break;
        }
        long n = sys_read(fd, syms_text + total, sizeof(syms_text) - total);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    sys_close(fd);
    if (total == 0) {
        return;
    }
    if (truncated) {
        printf("profile: %s is larger than %d bytes - not using it, because a "
               "partly-read symbol table gives confident wrong names\n",
               SYMS_PATH, SYMS_TEXT_MAX);
        return;
    }
    int n = symtab_parse(&syms, syms_storage, MAX_SYMS, syms_text, total);
    if (n <= 0) {
        return;
    }
    if (n == MAX_SYMS) {
        /* Hit the ceiling, so there may be symbols the table does not
         * have - and the ones it does have would own their addresses.
         * Same refusal, same reason. */
        printf("profile: %s has more than %d symbols - not using it\n",
               SYMS_PATH, MAX_SYMS);
        return;
    }
    if (!symtab_is_sorted(&syms)) {
        /* Reported rather than sorted here - see symtab.h. A file that
         * is not ascending means the generator changed, and resolving
         * against it would produce confident wrong names. */
        printf("profile: %s is not in ascending order - not using it\n", SYMS_PATH);
        return;
    }
    syms_loaded = 1;
}

static void print_name(uint64_t rip, int pid) {
    if (pid != PROF_PID_KERNEL) {
        /* A user address. There is no symbol file for a user program
         * here yet, and inventing one from the kernel's would be worse
         * than an address: it would be a wrong name. */
        printf("  (pid %d) 0x%llx", pid, (unsigned long long)rip);
        return;
    }
    if (syms_loaded) {
        const symtab_entry_t *e = symtab_lookup(&syms, rip);
        if (e) {
            printf("  %.*s+0x%llx", (int)e->name_len, e->name,
                   (unsigned long long)(rip - e->addr));
            return;
        }
    }
    printf("  0x%llx", (unsigned long long)rip);
}

/* Descending by count. An insertion sort over at most 2048 entries,
 * which is the kind of thing that would be embarrassing in the kernel
 * and is free here - this runs once, after the measurement is over. */
static void sort_desc(prof_sample_t *a, int n) {
    for (int i = 1; i < n; i++) {
        prof_sample_t key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].count < key.count) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

static int pct(uint64_t part, uint64_t whole) {
    if (whole == 0) {
        return 0;
    }
    return (int)((part * 100) / whole);
}

static int report(int top) {
    prof_stats_t st;
    if (sys_profile(PROFILE_OP_STATS, &st, 0) != 0) {
        printf("profile: cannot read the profiler (needs process-list)\n");
        return 1;
    }
    long n = sys_profile(PROFILE_OP_SAMPLES, samples, MAX_SAMPLES);
    if (n < 0) {
        printf("profile: cannot read the samples\n");
        return 1;
    }

    printf("samples %llu  kernel %llu (%d%%)  user %llu (%d%%)  idle %llu (%d%%)\n",
           (unsigned long long)st.samples,
           (unsigned long long)st.kernel, pct(st.kernel, st.samples),
           (unsigned long long)st.user, pct(st.user, st.samples),
           (unsigned long long)st.idle, pct(st.idle, st.samples));
    if (st.overflow) {
        /* Loud, because a report taken against a full table is a report
         * about the first 2048 addresses the workload touched, which is
         * a different thing from a profile. */
        printf("WARNING: %llu samples did not fit - this report is partial\n",
               (unsigned long long)st.overflow);
    }
    if (st.samples == 0) {
        printf("nothing was sampled. `profile start` first, or profile a "
               "workload with `profile run`.\n");
        return 0;
    }
    if (!syms_loaded) {
        printf("(no %s - addresses are unresolved. `make syms`.)\n", SYMS_PATH);
    }

    sort_desc(samples, (int)n);
    uint64_t busy = st.kernel + st.user;
    printf("%8s %5s  %s\n", "samples", "pct", "where");
    for (int i = 0; i < n && i < top; i++) {
        printf("%8llu %4d%%", (unsigned long long)samples[i].count,
               pct(samples[i].count, busy));
        print_name(samples[i].rip, samples[i].pid);
        printf("\n");
    }
    return 0;
}

static const char *syscall_name(int num);

static int syscalls_report(int top) {
    static prof_syscount_t counts[SYSCALL_COUNT];
    long n = sys_profile(PROFILE_OP_SYSCALLS, counts, SYSCALL_COUNT);
    if (n < 0) {
        printf("profile: cannot read the syscall table (needs process-list)\n");
        return 1;
    }

    /* Ranked by calls, carrying the number along so the name survives
     * the sort. A parallel array rather than a struct because
     * prof_syscount_t is the ABI's shape and adding a field to it here
     * would be two layouts for one struct - the thing Q3 was about. */
    static int order[SYSCALL_COUNT];
    int live = 0;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        if (counts[i].calls) {
            order[live++] = i;
            total += counts[i].calls;
        }
    }
    for (int i = 1; i < live; i++) {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 && counts[order[j]].calls < counts[key].calls) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    printf("%llu calls across %d of %d syscalls\n",
           (unsigned long long)total, live, (int)n);
    printf("%5s %10s %5s %12s  %s\n", "num", "calls", "pct", "cycles/call", "name");
    for (int i = 0; i < live && i < top; i++) {
        int num = order[i];
        uint64_t calls = counts[num].calls;
        uint64_t per = counts[num].cycles ? counts[num].cycles / calls : 0;
        printf("%5d %10llu %4d%% %12llu  %s\n", num,
               (unsigned long long)calls, pct(calls, total),
               (unsigned long long)per, syscall_name(num));
    }
    if (live && counts[order[0]].cycles == 0) {
        printf("(cycles are zero: timing is off. `profile timing on`.)\n");
    }
    return 0;
}

static int usage(void) {
    printf("usage: profile start|stop|reset|report [n]|syscalls [n]|\n");
    printf("       profile timing on|off\n");
    printf("       profile run <seconds> <command> [args...]\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return usage();
    }
    load_symbols();

    const char *cmd = argv[1];

    if (strcmp(cmd, "start") == 0) {
        return sys_profile(PROFILE_OP_START, NULL, 0) == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "stop") == 0) {
        return sys_profile(PROFILE_OP_STOP, NULL, 0) == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "reset") == 0) {
        int a = (int)sys_profile(PROFILE_OP_RESET, NULL, 0);
        int b = (int)sys_profile(PROFILE_OP_SYSRESET, NULL, 0);
        return (a == 0 && b == 0) ? 0 : 1;
    }
    if (strcmp(cmd, "report") == 0) {
        return report(argc > 2 ? atoi(argv[2]) : 20);
    }
    if (strcmp(cmd, "syscalls") == 0) {
        return syscalls_report(argc > 2 ? atoi(argv[2]) : 20);
    }
    if (strcmp(cmd, "timing") == 0) {
        if (argc < 3) {
            return usage();
        }
        int on = strcmp(argv[2], "on") == 0;
        long was = sys_profile(PROFILE_OP_TIMING, NULL, on);
        if (was < 0) {
            printf("profile: cannot change timing (needs process-list)\n");
            return 1;
        }
        printf("syscall timing %s (was %s)\n", on ? "on" : "off",
               was ? "on" : "off");
        return 0;
    }
    if (strcmp(cmd, "run") == 0) {
        if (argc < 4) {
            return usage();
        }
        int secs = atoi(argv[2]);
        if (secs <= 0) {
            secs = 1;
        }
        sys_profile(PROFILE_OP_RESET, NULL, 0);
        sys_profile(PROFILE_OP_SYSRESET, NULL, 0);
        if (sys_profile(PROFILE_OP_START, NULL, 0) != 0) {
            printf("profile: cannot start (needs process-list)\n");
            return 1;
        }
        /* argv[3] onward is the command and its arguments, and it is
         * already NULL-terminated - main's argv is. So the child's argv
         * is a slice of this one rather than a copy. */
        long pid = sys_spawnve(argv[3], (const char *const *)&argv[3], NULL);
        if (pid < 0) {
            sys_profile(PROFILE_OP_STOP, NULL, 0);
            printf("profile: cannot start %s\n", argv[3]);
            return 1;
        }
        /* Wait for the child, but not forever: a workload that hangs
         * should still produce the profile of it hanging, which is
         * usually the report somebody wanted. */
        long deadline = sys_uptime_ms() + (long)secs * 1000;
        for (;;) {
            if (sys_wait_nb((int)pid) != -2) {
                break;
            }
            if (sys_uptime_ms() >= deadline) {
                break;
            }
            sys_yield();
        }
        sys_profile(PROFILE_OP_STOP, NULL, 0);
        return report(20);
    }
    return usage();
}

/* The names, for the report. Deliberately a table here rather than a
 * string in system_api's syscall.h: that header is the ABI, it is
 * included by the kernel, and adding ninety-nine string literals to
 * every translation unit that names a syscall number would put a
 * kilobyte of text in the kernel to make one user program's output
 * prettier. A number with no name still ranks correctly. */
static const char *syscall_name(int num) {
    switch (num) {
    case SYS_write:       return "write";
    case SYS_read:        return "read";
    case SYS_open:        return "open";
    case SYS_close:       return "close";
    case SYS_readfile:    return "readfile";
    case SYS_writefile:   return "writefile";
    case SYS_spawn:       return "spawn";
    case SYS_wait:        return "wait";
    case SYS_exit:        return "exit";
    case SYS_yield:       return "yield";
    case SYS_getpid:      return "getpid";
    case SYS_stat:        return "stat";
    case SYS_getdents:    return "getdents";
    case SYS_fork:        return "fork";
    case SYS_execve:      return "execve";
    case SYS_waitpid:     return "waitpid";
    case SYS_lseek:       return "lseek";
    case SYS_mmap:        return "mmap";
    case SYS_munmap:      return "munmap";
    case SYS_uptime_ms:   return "uptime_ms";
    case SYS_unlink:      return "unlink";
    case SYS_mkdir:       return "mkdir";
    case SYS_rename:      return "rename";
    case SYS_fsync:       return "fsync";
    case SYS_profile:     return "profile";
    default:              return "?";
    }
}
