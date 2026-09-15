#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "paths.h"

#define SCRATCH PATH_TEMPORARY_DIRECTORY "basetest.tmp"

static long long span_ns(const struct timespec *a, const struct timespec *b) {
    return (long long)(b->tv_sec - a->tv_sec) * 1000000000ll +
           ((long long)b->tv_nsec - (long long)a->tv_nsec);
}

/* The three clocks this machine answers with CLOCK_MONOTONIC's value, and the
   one it answers with CLOCK_REALTIME's. They are read at different instants,
   so the claim being graded is that they are the SAME CLOCK - within a window
   far tighter than an unrelated clock could land in by accident - rather than
   that two reads are equal. */
static int the_monotonic_family_is_one_clock(void) {
    struct timespec base;
    if (clock_gettime(CLOCK_MONOTONIC, &base) != 0) {
        return 2;
    }
    const clockid_t same[] = {CLOCK_BOOTTIME, CLOCK_MONOTONIC_RAW,
                              CLOCK_MONOTONIC_COARSE};
    for (int i = 0; i < 3; i++) {
        struct timespec other;
        if (clock_gettime(same[i], &other) != 0) {
            return 3;
        }
        long long apart = span_ns(&base, &other);
        if (apart < 0 || apart > 50 * 1000 * 1000ll) {
            return 4;
        }
    }
    struct timespec wall;
    struct timespec coarse;
    if (clock_gettime(CLOCK_REALTIME, &wall) != 0 ||
        clock_gettime(CLOCK_REALTIME_COARSE, &coarse) != 0) {
        return 5;
    }
    long long apart = span_ns(&wall, &coarse);
    if (apart < 0 || apart > 50 * 1000 * 1000ll) {
        return 6;
    }
    /* An identifier this libc does not have has to be refused rather than
       silently answered with somebody else's clock. */
    struct timespec ignored;
    if (clock_gettime(4242, &ignored) == 0) {
        return 7;
    }
    if (clock_getres(4242, &ignored) == 0) {
        return 8;
    }
    return 0;
}

/* Processor time is accounted a scheduler tick at a time, so these two report
   the tick and the wall clocks report the millisecond they actually carry. A
   resolution claim that is finer than the counter behind it is a lie that
   costs nothing to tell. */
static int the_clocks_report_the_resolution_they_have(void) {
    struct timespec res;
    if (clock_getres(CLOCK_MONOTONIC, &res) != 0 || res.tv_sec != 0 ||
        res.tv_nsec != 1000000L) {
        return 9;
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        return 10;
    }
    if (clock_getres(CLOCK_PROCESS_CPUTIME_ID, &res) != 0 || res.tv_sec != 0 ||
        res.tv_nsec != (long)(1000000000L / hz)) {
        return 11;
    }
    if (clock_getres(CLOCK_THREAD_CPUTIME_ID, &res) != 0 || res.tv_sec != 0 ||
        res.tv_nsec != (long)(1000000000L / hz)) {
        return 12;
    }
    return 0;
}

static volatile unsigned long burn_sink;

static void burn_milliseconds(int ms) {
    struct timespec start;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        for (int i = 0; i < 20000; i++) {
            burn_sink += (unsigned long)i;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (span_ns(&start, &now) >= (long long)ms * 1000000ll) {
            return;
        }
    }
}

static void *burner(void *unused) {
    (void)unused;
    burn_milliseconds(400);
    return (void *)0;
}

/* The sharp one. A second thread burns processor time; the process clock has
   to have gained it and the calling thread's clock has to have NOT. An
   implementation where both identifiers answer the same counter passes every
   other check in this file and fails this one. */
static int a_process_is_more_than_the_calling_thread(void) {
    struct timespec thread_before;
    struct timespec process_before;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &thread_before) != 0 ||
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &process_before) != 0) {
        return 13;
    }
    pthread_t worker;
    if (pthread_create(&worker, (pthread_attr_t *)0, burner, (void *)0) != 0) {
        return 14;
    }
    if (pthread_join(worker, (void **)0) != 0) {
        return 15;
    }
    struct timespec thread_after;
    struct timespec process_after;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &thread_after) != 0 ||
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &process_after) != 0) {
        return 16;
    }
    long long thread_gained = span_ns(&thread_before, &thread_after);
    long long process_gained = span_ns(&process_before, &process_after);
    if (thread_gained < 0 || process_gained < 0) {
        return 17;
    }
    /* The worker burned 400 ms. This thread blocked in pthread_join for all
       of it, so the difference has to be most of that. */
    if (process_gained - thread_gained < 150 * 1000000ll) {
        return 18;
    }
    /* And the joined thread's time must not have landed on THIS thread, which
       is where the reaper used to put it. */
    if (thread_gained > 150 * 1000000ll) {
        return 19;
    }
    /* getrusage(RUSAGE_SELF) is the process and RUSAGE_THREAD is this thread,
       so the same gap has to show there. */
    struct rusage self;
    struct rusage mine;
    if (getrusage(RUSAGE_SELF, &self) != 0 || getrusage(RUSAGE_THREAD, &mine) != 0) {
        return 20;
    }
    long long self_us = (long long)self.ru_utime.tv_sec * 1000000ll +
                        self.ru_utime.tv_usec +
                        (long long)self.ru_stime.tv_sec * 1000000ll +
                        self.ru_stime.tv_usec;
    long long mine_us = (long long)mine.ru_utime.tv_sec * 1000000ll +
                        mine.ru_utime.tv_usec +
                        (long long)mine.ru_stime.tv_sec * 1000000ll +
                        mine.ru_stime.tv_usec;
    if (self_us - mine_us < 150000ll) {
        return 21;
    }
    return 0;
}

static char alternate[SIGSTKSZ];
static volatile int handler_ran;
static volatile int handler_was_on_the_alternate_stack;
static volatile int handler_saw_onstack_flag;

static void on_signal(int sig) {
    (void)sig;
    char here;
    const char *at = &here;
    handler_was_on_the_alternate_stack =
        (at >= alternate && at < alternate + sizeof(alternate));
    stack_t current;
    if (sigaltstack((const stack_t *)0, &current) == 0) {
        handler_saw_onstack_flag = (current.ss_flags & SS_ONSTACK) != 0;
    }
    handler_ran = 1;
}

/* Not "sigaltstack returned 0" - that a handler's own local actually LIVES on
   the registered stack, which is the only thing registering one buys. */
static int a_handler_runs_on_the_stack_it_was_given(void) {
    stack_t want;
    want.ss_sp = alternate;
    want.ss_size = sizeof(alternate);
    want.ss_flags = 0;
    if (sigaltstack(&want, (stack_t *)0) != 0) {
        return 22;
    }
    stack_t back;
    if (sigaltstack((const stack_t *)0, &back) != 0) {
        return 23;
    }
    if (back.ss_sp != alternate || back.ss_size != sizeof(alternate)) {
        return 24;
    }
    if (back.ss_flags & SS_ONSTACK) {
        return 25;
    }
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = on_signal;
    act.sa_flags = SA_ONSTACK;
    if (sigaction(SIGUSR1, &act, (struct sigaction *)0) != 0) {
        return 26;
    }
    handler_ran = 0;
    handler_was_on_the_alternate_stack = 0;
    handler_saw_onstack_flag = 0;
    if (kill(getpid(), SIGUSR1) != 0) {
        return 27;
    }
    for (int spins = 0; spins < 1000 && !handler_ran; spins++) {
        struct timespec nap = {0, 1000000};
        nanosleep(&nap, 0);
    }
    if (!handler_ran) {
        return 28;
    }
    if (!handler_was_on_the_alternate_stack) {
        return 29;
    }
    if (!handler_saw_onstack_flag) {
        return 30;
    }
    /* Without SA_ONSTACK the same handler has to run on the ordinary stack,
       which is what proves the flag is what moved it rather than the
       registration alone. */
    act.sa_flags = 0;
    if (sigaction(SIGUSR1, &act, (struct sigaction *)0) != 0) {
        return 31;
    }
    handler_ran = 0;
    handler_was_on_the_alternate_stack = 0;
    if (kill(getpid(), SIGUSR1) != 0) {
        return 32;
    }
    for (int spins = 0; spins < 1000 && !handler_ran; spins++) {
        struct timespec nap = {0, 1000000};
        nanosleep(&nap, 0);
    }
    if (!handler_ran) {
        return 33;
    }
    if (handler_was_on_the_alternate_stack) {
        return 34;
    }
    stack_t off;
    off.ss_sp = (void *)0;
    off.ss_size = 0;
    off.ss_flags = SS_DISABLE;
    if (sigaltstack(&off, (stack_t *)0) != 0) {
        return 35;
    }
    /* A stack under the floor is not a stack a frame fits on. */
    stack_t tiny;
    tiny.ss_sp = alternate;
    tiny.ss_size = MINSIGSTKSZ - 1;
    tiny.ss_flags = 0;
    if (sigaltstack(&tiny, (stack_t *)0) == 0) {
        return 36;
    }
    return 0;
}

/* SO_PEERCRED over a socketpair: both ends belong to this process, so both
   have to report this process and not, say, zero. */
static int a_socket_knows_who_is_at_the_other_end(void) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        return 37;
    }
    pid_t me = getpid();
    for (int i = 0; i < 2; i++) {
        struct ucred who;
        socklen_t length = sizeof(who);
        if (getsockopt(pair[i], SOL_SOCKET, SO_PEERCRED, &who, &length) != 0) {
            close(pair[0]);
            close(pair[1]);
            return 38;
        }
        if (length != sizeof(who)) {
            close(pair[0]);
            close(pair[1]);
            return 39;
        }
        if (who.pid != me) {
            close(pair[0]);
            close(pair[1]);
            return 40;
        }
        if (who.uid != 0 || who.gid != 0) {
            close(pair[0]);
            close(pair[1]);
            return 41;
        }
    }
    close(pair[0]);
    close(pair[1]);
    /* An ordinary file is not a socket and has no peer to report. */
    int fd = open(SCRATCH, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        return 42;
    }
    struct ucred nobody;
    socklen_t length = sizeof(nobody);
    int refused = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &nobody, &length);
    close(fd);
    if (refused == 0) {
        return 43;
    }
    return 0;
}

/* truncate by path, and the pair of seek functions a large-file interface
   spells with an o. libc++'s filebuf reaches this machine through them. */
static int a_file_can_be_cut_and_walked_by_offset(void) {
    FILE *f = fopen(SCRATCH, "wb");
    if (!f) {
        return 44;
    }
    for (int i = 0; i < 1000; i++) {
        if (fputc(i & 0xff, f) == -1) {
            fclose(f);
            return 45;
        }
    }
    if (ftello(f) != 1000) {
        fclose(f);
        return 46;
    }
    if (fseeko(f, 250, SEEK_SET) != 0 || ftello(f) != 250) {
        fclose(f);
        return 47;
    }
    if (fseeko(f, 100, SEEK_CUR) != 0 || ftello(f) != 350) {
        fclose(f);
        return 48;
    }
    fclose(f);
    if (truncate(SCRATCH, 400) != 0) {
        return 49;
    }
    struct stat st;
    if (stat(SCRATCH, &st) != 0) {
        return 50;
    }
    if (st.st_size != 400) {
        return 51;
    }
    f = fopen(SCRATCH, "rb");
    if (!f) {
        return 52;
    }
    if (fseeko(f, 0, SEEK_END) != 0 || ftello(f) != 400) {
        fclose(f);
        return 53;
    }
    fclose(f);
    if (truncate(PATH_TEMPORARY_DIRECTORY "basetest-absent.tmp", 0) == 0) {
        return 54;
    }
    unlink(SCRATCH);
    return 0;
}

/* The symbol-versioning structures are a description of bytes in a file, so
   what can be wrong about them is their layout. The gABI fixes these sizes,
   and a field of the wrong width or a member in the wrong order changes one
   of them. */
static int the_version_structures_are_the_shape_the_gabi_fixes(void) {
    if (sizeof(Elf64_Verdef) != 20) {
        return 55;
    }
    if (sizeof(Elf64_Verdaux) != 8) {
        return 56;
    }
    if (sizeof(Elf64_Verneed) != 16) {
        return 57;
    }
    if (sizeof(Elf64_Vernaux) != 16) {
        return 58;
    }
    if (sizeof(Elf32_Verdef) != 20 || sizeof(Elf32_Verdaux) != 8 ||
        sizeof(Elf32_Verneed) != 16 || sizeof(Elf32_Vernaux) != 16) {
        return 59;
    }
    Elf64_Verdef d;
    if ((char *)&d.vd_ndx - (char *)&d != 4) {
        return 60;
    }
    if ((char *)&d.vd_hash - (char *)&d != 8) {
        return 61;
    }
    if ((char *)&d.vd_next - (char *)&d != 16) {
        return 62;
    }
    /* The reserved section-index range, which is what tells a reader that an
       index is a marker rather than a section. */
    if (SHN_LORESERVE != 0xff00 || SHN_HIRESERVE != 0xffff) {
        return 63;
    }
    if (SHN_ABS <= SHN_LORESERVE || SHN_ABS >= SHN_HIRESERVE) {
        return 64;
    }
    if (DT_VERSYM != 0x6ffffff0 || DT_VERDEF != 0x6ffffffc ||
        DT_VERDEFNUM != 0x6ffffffd || DT_VERNEED != 0x6ffffffe ||
        DT_VERNEEDNUM != 0x6fffffff) {
        return 65;
    }
    return 0;
}

int main(void) {
    int rc;
    if ((rc = the_monotonic_family_is_one_clock()) != 0) {
        return rc;
    }
    if ((rc = the_clocks_report_the_resolution_they_have()) != 0) {
        return rc;
    }
    if ((rc = a_process_is_more_than_the_calling_thread()) != 0) {
        return rc;
    }
    if ((rc = a_handler_runs_on_the_stack_it_was_given()) != 0) {
        return rc;
    }
    if ((rc = a_socket_knows_who_is_at_the_other_end()) != 0) {
        return rc;
    }
    if ((rc = a_file_can_be_cut_and_walked_by_offset()) != 0) {
        return rc;
    }
    if ((rc = the_version_structures_are_the_shape_the_gabi_fixes()) != 0) {
        return rc;
    }
    return 0;
}
