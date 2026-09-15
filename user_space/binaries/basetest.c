#include <dlfcn.h>
#include <nl_types.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <sys/ucontext.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "signal.h"

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


/* M143. A handler's third argument used to be a null pointer, and a sampling
   profiler is a program that dereferences it. These check that the registers
   in it are the ones that were interrupted - RIP inside this function's
   caller, RSP somewhere on this thread's stack - and then that an EDIT to
   uc_mcontext takes effect, which is what makes it a context rather than a
   description of one. */
static volatile int context_signal_seen;
static volatile unsigned long context_rip;
static volatile unsigned long context_rsp;

static void context_reader(int signal_number, siginfo_t *info, void *raw) {
    (void)signal_number;
    (void)info;
    ucontext_t *context = (ucontext_t *)raw;
    context_signal_seen = raw != 0;
    if (context) {
        context_rip = (unsigned long)context->uc_mcontext.gregs[REG_RIP];
        context_rsp = (unsigned long)context->uc_mcontext.gregs[REG_RSP];
    }
}

static int a_handler_is_given_the_registers_it_interrupted(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = context_reader;
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGUSR1, &action, (struct sigaction *)0) != 0) {
        return 160;
    }
    int local = 0;
    context_signal_seen = 0;
    context_rip = 0;
    context_rsp = 0;
    if (raise(SIGUSR1) != 0) {
        return 161;
    }
    if (!context_signal_seen) {
        return 162;
    }
    if (context_rip == 0) {
        return 163;
    }
    /* The interrupted stack pointer has to be near this frame's own local -
       within a few kilobytes - and not zero, which is what it used to be. */
    unsigned long here = (unsigned long)&local;
    unsigned long distance = context_rsp > here ? context_rsp - here
                                                : here - context_rsp;
    if (context_rsp == 0 || distance > 65536u) {
        return 164;
    }
    return 0;
}

/* raise() is sys_kill, and the signal is delivered on that syscall's own
   return path - so the RAX the handler is shown is the syscall's result, and
   an edit to it is what raise() goes on to return. That makes the difference
   between an edited context and an untouched one directly observable, which
   is the only way to tell a real ucontext from a faithful copy of one.

   It also caught this test's first version, which asserted raise() == 0 with
   the editing handler installed and failed - because the edit had worked. */
#define CONTEXT_EDIT_SENTINEL 0x5EED

static volatile int context_edit_enabled;

static void context_editor(int signal_number, siginfo_t *info, void *raw) {
    (void)signal_number;
    (void)info;
    ucontext_t *context = (ucontext_t *)raw;
    if (context && context_edit_enabled) {
        context->uc_mcontext.gregs[REG_RAX] = CONTEXT_EDIT_SENTINEL;
    }
}

static int an_edit_to_the_context_takes_effect(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = context_editor;
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGUSR2, &action, (struct sigaction *)0) != 0) {
        return 165;
    }
    context_edit_enabled = 0;
    int untouched = raise(SIGUSR2);
    if (untouched != 0) {
        return 166;
    }
    context_edit_enabled = 1;
    int edited = raise(SIGUSR2);
    context_edit_enabled = 0;
    if (edited != CONTEXT_EDIT_SENTINEL) {
        return 167;
    }
    /* And the process is still standing: an rsp taken back out of the
       ucontext that was not the one it went in with would have faulted
       between there and here. */
    if (raise(SIGUSR2) != 0) {
        return 168;
    }
    return 0;
}

static int the_two_ucontext_layouts_agree(void) {
    /* One structure, two declarations: the kernel fills an os_ucontext_t and
       the handler reads a ucontext_t. M138's rule - a wrong offset compiles
       and links perfectly well - so the offsets are compared rather than
       trusted. */
    if (sizeof(ucontext_t) != sizeof(os_ucontext_t)) {
        return 169;
    }
    if (offsetof(ucontext_t, uc_stack) != offsetof(os_ucontext_t, uc_stack)) {
        return 170;
    }
    if (offsetof(ucontext_t, uc_mcontext) !=
        offsetof(os_ucontext_t, uc_mcontext)) {
        return 171;
    }
    if (offsetof(ucontext_t, uc_sigmask) !=
        offsetof(os_ucontext_t, uc_sigmask)) {
        return 172;
    }
    if (offsetof(mcontext_t, gregs) != offsetof(os_mcontext_t, gregs)) {
        return 173;
    }
    if (sizeof(((mcontext_t *)0)->gregs) !=
        sizeof(((os_mcontext_t *)0)->gregs)) {
        return 174;
    }
    stack_t declared;
    os_signal_stack_t inside;
    if (sizeof(declared) != sizeof(inside) ||
        offsetof(stack_t, ss_sp) != offsetof(os_signal_stack_t, ss_sp) ||
        offsetof(stack_t, ss_flags) != offsetof(os_signal_stack_t, ss_flags) ||
        offsetof(stack_t, ss_size) != offsetof(os_signal_stack_t, ss_size)) {
        return 175;
    }
    return 0;
}

/* M143. A semaphore that two threads actually pass through, not one that
   compiles. */
static sem_t handoff;
static volatile int handoff_count;

static void *semaphore_waiter(void *unused) {
    (void)unused;
    for (int i = 0; i < 100; i++) {
        if (sem_wait(&handoff) != 0) {
            return (void *)1;
        }
        handoff_count++;
    }
    return (void *)0;
}

static int a_semaphore_blocks_and_wakes(void) {
    if (sem_init(&handoff, 0, 0) != 0) {
        return 76;
    }
    int value = -1;
    if (sem_getvalue(&handoff, &value) != 0 || value != 0) {
        return 77;
    }
    if (sem_trywait(&handoff) == 0 || errno != EAGAIN) {
        return 78;
    }
    handoff_count = 0;
    pthread_t thread;
    if (pthread_create(&thread, (pthread_attr_t *)0, semaphore_waiter,
                       (void *)0) != 0) {
        return 153;
    }
    for (int i = 0; i < 100; i++) {
        if (sem_post(&handoff) != 0) {
            return 79;
        }
    }
    void *result = (void *)1;
    if (pthread_join(thread, &result) != 0 || result != (void *)0) {
        return 80;
    }
    if (handoff_count != 100) {
        return 81;
    }
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return 82;
    }
    deadline.tv_nsec += 20000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_nsec -= 1000000000;
        deadline.tv_sec++;
    }
    if (sem_timedwait(&handoff, &deadline) == 0 || errno != ETIMEDOUT) {
        return 83;
    }
    if (sem_destroy(&handoff) != 0) {
        return 84;
    }
    return 0;
}

/* M143. The attributes //base sets on a thread and a mutex. Each of these is
   either carried out or refused; none of them is stored and forgotten. */
static void *detached_body(void *unused) {
    (void)unused;
    return (void *)0;
}

static int the_thread_attributes_are_answered_truthfully(void) {
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
        return 85;
    }
    int state = -1;
    if (pthread_attr_getdetachstate(&attributes, &state) != 0 ||
        state != PTHREAD_CREATE_JOINABLE) {
        return 86;
    }
    if (pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) != 0) {
        return 87;
    }
    if (pthread_attr_getdetachstate(&attributes, &state) != 0 ||
        state != PTHREAD_CREATE_DETACHED) {
        return 88;
    }
    if (pthread_attr_setdetachstate(&attributes, 47) != EINVAL) {
        return 89;
    }
    if (PTHREAD_STACK_MIN < 4096u || PTHREAD_STACK_MIN < (unsigned)MINSIGSTKSZ) {
        return 90;
    }
    if (pthread_attr_setstacksize(&attributes, PTHREAD_STACK_MIN) != 0) {
        return 91;
    }
    pthread_t thread;
    if (pthread_create(&thread, &attributes, detached_body, (void *)0) != 0) {
        return 92;
    }
    if (pthread_attr_destroy(&attributes) != 0) {
        return 93;
    }

    pthread_mutexattr_t mutex_attributes;
    if (pthread_mutexattr_init(&mutex_attributes) != 0) {
        return 94;
    }
    if (pthread_mutexattr_setprotocol(&mutex_attributes, PTHREAD_PRIO_NONE) != 0) {
        return 95;
    }
    /* Refused, not accepted: there is no priority here to inherit. */
    if (pthread_mutexattr_setprotocol(&mutex_attributes,
                                      PTHREAD_PRIO_INHERIT) != ENOTSUP) {
        return 96;
    }
    if (pthread_mutexattr_destroy(&mutex_attributes) != 0) {
        return 97;
    }

    pthread_condattr_t condition_attributes;
    if (pthread_condattr_init(&condition_attributes) != 0) {
        return 98;
    }
    int clock = -1;
    if (pthread_condattr_getclock(&condition_attributes, &clock) != 0 ||
        clock != CLOCK_MONOTONIC) {
        return 99;
    }
    if (pthread_condattr_setclock(&condition_attributes, CLOCK_MONOTONIC) != 0) {
        return 100;
    }
    if (pthread_condattr_setclock(&condition_attributes,
                                  CLOCK_REALTIME) != ENOTSUP) {
        return 101;
    }
    if (pthread_condattr_destroy(&condition_attributes) != 0) {
        return 102;
    }
    return 0;
}

/* M143. One nice value, reported and defended. */
static int the_machine_reports_one_priority(void) {
    errno = 0;
    if (getpriority(PRIO_PROCESS, (id_t)getpid()) != 0 || errno != 0) {
        return 103;
    }
    errno = 0;
    if (getpriority(4711, (id_t)0) != -1 || errno != EINVAL) {
        return 104;
    }
    if (setpriority(PRIO_PROCESS, (id_t)0, 0) != 0) {
        return 105;
    }
    errno = 0;
    if (setpriority(PRIO_PROCESS, (id_t)0, -5) != -1 || errno != EPERM) {
        return 106;
    }
    errno = 0;
    if (setpriority(PRIO_PROCESS, (id_t)0, 5) != -1 || errno != EPERM) {
        return 107;
    }
    struct rlimit limit;
    if (getrlimit(RLIMIT_NICE, &limit) != 0 || limit.rlim_cur != 0) {
        return 108;
    }
    if (NZERO < 20) {
        return 109;
    }
    return 0;
}

/* M143. mincore answers from the page tables, so a page that has been touched
   is resident and one that has only been reserved is not. With no swap on
   this machine those two answers are exact rather than advisory. */
static int mincore_reports_what_is_resident(void) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return 110;
    }
    size_t pages = 8;
    size_t length = (size_t)page * pages;
    unsigned char *region = mmap((void *)0, length, PROT_READ | PROT_WRITE,
                                 MAP_ANON | MAP_PRIVATE, -1, 0);
    if (region == MAP_FAILED) {
        return 111;
    }
    unsigned char resident[8];
    memset(resident, 0xFF, sizeof(resident));
    if (mincore(region, length, resident) != 0) {
        munmap(region, length);
        return 112;
    }
    int any_before = 0;
    for (size_t i = 0; i < pages; i++) {
        if (resident[i] & 1) {
            any_before = 1;
        }
    }
    region[0] = 1;
    region[(size_t)page * 3] = 1;
    memset(resident, 0, sizeof(resident));
    if (mincore(region, length, resident) != 0) {
        munmap(region, length);
        return 113;
    }
    int touched_are_resident = (resident[0] & 1) && (resident[3] & 1);
    munmap(region, length);
    if (any_before) {
        return 114;
    }
    if (!touched_are_resident) {
        return 115;
    }
    return 0;
}

/* M143. FIONREAD is a count, not a readiness bit, and the count has to be
   right for the three kinds of descriptor that carry bytes. */
static int fionread_counts_what_is_waiting(void) {
    int ends[2];
    if (pipe(ends) != 0) {
        return 116;
    }
    int available = -1;
    if (ioctl(ends[0], FIONREAD, &available) != 0 || available != 0) {
        close(ends[0]);
        close(ends[1]);
        return 117;
    }
    if (write(ends[1], "abcdefghij", 10) != 10) {
        close(ends[0]);
        close(ends[1]);
        return 118;
    }
    if (ioctl(ends[0], FIONREAD, &available) != 0 || available != 10) {
        close(ends[0]);
        close(ends[1]);
        return 119;
    }
    close(ends[0]);
    close(ends[1]);

    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        return 120;
    }
    if (write(pair[0], "12345", 5) != 5) {
        close(pair[0]);
        close(pair[1]);
        return 121;
    }
    if (ioctl(pair[1], FIONREAD, &available) != 0 || available != 5) {
        close(pair[0]);
        close(pair[1]);
        return 122;
    }
    close(pair[0]);
    close(pair[1]);
    return 0;
}

/* M143. The rest of what //base names, each checked for the behaviour rather
   than for the symbol existing. */
static int the_remaining_surface_behaves(void) {
    /* pipe2's flags actually reach the descriptors. */
    int ends[2];
    if (pipe2(ends, O_CLOEXEC | O_NONBLOCK) != 0) {
        return 123;
    }
    if ((fcntl(ends[0], F_GETFD, 0) & FD_CLOEXEC) == 0) {
        close(ends[0]);
        close(ends[1]);
        return 124;
    }
    if ((fcntl(ends[1], F_GETFL, 0) & O_NONBLOCK) == 0) {
        close(ends[0]);
        close(ends[1]);
        return 125;
    }
    if (pipe2(ends, 0x40000000) == 0) {
        return 126;
    }
    close(ends[0]);
    close(ends[1]);

    /* Sealing through fcntl reaches M120's own seals. */
    int memory = memfd_create("basetest", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memory < 0) {
        return 127;
    }
    if (ftruncate64(memory, 4096) != 0) {
        close(memory);
        return 128;
    }
    if (fcntl(memory, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
        close(memory);
        return 129;
    }
    int seals = fcntl(memory, F_GET_SEALS);
    if (seals < 0 || (seals & F_SEAL_SHRINK) == 0 ||
        (seals & F_SEAL_GROW) == 0) {
        close(memory);
        return 130;
    }
    if (ftruncate(memory, 8192) == 0) {
        close(memory);
        return 131;
    }
    close(memory);

    /* fallocate makes the space and refuses what it cannot do. */
    int file = open(SCRATCH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (file < 0) {
        return 132;
    }
    if (fallocate(file, 0, 0, 4096) != 0) {
        close(file);
        return 133;
    }
    struct stat info;
    if (fstat(file, &info) != 0 || info.st_size < 4096) {
        close(file);
        return 134;
    }
    if (fallocate(file, FALLOC_FL_PUNCH_HOLE, 0, 1024) == 0 ||
        errno != EOPNOTSUPP) {
        close(file);
        return 135;
    }
    if (posix_fadvise(file, 0, 4096, POSIX_FADV_SEQUENTIAL) != 0) {
        close(file);
        return 136;
    }
    if (posix_fadvise(file, 0, 4096, 4711) != EINVAL) {
        close(file);
        return 137;
    }

    /* sendfile moves the bytes and leaves the source's own position alone.
       The string goes AT 4096 rather than at the position fallocate left
       alone, which is zero - the first version of this read the hole and
       said so. */
    if (lseek(file, 4096, SEEK_SET) != 4096) {
        close(file);
        return 138;
    }
    if (write(file, "sendfile-source", 15) != 15) {
        close(file);
        return 139;
    }
    if (lseek(file, 4096, SEEK_SET) != 4096) {
        close(file);
        return 152;
    }
    int destination_ends[2];
    if (pipe(destination_ends) != 0) {
        close(file);
        return 140;
    }
    off_t at = 4096;
    if (sendfile(destination_ends[1], file, &at, 15) != 15 || at != 4111) {
        close(file);
        close(destination_ends[0]);
        close(destination_ends[1]);
        return 141;
    }
    if (lseek(file, 0, SEEK_CUR) != 4096) {
        close(file);
        close(destination_ends[0]);
        close(destination_ends[1]);
        return 142;
    }
    char landed[16];
    memset(landed, 0, sizeof(landed));
    if (read(destination_ends[0], landed, 15) != 15 ||
        memcmp(landed, "sendfile-source", 15) != 0) {
        close(file);
        close(destination_ends[0]);
        close(destination_ends[1]);
        return 143;
    }

    /* futimes reaches the same file the descriptor names. */
    struct timeval when[2];
    when[0].tv_sec = 1000000;
    when[0].tv_usec = 0;
    when[1].tv_sec = 1000000;
    when[1].tv_usec = 0;
    if (futimes(file, when) != 0) {
        close(file);
        close(destination_ends[0]);
        close(destination_ends[1]);
        return 144;
    }
    close(destination_ends[0]);
    close(destination_ends[1]);
    close(file);
    unlink(SCRATCH);

    /* MADV_REMOVE is refused rather than quietly accepted. */
    long page = sysconf(_SC_PAGESIZE);
    void *region = mmap((void *)0, (size_t)page, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
    if (region == MAP_FAILED) {
        return 145;
    }
    int refused = madvise(region, (size_t)page, MADV_REMOVE) != 0;
    int accepted = madvise(region, (size_t)page, MADV_DONTNEED) == 0;
    munmap(region, (size_t)page);
    if (!refused) {
        return 146;
    }
    if (!accepted) {
        return 147;
    }

    /* dladdr says no rather than saying something wrong. */
    Dl_info where;
    memset(&where, 0xAB, sizeof(where));
    if (dladdr((const void *)the_remaining_surface_behaves, &where) != 0) {
        return 148;
    }
    if (where.dli_fname != (const char *)0 || where.dli_saddr != (void *)0) {
        return 149;
    }

    /* The device-number macros take apart what they put together. */
    dev_t device = makedev(259, 4098);
    if (major(device) != 259u || minor(device) != 4098u) {
        return 150;
    }

    /* POLLRDHUP and EPOLLRDHUP are the same bit, because the kernel reports
       one of them and portable code names the other. */
    if ((unsigned)POLLRDHUP != EPOLLRDHUP) {
        return 151;
    }
    return 0;
}


/* M144. What a fault was, rather than that one happened. The kernel has the
   trap vector and the page fault's error code, and reported SI_KERNEL for
   everything until this milestone; a debugger that prints "SEGV_MAPERR" is
   telling the reader there was no mapping there, which is a different bug
   from "there was one and you may not write it". */
static volatile int fault_code_seen;
static volatile void *fault_address_seen;
static sigjmp_buf fault_return;

static void fault_reader(int signal_number, siginfo_t *info, void *raw) {
    (void)signal_number;
    (void)raw;
    fault_code_seen = info ? info->si_code : 0;
    fault_address_seen = info ? info->si_addr : (void *)0;
    siglongjmp(fault_return, 1);
}

static int a_fault_says_which_kind_it_was(void) {
    struct sigaction action, previous;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = fault_reader;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    if (sigaction(SIGSEGV, &action, &previous) != 0) {
        return 180;
    }

    /* A page that was never mapped: there is nothing there to permit. */
    fault_code_seen = -1;
    volatile char *nowhere = (volatile char *)0x300000000000ull;
    if (sigsetjmp(fault_return, 1) == 0) {
        *nowhere = 1;
        sigaction(SIGSEGV, &previous, (struct sigaction *)0);
        return 181;
    }
    int unmapped_code = fault_code_seen;
    void *unmapped_address = (void *)fault_address_seen;

    /* A page that IS mapped, and read-only: the mapping is the difference. */
    long page = sysconf(_SC_PAGESIZE);
    char *readonly = mmap((void *)0, (size_t)page, PROT_READ,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
    if (readonly == MAP_FAILED) {
        sigaction(SIGSEGV, &previous, (struct sigaction *)0);
        return 182;
    }
    fault_code_seen = -1;
    if (sigsetjmp(fault_return, 1) == 0) {
        readonly[0] = 1;
        munmap(readonly, (size_t)page);
        sigaction(SIGSEGV, &previous, (struct sigaction *)0);
        return 183;
    }
    int protected_code = fault_code_seen;
    void *protected_address = (void *)fault_address_seen;
    munmap(readonly, (size_t)page);
    sigaction(SIGSEGV, &previous, (struct sigaction *)0);

    if (unmapped_code != SEGV_MAPERR) {
        return 184;
    }
    if (protected_code != SEGV_ACCERR) {
        return 185;
    }
    /* And si_addr is the address that faulted, at its POSIX spelling. */
    if (unmapped_address != (void *)nowhere) {
        return 186;
    }
    if (protected_address != (void *)readonly) {
        return 187;
    }
    return 0;
}

/* M144. The rest of what //base's own sources named after M143. */
static int the_last_of_the_base_surface_behaves(void) {
    /* gettid is the kernel's thread id, and a thread here IS a task - so the
       main thread's is its pid, and another thread's is not. */
    if (gettid() != getpid()) {
        return 188;
    }

    int policy = -1;
    struct sched_param parameters;
    memset(&parameters, 0, sizeof(parameters));
    if (pthread_getschedparam(pthread_self(), &policy, &parameters) != 0) {
        return 189;
    }
    if (policy != SCHED_OTHER || parameters.sched_priority != 0) {
        return 190;
    }
    if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &parameters) != 0) {
        return 191;
    }
    parameters.sched_priority = 7;
    if (pthread_setschedparam(pthread_self(), SCHED_RR, &parameters) != ENOTSUP) {
        return 192;
    }

    /* <sys/vfs.h> is <sys/statfs.h>, and the call behind both works. */
    struct statfs filesystem;
    if (statfs("/", &filesystem) != 0) {
        return 193;
    }
    if (filesystem.f_bsize == 0) {
        return 194;
    }

    /* A catalogue that is not there, said so rather than crashed on. */
    nl_catd catalogue = catopen("basetest", 0);
    if (catalogue != (nl_catd)-1) {
        return 195;
    }
    const char *fallback = "the string the caller passed in";
    if (catgets(catalogue, 1, 1, fallback) != fallback) {
        return 196;
    }
    if (catclose(catalogue) == 0) {
        return 197;
    }
    return 0;
}

/* M145. getrandom, asked the way Chromium asks it: the whole length in one
   call, from the main thread and from another one. base/rand_util_posix.cc
   requires the return value to equal the length exactly and falls through to
   /dev/urandom otherwise, so a short answer is not a smaller amount of
   randomness, it is a different code path. */
static volatile int worker_random_result;

static void *random_worker(void *unused) {
    (void)unused;
    unsigned char buffer[64];
    memset(buffer, 0, sizeof(buffer));
    ssize_t got = getrandom(buffer, sizeof(buffer), 0);
    worker_random_result = (got == (ssize_t)sizeof(buffer)) ? 1 : (int)got;
    return (void *)0;
}

static int getrandom_fills_what_it_is_asked_for(void) {
    unsigned char buffer[64];
    memset(buffer, 0, sizeof(buffer));
    if (getrandom(buffer, sizeof(buffer), 0) != (ssize_t)sizeof(buffer)) {
        return 198;
    }
    /* A heap buffer, which is a different region from the stack. */
    unsigned char *heap = malloc(64);
    if (!heap) {
        return 199;
    }
    memset(heap, 0, 64);
    ssize_t got = getrandom(heap, 64, 0);
    free(heap);
    if (got != 64) {
        return 200;
    }
    /* And from a thread, whose stack this kernel fills on demand. */
    worker_random_result = 0;
    pthread_t thread;
    if (pthread_create(&thread, (pthread_attr_t *)0, random_worker, (void *)0) != 0) {
        return 201;
    }
    if (pthread_join(thread, (void **)0) != 0) {
        return 202;
    }
    if (worker_random_result != 1) {
        return 203;
    }
    /* Eight bytes, which is the size base::RandUint64 asks for. */
    unsigned long long value = 0;
    if (getrandom(&value, sizeof(value), 0) != (ssize_t)sizeof(value)) {
        return 204;
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
    if ((rc = the_two_ucontext_layouts_agree()) != 0) {
        return rc;
    }
    if ((rc = a_handler_is_given_the_registers_it_interrupted()) != 0) {
        return rc;
    }
    if ((rc = an_edit_to_the_context_takes_effect()) != 0) {
        return rc;
    }
    if ((rc = a_semaphore_blocks_and_wakes()) != 0) {
        return rc;
    }
    if ((rc = the_thread_attributes_are_answered_truthfully()) != 0) {
        return rc;
    }
    if ((rc = the_machine_reports_one_priority()) != 0) {
        return rc;
    }
    if ((rc = mincore_reports_what_is_resident()) != 0) {
        return rc;
    }
    if ((rc = fionread_counts_what_is_waiting()) != 0) {
        return rc;
    }
    if ((rc = the_remaining_surface_behaves()) != 0) {
        return rc;
    }
    if ((rc = a_fault_says_which_kind_it_was()) != 0) {
        return rc;
    }
    if ((rc = the_last_of_the_base_surface_behaves()) != 0) {
        return rc;
    }
    if ((rc = getrandom_fills_what_it_is_asked_for()) != 0) {
        return rc;
    }
    return 0;
}
