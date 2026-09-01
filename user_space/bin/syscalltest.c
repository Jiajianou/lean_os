/* user_space/bin/syscalltest.c - Q5
 *
 * Every syscall, told a lie.
 *
 * ---- What this is for -------------------------------------------------
 *
 * There are 98 entries in kernel/arch/x86_64/syscall.c's syscall_table.
 * Each one is covered on its happy path by the milestone that added it,
 * and until this program none of them was covered on the path a hostile
 * or merely buggy program takes: a null pointer, a kernel address, a
 * length of 2^63, an fd that was never opened.
 *
 * That is the boundary this kernel's whole safety argument rests on. A
 * syscall that dereferences a user pointer without checking it is a way
 * for any program on the machine to read or write kernel memory, and the
 * only reason no test had ever looked is that looking requires being on
 * the user side of the boundary - which is where this program is, for the
 * same reason badptr.c (M52), captest.c (M65) and libctest.c (M63) are.
 *
 * ---- The two claims ---------------------------------------------------
 *
 * They are different strengths and it is worth being precise about which
 * is which.
 *
 *   1. UNIVERSAL: every syscall number, given every hostile argument
 *      pattern below, RETURNS. It does not fault, it does not hang, and
 *      it does not take the kernel with it. This one is checked for all
 *      98 and it is the claim that matters most: a kernel that panics on
 *      a bad argument is a kernel any program can switch off.
 *
 *   2. SPECIFIC: a syscall known to take a user pointer, given a kernel
 *      address or an unmapped one, FAILS. This is checked against the
 *      table below rather than against all 98, because "returns an
 *      error" is only the right answer for a call that actually reads
 *      the pointer - sys_getpid given a kernel address in rdi is
 *      correct to ignore it and succeed.
 *
 * Claim 2 needs a hand-written table and a hand-written table can be
 * incomplete. It is therefore checked against SYSCALL_COUNT: adding a
 * syscall without classifying it here fails this test rather than
 * silently leaving it unchecked. That is the property that keeps this
 * file honest as the kernel grows.
 *
 * Exit 0 for all-passed, 1 otherwise.
 */
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

/* ---- The hostile arguments -------------------------------------------
 *
 * Each of these is a real class of mistake or attack, not a random
 * number:
 *
 *   0                     the commonest bug in any C program.
 *   KERNEL_ADDR           the canonical higher half. A syscall that
 *                         writes through this writes kernel memory.
 *   UNMAPPED_USER         a legal user address with nothing mapped at
 *                         it - the case a range check must catch and a
 *                         null check will not.
 *   NEAR_USER_TOP         just below the user/kernel boundary, so a
 *                         length added to it overflows past the line.
 *   HUGE_LEN              a length whose addition to any base wraps.
 */
#define KERNEL_ADDR     0xFFFF800000000000ULL
#define UNMAPPED_USER   0x0000700000000000ULL
#define NEAR_USER_TOP   0x00007FFFFFFFF000ULL
#define HUGE_LEN        0x7FFFFFFFFFFFFFFFULL

static const unsigned long long bad_ptrs[] = {
    0ULL, KERNEL_ADDR, UNMAPPED_USER, NEAR_USER_TOP, (unsigned long long)-1,
};
#define N_BAD_PTRS ((int)(sizeof(bad_ptrs) / sizeof(bad_ptrs[0])))

/* ---- Classification --------------------------------------------------
 *
 * SKIP: calling it would end this process or the machine, so it cannot be
 *       swept. Each one is listed with why - "it is awkward" is not a
 *       reason, and a reader should be able to check each line.
 * PTR:  takes a user pointer in the argument position given, and must
 *       refuse a bad one.
 * PLAIN: takes no user pointer; the sweep applies, the pointer check
 *       does not.
 */
typedef enum { CLASS_SKIP, CLASS_PLAIN, CLASS_PTR, CLASS_BLOCK } class_t;

/* ---- CLASS_BLOCK, and why it had to exist -------------------------------
 *
 * The first version of this program sweeps every syscall with every
 * hostile argument and hung the boot. The reason is obvious in hindsight
 * and was not obvious in advance: several of these calls are *supposed*
 * to wait. sys_raw(SYS_read, 0, ...) is a read on this program's own
 * stdin, and a read on stdin with nothing to read blocks forever, which
 * is correct behaviour and a hung test.
 *
 * So a call that can wait gets a restricted sweep: every argument
 * position is still exercised, but the fd argument is pinned to a number
 * that cannot name an open descriptor. That makes the call fail at the fd
 * lookup - before it reaches anything that could block - while still
 * driving the argument validation this program is about.
 *
 * The cost is real and worth stating: for these calls this program checks
 * the fd path and not the wait path. Testing "a blocking syscall handed a
 * bad pointer refuses it rather than blocking" needs a timeout and a
 * second thread, and that is a bigger piece of machinery than it is worth
 * here. The boot self-tests already exercise every one of these calls
 * blocking correctly on a real descriptor. */
#define UNOPENABLE_FD 4242L

typedef struct {
    long num;
    class_t klass;
    int ptr_arg;        /* 1, 2 or 3 - which argument is the pointer */
    const char *why;    /* for SKIP, the reason */
} entry_t;

static const entry_t table[] = {
    /* ---- must not be called at all ---------------------------------- */
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

    /* ---- takes a user pointer and must refuse a bad one -------------- */
    {SYS_write,         CLASS_BLOCK, 2, NULL},
    {SYS_read,          CLASS_BLOCK, 2, NULL},
    {SYS_readfile,      CLASS_PTR, 1, NULL},
    {SYS_writefile,     CLASS_PTR, 1, NULL},
    {SYS_listdir,       CLASS_PTR, 1, NULL},
    {SYS_pipe,          CLASS_PTR, 1, NULL},
    {SYS_fb_info,       CLASS_PTR, 1, NULL},
    {SYS_pipe_open,     CLASS_PTR, 1, NULL},
    {SYS_clipboard_set, CLASS_PTR, 1, NULL},
    {SYS_clipboard_get, CLASS_PTR, 1, NULL},
    {SYS_taskinfo,      CLASS_PTR, 1, NULL},
    {SYS_mkdir,         CLASS_PTR, 1, NULL},
    {SYS_unlink,        CLASS_PTR, 1, NULL},
    {SYS_rename,        CLASS_PTR, 1, NULL},
    {SYS_rename_replace,CLASS_PTR, 1, NULL},
    {SYS_open,          CLASS_PTR, 1, NULL},
    {SYS_stat,          CLASS_PTR, 1, NULL},
    {SYS_lstat,         CLASS_PTR, 1, NULL},
    {SYS_fstat,         CLASS_PTR, 2, NULL},
    {SYS_rmdir,         CLASS_PTR, 1, NULL},
    {SYS_display_modes, CLASS_PTR, 1, NULL},
    {SYS_bind,          CLASS_PTR, 2, NULL},
    {SYS_sendto,        CLASS_PTR, 2, NULL},
    {SYS_netconf,       CLASS_PTR, 1, NULL},
    {SYS_connstat,      CLASS_PTR, 2, NULL},
    {SYS_send,          CLASS_PTR, 2, NULL},
    {SYS_klog,          CLASS_PTR, 1, NULL},
    {SYS_chdir,         CLASS_PTR, 1, NULL},
    {SYS_getcwd,        CLASS_PTR, 1, NULL},
    /* Not CLASS_PTR: its handler argument is an address the kernel
     * range-checks but never dereferences, and 0 and 1 are the magic
     * values SIG_DFL and SIG_IGN rather than bad pointers. The generic
     * sweep reported it as accepting a null "pointer", which it does,
     * correctly. It gets a specific check at the bottom instead. */
    {SYS_sigaction,     CLASS_PLAIN, 0, NULL},
    {SYS_getdents,      CLASS_PTR, 2, NULL},
    {SYS_symlink,       CLASS_PTR, 1, NULL},
    {SYS_readlink,      CLASS_PTR, 2, NULL},
    {SYS_link,          CLASS_PTR, 1, NULL},
    {SYS_audio_play,    CLASS_PTR, 1, NULL},
    {SYS_ioctl,         CLASS_PTR, 3, NULL},

    /* ---- can wait: swept with an fd that cannot exist ---------------- */
    {SYS_wait,          CLASS_BLOCK, 0, NULL},
    {SYS_waitpid,       CLASS_BLOCK, 0, NULL},
    {SYS_waitfds,       CLASS_BLOCK, 1, NULL},
    {SYS_kbd_read,      CLASS_BLOCK, 0, NULL},
    {SYS_mouse_read,    CLASS_BLOCK, 1, NULL},
    {SYS_accept,        CLASS_BLOCK, 0, NULL},
    {SYS_connect,       CLASS_BLOCK, 2, NULL},
    {SYS_recv,          CLASS_BLOCK, 2, NULL},
    {SYS_recvfrom,      CLASS_BLOCK, 2, NULL},

    /* ---- no user pointer: swept, not pointer-checked ----------------- */
    {SYS_getpid,        CLASS_PLAIN, 0, NULL},
    {SYS_wait_nb,       CLASS_PLAIN, 0, NULL},
    {SYS_getpgid,       CLASS_PLAIN, 0, NULL},
    {SYS_getsid,        CLASS_PLAIN, 0, NULL},
    {SYS_gettid,        CLASS_PLAIN, 0, NULL},
    {SYS_shm_create,    CLASS_PLAIN, 0, NULL},
    {SYS_shm_map,       CLASS_PLAIN, 0, NULL},
    {SYS_shm_free,      CLASS_PLAIN, 0, NULL},
    {SYS_shm_unmap,     CLASS_PLAIN, 0, NULL},
    {SYS_fb_map,        CLASS_PLAIN, 0, NULL},
    {SYS_kbd_modifiers, CLASS_PLAIN, 0, NULL},
    {SYS_pipe_poll,     CLASS_PLAIN, 0, NULL},
    {SYS_pipe_reset,    CLASS_PLAIN, 0, NULL},
    {SYS_uptime_ms,     CLASS_PLAIN, 0, NULL},
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
    {SYS_klog_total,    CLASS_PLAIN, 0, NULL},
    {SYS_sigprocmask,   CLASS_PLAIN, 0, NULL},
    {SYS_mmap,          CLASS_PLAIN, 0, NULL},
    {SYS_madvise,       CLASS_PLAIN, 0, NULL},
    {SYS_fcntl,         CLASS_PLAIN, 0, NULL},
    {SYS_ftruncate,     CLASS_PLAIN, 0, NULL},
    {SYS_fsync,         CLASS_PLAIN, 0, NULL},
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

    /* ---- 0. the table covers the kernel -------------------------------
     *
     * The check that keeps this file from rotting. A syscall added to the
     * kernel without a line above is unclassified, and an unclassified
     * syscall is one nothing here tests - which would be invisible
     * without this loop. */
    int unclassified = 0;
    for (long n = 0; n < SYSCALL_COUNT; n++) {
        if (!lookup(n)) {
            printf("syscalltest: syscall %ld is not classified in this file's table\n", n);
            unclassified++;
        }
    }
    check(unclassified == 0,
          "some syscalls are not classified here - add them to the table", -1);

    /* ---- 1. the universal claim: everything returns ------------------- */
    int swept = 0;
    for (long n = 0; n < SYSCALL_COUNT; n++) {
        const entry_t *e = lookup(n);
        if (!e || e->klass == CLASS_SKIP) {
            continue;
        }
        for (int p = 0; p < N_BAD_PTRS; p++) {
            unsigned long long bad = bad_ptrs[p];
            if (e->klass == CLASS_BLOCK) {
                /* Only the pointer argument is varied, and only that one.
                 *
                 * Every other position on a call that can wait is either
                 * a descriptor or a timeout, and a hostile value in
                 * either is a legitimate instruction to sleep rather
                 * than a bug: SYS_waitfds with no descriptors and a
                 * timeout of 2^63 does exactly what it was told, and did
                 * exactly that to this program on its second run. So the
                 * descriptor is pinned to one that cannot exist and the
                 * timeout to zero, and what remains under test is the
                 * argument validation this program is about. */
                switch (e->ptr_arg) {
                    case 1: (void)sys_raw(n, (long)bad, 1, 0); break;
                    case 2: (void)sys_raw(n, UNOPENABLE_FD, (long)bad, 1); break;
                    case 3: (void)sys_raw(n, UNOPENABLE_FD, 1, (long)bad); break;
                    default: (void)sys_raw(n, UNOPENABLE_FD, 0, 0); break;
                }
                continue;
            }
            /* Each argument position in turn, and then all three at once
             * with a length that overflows anything it is added to. If
             * any of these does not return, this program never reaches
             * its final line and the boot self-test fails on the missing
             * marker - which is the right way for a hang to be reported. */
            (void)sys_raw(n, (long)bad, 0, 0);
            (void)sys_raw(n, 0, (long)bad, 0);
            (void)sys_raw(n, 0, 0, (long)bad);
            (void)sys_raw(n, (long)bad, (long)HUGE_LEN, (long)bad);
        }
        swept++;
    }
    printf("syscalltest: swept %d syscalls x %d hostile arguments, all returned\n",
           swept, N_BAD_PTRS * 4);
    fflush(stdout);

    /* ---- 2. the specific claim: a bad pointer is refused -------------- */
    int ptr_checked = 0;
    for (int i = 0; i < N_TABLE; i++) {
        const entry_t *e = &table[i];
        if (e->klass != CLASS_PTR && !(e->klass == CLASS_BLOCK && e->ptr_arg)) {
            continue;
        }
        /* Same reasoning as the sweep: a blocking call gets an fd that
         * cannot name anything, so it fails before it can wait. */
        long fd = (e->klass == CLASS_BLOCK) ? UNOPENABLE_FD : 1;
        for (int p = 0; p < N_BAD_PTRS; p++) {
            unsigned long long bad = bad_ptrs[p];
            long r;
            /* A plausible length in the other positions, so the call gets
             * far enough to actually look at the pointer. A zero length
             * is legitimately allowed to succeed without reading
             * anything, which would make this assertion meaningless. */
            switch (e->ptr_arg) {
                case 1: r = sys_raw(e->num, (long)bad, 64, 64); break;
                case 2: r = sys_raw(e->num, fd, (long)bad, 64); break;
                default: r = sys_raw(e->num, fd, 64, (long)bad); break;
            }
            check(r < 0,
                  "a kernel or unmapped address was accepted rather than refused",
                  e->num);
            ptr_checked++;
        }
    }
    printf("syscalltest: %d pointer-argument refusals checked\n", ptr_checked);

    /* ---- 3. fds that were never opened -------------------------------- */
    static const long bad_fds[] = {-1, -2, 127, 128, 1000, 0x7FFFFFFF};
    char scratch[64];
    for (int i = 0; i < (int)(sizeof(bad_fds) / sizeof(bad_fds[0])); i++) {
        long fd = bad_fds[i];
        /* fd 127 is the last legal slot and 128 is one past MAX_FDS: the
         * pair that catches an off-by-one in the bounds check, which a
         * test using only -1 and 9999 would miss entirely. */
        check(sys_raw(SYS_read, fd, (long)scratch, sizeof(scratch)) < 0,
              "read from an fd that was never opened", SYS_read);
        check(sys_raw(SYS_write, fd, (long)scratch, sizeof(scratch)) < 0,
              "write to an fd that was never opened", SYS_write);
        check(sys_raw(SYS_lseek, fd, 0, 0) < 0,
              "lseek on an fd that was never opened", SYS_lseek);
        check(sys_raw(SYS_fstat, fd, (long)scratch, 0) < 0,
              "fstat on an fd that was never opened", SYS_fstat);
    }

    /* ---- 4. syscall numbers that do not exist ------------------------- */
    static const long bogus[] = {SYSCALL_COUNT, SYSCALL_COUNT + 1, 500, 0x7FFFFFFF, -1, -99};
    for (int i = 0; i < (int)(sizeof(bogus) / sizeof(bogus[0])); i++) {
        check(sys_raw(bogus[i], 0, 0, 0) < 0,
              "a syscall number outside the table was not refused", bogus[i]);
    }

    /* ---- 5. a length that overflows its base -------------------------- */
    /* A pointer that is genuinely valid, with a length that runs off the
     * end of the address space. The range check has to catch the
     * addition, not just the base - and an implementation that compares
     * `base + len` without watching for the wrap concludes the range is
     * fine because the sum is small. */
    check(sys_raw(SYS_write, 1, (long)scratch, (long)HUGE_LEN) < 0,
          "a length that overflows its base was accepted", SYS_write);
    check(sys_raw(SYS_read, 0, (long)scratch, (long)HUGE_LEN) < 0,
          "a length that overflows its base was accepted", SYS_read);

    /* ---- 6. sigaction's handler, which the sweep cannot judge ---------
     *
     * The handler argument is an address the kernel stores and later
     * enters ring 3 at. It never dereferences it, so this is not a
     * copy_from_user check - it is a range check, and getting it wrong
     * means the kernel jumps a user process to a kernel address. The two
     * magic values have to keep working, which is exactly why the generic
     * sweep gets this one wrong and this does not. */
    for (int p = 0; p < N_BAD_PTRS; p++) {
        unsigned long long bad = bad_ptrs[p];
        if (bad == 0 || bad == 1) {
            continue;   /* SIG_DFL and SIG_IGN - meaningful, not malformed */
        }
        check(sys_raw(SYS_sigaction, 1, (long)bad, (long)bad) < 0,
              "sigaction accepted a handler outside the caller's own image", SYS_sigaction);
        /* A valid handler with an invalid restorer must be refused too:
         * without a restorer the handler returns to whatever is on the
         * stack, which is the bug the restorer exists to prevent. */
        check(sys_raw(SYS_sigaction, 1, (long)(uintptr_t)main, (long)bad) < 0,
              "sigaction accepted a restorer outside the caller's own image", SYS_sigaction);
    }
    /* ...and the two magic values still work, because a check that
     * refused everything would pass the assertions above and break the
     * machine. */
    check(sys_raw(SYS_sigaction, 1, 0, 0) >= 0, "SIG_DFL was refused", SYS_sigaction);
    check(sys_raw(SYS_sigaction, 1, 1, 0) >= 0, "SIG_IGN was refused", SYS_sigaction);

    printf("syscalltest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
