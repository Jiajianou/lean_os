#include <stdio.h>
#include <string.h>

#include "syscall.h"
#include "syscall_wrappers.h"

static int failures;
static int checks;

static void check(int ok, const char *what, long num) {
    checks++;
    if (!ok) {
        printf("syscalltest: FAILED - syscall %ld: %s\n", num, what);
        failures++;
    }
}

#define KERNEL_ADDRESS     0xFFFF800000000000ULL
#define UNMAPPED_USER   0x0000700000000000ULL
#define NEAR_USER_TOP   0x00007FFFFFFFF000ULL
#define HUGE_LENGTH        0x7FFFFFFFFFFFFFFFULL

static const unsigned long long bad_ptrs[] = {
    0ULL, KERNEL_ADDRESS, UNMAPPED_USER, NEAR_USER_TOP, (unsigned long long)-1,
};
#define N_BAD_PTRS ((int)(sizeof(bad_ptrs) / sizeof(bad_ptrs[0])))

typedef enum { CLASS_SKIP, CLASS_PLAIN, CLASS_POINTER, CLASS_BLOCK } class_t;

#define UNOPENABLE_FILE_DESCRIPTOR 4242L

typedef struct {
    long num;
    class_t klass;
    int pointer_argument;
    const char *why;
} entry_t;

static const entry_t table[] = {
    {SYS_exit,          CLASS_SKIP, 0, "ends this process"},
    {SYS_shutdown,      CLASS_SKIP, 0, "switches the machine off"},
    {SYS_thread_exit,   CLASS_SKIP, 0, "ends this thread"},
    {SYS_sigreturn,     CLASS_SKIP, 0, "only valid on a signal frame; returns to a fabricated context"},
    {SYS_kill,          CLASS_SKIP, 0, "a wild pid argument could reach this process or the compositor"},
    {SYS_fork,          CLASS_SKIP, 0, "a child sweeping the table alongside its parent is not what is under test"},
    {SYS_execve,        CLASS_SKIP, 0, "replaces this program"},
    {SYS_spawn,         CLASS_SKIP, 0, "covered by M40's own failure-path self-test"},
    {SYS_close,         CLASS_SKIP, 0, "would close this program's stdout mid-report"},
    {SYS_dup2,          CLASS_SKIP, 0, "same - it rearranges the fd table this program is printing through"},
    {SYS_display_set_mode, CLASS_SKIP, 0, "changes the resolution out from under the desktop"},
    {SYS_settime,       CLASS_SKIP, 0, "moves the clock every other test is measured against"},
    {SYS_dropcaps,      CLASS_SKIP, 0, "capabilities only shrink - dropping them would disarm the rest of this sweep"},
    {SYS_sbrk,          CLASS_SKIP, 0, "a huge argument moves this program's own break; malloc would then be wrong"},
    {SYS_munmap,        CLASS_SKIP, 0, "could unmap this program's own text"},
    {SYS_mprotect,      CLASS_SKIP, 0, "could make this program's own text unreadable"},
    {SYS_setsid,        CLASS_SKIP, 0, "detaches this program from the session running it"},
    {SYS_setpgid,       CLASS_SKIP, 0, "same"},
    {SYS_thread_create, CLASS_SKIP, 0, "a thread entered at a wild address is a fault by construction"},

    {SYS_write,         CLASS_BLOCK, 2, NULL},
    {SYS_read,          CLASS_BLOCK, 2, NULL},
    {SYS_readfile,      CLASS_POINTER, 1, NULL},
    {SYS_writefile,     CLASS_POINTER, 1, NULL},
    {SYS_listdir,       CLASS_POINTER, 1, NULL},
    {SYS_pipe,          CLASS_POINTER, 1, NULL},
    {SYS_framebuffer_info,       CLASS_POINTER, 1, NULL},
    {SYS_pipe_open,     CLASS_POINTER, 1, NULL},
    {SYS_clipboard_set, CLASS_POINTER, 1, NULL},
    {SYS_clipboard_get, CLASS_POINTER, 1, NULL},
    {SYS_taskinfo,      CLASS_POINTER, 1, NULL},
    {SYS_mkdir,         CLASS_POINTER, 1, NULL},
    {SYS_unlink,        CLASS_POINTER, 1, NULL},
    {SYS_rename,        CLASS_POINTER, 1, NULL},
    {SYS_rename_replace,CLASS_POINTER, 1, NULL},
    {SYS_open,          CLASS_POINTER, 1, NULL},
    {SYS_stat,          CLASS_POINTER, 1, NULL},
    {SYS_lstat,         CLASS_POINTER, 1, NULL},
    {SYS_fstat,         CLASS_POINTER, 2, NULL},
    {SYS_rmdir,         CLASS_POINTER, 1, NULL},
    {SYS_display_modes, CLASS_POINTER, 1, NULL},
    {SYS_bind,          CLASS_POINTER, 2, NULL},
    {SYS_sendto,        CLASS_POINTER, 2, NULL},
    {SYS_netconf,       CLASS_POINTER, 1, NULL},
    {SYS_connstat,      CLASS_POINTER, 2, NULL},
    {SYS_send,          CLASS_POINTER, 2, NULL},
    {SYS_kernel_log,          CLASS_POINTER, 1, NULL},
    {SYS_chdir,         CLASS_POINTER, 1, NULL},
    {SYS_getcwd,        CLASS_POINTER, 1, NULL},
    {SYS_sigaction,     CLASS_PLAIN, 0, NULL},
    {SYS_getdents,      CLASS_POINTER, 2, NULL},
    {SYS_symlink,       CLASS_POINTER, 1, NULL},
    {SYS_readlink,      CLASS_POINTER, 2, NULL},
    {SYS_link,          CLASS_POINTER, 1, NULL},
    {SYS_audio_play,    CLASS_POINTER, 1, NULL},
    {SYS_ioctl,         CLASS_POINTER, 3, NULL},
    {SYS_profile,       CLASS_POINTER, 2, NULL},

    {SYS_wait,          CLASS_BLOCK, 0, NULL},
    {SYS_waitpid,       CLASS_BLOCK, 0, NULL},
    {SYS_waitfds,       CLASS_BLOCK, 1, NULL},
    {SYS_keyboard_read,      CLASS_BLOCK, 0, NULL},
    {SYS_mouse_read,    CLASS_BLOCK, 1, NULL},
    {SYS_accept,        CLASS_BLOCK, 0, NULL},
    {SYS_connect,       CLASS_BLOCK, 2, NULL},
    {SYS_receive,          CLASS_BLOCK, 2, NULL},
    {SYS_peek,          CLASS_BLOCK, 2, NULL},
    {SYS_recvfrom,      CLASS_BLOCK, 2, NULL},

    {SYS_getpid,        CLASS_PLAIN, 0, NULL},
    {SYS_wait_nb,       CLASS_PLAIN, 0, NULL},
    {SYS_getpgid,       CLASS_PLAIN, 0, NULL},
    {SYS_getsid,        CLASS_PLAIN, 0, NULL},
    {SYS_gettid,        CLASS_PLAIN, 0, NULL},
    {SYS_shared_memory_create,    CLASS_PLAIN, 0, NULL},
    {SYS_shared_memory_map,       CLASS_PLAIN, 0, NULL},
    {SYS_shared_memory_free,      CLASS_PLAIN, 0, NULL},
    {SYS_shared_memory_unmap,     CLASS_PLAIN, 0, NULL},
    {SYS_framebuffer_map,        CLASS_PLAIN, 0, NULL},
    {SYS_keyboard_modifiers, CLASS_PLAIN, 0, NULL},
    {SYS_pipe_poll,     CLASS_PLAIN, 0, NULL},
    {SYS_pipe_reset,    CLASS_PLAIN, 0, NULL},
    {SYS_uptime_ms,     CLASS_PLAIN, 0, NULL},
    {SYS_clock_ns,      CLASS_PLAIN, 0, NULL},
    {SYS_thread_detach, CLASS_PLAIN, 0, NULL},
    {SYS_idle_ticks,    CLASS_PLAIN, 0, NULL},
    {SYS_yield,         CLASS_PLAIN, 0, NULL},
    {SYS_task_alive,    CLASS_PLAIN, 0, NULL},
    {SYS_lseek,         CLASS_PLAIN, 0, NULL},
    {SYS_time,          CLASS_PLAIN, 0, NULL},
    {SYS_audio_claim,   CLASS_PLAIN, 0, NULL},
    {SYS_audio_release, CLASS_PLAIN, 0, NULL},
    {SYS_audio_volume,  CLASS_PLAIN, 0, NULL},
    {SYS_beep,          CLASS_PLAIN, 0, NULL},
    {SYS_socket,        CLASS_PLAIN, 0, NULL},
    {SYS_listen,        CLASS_PLAIN, 0, NULL},
    {SYS_sockpoll,      CLASS_PLAIN, 0, NULL},
    {SYS_getcaps,       CLASS_PLAIN, 0, NULL},
    {SYS_kernel_log_total,    CLASS_PLAIN, 0, NULL},
    {SYS_sigprocmask,   CLASS_PLAIN, 0, NULL},
    {SYS_mmap,          CLASS_PLAIN, 0, NULL},
    {SYS_madvise,       CLASS_PLAIN, 0, NULL},
    {SYS_fcntl,         CLASS_PLAIN, 0, NULL},
    {SYS_ftruncate,     CLASS_PLAIN, 0, NULL},
    {SYS_fsync,         CLASS_PLAIN, 0, NULL},

    {SYS_rusage,        CLASS_POINTER, 2, NULL},
    {SYS_statvfs,       CLASS_POINTER, 1, NULL},
    {SYS_utime,         CLASS_POINTER, 1, NULL},
    {SYS_fdpath,        CLASS_POINTER, 2, NULL},
    {SYS_getppid,       CLASS_PLAIN, 0, NULL},
    {SYS_sync,          CLASS_PLAIN, 0, NULL},
    {SYS_meminfo,       CLASS_POINTER, 1, NULL},
    {SYS_alarm,         CLASS_PLAIN, 0, NULL},
    {SYS_msync,         CLASS_PLAIN, 0, NULL},
    {SYS_arch_prctl,    CLASS_PLAIN, 0, NULL},
    {SYS_futex,         CLASS_PLAIN, 0, NULL},
    {SYS_getrandom,     CLASS_POINTER, 1, NULL},
    {SYS_pread,         CLASS_POINTER, 2, NULL},
    {SYS_pwrite,        CLASS_POINTER, 2, NULL},

    {SYS_socketpair,    CLASS_POINTER, 2, NULL},
    {SYS_bindun,        CLASS_POINTER, 2, NULL},
    {SYS_connectun,     CLASS_POINTER, 2, NULL},
    {SYS_sendmsg,       CLASS_BLOCK, 2, NULL},
    {SYS_recvmsg,       CLASS_BLOCK, 2, NULL},
    {SYS_sockshut,      CLASS_PLAIN, 0, NULL},

    {SYS_epoll_wait,      CLASS_BLOCK, 2, NULL},
    {SYS_timerfd_settime, CLASS_POINTER, 3, NULL},
    {SYS_timerfd_gettime, CLASS_POINTER, 2, NULL},
    {SYS_epoll_create,   CLASS_SKIP, 0, "creates a descriptor for any argument; checked by name in section 7"},
    {SYS_eventfd,        CLASS_SKIP, 0, "same - and a kernel address is a perfectly good initial count"},
    {SYS_timerfd_create, CLASS_SKIP, 0, "same"},
    {SYS_epoll_control,      CLASS_SKIP, 0, "its pointer is argument 4, past this table's reach; checked by name in section 7"},

    {SYS_memfd_create,   CLASS_SKIP, 0, "creates a descriptor and takes a pointer; checked by name in section 7"},
    {SYS_memfd_seal,     CLASS_PLAIN, 0, NULL},

    {SYS_unix_peer_credentials, CLASS_POINTER, 2, NULL},
    {SYS_sigaltstack,    CLASS_POINTER, 1, NULL},
    {SYS_mincore,        CLASS_POINTER, 3, NULL},
    {SYS_sockname,       CLASS_POINTER, 2, NULL},

    {SYS_thread_setname, CLASS_POINTER, 1, NULL},
    {SYS_thread_getname, CLASS_POINTER, 1, NULL},

    /* M169. The pointer is the whole reason these are system calls: a
       getrlimit that cannot write its answer has to say EFAULT rather than
       take the signal, which is what a C library answering for itself
       does. */
    {SYS_getrlimit,      CLASS_POINTER, 2, NULL},
    {SYS_setrlimit,      CLASS_POINTER, 2, NULL},
};
#define N_TABLE ((int)(sizeof(table) / sizeof(table[0])))

static const entry_t *lookup(long num) {
    for (int i = 0; i < N_TABLE; i++) {
        if (table[i].num == num) {
            return &table[i];
        }
    }
    return NULL;
}

int main(void) {
    printf("syscalltest: sweeping %d syscall numbers\n", SYSCALL_COUNT);
    fflush(stdout);

    int unclassified = 0;
    for (long n = 0; n < SYSCALL_COUNT; n++) {
        if (!lookup(n)) {
            printf("syscalltest: syscall %ld is not classified in this file's table\n", n);
            unclassified++;
        }
    }
    check(unclassified == 0,
          "some syscalls are not classified here - add them to the table", -1);

    int swept = 0;
    for (long n = 0; n < SYSCALL_COUNT; n++) {
        const entry_t *e = lookup(n);
        if (!e || e->klass == CLASS_SKIP) {
            continue;
        }
        for (int p = 0; p < N_BAD_PTRS; p++) {
            unsigned long long bad = bad_ptrs[p];
            if (e->klass == CLASS_BLOCK) {
                switch (e->pointer_argument) {
                    case 1: (void)sys_raw(n, (long)bad, 1, 0); break;
                    case 2: (void)sys_raw(n, UNOPENABLE_FILE_DESCRIPTOR, (long)bad, 1); break;
                    case 3: (void)sys_raw(n, UNOPENABLE_FILE_DESCRIPTOR, 1, (long)bad); break;
                    default: (void)sys_raw(n, UNOPENABLE_FILE_DESCRIPTOR, 0, 0); break;
                }
                continue;
            }
            (void)sys_raw(n, (long)bad, 0, 0);
            (void)sys_raw(n, 0, (long)bad, 0);
            (void)sys_raw(n, 0, 0, (long)bad);
            (void)sys_raw(n, (long)bad, (long)HUGE_LENGTH, (long)bad);
        }
        swept++;
    }
    printf("syscalltest: swept %d syscalls x %d hostile arguments, all returned\n",
           swept, N_BAD_PTRS * 4);
    fflush(stdout);

    int pointer_checked = 0;
    for (int i = 0; i < N_TABLE; i++) {
        const entry_t *e = &table[i];
        if (e->klass != CLASS_POINTER && !(e->klass == CLASS_BLOCK && e->pointer_argument)) {
            continue;
        }
        long fd = (e->klass == CLASS_BLOCK) ? UNOPENABLE_FILE_DESCRIPTOR : 1;
        for (int p = 0; p < N_BAD_PTRS; p++) {
            unsigned long long bad = bad_ptrs[p];
            long r;
            switch (e->pointer_argument) {
                case 1: r = sys_raw(e->num, (long)bad, 64, 64); break;
                case 2: r = sys_raw(e->num, fd, (long)bad, 64); break;
                default: r = sys_raw(e->num, fd, 64, (long)bad); break;
            }
            check(r < 0,
                  "a kernel or unmapped address was accepted rather than refused",
                  e->num);
            pointer_checked++;
        }
    }
    printf("syscalltest: %d pointer-argument refusals checked\n", pointer_checked);

    static const long bad_file_descriptors[] = {-1, -2, 127, 128, 1000, 0x7FFFFFFF};
    char scratch[64];
    for (int i = 0; i < (int)(sizeof(bad_file_descriptors) / sizeof(bad_file_descriptors[0])); i++) {
        long fd = bad_file_descriptors[i];
        check(sys_raw(SYS_read, fd, (long)scratch, sizeof(scratch)) < 0,
              "read from an fd that was never opened", SYS_read);
        check(sys_raw(SYS_write, fd, (long)scratch, sizeof(scratch)) < 0,
              "write to an fd that was never opened", SYS_write);
        check(sys_raw(SYS_lseek, fd, 0, 0) < 0,
              "lseek on an fd that was never opened", SYS_lseek);
        check(sys_raw(SYS_fstat, fd, (long)scratch, 0) < 0,
              "fstat on an fd that was never opened", SYS_fstat);
    }

    static const long bogus[] = {SYSCALL_COUNT, SYSCALL_COUNT + 1, 500, 0x7FFFFFFF, -1, -99};
    for (int i = 0; i < (int)(sizeof(bogus) / sizeof(bogus[0])); i++) {
        check(sys_raw(bogus[i], 0, 0, 0) < 0,
              "a syscall number outside the table was not refused", bogus[i]);
    }

    check(sys_raw(SYS_write, 1, (long)scratch, (long)HUGE_LENGTH) < 0,
          "a length that overflows its base was accepted", SYS_write);
    check(sys_raw(SYS_read, 0, (long)scratch, (long)HUGE_LENGTH) < 0,
          "a length that overflows its base was accepted", SYS_read);

    for (int p = 0; p < N_BAD_PTRS; p++) {
        unsigned long long bad = bad_ptrs[p];
        if (bad == 0 || bad == 1) {
            continue;
        }
        check(sys_raw(SYS_sigaction, 1, (long)bad, (long)bad) < 0,
              "sigaction accepted a handler outside the caller's own image", SYS_sigaction);
        check(sys_raw(SYS_sigaction, 1, (long)(uintptr_t)main, (long)bad) < 0,
              "sigaction accepted a restorer outside the caller's own image", SYS_sigaction);
    }
    check(sys_raw(SYS_sigaction, 1, 0, 0) >= 0, "SIG_DFL was refused", SYS_sigaction);
    check(sys_raw(SYS_sigaction, 1, 1, 0) >= 0, "SIG_IGN was refused", SYS_sigaction);

    {
        check(sys_raw(SYS_eventfd, 0, 0x4, 0) < 0,
              "eventfd accepted a flag this kernel does not have", SYS_eventfd);
        check(sys_raw(SYS_eventfd, 0, (long)0xFFFFFFFF, 0) < 0,
              "eventfd accepted every flag at once", SYS_eventfd);
        long efd = sys_raw(SYS_eventfd, 1, 1  , 0);
        check(efd >= 0, "eventfd refused EFD_SEMAPHORE", SYS_eventfd);
        if (efd >= 0) {
            sys_raw(SYS_close, efd, 0, 0);
        }

        check(sys_raw(SYS_timerfd_create, 7, 0, 0) < 0,
              "timerfd_create accepted a clock that does not exist", SYS_timerfd_create);
        check(sys_raw(SYS_timerfd_create, 1, 0x4, 0) < 0,
              "timerfd_create accepted a flag this kernel does not have", SYS_timerfd_create);
        long tfd = sys_raw(SYS_timerfd_create, 1  , 0, 0);
        check(tfd >= 0, "timerfd_create refused CLOCK_MONOTONIC", SYS_timerfd_create);

        check(sys_raw(SYS_epoll_create, 0x4, 0, 0) < 0,
              "epoll_create accepted a flag this kernel does not have", SYS_epoll_create);
        long epfd = sys_raw(SYS_epoll_create, 0, 0, 0);
        check(epfd >= 0, "epoll_create refused a plain set", SYS_epoll_create);

        if (epfd >= 0 && tfd >= 0) {
            for (int p = 0; p < N_BAD_PTRS; p++) {
                unsigned long long bad = bad_ptrs[p];
                check(sys_epoll_control((int)epfd, 1  , (int)tfd,
                                    (const os_epoll_event_t *)(uintptr_t)bad) < 0,
                      "epoll_ctl accepted an event structure at a kernel or unmapped address",
                      SYS_epoll_control);
            }
            os_epoll_event_t ev = {1  , 0, 0};
            check(sys_epoll_control((int)epfd, 1, (int)epfd, &ev) < 0,
                  "epoll_ctl accepted an epoll set watching itself", SYS_epoll_control);
            check(sys_epoll_control((int)epfd, 1, 127, &ev) < 0,
                  "epoll_ctl accepted a descriptor that was never opened", SYS_epoll_control);
            check(sys_epoll_control((int)epfd, 1, (int)tfd, &ev) == 0,
                  "epoll_ctl refused a valid registration", SYS_epoll_control);
        }
        for (int p = 0; p < N_BAD_PTRS; p++) {
            unsigned long long bad = bad_ptrs[p];
            if (bad == 0) {
                continue;
            }
            long r = sys_raw(SYS_memfd_create, (long)bad, 0, 0);
            check(r < 0, "memfd_create accepted a name at a kernel or unmapped address",
                  SYS_memfd_create);
            if (r >= 0) {
                sys_raw(SYS_close, r, 0, 0);
            }
        }
        check(sys_raw(SYS_memfd_create, 0, 0x4, 0) < 0,
              "memfd_create accepted a flag this kernel does not have", SYS_memfd_create);
        long mfd = sys_raw(SYS_memfd_create, 0, 2  , 0);
        check(mfd >= 0, "memfd_create refused MFD_ALLOW_SEALING", SYS_memfd_create);
        if (mfd >= 0) {
            check(sys_raw(SYS_memfd_seal, mfd, 0x40, 0) < 0,
                  "memfd_seal accepted a seal this kernel does not have", SYS_memfd_seal);
            check(sys_raw(SYS_memfd_seal, mfd, 0, 0) == 0,
                  "memfd_seal could not report an unsealed descriptor's seals",
                  SYS_memfd_seal);
            sys_raw(SYS_close, mfd, 0, 0);
        }
        check(sys_raw(SYS_memfd_seal, 1, 0, 0) < 0,
              "memfd_seal accepted stdout", SYS_memfd_seal);

        if (tfd >= 0) {
            sys_raw(SYS_close, tfd, 0, 0);
        }
        if (epfd >= 0) {
            sys_raw(SYS_close, epfd, 0, 0);
        }
    }

    {
        /* M141. sigaltstack's SECOND argument is a pointer too, and the table
           above can only name one per syscall. bad_ptrs[0] is skipped on
           purpose: a null pointer is not a bad address to this call, it is
           how POSIX spells "do not report the old stack", and
           sigaltstack(NULL, NULL) returning 0 is the specified behaviour
           rather than a refusal this kernel forgot. */
        for (int p = 1; p < N_BAD_PTRS; p++) {
            check(sys_raw(SYS_sigaltstack, 0, (long)bad_ptrs[p], 0) < 0,
                  "sigaltstack wrote the old stack through a bad pointer",
                  SYS_sigaltstack);
        }
        check(sys_raw(SYS_sigaltstack, 0, 0, 0) == 0,
              "sigaltstack(NULL, NULL) is a query POSIX allows, not an error",
              SYS_sigaltstack);
        /* And SO_PEERCRED has to refuse a descriptor that is not a socket at
           all rather than answer about one. */
        char credentials[32];
        check(sys_raw(SYS_unix_peer_credentials, 1, (long)credentials, 0) < 0,
              "peer credentials were reported for stdout",
              SYS_unix_peer_credentials);
        check(sys_raw(SYS_unix_peer_credentials, UNOPENABLE_FILE_DESCRIPTOR,
                      (long)credentials, 0) < 0,
              "peer credentials were reported for an fd that was never opened",
              SYS_unix_peer_credentials);
    }

    printf("syscalltest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
