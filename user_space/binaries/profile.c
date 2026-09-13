#include "symtab.h"
#include "syscall_wrappers.h"

#include <profile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAMPLES 2048
static prof_sample_t samples[MAX_SAMPLES];

#define SYMS_PATH "/etc/kernel.syms"

#define MAX_SYMS 2048
#define SYMS_TEXT_MAX (64 * 1024)
static char syms_text[SYMS_TEXT_MAX];
static symtab_entry_t syms_storage[MAX_SYMS];
static symtab_t syms;
static int syms_loaded;

static void load_symbols(void) {
    int fd = (int)sys_open(SYMS_PATH, 0);
    if (fd < 0) {
        return;
    }
    size_t total = 0;
    int truncated = 0;
    for (;;) {
        if (total >= sizeof(syms_text)) {
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
        printf("profile: %s has more than %d symbols - not using it\n",
               SYMS_PATH, MAX_SYMS);
        return;
    }
    if (!symtab_is_sorted(&syms)) {
        printf("profile: %s is not in ascending order - not using it\n", SYMS_PATH);
        return;
    }
    syms_loaded = 1;
}

static void print_name(uint64_t rip, int pid) {
    if (pid != PROF_PID_KERNEL) {
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
        long pid = sys_spawnve(argv[3], (const char *const *)&argv[3], NULL);
        if (pid < 0) {
            sys_profile(PROFILE_OP_STOP, NULL, 0);
            printf("profile: cannot start %s\n", argv[3]);
            return 1;
        }
        long deadline = sys_uptime_ms() + (long)secs * 1000;
        for (;;) {
            if (sys_wait_nb((int)pid) != -2) {
                break;
            }
            if (sys_uptime_ms() >= deadline) {
                break;
            }
            int nofds = -1;
            (void)sys_waitfds(&nofds, 0, 200);
        }
        sys_profile(PROFILE_OP_STOP, NULL, 0);
        return report(20);
    }
    return usage();
}

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
