#include "syscall_entry.h"

#include <stdint.h>

#include "arch/x86_64/cpu.h" /* MAX_CPUS - M68 */
#include "arch/x86_64/tsc.h" /* M101 - the syscall accounting bracket's clock */
#include "drivers/ac97.h"
#include "drivers/dispi.h"
#include "drivers/pcspk.h"
#include "drivers/fb.h"
#include "drivers/keyboard.h"
#include "drivers/klog.h"
#include "drivers/mouse.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "lib/libk.h"
#include "fs/leanfs.h"
#include "fs/openfile.h"
#include "fs/vfs.h"
#include "ipc/clipboard.h"
#include "ipc/pipe.h"
#include "ipc/shm.h"
#include "drivers/blk.h" /* M104: blk_flush, for fsync and sync */
#include "mm/filemap.h" /* M91 (second attempt): shared file pages */
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "power/power.h" /* M47 - power_shutdown, and power_mode.h's POWER_OFF/POWER_REBOOT through it */
#include "os_fs.h"   /* system_api/include/os_fs.h - os_statvfs_t, M88 */
#include "proc.h"      /* system_api/include/proc.h - task_info_t, M45. Resolves to the system_api one, not kernel/proc/proc.h below: a quoted include searches the *including* file's own directory first (kernel/arch/x86_64/, which has no proc.h), then -Ikernel (no kernel/proc.h either), then -Isystem_api/include. */
#include "proc/proc.h"
#include "proc/elf.h"   /* M48 - elf_validate, to tell "not a program" apart from "no such file" */
#include "profile/sampler.h" /* M101 - SYS_profile's control and readout */
#include "profile/syscount.h" /* M101 - the per-syscall accounting bracket below */
#include "sched/sched.h"
#include "signal.h"  /* system_api/include/signal.h - SIGKILL/SIGTERM */
#include "spawn_error.h" /* system_api/include/spawn_error.h - M48's distinct SYS_spawn failure codes */
#include "syscall.h" /* system_api/include/syscall.h - the shared ABI, on the include path via Makefile's -Isystem_api/include */
#include "display.h" /* system_api/include/display.h - display_mode_t, M58 */
#include "os_time.h" /* system_api/include/os_time.h - os_datetime_t, M59 */
#include "os_net.h"  /* system_api/include/os_net.h - os_sockaddr_t/os_netconf_t, M64 */
#include "dev/tty.h" /* M85: the terminal SYS_ioctl talks to */
#include "mman.h"    /* system_api/include/mman.h - the PROT_ and MAP_ flags, M78 */
#include "caps.h"    /* system_api/include/caps.h - CAP_*, M65 */
#include "net/net.h"
#include "net/socket.h"
#include "net/tcp.h"
#include "wm.h"      /* system_api/include/wm.h - wm_fb_info_t, M20 */

/* Every syscall implementation shares one signature regardless of how
 * many of its six argument slots it actually uses - keeps the dispatch
 * table trivial. Unused args are silently ignored by whichever function
 * doesn't need them. */
typedef long (*syscall_fn_t)(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6);

/* ---- M52: user pointers ----------------------------------------------
 *
 * sys_write's comment here used to read "buf is trusted as-is for now;
 * validating that a ring-3 pointer is actually mapped and owned by the
 * caller is left for whenever a genuinely untrusted program needs to run
 * here", and had said so since M8. The threat-model argument was always
 * right and was also beside the point: the programs this OS runs are its
 * own, and the bug this prevents is a *buggy* app taking the machine down
 * instead of only itself.
 *
 * Two rules, applied by every syscall below that is handed an address:
 *
 *  1. the range lies inside the caller's own private region (proc.h's
 *     USER_REGION_BASE/LIMIT, i.e. PML4[1]), and
 *  2. every page of it is actually present and user-accessible in the
 *     caller's own address space (vmm_user_range_ok).
 *
 * Failing either is -1, never a fault. Together they turn the whole
 * garbage-argument matrix - null, a kernel address, one byte before the
 * user region, one byte past the end of a mapped page, and a length that
 * overflows the range - into ordinary error returns.
 *
 * Three shapes, for three genuinely different cases:
 *
 *  - a *fixed-size* payload the kernel produces (a struct, an fd pair,
 *    one character) goes out through copy_to_user, so the caller's
 *    address is touched in exactly one place and the producing code
 *    never sees it at all.
 *  - a NUL-terminated *string* the caller supplies (a path, a pipe name)
 *    comes in through copy_str_from_user, which validates page by page
 *    as it goes - "how long is it" being precisely the question that
 *    cannot be answered before reading it.
 *  - a *bulk* buffer whose length the caller chooses (a write, a file
 *    read, the clipboard) is range-checked with user_range_ok and then
 *    used in place. There is no bounded bounce buffer for an arbitrary
 *    length, and allocating one per call would turn every large write
 *    into an allocation that can fail.
 *
 * This paragraph used to say there was deliberately no copy_from_user
 * counterpart, because no syscall took a fixed-size struct *from* the
 * caller, and that "the day one is needed it is four lines". That day was
 * M64 - SYS_sendto's payload - and the function has been sitting a few
 * lines below ever since. Corrected here rather than left standing,
 * because a comment that says a thing does not exist while it does is
 * worse than no comment at all: it is the one a reader believes without
 * checking. M81 reads it too (SYS_getdents's cookie)
 *
 * In-place use is safe here for a reason worth stating rather than
 * assuming: the only way a mapping in the private region can go away is
 * SYS_shm_free, and a process has exactly one thread of control - so
 * while it is blocked inside a syscall there is nobody who could unmap
 * the buffer it just passed. Another *process* cannot touch it: that is
 * what PML4[1] being private means. The day this project grows threads
 * within a process, this comment is the thing that stops being true. */
static int user_range_ok(uint64_t addr, uint64_t len, int need_write) {
    /* Null is refused from *any* ring, before the kernel-thread exemption
     * below. Address 0 is inside the identity map, so a kernel-thread
     * caller passing it would not fault - it would quietly scribble on
     * physical page 0 - and M50's own "SYS_taskinfo with a null buffer"
     * row is precisely that case asserted from kernel_main. Nobody, at
     * any privilege level, means address 0 when they pass a buffer. */
    if (addr == 0) {
        return 0;
    }
    /* A kernel thread calling a syscall directly is passing kernel
     * pointers, which is what it is supposed to do - the boot self-tests
     * in kernel.c have driven SYS_pipe_open, SYS_writefile and SYS_write
     * this way since M13. There is nothing to protect the kernel from
     * here: this check exists because a *ring-3* pointer is untrusted,
     * and a task sharing the kernel's own address space is by definition
     * not one. Told apart by which PML4 it runs on, which is exactly the
     * distinction (process_spawn gives a task a private one; task_spawn
     * does not).
     *
     * The consequence for testing is worth stating: a garbage-argument
     * matrix run from kernel_main would prove nothing, because every row
     * would take this early return. M52's matrix therefore runs from a
     * real user program - user_space/bin/badptr.c - and the [m52]
     * self-test only spawns it and grades its exit code. */
    if (sched_current()->pml4_phys == vmm_kernel_pml4_phys()) {
        return 1;
    }
    if (len == 0) {
        return 1; /* nothing to touch; a null one was already refused above */
    }
    if (addr < USER_REGION_BASE || addr >= USER_REGION_LIMIT) {
        return 0;
    }
    uint64_t end = addr + len;
    if (end < addr || end > USER_REGION_LIMIT) {
        return 0; /* wrapped, or ran off the top of the private region */
    }
    /* M82: build any page of this range that the caller reserved with
     * SYS_mmap and has not touched yet, *before* asking the page tables
     * whether it is mapped.
     *
     * Without this, demand paging would have quietly broken the most
     * ordinary thing a program does with an mmap: pass it to a syscall.
     * `read(fd, mmap(...), n)` would have found no page table entry at
     * the buffer, and the check below would have called the caller's own
     * memory not its own. sched_prefault_range only builds pages inside
     * the caller's own mmap regions and only with the protection those
     * regions were given, so this widens nothing - the check below is
     * still the one that decides. */
    sched_prefault_range(addr, len, need_write);
    return vmm_user_range_ok(sched_current()->pml4_phys, addr, len, need_write);
}

static int copy_to_user(uint64_t dst, const void *src, uint64_t len) {
    if (!user_range_ok(dst, len, 1)) {
        return -1;
    }
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return 0;
}

/* M64: the mirror of copy_to_user, for a buffer whose length the caller
 * chose. Every earlier syscall that read user memory either read a
 * string (below) or read into a fixed-size struct, so this is the first
 * one that needed it - SYS_sendto's payload. */
static int copy_from_user(void *dst, uint64_t src, uint64_t len) {
    if (!user_range_ok(src, len, 0)) {
        return -1;
    }
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return 0;
}

/* A NUL-terminated string whose length nobody knows yet - a path, a pipe
 * name. Validated a page at a time as the copy crosses into each one,
 * because "how long is it" is precisely the question that cannot be
 * answered before reading it. Refuses (rather than truncating) a string
 * that reaches `max` without a NUL: a silently truncated path is a
 * different file, which is a worse failure than not opening one. */
static int copy_str_from_user(char *dst, uint64_t src, uint64_t max) {
    uint64_t checked_to = 0; /* one past the last address validated so far */
    for (uint64_t i = 0; i < max; i++) {
        uint64_t at = src + i;
        if (at >= checked_to) {
            uint64_t page = at & ~(PAGE_SIZE - 1);
            if (!user_range_ok(page, PAGE_SIZE, 0)) {
                return -1;
            }
            checked_to = page + PAGE_SIZE;
        }
        dst[i] = *(const char *)at;
        if (dst[i] == '\0') {
            return 0;
        }
    }
    return -1;
}

/* ---- M75: a path, resolved against the caller's own directory --------
 *
 * kernel/fs/leanfs.h says it plainly: "every call below takes an
 * absolute path", and "anything using '.' or '..' to climb" is refused,
 * because the format stores neither. That was a true statement about a
 * filesystem and a false one about an operating system - a working
 * directory is a property of a *caller*, and leanfs has none, so this is
 * the layer that owns it.
 *
 * Everything a path-taking syscall receives comes through here first:
 *
 *   - an absolute path is normalized and passed down
 *   - a relative one is joined onto the calling task's cwd and normalized
 *   - "." and ".." are resolved *textually*, here, before leanfs ever
 *     sees them
 *
 * Textually, and that is the honest description rather than a shortcut:
 * with no symbolic links on this machine, "/a/b/.." and "/a" name the
 * same directory by construction, so there is nothing a lookup could
 * tell us that the text does not already say. The day this filesystem
 * grows links is the day that stops being true, and this comment is
 * where it stops.
 *
 * Refuses rather than truncates when the result will not fit, the same
 * rule copy_str_from_user follows and for the same reason.
 */
/* M81: the most path components this layer will resolve. See `starts`
 * below for why it is a number of its own rather than a function of
 * LEANFS_MAX_PATH. */
#define PATH_MAX_DEPTH 128

static int path_normalize(char *out, const char *in) {
    /* A stack of component start offsets in `out`, so ".." can pop the
     * last one without re-scanning.
     *
     * M81: bounded by PATH_MAX_DEPTH rather than by LEANFS_MAX_PATH / 2.
     * The old bound was exactly right - a component needs at least two
     * bytes, so a path can hold at most half its length in components -
     * and it was 64 entries while a path was 128 bytes. At 4096 it is
     * 2048 entries, which is 8 KiB of kernel stack to describe a
     * pathological path nobody will type. A tree deeper than
     * PATH_MAX_DEPTH is refused, which is a real limit and a far smaller
     * one than a filesystem this shape can reach: the deepest path in
     * anything this OS has ever held is four. */
    int starts[PATH_MAX_DEPTH];
    int depth = 0;
    int n = 0;

    if (in[0] != '/') {
        return -1; /* callers join the cwd on first; this half only normalizes */
    }
    out[n++] = '/';

    int i = 0;
    while (in[i]) {
        while (in[i] == '/') {
            i++;
        }
        if (!in[i]) {
            break;
        }
        /* One component, up to the next '/'. */
        int c_start = i;
        while (in[i] && in[i] != '/') {
            i++;
        }
        int c_len = i - c_start;

        if (c_len == 1 && in[c_start] == '.') {
            continue; /* "." is where we already are */
        }
        if (c_len == 2 && in[c_start] == '.' && in[c_start + 1] == '.') {
            if (depth > 0) {
                n = starts[--depth];
                /* Drop the '/' that preceded the component we just
                 * removed, unless doing so would leave an empty path. */
                if (n > 1) {
                    n--;
                }
            }
            /* ".." at the root is the root. Refusing here would make
             * "cd .." from "/" an error, which no system does. */
            continue;
        }
        if (c_len > LEANFS_MAX_NAME) {
            return -1;
        }
        if (depth >= (int)(sizeof(starts) / sizeof(starts[0]))) {
            return -1;
        }
        if (n > 1) {
            if (n + 1 >= LEANFS_MAX_PATH) {
                return -1;
            }
            out[n++] = '/';
        }
        starts[depth++] = n;
        if (n + c_len >= LEANFS_MAX_PATH) {
            return -1;
        }
        for (int k = 0; k < c_len; k++) {
            out[n++] = in[c_start + k];
        }
    }
    out[n] = '\0';
    return 0;
}

/* Copies a path in from user space and resolves it. The one entry point
 * every path-taking syscall below uses, so that "relative names work"
 * is a property of the syscall layer rather than of whichever handlers
 * remembered. */
static int copy_path_from_user(char *out, uint64_t src) {
    char raw[LEANFS_MAX_PATH];
    if (copy_str_from_user(raw, src, sizeof(raw)) != 0) {
        return -1;
    }
    if (raw[0] == '/') {
        return path_normalize(out, raw);
    }
    /* Relative: join onto the caller's directory. Built in a second
     * buffer because path_normalize reads its input while writing its
     * output.
     *
     * M81: one PATH_MAX rather than two. The doubled buffer existed so
     * that a join which overflows could still be *normalized* down under
     * the limit ("/very/long/cwd/../../x"), and at a 128-byte PATH_MAX
     * that was a plausible thing for a caller to do. At 4096 it is not,
     * and 8 KiB of kernel stack to keep the possibility open is the wrong
     * trade. A join that does not fit is refused before it is
     * normalized, which is a slightly stricter rule stated here rather
     * than discovered. */
    char joined[LEANFS_MAX_PATH];
    /* M79: the working directory belongs to the process, not the task -
     * POSIX is explicit that a chdir in one thread is seen by all of
     * them, and it is the same argument sched_vm_owner makes about the
     * heap: two answers to a question that has one is a bug waiting. */
    task_t *self = sched_vm_owner(sched_current());
    const char *cwd = (self->cwd[0] == '/') ? self->cwd : "/";
    int n = 0;
    while (cwd[n] && n < LEANFS_MAX_PATH) {
        joined[n] = cwd[n];
        n++;
    }
    if (n == 0 || joined[n - 1] != '/') {
        joined[n++] = '/';
    }
    for (int i = 0; raw[i]; i++) {
        if (n >= (int)sizeof(joined) - 1) {
            return -1;
        }
        joined[n++] = raw[i];
    }
    joined[n] = '\0';
    return path_normalize(out, joined);
}

/* ---- M65: capabilities -----------------------------------------------
 *
 * One predicate, used at every gate. Deliberately a plain function
 * rather than a macro that returns: the call sites read better as an
 * ordinary `if`, and every one of them wants to answer with that
 * syscall's own idea of failure (-1, or a SPAWN_ERR_, or 0) rather than
 * with a shared one.
 *
 * A denial is logged once per process per capability. Silent refusal is
 * how a permission model turns into an unexplained bug: the program sees
 * -1 from a call that has ten other reasons to return -1, and whoever is
 * looking at it has nothing to go on. */
static uint32_t cap_denials_logged[MAX_TASKS];

static int has_cap(uint32_t cap) {
    task_t *self = sched_current();
    if (self->caps & cap) {
        return 1;
    }
    int slot = PID_SLOT(self->id);
    if (slot >= 0 && slot < MAX_TASKS && !(cap_denials_logged[slot] & cap)) {
        cap_denials_logged[slot] |= cap;
        klog_puts("[caps] ");
        klog_puts(self->name);
        klog_puts(" was refused '");
        for (int i = 0; i < CAP_NAME_COUNT; i++) {
            if (CAP_NAMES[i].bit == cap) {
                klog_puts(CAP_NAMES[i].name);
                break;
            }
        }
        klog_puts("' - see system_api/include/caps.h\n");
    }
    return 0;
}

static long sys_write(uint64_t fd, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FDS || !user_range_ok(buf, len, 0)) {
        return -1;
    }
    fd_slot_t *slot = &sched_current()->fds[fd];
    const char *s = (const char *)buf;
    if (slot->type == FD_STDOUT) {
        for (uint64_t i = 0; i < len; i++) {
            klog_putc(s[i]);
        }
        return (long)len;
    }
    if (slot->type == FD_PIPE_WRITE) {
        return pipe_write(slot->pipe, s, (size_t)len);
    }
    if (slot->type == FD_FILE) {
        if (!slot->file->writable) {
            return -1;
        }
        int64_t n = vfs_handle_write(slot->file->handle, s, (size_t)len, slot->file->offset);
        if (n < 0) {
            return -1;
        }
        slot->file->offset += (uint32_t)n;
        return (long)n;
    }
    return -1;
}

/* fd=0 (stdin/keyboard) blocks (cooperatively yields) until at least one
 * byte is available, then greedily grabs whatever's immediately ready
 * without blocking further - line editing/echo is the shell's job (user
 * space), not this syscall's. A pipe read fd defers entirely to
 * pipe_read, which already implements the identical "block for the
 * first byte, then drain what's ready" contract. */
static long sys_read(uint64_t fd, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FDS || !user_range_ok(buf, len, 1)) {
        return -1;
    }
    fd_slot_t *slot = &sched_current()->fds[fd];
    char *dst = (char *)buf;
    if (slot->type == FD_STDIN) {
        uint64_t n = 0;
        while (n < len) {
            /* M68: sampled before the read, so a keystroke arriving
             * between the read and the park below is seen as "the world
             * moved" rather than lost. */
            uint64_t seq = sched_event_seq();
            int c = keyboard_read();
            if (c == -1) {
                if (n > 0) {
                    break;
                }
                /* M42: a shell parked on an empty keyboard buffer is past
                 * this handler's own signal check and gets no timer tick
                 * while it's current - see sched_deliver_pending_signal.
                 * sched_block_on makes that check itself, first thing.
                 *
                 * M68: this was `schedule()`, which left the task READY -
                 * so a shell sitting at a prompt was indistinguishable
                 * from a program that wanted the CPU, forever. It is the
                 * single clearest example of why no core on this machine
                 * had ever halted. Now it leaves the run queue and the
                 * keyboard IRQ wakes it.
                 *
                 * There is no condition lock to hand over here the way
                 * pipe_read has one: the keyboard ring buffer is written
                 * by an interrupt handler, and the wake happens in that
                 * same handler *after* the byte is in the buffer. So the
                 * ordering that matters is the driver's, not this
                 * caller's - a keystroke that lands between the
                 * keyboard_read above and the park below leaves this task
                 * READY again immediately, which costs one extra trip
                 * round this loop and loses nothing. A dummy lock keeps
                 * sched_block_on's signature honest rather than growing a
                 * lockless variant, which closes it with a sequence
                 * counter instead - see sched.h. */
                sched_block_on_seq(SCHED_KEYBOARD_CHAN, 0, seq);
                /* M76: and a caught signal is a reason to stop waiting.
                 * A shell parked here is exactly the process a Ctrl+C is
                 * aimed at.
                 *
                 * M98 changed what it returns when nothing has been
                 * read. M76's own line said "returning 0 rather than -1:
                 * every caller of a blocking read on this machine
                 * already loops on a short read" - true, and true only
                 * of callers written here. A zero means end of file to
                 * everyone else. See OS_ERR_INTR. */
                if (sched_signal_pending()) {
                    return n ? (long)n : -OS_ERR_INTR;
                }
                continue;
            }
            dst[n++] = (char)c;
        }
        return (long)n;
    }
    if (slot->type == FD_PIPE_READ) {
        return pipe_read(slot->pipe, dst, (size_t)len);
    }
    /* M59: and a file, which is the whole point of the fd table having
     * existed since M14 without one. The offset lives in the shared
     * open-file entry rather than in this slot, so two fds made by
     * SYS_dup2 advance one position between them. */
    if (slot->type == FD_FILE) {
        /* M89: a directory can be open (see leanfs_open) and cannot be
         * read. Its bytes are leanfs's own records, and handing those to
         * a program would be exporting the on-disk format through a
         * call that promises file contents. SYS_getdents is the call
         * that reads a directory, and it takes a path.
         *
         * M91: read from the open-file entry rather than by asking the
         * filesystem. This was a vfs_handle_stat on every read - a lock
         * and an inode fetch per call - and it doubled the cost of
         * reading a megabyte. See openfile_t's own note. */
        if (slot->file->is_dir) {
            return -1;
        }
        /* M85 (second attempt): a path that can be empty and later not.
         *
         * Every file this call has ever read was ready by definition -
         * bytes on a disk do not arrive later. A pty is the first thing
         * openable by name that is genuinely sometimes empty, and a read
         * on one has to park exactly as a read on a pipe does, or a
         * terminal emulator spins a core waiting for its shell to say
         * something.
         *
         * vfs_handle_readable answers 1 for everything else, so this loop
         * runs zero times for a regular file and costs one predictable
         * branch - which is why the check is here rather than behind a
         * second descriptor type. */
        while (!vfs_handle_readable(slot->file->handle)) {
            uint64_t seq = sched_event_seq();
            if (vfs_handle_readable(slot->file->handle)) {
                break; /* it became ready between the test and the sample */
            }
            sched_block_on_seq(SCHED_POLL_CHAN, 0, seq);
            if (sched_signal_pending()) {
                /* Same answer as the keyboard path above, and M98
                 * changed both together: -OS_ERR_INTR, because nothing
                 * has been read and a 0 here claims end of file. */
                return -OS_ERR_INTR;
            }
        }
        int64_t n = vfs_handle_read(slot->file->handle, dst, (size_t)len, slot->file->offset);
        if (n < 0) {
            return -1;
        }
        slot->file->offset += (uint32_t)n;
        return (long)n;
    }
    return -1;
}

static long sys_exit(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    /* M79: POSIX exit() ends a *process*, and a process is now more than
     * one task. Every other thread in this group is SIGKILLed before
     * this one leaves - they die at their next syscall or tick, and
     * whichever of them is last out is the one that tears the address
     * space down (see task_exit_with_code). Doing it in the other order
     * would leave a thread running in an address space that had just
     * been freed. */
    sched_kill_thread_group(sched_current());
    task_exit_with_code((int)code); /* noreturn */
}

/* M79: the thread GROUP's id. Identical to the task's own for anything
 * that is not a thread, which is every task that existed before M79 -
 * so nothing that called this before means anything different now. */
static long sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return sched_current()->tgid;
}

static long sys_gettid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return sched_current()->id;
}

/* ---- M79: the two calls that make a thread ----------------------------
 *
 * Deliberately small. The kernel makes a task in the caller's address
 * space and drops it into ring 3 at an address the caller chose, on a
 * stack the caller allocated; everything a pthread *is* - the start
 * routine's return value, joining, detaching, a mutex - is built on top
 * of that in user_space/libc/src/pthread.c, using memory the two threads
 * now share. That split is not a shortcut: shared memory is exactly what
 * makes a thread library implementable in user space, and a kernel that
 * owned pthread_t would be a kernel with an opinion about a C library.
 */
static long sys_thread_create(uint64_t entry, uint64_t arg, uint64_t stack_top,
                               uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    /* Both addresses have to be the caller's own, for the same reason a
     * signal handler's does (sys_sigaction): the kernel is about to put
     * them into a ring-3 iretq frame, and one that is not mapped in this
     * address space is a fault the moment the thread starts, reported
     * against a program that did nothing wrong at the point it looks
     * wrong. */
    if (entry < USER_REGION_BASE || entry >= USER_REGION_LIMIT) {
        return -1;
    }
    if (stack_top < USER_REGION_BASE || stack_top >= USER_REGION_LIMIT) {
        return -1;
    }
    /* ---- M99: the alignment, and the sentence that had it backwards --
     *
     * M79 wrote: "the stack top must be 16-byte aligned, because that is
     * what the SysV ABI requires at a function's entry and there is no
     * `call` here to have pushed a return address". The first half is a
     * good rule for the caller and the second half is exactly wrong.
     *
     * SysV requires RSP to be 16-byte aligned **at the call
     * instruction**, which means a function's first instruction runs
     * with `RSP % 16 == 8` - the return address the call pushed. Every
     * function GCC compiles is built on that: it lays out its
     * 16-byte-aligned locals at offsets from RBP that are correct only
     * if RSP was 8 mod 16 on entry.
     *
     * So entering a thread with RSP exactly 16-aligned puts every
     * aligned spill slot in that thread 8 bytes out, and the first
     * `movaps` to one is a #GP. It cost nothing for twenty milestones
     * because nothing that ran on a thread here spilled an XMM register
     * to an aligned slot. **CPython's test_io does**, and the crash is
     * `0F 29 45 80` - `movaps %xmm0, -0x80(%rbp)` - at an address ending
     * in 8.
     *
     * The check stays, because 16-byte alignment is a real and checkable
     * property of the memory the caller allocated and rounding it
     * silently would move a stack away from its guard page. What changes
     * is what the kernel does with it: the thread starts 8 bytes below,
     * so its entry function is entered exactly as a `call` would have
     * entered it. */
    if ((stack_top & 15) != 0) {
        return -1;
    }
    uint64_t entry_rsp = stack_top - 8;
    if (!sched_has_free_task_slot()) {
        return -1;
    }
    task_t *self = sched_current();
    task_t *t = process_spawn_thread(self->name, entry, entry_rsp, arg);
    return t ? (long)t->id : -1;
}

static long sys_thread_exit(uint64_t value, uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    /* This thread only. The address space outlives it if anybody else is
     * still using it - see task_exit_with_code's last-one-out rule. */
    task_exit_with_code((int)value); /* noreturn */
}

/* Combines fork+exec into one call: reads `path` from the filesystem and
 * loads it as a brand-new process via process_spawn (M9) - a fork/exec
 * "equivalent" (M13's own wording), not literal fork() semantics (no
 * address-space duplication). The child inherits the caller's whole fd
 * table (sched.c's task_spawn_common), so a pipe set up beforehand
 * carries over. */
/* ---- M84: argv and envp, copied out of the caller's address space -----
 *
 * Written for SYS_execve, which needs it for a reason SYS_spawn does not:
 * it is about to destroy the address space these strings live in, so
 * copying them into kernel buffers first is not an optimisation but the
 * only order that works.
 *
 * SYS_spawn still has its own copy of this a few hundred lines below, and
 * that is a duplication rather than a design - two places that decode
 * untrusted user pointers should be one. It was left alone because the
 * conversion is entangled with spawn's `#!` handling, which rebuilds the
 * vector a third way, and doing that surgery in the same change as
 * exec's first working version would have made a failure in either
 * impossible to attribute. `path_is_argv0` exists so the conversion is a
 * call-site change when it happens: spawn passes 1, exec passes 0.
 *
 * Most rules here were already in sys_spawn and are unchanged: the arrays
 * are user memory and every pointer in them is a user pointer, so neither
 * is trusted (M52); a vector too long or strings too large are truncated
 * at the last whole entry rather than refused, because half an argument
 * names something else and a program seeing fewer arguments than it was
 * given is a failure it can report itself.
 *
 * `path_is_argv0` is the one thing the two callers disagree about, and
 * getting it wrong is not subtle once it is seen. SYS_spawn's ABI says
 * argv holds the arguments *after* the program name and the kernel
 * supplies argv[0] itself, because that is the one element it knows for
 * certain. execve's contract is POSIX's: argv is the COMPLETE vector,
 * argv[0] included, and a program is entitled to put whatever it likes
 * there.
 *
 * The first version of this took spawn's rule for both, which shifted
 * every argument of every exec by one - so `execv(self, {self, "sig"})`
 * arrived as argv[1] = self, the mode never matched, and the test
 * program fell through to its own main path and re-exec'd itself. It
 * presented as a boot that stopped after three ELF loads with no error
 * at all.
 *
 * argv and envp get separate buffers, and that is deliberate: an
 * environment is the larger of the two by an order of magnitude on any
 * real system, and sharing one page would make a long PATH silently
 * truncate the arguments.
 *
 * Returns 0, or -1 having freed whatever it allocated. A NULL `envp_ptr`
 * leaves `v->envp` NULL, which every layer below reads as "inherit the
 * caller's own environment".
 */
typedef struct {
    char *argbuf;
    char *envbuf;
    const char *argv[SPAWN_MAX_ARGS + 1];
    int argc;
    const char *envv[USER_ENV_MAX_VARS + 1];
    const char *const *envp;
} user_vectors_t;

static void free_vectors(user_vectors_t *v) {
    kfree(v->argbuf);
    kfree(v->envbuf);
    v->argbuf = (char *)0;
    v->envbuf = (char *)0;
}

static int copy_vectors_from_user(const char *path, uint64_t arg_ptr,
                                  uint64_t envp_ptr, int path_is_argv0,
                                  user_vectors_t *v) {
    v->argbuf = (char *)kmalloc(PAGE_SIZE);
    v->envbuf = (char *)0;
    v->envp = (const char *const *)0;
    v->argc = 0;
    if (!v->argbuf) {
        return -1;
    }

    size_t used = 0;
    if (path_is_argv0) {
        size_t len = k_strlen(path) + 1;
        k_memcpy(v->argbuf, path, len);
        v->argv[v->argc++] = v->argbuf;
        used = len;
    }
    if (arg_ptr) {
        for (int i = 0; v->argc < SPAWN_MAX_ARGS; i++) {
            if (!user_range_ok(arg_ptr + (uint64_t)i * sizeof(uint64_t), sizeof(uint64_t), 0)) {
                break;
            }
            uint64_t slot = ((const uint64_t *)arg_ptr)[i];
            if (slot == 0) {
                break;
            }
            char *dst = v->argbuf + used;
            size_t room = PAGE_SIZE - used;
            if (room < 2 || copy_str_from_user(dst, slot, room) != 0) {
                break;
            }
            v->argv[v->argc++] = dst;
            used += k_strlen(dst) + 1;
        }
    }
    v->argv[v->argc] = (const char *)0;

    if (envp_ptr) {
        v->envbuf = (char *)kmalloc(USER_ENV_MAX_BYTES);
        if (!v->envbuf) {
            free_vectors(v);
            return -1;
        }
        int envc = 0;
        size_t eused = 0;
        for (int i = 0; envc < USER_ENV_MAX_VARS; i++) {
            if (!user_range_ok(envp_ptr + (uint64_t)i * sizeof(uint64_t), sizeof(uint64_t), 0)) {
                break;
            }
            uint64_t slot = ((const uint64_t *)envp_ptr)[i];
            if (slot == 0) {
                break;
            }
            char *dst = v->envbuf + eused;
            size_t room = USER_ENV_MAX_BYTES - eused;
            if (room < 2 || copy_str_from_user(dst, slot, room) != 0) {
                break;
            }
            v->envv[envc++] = dst;
            eused += k_strlen(dst) + 1;
        }
        v->envv[envc] = (const char *)0;
        v->envp = v->envv;
    }
    return 0;
}

static long sys_spawn(uint64_t path_ptr, uint64_t arg_ptr, uint64_t envp_ptr, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    /* M52: both strings are copied in before anything is done with them.
     * A path longer than leanfs can name is refused rather than
     * truncated - see copy_str_from_user - since a truncated path names a
     * different file. SPAWN_ERR_NOT_FOUND is the honest answer for a path
     * this kernel cannot read at all, which is what an unmapped pointer
     * amounts to. */
    char path[LEANFS_MAX_PATH];
    /* M75: resolved against the caller's directory like every other path
     * this kernel is handed. `sh ./script` and a launcher spawning
     * something out of the directory it is sitting in both depend on it,
     * and a spawn is the one path-taking call where "it silently did not
     * exist" is the least useful possible answer. */
    if (copy_path_from_user(path, path_ptr) != 0) {
        return SPAWN_ERR_NOT_FOUND;
    }
    /* M60: a real argument vector, copied in one string at a time.
     *
     * The whole vector shares one PAGE_SIZE heap buffer for the same
     * reason the single string used to have one: process_spawnv copies it
     * into a PAGE_SIZE frame mapped at USER_ARG_ADDR, so a page is
     * exactly what fits and anything bigger would be copied in only to be
     * dropped. Each `char *` in the caller's array is validated and
     * copied individually - the array itself is user memory and the
     * pointers in it are user pointers, neither of which this kernel
     * trusts (M52).
     *
     * A vector longer than SPAWN_MAX_ARGS, or strings that overflow the
     * page, are truncated at the last whole argument rather than
     * refused: half an argument names something else, and a program
     * seeing fewer arguments than it was given is a failure it can
     * report itself. */
    /* M89: the staging buffer is the argument region's size, not one
     * page. It was a page when SPAWN_MAX_ARGS was 16; both moved for the
     * same reason - see SPAWN_MAX_ARGS and USER_ARG_PAGES. Heap rather
     * than stack: 128 KiB does not fit in a 32 KiB kernel stack. */
    char *arg = (char *)kmalloc(USER_ARG_BYTES);
    if (!arg) {
        return SPAWN_ERR_NO_MEMORY;
    }
    const char *argv[SPAWN_MAX_ARGS + 1];
    int argc = 0;
    size_t used = 0;
    /* argv[0] is always the path, whatever the caller passed - it is the
     * one element the kernel knows for certain and the one a program is
     * entitled to assume is there. */
    {
        size_t len = k_strlen(path) + 1;
        k_memcpy(arg, path, len);
        argv[argc++] = arg;
        used = len;
    }
    if (arg_ptr) {
        for (int i = 0; argc < SPAWN_MAX_ARGS; i++) {
            uint64_t slot;
            if (!user_range_ok(arg_ptr + (uint64_t)i * sizeof(uint64_t), sizeof(uint64_t), 0)) {
                break;
            }
            slot = ((const uint64_t *)arg_ptr)[i];
            if (slot == 0) {
                break;
            }
            char *dst = arg + used;
            size_t room = USER_ARG_BYTES - used;
            if (room < 2 || copy_str_from_user(dst, slot, room) != 0) {
                break;
            }
            argv[argc++] = dst;
            used += k_strlen(dst) + 1;
        }
    }
    argv[argc] = (const char *)0;

    /* ---- M75: the environment -----------------------------------------
     *
     * Copied in exactly the way argv above is, and for exactly the same
     * reasons: the array is user memory and every pointer in it is a
     * user pointer, so neither is trusted (M52). A NULL `envp` is the
     * inherit case and is passed straight through to
     * process_spawnve_capped, which is the layer that can see the
     * caller's own block.
     *
     * A separate heap buffer rather than more of argv's page: an
     * environment is the larger of the two by an order of magnitude on
     * any real system, and sharing one page would make a long PATH
     * silently truncate the arguments. */
    char *envbuf = (char *)0;
    const char *envv[USER_ENV_MAX_VARS + 1];
    const char *const *envp = (const char *const *)0;
    if (envp_ptr) {
        envbuf = (char *)kmalloc(USER_ENV_MAX_BYTES);
        if (!envbuf) {
            kfree(arg);
            return SPAWN_ERR_NO_MEMORY;
        }
        int envc = 0;
        size_t eused = 0;
        for (int i = 0; envc < USER_ENV_MAX_VARS; i++) {
            if (!user_range_ok(envp_ptr + (uint64_t)i * sizeof(uint64_t), sizeof(uint64_t), 0)) {
                break;
            }
            uint64_t slot = ((const uint64_t *)envp_ptr)[i];
            if (slot == 0) {
                break;
            }
            char *dst = envbuf + eused;
            size_t room = USER_ENV_MAX_BYTES - eused;
            if (room < 2 || copy_str_from_user(dst, slot, room) != 0) {
                break; /* truncated at the last whole variable, as argv is */
            }
            envv[envc++] = dst;
            eused += k_strlen(dst) + 1;
        }
        envv[envc] = (const char *)0;
        envp = envv;
    }

    /* M59: sized from the file rather than from the format's ceiling.
     * That ceiling used to be 72 KiB, which was a fine over-allocation;
     * double indirection made it 8 MiB, at which point "allocate the
     * largest a file could possibly be" is eight megabytes of kernel heap
     * per spawn - transient, but taken while a compositor may be
     * launching several apps at once. SYS_stat is the call that makes
     * asking first possible. */
    leanfs_stat_t st;
    if (vfs_stat(path, &st) != 0 || st.is_dir) {
        kfree(arg);
        kfree(envbuf);
        return SPAWN_ERR_NOT_FOUND;
    }
    uint8_t *image = (uint8_t *)kmalloc(st.size ? st.size : 1);
    if (!image) {
        kfree(arg);
        kfree(envbuf);
        return SPAWN_ERR_NO_MEMORY;
    }
    int64_t size = vfs_read(path, image, st.size);
    if (size < 0) {
        kfree(image);
        kfree(arg);
        kfree(envbuf);
        return SPAWN_ERR_NOT_FOUND;
    }

    /* ---- M72: `#!` ------------------------------------------------------
     *
     * The one change that makes a script a program. A file starting with
     * `#!` is not an image to load - it names an interpreter, and what
     * actually gets spawned is that interpreter with this file's path as
     * its first argument.
     *
     * Done here, at the syscall, rather than in process_spawn: this is
     * the layer that has a *path* to hand to the interpreter, and the
     * whole mechanism is "run something else, and tell it about this
     * file". process_spawn takes an image and could not name it.
     *
     * The effect is that every launcher on this machine gets scripts for
     * free - the shell, the compositor's Spotlight, a desktop icon, the
     * file manager - because none of them has to know. That is the same
     * reason M65 put the capability manifest in the kernel rather than in
     * the compositor: a rule only one launcher consults is a rule with a
     * way around it.
     *
     * Deliberately bounded and unclever: one level of indirection (an
     * interpreter that is itself a script is refused, not recursed into,
     * because a cycle here is an unkillable spawn loop), no arguments
     * after the interpreter path on the `#!` line, and the original
     * argv is preserved after the script path. */
    if (size >= 2 && image[0] == '#' && image[1] == '!') {
        char interp[LEANFS_MAX_PATH];
        int n = 0;
        int64_t i = 2;
        while (i < size && (image[i] == ' ' || image[i] == '\t')) {
            i++;
        }
        while (i < size && image[i] != '\n' && image[i] != '\r' &&
               image[i] != ' ' && n < (int)sizeof(interp) - 1) {
            interp[n++] = (char)image[i++];
        }
        interp[n] = '\0';

        kfree(image);
        if (n == 0) {
            kfree(arg);
            kfree(envbuf);
            return SPAWN_ERR_BAD_IMAGE; /* "#!" with no interpreter names nothing */
        }

        /* Rebuild the vector. process_spawnv takes a COMPLETE argv -
         * sys_spawn above builds argv[0] itself and hands the whole thing
         * over - so argv[0] here must be the interpreter's own path, and
         * the script becomes argv[1].
         *
         * Getting this wrong is not a subtle failure: /bin/sh treats
         * "given a path" as "run this script" and "given nothing" as
         * "read stdin", so an off-by-one here makes every script spawn an
         * interactive shell that blocks forever on a keyboard nobody is
         * typing at. Which is exactly what the first version of this did,
         * and it presented as a boot that stopped with no message. */
        const char *shifted[SPAWN_MAX_ARGS + 2];
        int sc = 0;
        shifted[sc++] = interp; /* argv[0]: the interpreter, as any program expects */
        shifted[sc++] = path;   /* argv[1]: the script it was asked to run */
        for (int a = 1; a < argc && sc < SPAWN_MAX_ARGS; a++) {
            shifted[sc++] = argv[a];
        }
        shifted[sc] = (const char *)0;

        leanfs_stat_t ist;
        if (vfs_stat(interp, &ist) != 0 || ist.is_dir) {
            kfree(arg);
            kfree(envbuf);
            return SPAWN_ERR_NOT_FOUND;
        }
        uint8_t *iimage = (uint8_t *)kmalloc(ist.size ? ist.size : 1);
        if (!iimage) {
            kfree(arg);
            kfree(envbuf);
            return SPAWN_ERR_NO_MEMORY;
        }
        int64_t isize = vfs_read(interp, iimage, ist.size);
        /* One level only: an interpreter that is itself a script would
         * need this whole block again, and a `#!` cycle would spawn
         * forever. Refusing is the answer with no failure mode. */
        if (isize < 2 || (iimage[0] == '#' && iimage[1] == '!') ||
            !elf_validate(iimage, (size_t)isize)) {
            kfree(iimage);
            kfree(arg);
            kfree(envbuf);
            return SPAWN_ERR_BAD_IMAGE;
        }
        if (!sched_has_free_task_slot()) {
            kfree(iimage);
            kfree(arg);
            kfree(envbuf);
            return SPAWN_ERR_NO_TASK_SLOT;
        }
        const char *iname = interp;
        for (const char *c = interp; *c; c++) {
            if (*c == '/') {
                iname = c + 1;
            }
        }
        task_t *it = process_spawnve(iname, iimage, (size_t)isize, shifted, envp);
        kfree(iimage);
        kfree(arg);
        kfree(envbuf);
        return it ? (long)it->id : SPAWN_ERR_NO_MEMORY;
    }

    /* M48: process_spawn makes both of these checks itself and answers
     * NULL either way, which is precisely the ambiguity this milestone is
     * removing - so they are asked here too, where the answers can still
     * be told apart. elf_validate is one pass over the program headers
     * and sched_has_free_task_slot is a comparison; a spawn reads a whole
     * file off disk first, so neither is worth the ambiguity it would
     * save. */
    if (!elf_validate(image, (size_t)size)) {
        kfree(image);
        kfree(arg);
        kfree(envbuf);
        return SPAWN_ERR_BAD_IMAGE;
    }
    if (!sched_has_free_task_slot()) {
        kfree(image);
        kfree(arg);
        kfree(envbuf);
        return SPAWN_ERR_NO_TASK_SLOT;
    }

    /* process_spawn's elf_load synchronously copies every byte it needs
     * into fresh physical frames before returning, so freeing this
     * buffer right away is safe - nothing keeps pointing at it. */
    /* M45: the name is what this process gets listed as - the only
     * human-readable identity anything at this layer has for it.
     *
     * M53: the *basename*, not the whole path. A task manager row
     * reading "/bin/desktop_icons" says nothing "desktop_icons" doesn't,
     * spends six of TASK_NAME_MAX's 24 characters saying where every
     * program on this system lives, and would break every self-test that
     * asks the scheduler what a task is called. The path is what gets
     * loaded; the name is what gets shown. */
    const char *name = path;
    for (const char *c = path; *c; c++) {
        if (*c == '/') {
            name = c + 1;
        }
    }
    task_t *t = process_spawnve(name, image, (size_t)size, argv, envp);
    kfree(image);
    kfree(arg);
    kfree(envbuf);
    if (!t) {
        /* Everything else was checked above, so the only ways left to
         * fail are running out of frames while mapping the image or out
         * of heap for the launch args - and the task table filling
         * between that check and this one, which is the same answer a
         * caller can act on ("try again"). */
        return SPAWN_ERR_NO_MEMORY;
    }
    return t->id;
}

/* pid == -1 means "wait for any of my children"; returns that child's
 * pid (not its exit code - call sys_wait again on the specific pid to
 * get that, which returns immediately since a terminated task's slot
 * stays valid forever). Returns -1 immediately, without blocking, once
 * there are no unreaped children left at all - so a caller doesn't wait
 * forever for a child that will never come. Both modes poll +
 * cooperatively yield rather than using a real blocking wait queue. */
static long sys_wait(uint64_t pid_arg, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = sched_current();

    if ((int64_t)pid_arg == -1) {
        for (;;) {
            int any_children = 0;
            /* M54: by *slot*, not by id. A pid is no longer an index, so
             * walking the table means walking slots - sched_task_by_slot
             * exists precisely so this loop cannot accidentally become a
             * lookup that a recycled generation would answer wrongly. */
            uint64_t seq = sched_event_seq(); /* M68: before the scan, see sched.h */
            int total = sched_task_count();
            for (int i = 0; i < total; i++) {
                task_t *t = sched_task_by_slot(i);
                if (!t || t->parent_id != self->id || t->reaped) {
                    continue;
                }
                any_children = 1;
                if (t->state == TASK_TERMINATED) {
                    t->reaped = 1;
                    /* Read the pid out before the slot is released - the
                     * whole point of sched_reap_slot is that afterwards
                     * this task_t belongs to nobody. */
                    int pid = t->id;
                    sched_reap_slot(t);
                    return pid;
                }
            }
            if (!any_children) {
                return -1;
            }
            /* M68: wait(-1) has no single child to park on, so it parks
             * on the poll channel with a short deadline and re-scans.
             * That is a compromise and worth naming: a task_exit wakes
             * the poll channel, so the common case is an immediate
             * wake-up rather than a timeout, and the deadline is there
             * only so that a lost wake can cost 50 ms rather than
             * forever. The alternative - a channel per parent - is real
             * plumbing for a call this OS makes from one place. */
            sched_block_on_seq(SCHED_POLL_CHAN, pit_get_ticks() * (1000 / PIT_HZ) + 50, seq);
        }
    }

    task_t *t = sched_task_by_id((int)pid_arg);
    if (!t) {
        return -1;
    }
    while (t->state != TASK_TERMINATED) {
        /* M68: park on the child itself. task_exit_with_code wakes this
         * exact address, so a parent waiting on one child is woken by
         * that child and by nothing else. */
        uint64_t seq = sched_event_seq();
        if (t->state == TASK_TERMINATED) {
            break; /* re-tested after the sample, so the wake cannot be missed */
        }
        sched_block_on_seq((const void *)t, pit_get_ticks() * (1000 / PIT_HZ) + 200, seq);
    }
    t->reaped = 1;
    int code = t->exit_code;
    sched_reap_slot(t); /* M54: the exit status has been consumed, so the slot can come back */
    return code;
}

/* Whole-file read by name - no open/close/lseek yet, matching how few
 * programs actually need file I/O so far (cat is the one). */
static long sys_readfile(uint64_t name_ptr, uint64_t buf, uint64_t maxlen, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, name_ptr) != 0 ||
        !user_range_ok(buf, maxlen, 1)) {
        return -1;
    }
    return (long)vfs_read(path, (void *)buf, (size_t)maxlen);
}

/* M33: mirrors sys_readfile's shape exactly - see SYS_writefile's own doc
 * comment (system_api/include/syscall.h) for why this hadn't been needed
 * until now. */
static long sys_writefile(uint64_t name_ptr, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_FS_WRITE)) {
        return -1; /* M65 */
    }
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, name_ptr) != 0 ||
        !user_range_ok(buf, len, 0)) {
        return -1;
    }
    return vfs_write(path, (const void *)buf, (size_t)len);
}

/* M53: takes a path now. It used to be SYS_listfiles(buf, maxlen), which
 * had no path to take because leanfs was flat and "every file there is"
 * was the only answer available - the reason the launcher offered to run
 * settings.conf and the file manager listed this OS's own executables
 * next to your text files. */
/* M56 - see SYS_unlink's contract. */
static long sys_unlink(uint64_t path_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_FS_WRITE)) {
        return -1; /* M65 */
    }
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0) {
        return -1;
    }
    return vfs_unlink(path);
}

/* M56 - see SYS_rename's contract. Two user strings, both copied in
 * before either is used, for the same reason every other path is. */
static long sys_rename(uint64_t old_ptr, uint64_t new_ptr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_FS_WRITE)) {
        return -1; /* M65 */
    }
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char old_path[LEANFS_MAX_PATH];
    char new_path[LEANFS_MAX_PATH];
    if (copy_path_from_user(old_path, old_ptr) != 0 ||
        copy_path_from_user(new_path, new_ptr) != 0) {
        return -1;
    }
    return vfs_rename(old_path, new_path);
}

static long sys_listdir(uint64_t path_ptr, uint64_t buf, uint64_t maxlen, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0 ||
        !user_range_ok(buf, maxlen, 1)) {
        return -1;
    }
    if (!vfs_is_dir(path)) {
        return -1; /* an ordinary file, or nothing at all - either way not something with contents to list */
    }
    return (long)vfs_list(path, (char *)buf, (size_t)maxlen);
}

/* M81: as many whole directory records as fit in the caller's buffer,
 * resuming from `cookie`.
 *
 * The one design decision worth stating: a partial record is never
 * written. The loop fetches an entry, works out what it costs, and stops
 * *without consuming it* if it will not fit - which is why `cookie` is
 * only advanced past entries that actually reached the caller. Getting
 * that backwards is how a directory walk silently skips a file, and a
 * skipped file in a tree walk is a much worse failure than a short read.
 *
 * `cookie` lives in user space and is read and written each call. That
 * makes it the caller's business to keep, which is right - it is a
 * position in *their* walk, and two programs reading one directory have
 * two of them and no shared state in the kernel at all.
 */
static long sys_getdents(uint64_t path_ptr, uint64_t cookie_ptr, uint64_t buf,
                         uint64_t buflen, uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0 ||
        !user_range_ok(cookie_ptr, sizeof(uint32_t), 1) ||
        !user_range_ok(buf, buflen, 1)) {
        return -1;
    }
    /* Resolved once for the whole fetch, not once per entry: walking
     * "/a/b/c" costs a block read per component, and paying that per
     * record would put a hidden factor of the path depth on a directory
     * of thousands - which is the case this call exists for.
     *
     * M87: a synthetic directory (/dev, /proc) has no handle form, for
     * the reason vfs_dir_open states - there is no inode to open, and
     * its contents are generated from the path anyway, so the walk is
     * the same cost either way. `dir < 0` after vfs_is_dir has already
     * said yes means exactly that case, and the loop below uses the path
     * form instead. */
    int dir = vfs_dir_open(path);

    uint32_t cookie;
    if (copy_from_user(&cookie, cookie_ptr, sizeof(cookie)) != 0) {
        return -1;
    }

    size_t written = 0;
    for (;;) {
        leanfs_dir_entry_t e;
        uint32_t next = cookie;
        int rc = (dir >= 0) ? vfs_readdir_at(dir, &next, &e)
                            : vfs_readdir(path, &next, &e);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            break; /* end of the directory */
        }

        size_t name_len = k_strlen(e.name);
        size_t need = (sizeof(os_dirent_t) + name_len + 1 + 7) & ~(size_t)7;
        if (written + need > buflen) {
            /* Does not fit. Leave `cookie` where it was so this entry is
             * the first one the next call returns, and report what did
             * fit. A caller whose buffer is at least OS_DIRENT_MAX never
             * sees this on the first record, so "0 bytes" always means
             * end-of-directory and never "your buffer is too small". */
            break;
        }

        os_dirent_t rec;
        rec.ino = e.inode;
        rec.reclen = (unsigned short)need;
        rec.type = e.is_link ? OS_DT_LNK : (e.is_dir ? OS_DT_DIR : OS_DT_REG);
        rec.name_len = (unsigned char)name_len;
        if (copy_to_user(buf + written, &rec, sizeof(rec)) != 0 ||
            copy_to_user(buf + written + sizeof(rec), e.name, name_len + 1) != 0) {
            return -1;
        }
        written += need;
        cookie = next;
    }

    if (copy_to_user(cookie_ptr, &cookie, sizeof(cookie)) != 0) {
        return -1;
    }
    return (long)written;
}

/* M53: one directory, whose parent must already exist. */
static long sys_mkdir(uint64_t path_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_FS_WRITE)) {
        return -1; /* M65 */
    }
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0) {
        return -1;
    }
    return vfs_mkdir(path);
}

/* Only SIGKILL/SIGTERM are recognized ("basic set", M14) and both have
 * the same effect - there's no handler registration, so there's nothing
 * to distinguish them by yet beyond the exit code SYS_wait sees
 * (128 + signal). Delivery isn't instantaneous: it's checked at the
 * target's next syscall entry (syscall_handler, below) or scheduler tick
 * (sched.c) - enough for signals whose only action is "terminate". */
/* M85: may `self` signal `t`? Factored out of sys_kill, which had it
 * inline, because signalling a whole process group has to ask the same
 * question once per member - and asking it differently in two places is
 * how a permission model grows a hole.
 *
 * M65: a parent may always signal its own descendants, with or without
 * CAP_KILL_ANY - the relationship that gave you the pid is the one that
 * entitles you to use it, and a launcher that cannot stop what it started
 * is not a launcher. Anything else needs the capability.
 *
 * M76: and a process may always signal itself. raise() is kill(getpid())
 * and nothing else, so without this the most ordinary use of a signal
 * there is - a program telling itself something - would need the
 * capability to kill *other people's* processes. That is identity, not a
 * relationship, which is why it is a separate test from the walk.
 *
 * The walk goes up the whole parent chain rather than one level, because
 * a shell that spawned a program that spawned a program is still the
 * reason all three are running. It is bounded by MAX_TASKS: parent ids
 * are never reassigned to form a cycle, but a bound costs one comparison
 * and a kernel that loops here hangs the machine. */
static int may_signal(task_t *self, task_t *t) {
    if (t == self) {
        return 1;
    }
    task_t *up = t;
    for (int depth = 0; up && depth < MAX_TASKS; depth++) {
        if (up->parent_id == self->id) {
            return 1;
        }
        up = up->parent_id >= 0 ? sched_task_by_id(up->parent_id) : (task_t *)0;
    }
    return has_cap(CAP_KILL_ANY);
}

static long sys_kill(uint64_t pid, uint64_t sig, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    /* M76: every signal in signal.h, not the two that kill. sig 0 is the
     * "does this pid exist and may I signal it" probe every `kill -0`
     * in the world is, and delivers nothing. */
    if ((long)sig < 0 || sig > SIG_MAX) {
        return -1;
    }

    /* ---- M85: signalling a whole process group ------------------------
     *
     * `kill(-pgid, sig)` and `kill(0, sig)` are how a terminal interrupts
     * a *pipeline* rather than one of its stages - which is the thing ^C
     * actually does, and the reason process groups exist at all. A shell
     * that could only signal one pid would leave the other two stages of
     * `a | b | c` running with their input gone.
     *
     * The permission rule is the same one the single-pid case uses, asked
     * once per member: a parent may always signal its own descendants,
     * anything else needs CAP_KILL_ANY. Asked per member rather than once
     * for the group, because a group can contain processes this caller
     * did not start - and the ones it did start should still be signalled
     * rather than the whole call being refused. */
    if ((long)pid <= 0) {
        task_t *self_g = sched_current();
        int target_pgid = ((long)pid == 0) ? self_g->pgid : (int)(-(long)pid);
        int delivered = 0;
        int total = sched_task_count();
        for (int i = 0; i < total; i++) {
            task_t *m = sched_task_by_slot(i);
            if (!m || m->pgid != target_pgid || m->state == TASK_TERMINATED ||
                m->state == TASK_FREE) {
                continue;
            }
            if (!may_signal(self_g, m)) {
                continue;
            }
            delivered++;
            if (sig != 0) {
                sched_raise_signal(m, (int)sig);
            }
        }
        /* No member this caller may signal is the same answer as no such
         * group: -1, rather than a silent success that would let a shell
         * believe it had stopped a job it had not. */
        return delivered > 0 ? 0 : -1;
    }

    task_t *t = sched_task_by_id((int)pid);
    if (!t || t->state == TASK_TERMINATED) {
        return -1;
    }
    /* M65: a parent may always kill its own children, with or without
     * CAP_KILL_ANY - the relationship that gave you the pid is the one
     * that entitles you to use it, and a launcher that cannot stop what
     * it started is not a launcher. Anything else needs the capability.
     *
     * Walked up the parent chain rather than checked one level, because
     * a shell that spawned a program that spawned a program is still the
     * reason all three are running. The walk is bounded by MAX_TASKS:
     * parent ids are never reassigned to form a cycle, but a bound costs
     * one comparison and a kernel that loops here hangs the machine. */
    task_t *self = sched_current();
    if (!may_signal(self, t)) {
        return -1;
    }
    if (sig == 0) {
        return 0; /* the permission probe, answered by getting this far */
    }
    /* M76: the *target's* disposition decides what happens, not this
     * call - see sched_raise_signal. Before this milestone the two lines
     * "t->pending_signal = sig" were the entire signal model, and they
     * were correct precisely because the only two signals that existed
     * both killed. */
    sched_raise_signal(t, (int)sig);
    return 0;
}

/* Installs a pipe's read/write ends into the caller's own fd table (the
 * first two FD_NONE slots) and writes their fd numbers to fds_out[2].
 * SYS_spawn afterward copies the whole table, so a child spawned after
 * this inherits both ends. */
static long sys_pipe(uint64_t fds_out_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    /* M52: checked before anything is allocated, not after. A pipe
     * created and two fd slots claimed for a caller whose output pointer
     * then turns out to be garbage would be a leak caused by the error
     * path itself. */
    int out[2];
    if (!user_range_ok(fds_out_ptr, sizeof(out), 1)) {
        return -1;
    }
    task_t *self = sched_current();
    int read_fd = -1, write_fd = -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (self->fds[i].type == FD_NONE) {
            if (read_fd < 0) {
                read_fd = i;
            } else {
                write_fd = i;
                break;
            }
        }
    }
    if (read_fd < 0 || write_fd < 0) {
        return -1;
    }

    pipe_t *p = pipe_create();
    if (!p) {
        return -1;
    }
    self->fds[read_fd].type = FD_PIPE_READ;
    self->fds[read_fd].cloexec = 0; /* M84: a fresh descriptor is not close-on-exec until fcntl says so */
    self->fds[read_fd].pipe = p;
    self->fds[write_fd].type = FD_PIPE_WRITE;
    self->fds[write_fd].cloexec = 0; /* M84: a fresh descriptor is not close-on-exec until fcntl says so */
    self->fds[write_fd].pipe = p;

    out[0] = read_fd;
    out[1] = write_fd;
    return copy_to_user(fds_out_ptr, out, sizeof(out));
}

/* Read-only: nothing needs to *change* a process's group yet (no job
 * control in this shell), so there's no setpgid to go with it. */
/* M87: symbolic links. See SYS_symlink for the argument order, which is
 * symlink(2)'s and is the reverse of what most people guess. */
static long sys_symlink(uint64_t target_ptr, uint64_t path_ptr, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!has_cap(CAP_FS_WRITE)) {
        return -1;
    }
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0) {
        return -1;
    }
    /* The TARGET is copied as a plain string, not resolved as a path:
     * a symbolic link may legitimately point at something that does not
     * exist yet, and may be relative to the directory it sits in rather
     * than to the caller's. Normalising it here would quietly turn a
     * relative link into an absolute one and break the first case a
     * program uses it for. */
    char target[LEANFS_MAX_PATH];
    if (copy_str_from_user(target, target_ptr, sizeof(target)) != 0) {
        return -1;
    }
    return vfs_symlink(path, target);
}

/* M93: both arguments are real paths, unlike SYS_symlink's target - a
 * hard link names an inode that has to exist right now, so the second
 * one is resolved exactly like the first. That difference is the whole
 * difference between the two calls at this layer. */
static long sys_link(uint64_t old_ptr, uint64_t new_ptr, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!has_cap(CAP_FS_WRITE)) {
        return -1;
    }
    char old_path[LEANFS_MAX_PATH];
    char new_path[LEANFS_MAX_PATH];
    if (copy_path_from_user(old_path, old_ptr) != 0 ||
        copy_path_from_user(new_path, new_ptr) != 0) {
        return -1;
    }
    return vfs_link(old_path, new_path);
}

/* M93: see SYS_fsync's ABI note for what this does and does not promise
 * on a write-through filesystem. The fd is checked rather than ignored
 * because the answer for a pipe is "there is nothing here to make
 * durable", and 0 would be a claim. */
static long sys_fsync(uint64_t fd, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FDS) {
        return -1;
    }
    if (sched_current()->fds[fd].type != FD_FILE) {
        return -1;
    }
    /* M104: and the block cache under it, which is where a file's bytes
     * may now be sitting. vfs_sync marks the superblock clean; without
     * the flush this would be a promise about a disk that had not been
     * written. */
    vfs_sync();
    blk_flush();
    return 0;
}

/* M89: alarm(2) - see SYS_alarm and sched_set_alarm. */
static long sys_alarm(uint64_t seconds, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    /* Clamped rather than refused: a caller asking for more than 68
     * years of delay has made an arithmetic mistake, and the honest
     * outcome is the longest alarm this can express rather than an error
     * it will not check. */
    if (seconds > 0xffffffffu) {
        seconds = 0xffffffffu;
    }
    return (long)sched_set_alarm(sched_current(), (unsigned int)seconds);
}

/* M89: how much memory there is - see SYS_meminfo. */
static long sys_meminfo(uint64_t out_ptr, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    os_meminfo_t info;
    info.total_frames = pmm_total_frame_count();
    info.free_frames = pmm_free_frame_count();
    info.page_size = PAGE_SIZE;
    if (copy_to_user(out_ptr, &info, sizeof(info)) != 0) {
        return -1;
    }
    return 0;
}

/* M89: flush everything - see SYS_sync, and sys_fsync above for why the
 * two calls exist rather than one taking a sentinel descriptor. */
static long sys_sync(uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    vfs_sync();
    return 0;
}

/* ---- M96: the thread pointer and the futex ----------------------------
 *
 * Two calls that between them turn "there are threads" into "there are
 * threads a C runtime can be written against". Before them, `__thread`
 * had nowhere to live and every wait in <pthread.h> was a spin-then-
 * yield loop that burned a whole core while it waited.
 */
static long sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = sched_current();
    switch (code) {
    case ARCH_SET_FS:
        /* Checked for being a user address, and for nothing else. A
         * thread pointer is a number the runtime chose and this kernel
         * never dereferences it - but a task resumed with a KERNEL
         * address in FS would let one `mov %fs:0, %rax` in ring 3 read
         * kernel memory through a segment override, which is the whole
         * reason this is checked at all.
         *
         * One byte, not eight: what matters is which half of the address
         * space it is in, and a thread pointer legitimately points at
         * the END of a TLS block, where the byte after it may be
         * unmapped. */
        if (addr != 0 && !user_range_ok(addr, 1, 0)) {
            return -1;
        }
        self->fs_base = addr;
        /* Written now as well as at the next switch, because the caller
         * expects `%fs:0` to work on the instruction after this call and
         * may not be switched away from before then. */
        cpu_write_msr(MSR_FS_BASE, addr);
        return 0;
    case ARCH_GET_FS:
        if (!user_range_ok(addr, sizeof(uint64_t), 1)) {
            return -1;
        }
        return copy_to_user(addr, &self->fs_base, sizeof(self->fs_base));
    default:
        /* ARCH_SET_GS and ARCH_GET_GS land here. Refused by number
         * rather than accepted and ignored - see system_api/proc.h. */
        return -1;
    }
}

/* ---- the futex ---------------------------------------------------------
 *
 * One lock for every futex on the machine, and that is a decision rather
 * than a simplification. sched_block_on's contract is that the caller
 * holds the lock guarding its condition, so that a waker cannot run
 * between "the value is still what I expected" and "I am asleep" - the
 * two states a lost wakeup slips between. A per-address lock would be a
 * hash table of locks protecting a check that costs four instructions;
 * one lock makes the contract obviously satisfied and costs a contended
 * acquire per futex operation on a machine with eight cores and no
 * measured futex traffic. M69's rule: the measurement is what would
 * change this.
 */
static spinlock_t futex_lock;

static long sys_futex(uint64_t addr, uint64_t op, uint64_t val,
                      uint64_t timeout_ms, uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    if ((addr & 3u) != 0) {
        return -1; /* a futex word is a naturally aligned uint32_t */
    }
    if (!user_range_ok(addr, sizeof(uint32_t), 0)) {
        return -1;
    }
    if (op == FUTEX_WAKE) {
        /* `val` is a count; INT_MAX is what a broadcast passes. Capped
         * at MAX_TASKS because there cannot be more waiters than tasks
         * and an unbounded number would be a number this kernel cannot
         * mean. */
        int max = (val > (uint64_t)(unsigned)MAX_TASKS) ? MAX_TASKS : (int)val;
        return sched_wake_n((const void *)addr, max);
    }
    if (op != FUTEX_WAIT) {
        return -1;
    }

    uint64_t flags = spin_lock_irqsave(&futex_lock);
    uint32_t seen = *(const volatile uint32_t *)addr;
    if (seen != (uint32_t)val) {
        /* The value already changed. Not an error: what the caller was
         * waiting for happened between its own check and this call,
         * which is exactly the window the value check exists to cover.
         * -1 rather than 0 so the caller can tell "you were woken" from
         * "you never slept" - a condition variable needs that
         * difference. */
        spin_unlock_irqrestore(&futex_lock, flags);
        return -1;
    }
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = pit_get_ticks() * (1000 / PIT_HZ) + timeout_ms;
    }
    sched_block_on((const void *)addr, deadline, &futex_lock, &flags);
    spin_unlock_irqrestore(&futex_lock, flags);
    /* Woken, or the deadline passed. Told apart by the clock rather than
     * by a return value from sched_block_on, which has none: a waiter
     * that was woken one millisecond before its deadline and reports a
     * timeout has told a caller to re-check a condition it would re-check
     * anyway, so the failure mode of getting this wrong is a spurious
     * loop rather than a missed wake. */
    if (deadline != 0 && pit_get_ticks() * (1000 / PIT_HZ) >= deadline) {
        return -2;
    }
    return 0;
}

/* M89: who spawned this - see SYS_getppid. */
static long sys_getppid(uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return sched_current()->parent_id;
}

/* M89: the name behind a descriptor - see SYS_fdpath.
 *
 * A read of a string this kernel already holds, so the only interesting
 * part is what it refuses: a descriptor that is not an open file has no
 * path at all, and one whose path did not fit in the open-file entry has
 * a truncated one, which is worse than none. Both are -1. */
static long sys_fdpath(uint64_t fd, uint64_t out_ptr, uint64_t out_len,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FDS) {
        return -1;
    }
    task_t *self = sched_current();
    if (self->fds[fd].type != FD_FILE || !self->fds[fd].file) {
        return -1;
    }
    const char *p = self->fds[fd].file->path;
    if (p[0] != '/') {
        return -1; /* recorded as "" because it did not fit */
    }
    uint64_t n = 0;
    while (p[n]) {
        n++;
    }
    if (out_len < n + 1) {
        return -1;
    }
    if (copy_to_user(out_ptr, p, n + 1) != 0) {
        return -1;
    }
    return (long)n;
}

/* ---- M88 (second attempt) ---------------------------------------------
 *
 * See the ABI notes at SYS_rusage, SYS_statvfs and SYS_utime for what
 * each one promises. The three are here together because they are one
 * milestone and because each is short: every number they report was
 * already known to some part of this kernel and had simply never been
 * asked for from outside it.
 */
static long sys_rusage(uint64_t who, uint64_t out_ptr, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!user_range_ok(out_ptr, sizeof(os_rusage_t), 1)) {
        return -1;
    }
    task_t *self = sched_current();
    os_rusage_t r;
    if (who == OS_RUSAGE_SELF) {
        r.user_ticks = self->user_ticks;
        r.sys_ticks = self->sys_ticks;
        /* M98: the live address space's peak, which is the only one this
         * task can still be adding to, against everything already
         * captured from an exec'd-away image or a joined thread. */
        uint64_t peak = vmm_rss_peak_pages(self->pml4_phys);
        r.max_rss_pages = (peak > self->max_rss_pages) ? peak : self->max_rss_pages;
    } else if (who == OS_RUSAGE_CHILDREN) {
        r.user_ticks = self->child_user_ticks;
        r.sys_ticks = self->child_sys_ticks;
        r.max_rss_pages = self->child_max_rss_pages;
    } else {
        return -1; /* a third value would be a fourth meaning nothing here has */
    }
    *(os_rusage_t *)out_ptr = r;
    return 0;
}

static long sys_statvfs(uint64_t path_ptr, uint64_t out_ptr, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0 ||
        !user_range_ok(out_ptr, sizeof(os_statvfs_t), 1)) {
        return -1;
    }
    vfs_statvfs_t st;
    if (vfs_statvfs(path, &st) != 0) {
        return -1;
    }
    /* Converted field by field rather than copied whole, exactly as
     * SYS_stat does it: vfs_statvfs_t is what a filesystem knows and
     * os_statvfs_t is an ABI. They happen to have the same shape today
     * and a memcpy would make that a requirement nobody wrote down. */
    os_statvfs_t *out = (os_statvfs_t *)out_ptr;
    out->block_size = st.block_size;
    out->total_blocks = st.total_blocks;
    out->free_blocks = st.free_blocks;
    out->total_inodes = st.total_inodes;
    out->free_inodes = st.free_inodes;
    out->name_max = st.name_max;
    return 0;
}

static long sys_utime(uint64_t path_ptr, uint64_t mtime, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!has_cap(CAP_FS_WRITE)) {
        return -1;
    }
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0) {
        return -1;
    }
    return vfs_utime(path, (uint32_t)mtime);
}

static long sys_readlink(uint64_t path_ptr, uint64_t buf, uint64_t len,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0 || !user_range_ok(buf, len, 1)) {
        return -1;
    }
    return (long)vfs_readlink(path, (char *)buf, (size_t)len);
}

static long sys_lstat(uint64_t path_ptr, uint64_t out_ptr, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0 ||
        !user_range_ok(out_ptr, sizeof(os_stat_t), 1)) {
        return -OS_ERR_FAULT;
    }
    leanfs_stat_t st;
    if (vfs_lstat(path, &st) != 0) {
        return -OS_ERR_NOENT; /* M98: same fact as SYS_stat's - see there */
    }
    /* Converted field by field rather than copied whole, exactly as
     * SYS_stat does it. leanfs_stat_t is a filesystem's internal record
     * and os_stat_t is an ABI; copying one onto the other would make
     * their layouts a thing that has to stay accidentally identical, and
     * os_time.h's own header comment says they are deliberately not the
     * same type. */
    os_stat_t out;
    k_memset(&out, 0, sizeof(out)); /* M99 - see sys_stat */
    out.size = st.size;
    out.mtime = st.mtime;
    out.is_dir = st.is_dir;
    out.is_link = st.is_link;
    /* M99: a link is a link first. `kind` says what the entry IS, and
     * is_link is what says the caller is looking at the link rather than
     * through it - so a link's kind is the kind of the link itself,
     * which on this filesystem is a file with a path in it. */
    out.kind = st.is_dir ? OS_STAT_DIR : OS_STAT_FILE;
    out.inode = st.inode; /* M89 */
    return copy_to_user(out_ptr, &out, sizeof(out));
}

/* M87: set a file's length. Needs the descriptor to be writable, for the
 * same reason a write does - shortening a file is the most destructive
 * thing a caller can do to it without deleting it. */
static long sys_ftruncate(uint64_t fd, uint64_t length, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!has_cap(CAP_FS_WRITE)) {
        return -1;
    }
    task_t *self = sched_current();
    if (fd >= MAX_FDS || self->fds[fd].type != FD_FILE) {
        return -1;
    }
    openfile_t *of = self->fds[fd].file;
    if (!of || !of->writable) {
        return -1;
    }
    if (length > (uint64_t)LEANFS_MAX_FILE_SIZE) {
        return -1;
    }
    return vfs_handle_truncate_to(of->handle, (uint32_t)length);
}

/* ---- M85: ioctl, and only what a terminal needs -----------------------
 *
 * The first ioctl in this kernel, and the shape of it is the decision:
 * five commands with behaviour behind every one, rather than a general
 * "pass an integer to a driver" door. A door is what ioctl became
 * everywhere else, and it became that by being open before anything
 * needed it.
 *
 * M85 (second attempt): `fd` is no longer restricted to 0/1/2.
 *
 * It was, and the comment here said so: "there is one terminal and fd
 * 0/1/2 are it", with M87 named as where that stops being true. M87 gave
 * the terminal a path and the restriction stayed, because there was
 * still one terminal. There is not any more - /dev/ptmx makes them - so
 * the question this call asks is "which terminal is this descriptor",
 * and a descriptor that is not one is the only refusal.
 */
static tty_t *tty_for_fd(task_t *self, uint64_t fd, int *pty_number) {
    *pty_number = -1;
    if (fd >= MAX_FDS) {
        return NULL;
    }
    fd_slot_t *slot = &self->fds[fd];
    if (slot->type == FD_STDIN || slot->type == FD_STDOUT) {
        /* The console, and still an approximation: fd 0 may have been
         * redirected to a pipe by a shell and would then not be a
         * terminal at all. It is the approximation isatty has made since
         * M77 and narrowing it needs the fd table to record what a
         * descriptor was opened on, which nothing here does. */
        return tty_console();
    }
    if (slot->type == FD_FILE) {
        return (tty_t *)vfs_handle_tty(slot->file->handle, pty_number);
    }
    return NULL;
}

static long sys_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t a4,
                      uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = sched_current();
    int pty_number = -1;
    tty_t *t = tty_for_fd(self, fd, &pty_number);
    if (!t) {
        return -1; /* not a terminal - which is what ENOTTY means */
    }

    switch (cmd) {
    case TCGETS:
        if (copy_to_user(arg, &t->tio, sizeof(t->tio)) != 0) {
            return -1;
        }
        return 0;
    case TCSETS: {
        struct termios in;
        if (copy_from_user(&in, arg, sizeof(in)) != 0) {
            return -1;
        }
        /* Taken as given. There is no hardware here to refuse a setting -
         * c_cflag describes a UART nothing dials - so the only honest
         * failure would be a flag this discipline does not implement, and
         * termios.h defines none of those on purpose. */
        t->tio = in;
        return 0;
    }
    case TIOCGWINSZ: {
        struct winsize ws;
        ws.ws_row = t->rows;
        ws.ws_col = t->cols;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        if (copy_to_user(arg, &ws, sizeof(ws)) != 0) {
            return -1;
        }
        return 0;
    }
    /* M85: the write half of TIOCGWINSZ. Only a pty has one worth
     * setting - the console's size is a fact about the framebuffer - and
     * the refusal is the honest answer rather than a silent success that
     * would leave a program believing it had resized the screen. */
    case TIOCSWINSZ: {
        struct winsize ws;
        if (pty_number < 0) {
            return -1;
        }
        if (copy_from_user(&ws, arg, sizeof(ws)) != 0) {
            return -1;
        }
        t->rows = ws.ws_row;
        t->cols = ws.ws_col;
        /* SIGWINCH is what a program is actually waiting for here, and it
         * is not raised: system_api/include/signal.h has no SIGWINCH, and
         * inventing one so that this line could exist would be a signal
         * number nothing delivers. A program that asks the size when it
         * draws gets the new one; a program that waits to be told does
         * not. Written down rather than left to be discovered. */
        return 0;
    }
    /* M85: which /dev/pts/<n> is on the other end of this master. */
    case TIOCGPTN: {
        if (pty_number < 0) {
            return -1; /* the console is not a pty and has no number */
        }
        if (copy_to_user(arg, &pty_number, sizeof(pty_number)) != 0) {
            return -1;
        }
        return 0;
    }
    case TIOCGPGRP:
        if (copy_to_user(arg, &t->fg_pgid, sizeof(t->fg_pgid)) != 0) {
            return -1;
        }
        return 0;
    case TIOCSPGRP: {
        int pgid = 0;
        if (copy_from_user(&pgid, arg, sizeof(pgid)) != 0) {
            return -1;
        }
        /* An unclaimed terminal becomes this session's the first time a
         * process in it puts a job in the foreground. That is what a
         * shell does immediately after setsid, and doing it here rather
         * than in a separate "claim" call means there is no window in
         * which a terminal has a foreground group but no owner. */
        if (t->sid == 0) {
            t->sid = self->sid;
        }
        if (t->sid != self->sid) {
            return -1; /* somebody else's terminal */
        }
        t->fg_pgid = pgid;
        return 0;
    }
    /* M89: give up the controlling terminal.
     *
     * Only the session that owns this terminal may release it, and only
     * a caller that actually has one - a process with no controlling
     * terminal calling this is a program that thinks it detached and did
     * not, which is worth an error rather than a silent success.
     *
     * Releasing it leaves fg_pgid at 0 as well as sid: a terminal with
     * no session must not keep pointing at a foreground group, because
     * the next session to claim it would inherit somebody else's job as
     * its own foreground. */
    /* M89: claim this terminal. The two rules are POSIX's, and each one
     * stops a real mistake: only a session leader may claim (so a child
     * cannot re-point its parent's session at a different terminal), and
     * only an unowned terminal may be claimed (so one session cannot
     * take another's). `arg` is the "steal it anyway" flag on Linux and
     * is ignored here - stealing needs a privilege model this machine
     * does not have, and silently honouring it would be the fake check
     * M65 refused. */
    case TIOCSCTTY:
        if (self->sid != self->id) {
            return -1; /* not a session leader */
        }
        if (t->sid != 0 && t->sid != self->sid) {
            return -1; /* somebody else's terminal */
        }
        t->sid = self->sid;
        /* M85 (second attempt): and the session leader's group becomes
         * the foreground one.
         *
         * This line was missing and its absence is invisible until there
         * is a second terminal. On the console it did not matter -
         * nothing else was ever going to claim it, and the shell called
         * tcsetpgrp immediately afterwards. On a pty it is the whole
         * thing: the process that claims the terminal is the one in it,
         * and the process on the other end (a terminal emulator, in a
         * different session) is not allowed to call TIOCSPGRP on a
         * terminal it does not own - correctly, because that check is
         * what stops one session stealing another's job control. So
         * without this, a ^C typed into a pty raised SIGINT on process
         * group 0, which is nobody.
         *
         * Only for a terminal that had no foreground group: re-claiming
         * one must not steal the job the session is already running. */
        if (t->fg_pgid == 0) {
            t->fg_pgid = self->pgid;
        }
        return 0;
    case TIOCNOTTY:
        if (t->sid == 0 || t->sid != self->sid) {
            return -1;
        }
        t->sid = 0;
        t->fg_pgid = 0;
        return 0;
    default:
        return -1;
    }
}

/* ---- M85: process groups and sessions ---------------------------------
 *
 * The three calls that make "which program is the terminal talking to" a
 * question with an answer. A process group is a *job* - the stages of one
 * pipeline, killed and suspended together. A session is a set of groups
 * sharing one terminal, and is the level at which a controlling terminal
 * is owned.
 *
 * setpgid's rules are POSIX's and each one exists to stop a real mistake:
 * a process may only change its own group or that of a child it spawned
 * (so a program cannot rearrange somebody else's jobs), and it may not
 * move a process into a group in a different session (so a job cannot be
 * moved to a terminal it does not belong to). `pid` 0 means the caller
 * and `pgid` 0 means "make it a leader of its own group", which is what
 * every shell passes.
 */
static long sys_setpgid(uint64_t pid_arg, uint64_t pgid_arg, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = sched_current();
    int pid = (int)pid_arg;
    int pgid = (int)pgid_arg;

    task_t *t = (pid == 0) ? self : sched_task_by_id(pid);
    if (!t || t->state == TASK_TERMINATED || t->state == TASK_FREE) {
        return -1;
    }
    if (t != self && t->parent_id != self->id) {
        return -1; /* somebody else's job is not this program's to rearrange */
    }
    if (pgid == 0) {
        pgid = t->id; /* leader of its own group - what every shell asks for */
    }
    if (pgid != t->id) {
        /* Joining an existing group: it has to exist, and it has to be in
         * this session. A group in another session is a job on another
         * terminal. */
        task_t *leader = sched_task_by_id(pgid);
        if (!leader || leader->sid != t->sid) {
            return -1;
        }
    }
    t->pgid = pgid;
    return 0;
}

/* Makes the caller the leader of a brand-new session and a brand-new
 * process group, with NO controlling terminal. That last part is the
 * whole reason anything calls this: a process with no terminal cannot be
 * sent SIGINT by one, which is what "run in the background, detached"
 * actually means.
 *
 * Refused for a process that is already a group leader, which is POSIX's
 * rule and not an arbitrary one: the new session's id would collide with
 * the group it already leads, and a session and a group that share an id
 * without being the same thing is a knot nothing untangles. */
static long sys_setsid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = sched_current();
    if (self->pgid == self->id) {
        return -1;
    }
    self->sid = self->id;
    self->pgid = self->id;
    return self->id;
}

static long sys_getsid(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *t = (pid == 0) ? sched_current() : sched_task_by_id((int)pid);
    if (!t) {
        return -1;
    }
    return t->sid;
}

static long sys_getpgid(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *t = sched_task_by_id((int)pid);
    if (!t) {
        return -1;
    }
    return t->pgid;
}

/* M19: growth-only sbrk. `increment_u` is really a signed byte count
 * (system_api's own convention has no unsigned/signed distinction, every
 * arg rides in a uint64_t) - a negative value is rejected outright rather
 * than treated as a shrink request, since nothing this project's
 * malloc.c (user_space/lib) ever does needs to give pages back. Maps
 * whole new pages on demand via vmm_map_page_in, same "grow by exactly
 * what's needed, page by page" shape as the kernel's own heap.c. */
static long sys_sbrk(uint64_t increment_u, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    int64_t increment = (int64_t)increment_u;
    if (increment < 0) {
        return -1;
    }
    /* M79: the *address space's* break, not this task's. A thread shares
     * a page table with its leader, so two independent breaks would each
     * grow into the other's memory - see sched_vm_owner. */
    task_t *cur = sched_vm_owner(sched_current());
    uint64_t old_brk = cur->heap_brk;
    uint64_t new_brk = old_brk + (uint64_t)increment;
    if (new_brk > USER_HEAP_LIMIT || new_brk < old_brk /* overflow */) {
        return -1;
    }
    /* M29: pmm_try_alloc_frame, not pmm_alloc_frame - a user process
     * growing its own heap past whatever physical memory remains must
     * fail *this* syscall, not panic every other task on the system along
     * with it. Whatever got mapped before the shortfall stays mapped
     * (heap_mapped_end only ever advances, same as every other path
     * through this heap - see proc.h's own note on the invariant
     * heap_brk <= heap_mapped_end) - safe to leave as-is since it's
     * genuinely-owned, valid memory, just more than this one request
     * needed; the caller sees a clean failure and can retry smaller. */
    while (cur->heap_mapped_end < new_brk) {
        uint64_t phys = pmm_try_alloc_frame();
        if (phys == 0) {
            return -1;
        }
        /* Q9: and the MAPPING can run out too, which the comment above
         * did not cover and this call did not survive.
         *
         * M29 converted the frame allocation here for exactly the reason
         * it gives - "a user process growing its own heap past whatever
         * physical memory remains must fail *this* syscall, not panic
         * every other task on the system along with it" - and then
         * handed the frame to the panicking form of the mapper. A page
         * table is itself a frame, so a program that grows its heap far
         * enough exhausts the page-table allocations too, and did it by
         * halting the machine. The argument is M29's, unchanged; this is
         * the other half of the same line.
         *
         * The frame goes back rather than being left mapped-nowhere:
         * nothing points at it, so unlike the pages already mapped it is
         * not "genuinely-owned, valid memory" - it is a leak. */
        if (vmm_try_map_page_in(cur->pml4_phys, cur->heap_mapped_end, phys,
                                VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
            pmm_free_frame(phys);
            return -1;
        }
        cur->heap_mapped_end += PAGE_SIZE;
    }
    cur->heap_brk = new_brk;
    return (long)old_brk;
}

static long sys_shm_create(uint64_t size, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return shm_create((size_t)size, sched_current()->id);
}

/* Maps segment `id` into the caller's own address space at the next free
 * slot of its private shm region (task_t's shm_next_vaddr, USER_SHM_BASE
 * upward) - like the kernel/user heaps, this only ever grows forward,
 * never reuses an address a previous SYS_shm_map call already claimed. */
/* M55 - see SYS_shm_unmap's contract. Unmaps, never frees. */
static long sys_shm_unmap(uint64_t vaddr, uint64_t bytes, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if ((vaddr & (PAGE_SIZE - 1)) != 0 || bytes == 0) {
        return -1; /* not something SYS_shm_map ever returned */
    }
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t end = vaddr + pages * PAGE_SIZE;
    /* The shm window only. A caller that could name any address here
     * could unmap its own code out from under itself, or - worse, since
     * PML4[0] is the same subtree in every address space - a kernel page.
     * That is the hole M52 closed in SYS_shm_free and it is not being
     * reopened next to it. */
    if (end < vaddr || vaddr < USER_SHM_BASE || end > USER_FB_BASE) {
        return -1;
    }
    uint64_t pml4 = sched_current()->pml4_phys;
    for (uint64_t i = 0; i < pages; i++) {
        /* Return value ignored: a page that was not mapped is nothing to
         * undo. What matters is that nothing is still mapped afterwards. */
        (void)vmm_unmap_page_in(pml4, vaddr + i * PAGE_SIZE);
    }
    return 0;
}

static long sys_shm_map(uint64_t id, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    int64_t size = shm_get_size((int)id);
    if (size < 0) {
        return -1;
    }
    /* M79: through the address space's owner, for the same reason
     * SYS_sbrk is - two threads with independent shm cursors would map
     * two different segments on top of each other. */
    task_t *cur = sched_vm_owner(sched_current());
    uint64_t vaddr = cur->shm_next_vaddr;
    if (shm_map_into((int)id, cur->pml4_phys, vaddr, VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
        return -1;
    }
    uint64_t pages = ((uint64_t)size + PAGE_SIZE - 1) / PAGE_SIZE;
    cur->shm_next_vaddr += pages * PAGE_SIZE;
    return (long)vaddr;
}

/* M20: read-only framebuffer geometry - see SYS_fb_map for the pixels
 * themselves. */
/* M50: see SYS_shm_free's own doc comment. The order here is the whole
 * point: unmap from this process first, *then* hand the frames back, so
 * there is never a window in which a freed frame is still reachable
 * through a live page table. (The dead client that also had it mapped is
 * a different matter and a safe one - its address space is never loaded
 * again, and this kernel has no vmm_destroy_address_space to reclaim it
 * with either way.)
 *
 * `vaddr` comes from the caller because nothing in this kernel records
 * which address spaces a segment is mapped into. Adding that registry
 * would be real bookkeeping for exactly one caller that already knows the
 * answer; passing a wrong vaddr unmaps the caller's own pages, which is
 * no worse than any other bad pointer this ABI accepts. */
static long sys_shm_free(uint64_t id, uint64_t vaddr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    int64_t pages = shm_page_count((int)id);
    if (pages < 0) {
        return -1;
    }
    if (vaddr != 0) {
        if ((vaddr & (PAGE_SIZE - 1)) != 0) {
            return -1; /* not something SYS_shm_map ever returned */
        }
        /* M52: nor is anything outside the caller's own private region.
         * Without this an address in PML4[0] would have been unmapped
         * from the *shared kernel* map - vmm_unmap_page_in walks whatever
         * hierarchy the address indexes into, and PML4[0] is the same
         * subtree in every address space. That is a user-triggerable way
         * to take the machine down, which is precisely what this
         * milestone is about. */
        uint64_t last = vaddr + (uint64_t)pages * PAGE_SIZE;
        if (vaddr < USER_REGION_BASE || last > USER_REGION_LIMIT || last < vaddr) {
            return -1;
        }
        uint64_t pml4 = sched_current()->pml4_phys;
        for (int64_t i = 0; i < pages; i++) {
            /* Return value ignored on purpose: a page that wasn't mapped
             * is nothing to undo, and the caller asking to free a segment
             * it never mapped (vaddr from a stale variable, say) must not
             * be able to fail the free. What matters is that nothing
             * stays mapped to a frame that is about to be handed back. */
            (void)vmm_unmap_page_in(pml4, vaddr + (uint64_t)i * PAGE_SIZE);
        }
    }
    return shm_free((int)id, sched_current()->id);
}

static long sys_fb_info(uint64_t out_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    wm_fb_info_t out;
    out.width = fb_width();
    out.height = fb_height();
    out.pitch = fb_pitch_bytes();
    out.bpp = 32;
    return copy_to_user(out_ptr, &out, sizeof(out));
}

/* Maps the real linear framebuffer into the caller's address space at a
 * fixed address (proc.h's USER_FB_BASE) page by page, the same identity-
 * style vmm_map_page_in loop fb_init itself uses for the kernel's own
 * mapping - two entirely separate virtual mappings (kernel's and this
 * process's) end up pointing at the exact same physical frames, which is
 * fine and coherent on x86 (physical memory is a single global resource;
 * there's no aliasing hazard for normal write-back memory). By
 * convention only the compositor calls this - nothing enforces that
 * yet, matching this project's existing no-permission-model trust level. */
static long sys_fb_map(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_FRAMEBUFFER)) {
        return (long)(uint64_t)-1; /* M65: the compositor owns the screen; nothing it launches may paint on it */
    }
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *cur = sched_current();
    uint64_t phys_base = fb_phys_addr();
    /* M58: the *mapped* extent, not this mode's. fb.c only ever grows its
     * mapping, so after a mode change the high-water mark is what both
     * sides have to cover - a client whose mapping stopped at the old
     * mode's size would draw off the end of it. */
    uint64_t size = fb_mapped_bytes();
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        /* Q9: a mapping that runs out of page tables refuses the call
         * rather than the machine. Partial mappings are left in place -
         * the address space is torn down with the process, and the
         * caller has been told the framebuffer is not there. */
        if (vmm_try_map_page_in(cur->pml4_phys, USER_FB_BASE + i * PAGE_SIZE,
                                phys_base + i * PAGE_SIZE,
                                VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
            return (long)(uint64_t)-1;
        }
    }
    /* Q7: the screen now has a user-space owner, so the kernel console
     * stops writing to it. Until this line, every process's stdout was
     * painting glyphs into the framebuffer the compositor was composing
     * into - and scrolling the whole thing up sixteen pixels whenever the
     * console cursor reached the bottom row. See klog.c. */
    klog_release_console();
    return (long)USER_FB_BASE;
}

static long sys_mouse_read(uint64_t out_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    /* The range is checked before the read, so a garbage pointer cannot
     * consume an event and then fail to deliver it - which would lose
     * that event for good, the queue being the only copy. */
    mouse_event_t ev;
    if (!user_range_ok(out_ptr, sizeof(ev), 1)) {
        return -1;
    }
    if (!mouse_read(&ev)) {
        return 0;
    }
    return copy_to_user(out_ptr, &ev, sizeof(ev)) == 0 ? 1 : -1;
}

/* Like sys_pipe, but installs a *named* pipe (pipe_named) instead of a
 * fresh anonymous one - see kernel/ipc/pipe.h's header comment for why
 * that's the rendezvous mechanism M20's compositor/client protocol
 * needs. Fd-slot-finding logic is identical to sys_pipe's, deliberately
 * not factored out for two call sites this small. */
static long sys_pipe_open(uint64_t name_ptr, uint64_t fds_out_ptr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    /* Both arguments checked before any state is touched, same reason
     * sys_pipe does. A name too long to be a named pipe is refused rather
     * than truncated: a truncated name is a *different* pipe, and this is
     * a rendezvous mechanism where that means connecting two programs
     * that never meant to talk. */
    char name[NAMED_PIPE_NAME_LEN];
    int out[2];
    if (copy_str_from_user(name, name_ptr, sizeof(name)) != 0 ||
        !user_range_ok(fds_out_ptr, sizeof(out), 1)) {
        return -1;
    }
    task_t *self = sched_current();
    int read_fd = -1, write_fd = -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (self->fds[i].type == FD_NONE) {
            if (read_fd < 0) {
                read_fd = i;
            } else {
                write_fd = i;
                break;
            }
        }
    }
    if (read_fd < 0 || write_fd < 0) {
        return -1;
    }

    pipe_t *p = pipe_named(name);
    if (!p) {
        return -1;
    }
    self->fds[read_fd].type = FD_PIPE_READ;
    self->fds[read_fd].cloexec = 0; /* M84: a fresh descriptor is not close-on-exec until fcntl says so */
    self->fds[read_fd].pipe = p;
    self->fds[write_fd].type = FD_PIPE_WRITE;
    self->fds[write_fd].cloexec = 0; /* M84: a fresh descriptor is not close-on-exec until fcntl says so */
    self->fds[write_fd].pipe = p;

    out[0] = read_fd;
    out[1] = write_fd;
    return copy_to_user(fds_out_ptr, out, sizeof(out));
}

/* M21: the raw-event counterpart to sys_read's fd=0 line-blocking
 * contract - pops one decoded character straight off keyboard.h's ring
 * buffer without ever blocking, so a caller juggling several input
 * sources in one loop (the compositor) doesn't stall on this one. */
static long sys_kbd_read(uint64_t out_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    /* Checked before the read, for the same reason sys_mouse_read is:
     * the keyboard queue is the only copy of that character. */
    if (!user_range_ok(out_ptr, sizeof(char), 1)) {
        return -1;
    }
    int c = keyboard_read();
    if (c == -1) {
        return 0;
    }
    char ch = (char)c;
    return copy_to_user(out_ptr, &ch, sizeof(ch)) == 0 ? 1 : -1;
}

/* M21: a non-consuming peek at how many bytes a pipe read fd has ready -
 * pipe_t's count field is already right here (kernel/ipc/pipe.h), so this
 * needs no new pipe.c entry point, just a bounds/type check identical in
 * shape to sys_write/sys_read's. */
static long sys_pipe_poll(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FDS) {
        return -1;
    }
    fd_slot_t *slot = &sched_current()->fds[fd];
    if (slot->type != FD_PIPE_READ) {
        return -1;
    }
    return (long)pipe_buffered(slot->pipe); /* M67: under pipe.c's lock, not a reach into the struct */
}

/* M21: milliseconds since pit_init() - just a unit conversion over the
 * same tick counter M6/M7's self-tests already read directly. */
static long sys_uptime_ms(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return (long)(pit_get_ticks() * (1000 / PIT_HZ));
}

/* The missing half of SYS_pipe/SYS_pipe_open: those only ever install a
 * pipe's ends at brand-new fd numbers, so a caller that wants an
 * *existing* fd number (its own stdout, fd 1) to become a pipe end
 * instead - so a child it's about to SYS_spawn inherits that pipe as its
 * own fd 1 - has no way to get there without this. Plain slot copy, no
 * refcounting: this project has no SYS_close to release whatever newfd
 * used to hold, the same "hasn't been needed yet" simplicity pipe.h's
 * header comment already notes for pipe ownership generally. */
static long sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (oldfd >= MAX_FDS || newfd >= MAX_FDS) {
        return -1;
    }
    task_t *self = sched_current();
    if (self->fds[oldfd].type == FD_NONE) {
        return -1;
    }
    if (newfd == oldfd) {
        return (long)newfd; /* dup2(fd, fd) is a no-op everywhere it exists, and releasing then retaining the same slot would not be */
    }
    /* M59: newfd's old occupant is genuinely released now, rather than
     * overwritten. SYS_dup2's own comment used to say it deliberately did
     * not close - which was the honest description when nothing was
     * refcounted and "closing" meant blanking a slot. With a refcount it
     * would be a leak: the shell's `> out.txt` points fd 1 at a file, and
     * whatever fd 1 was would never be given back. */
    fd_release(&self->fds[newfd]);
    self->fds[newfd] = self->fds[oldfd];
    fd_retain(&self->fds[newfd]);
    return (long)newfd;
}

/* Non-blocking counterpart to SYS_wait: never yields, just reports
 * whether pid has already terminated - -2 (not -1, which already means
 * "no such task") is "still running", so a caller with its own event
 * loop to keep servicing (a GUI terminal, spawning a child while it
 * keeps redrawing/routing input) can poll this every iteration instead
 * of blocking. Reaps pid the same way SYS_wait does once it does report
 * a real exit code. */
/* M50: the release half of the fd table, which this project has gone
 * fifty milestones without.
 *
 * It only clears the caller's own slot. It deliberately does not free the
 * pipe object, close the pipe, or touch anyone else's table: pipes here
 * are not reference-counted (see kernel/ipc/pipe.h), a named pipe is
 * *meant* to outlive every fd that has ever pointed at it - that is the
 * whole rendezvous mechanism the window protocol is built on - and an
 * anonymous pipe's other end is usually held by a child that was spawned
 * with a copy of this table. Freeing anything here would turn "I am done
 * with this descriptor" into "everyone else's is now dangling".
 *
 * So this is exactly as much close() as this kernel can honestly
 * implement, and it is the part that was actually leaking: fd *slots*,
 * of which there are MAX_FDS per task and which a long-lived process
 * (the compositor, the shell) burns two of on every reconnect. */
static long sys_close(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FDS) {
        return -1;
    }
    task_t *self = sched_current();
    if (self->fds[fd].type == FD_NONE) {
        return -1; /* closing something already closed is a caller bug worth reporting, not a no-op */
    }
    fd_release(&self->fds[fd]);
    return 0;
}

/* M101: the profiler's whole control and readout surface.
 *
 * One syscall with an operation selector rather than eight syscall
 * numbers, which is a departure from how every other call in this table
 * is shaped and is worth defending. The eight operations are one
 * instrument: they share a capability, a lifetime and a set of structs,
 * and nothing will ever call PROFILE_OP_STOP that could not also call
 * PROFILE_OP_START. Eight numbers would spend eight entries of a table
 * whose size this arc is already trying to justify, to express a thing
 * that is genuinely one interface. `ioctl` is the precedent already in
 * this file.
 *
 * Every op is gated once, at the top. There is no read-only subset here
 * that deserves a weaker gate: the sample list is the part that reveals
 * what other processes are doing, and it is the part a reader wants. */
static long sys_profile(uint64_t op, uint64_t arg1, uint64_t arg2, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (!has_cap(CAP_PROCESS_LIST)) {
        return -1; /* M65 */
    }

    switch (op) {
    case PROFILE_OP_START:
        profile_start();
        return 0;
    case PROFILE_OP_STOP:
        profile_stop();
        return 0;
    case PROFILE_OP_RESET:
        profile_reset();
        return 0;
    case PROFILE_OP_STATS: {
        prof_stats_t st;
        profile_get_stats(&st);
        return copy_to_user(arg1, &st, sizeof(st)) == 0 ? 0 : -1;
    }
    case PROFILE_OP_SAMPLES: {
        if (arg2 == 0 || arg2 > PROF_BUCKETS ||
            !user_range_ok(arg1, arg2 * sizeof(prof_sample_t), 1)) {
            return -1;
        }
        /* Straight into the caller's buffer. The alternative - snapshot
         * into a kernel array and then copy - would need PROF_BUCKETS *
         * sizeof(prof_sample_t) of stack or heap for no gain: the range
         * is validated above, and profile_snapshot holds its lock for
         * the walk either way. */
        return profile_snapshot((prof_sample_t *)arg1, (int)arg2);
    }
    case PROFILE_OP_SYSCALLS: {
        if (arg2 == 0 || arg2 > SYSCALL_COUNT ||
            !user_range_ok(arg1, arg2 * sizeof(prof_syscount_t), 1)) {
            return -1;
        }
        /* Indexed by syscall number, holes included. A caller that
         * wanted a dense list would have to be told which number each
         * row was, which is the same information in a worse shape. */
        prof_syscount_t *out = (prof_syscount_t *)arg1;
        for (uint64_t i = 0; i < arg2; i++) {
            syscount_entry_t e;
            syscount_get((int)i, &e);
            out[i].calls = e.calls;
            out[i].cycles = e.cycles;
        }
        return (long)arg2;
    }
    case PROFILE_OP_SYSRESET:
        syscount_reset();
        return 0;
    case PROFILE_OP_TIMING:
        return syscount_set_timing(arg1 ? 1 : 0);
    default:
        return -1;
    }
}

static long sys_wait_nb(uint64_t pid_arg, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *t = sched_task_by_id((int)pid_arg);
    if (!t) {
        return -1;
    }
    if (t->state != TASK_TERMINATED) {
        return -2;
    }
    t->reaped = 1;
    int code = t->exit_code;
    sched_reap_slot(t); /* M54: same as SYS_wait - consuming the status is what frees the slot */
    return code;
}

/* Voluntary cooperative yield - see SYS_yield's comment in
 * system_api/include/syscall.h for why the GUI stack needs this. Just
 * forces an immediate reschedule the same way scheduler_tick_cpu does at
 * the end of a real time slice, except on the caller's own request
 * rather than the PIT's. */
static long sys_yield(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    schedule();
    return 0;
}

/* M29: non-reaping liveness peek - see SYS_task_alive's doc comment
 * (system_api/include/syscall.h) for why this has to be a separate call
 * from SYS_wait_nb rather than just "call that and ignore the exit code":
 * SYS_wait_nb sets `reaped` on a terminated task, which is only correct
 * for that task's actual parent. */
static long sys_task_alive(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *t = sched_task_by_id((int)pid);
    if (!t) {
        return -1;
    }
    if (t->state != TASK_TERMINATED) {
        return 1;
    }
    return t->exit_code == 0 ? 2 : 0;
}

/* M29: see pipe_reset's own comment (kernel/ipc/pipe.h) - resets whichever
 * pipe `fd` names, identical bounds/type check to sys_pipe_poll's. */
static long sys_pipe_reset(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FDS) {
        return -1;
    }
    fd_slot_t *slot = &sched_current()->fds[fd];
    if (slot->type != FD_PIPE_READ && slot->type != FD_PIPE_WRITE) {
        return -1;
    }
    pipe_reset(slot->pipe);
    return 0;
}

/* M32: see SYS_kbd_modifiers' own doc comment (system_api/include/
 * syscall.h) - a live peek, not a buffered/consumed read, so it takes no
 * arguments and every caller sees the current physical state. */
static long sys_kbd_modifiers(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return keyboard_modifiers();
}

/* M45: a whole-shot snapshot of the task table - see SYS_taskinfo's own
 * doc comment (system_api/include/syscall.h) and system_api/include/
 * proc.h for the record layout.
 *
 * Read-only and allocation-free, which is what lets it be honest about
 * being a snapshot: nothing here can fail partway and leave a caller
 * holding a half-filled iterator. Terminated tasks are included on
 * purpose - their slots are never recycled (sched.c's task_spawn_common)
 * and their exit code is exactly what a task manager wants to show for
 * something that just died, rather than the row silently vanishing.
 *
 * sched_task_count() is read once up front rather than per iteration:
 * tasks are only ever appended, so a task created *during* this loop
 * simply isn't in this snapshot - which is the correct answer for a
 * snapshot, and strictly better than a count that grows under the
 * bounds check. */
static long sys_taskinfo(uint64_t buf, uint64_t max_entries, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_PROCESS_LIST)) {
        return -1; /* M65 */
    }
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    /* M52: the cap is TASK_INFO_MAX rather than a hand-picked 4096.
     * There has never been anything for entries past the task table to
     * hold, and "a number comfortably larger than the real one" is the
     * exact shape of near-duplicate cap this project has shipped three
     * bugs behind. An out-of-range count is an error return, never a
     * silent clamp: a caller that asked for more than exists has a bug,
     * and quietly answering a different question hides it. */
    if (max_entries == 0 || max_entries > TASK_INFO_MAX ||
        !user_range_ok(buf, max_entries * sizeof(task_info_t), 1)) {
        return -1;
    }
    task_info_t *out = (task_info_t *)buf;
    /* M54: by slot, and free slots are skipped - so this reports what is
     * *live* rather than everything that has ever existed. Before slot
     * recycling those were the same list, which is why the task manager
     * used to show every boot self-test that had ever run. */
    int total = sched_task_count();
    uint64_t written = 0;
    for (int i = 0; i < total && written < max_entries; i++) {
        task_t *t = sched_task_by_slot(i);
        if (!t) {
            continue;
        }
        task_info_t *e = &out[written];
        e->pid = t->id;
        e->parent_pid = t->parent_id;
        e->pgid = t->pgid;
        /* M68: TASK_BLOCKED gets its own value rather than falling through
         * to READY. A task manager that showed every sleeping process as
         * "ready" would be describing the machine this OS stopped being. */
        e->state = (t->state == TASK_TERMINATED) ? TASK_INFO_TERMINATED
                 : (t->state == TASK_RUNNING)    ? TASK_INFO_RUNNING
                 : (t->state == TASK_BLOCKED)    ? TASK_INFO_BLOCKED
                                                 : TASK_INFO_READY;
        e->exit_code = t->exit_code;
        int fds = 0;
        for (int f = 0; f < MAX_FDS; f++) {
            if (t->fds[f].type != FD_NONE) {
                fds++;
            }
        }
        e->open_fds = fds;
        e->shm_segments = shm_count_by_owner(t->id);
        int n = 0;
        for (; t->name[n] && n < TASK_INFO_NAME_MAX - 1; n++) {
            e->name[n] = t->name[n];
        }
        e->name[n] = '\0';
        written++;
    }
    return (long)written;
}

static long sys_clipboard_set(uint64_t buf, uint64_t len, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_CLIPBOARD)) {
        return -1; /* M65 */
    }
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!user_range_ok(buf, len, 0)) {
        return -1;
    }
    clipboard_set((const void *)buf, (size_t)len);
    return 0;
}

static long sys_clipboard_get(uint64_t buf, uint64_t maxlen, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_CLIPBOARD)) {
        return -1; /* M65: read is the half that matters - a clipboard any program may read at will is a keylogger with a delay */
    }
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!user_range_ok(buf, maxlen, 1)) {
        return -1;
    }
    return (long)clipboard_get((void *)buf, (size_t)maxlen);
}

/* M47: see SYS_shutdown's own doc comment. Returns only for a mode this
 * kernel doesn't recognize; on POWER_OFF/POWER_REBOOT power_shutdown
 * never comes back, so the caller's own return value is unobservable. */
static long sys_shutdown(uint64_t mode, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_POWER)) {
        return -1; /* M65 - and this is the check SYS_shutdown's own comment refused to fake until there was something behind it */
    }
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (mode != POWER_OFF && mode != POWER_REBOOT) {
        return -1;
    }
    power_shutdown((int)mode);
}

/* ---- M59: files with descriptors ------------------------------------ */

/* Finds a free fd, or -1. The two lowest are stdin/stdout by convention
 * and are never handed out here even if a caller closed them - a
 * program that closed its own stdout getting a *file* back as fd 1 from
 * an unrelated open is exactly the kind of surprise this ABI does not
 * need. */
static int alloc_fd(task_t *t) {
    for (int i = 2; i < MAX_FDS; i++) {
        if (t->fds[i].type == FD_NONE) {
            return i;
        }
    }
    return -1;
}

static long sys_open(uint64_t path_ptr, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0) {
        return -1;
    }
    int writable = (flags & OPEN_WRITE) != 0;
    /* M65: gated on the *flags*, not on the call. Reading is not a
     * capability in this OS - there are no secrets on the disk and
     * claiming a read boundary that nothing enforces would be exactly
     * the fake check caps.h exists to avoid - but creating, truncating
     * and writing are. Checked before vfs_open so an OPEN_CREATE that
     * will be refused does not leave the file behind. */
    if ((flags & (OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE)) && !has_cap(CAP_FS_WRITE)) {
        return -1;
    }
    /* M87: the create flags travel together now - OPEN_EXCL means
     * nothing without OPEN_CREATE, and leanfs is the layer that can make
     * the pair atomic because it is the one holding the lock. */
    int create_flags = ((flags & OPEN_CREATE) ? LEANFS_OPEN_CREATE : 0) |
                       ((flags & OPEN_EXCL) ? LEANFS_OPEN_EXCL : 0);
    if ((flags & OPEN_EXCL) && !(flags & OPEN_CREATE)) {
        return -1; /* O_EXCL without O_CREAT is undefined; refusing is the honest reading */
    }
    /* M89: O_NOFOLLOW, which M87 named and did not land.
     *
     * Asked before the open rather than during it, which makes it a
     * check and not a guarantee: a symlink created between the lstat and
     * the open is followed. That is the same non-atomicity <fcntl.h>
     * documents for the *at() family, for the same reason - one
     * principal, no adversary - and it is written here rather than left
     * to be inferred from the absence of a lock. */
    if (flags & OPEN_NOFOLLOW) {
        leanfs_stat_t st;
        if (vfs_lstat(path, &st) == 0 && st.is_link) {
            return -1;
        }
    }
    int handle = vfs_open(path, create_flags);
    if (handle < 0) {
        return -1;
    }
    /* M89: a directory opens read-only and for nothing else.
     *
     * leanfs_open will now hand back a directory handle (see its note on
     * why the *at() family needs one). Everything that would modify it
     * is refused here rather than there, because "may I write to this"
     * is a question about the caller's flags and leanfs_open does not
     * see them. A write to a directory that succeeded would be writing
     * over its records. */
    int opening_dir = vfs_is_dir(path);
    if (opening_dir &&
        (flags & (OPEN_WRITE | OPEN_TRUNCATE | OPEN_APPEND | OPEN_CREATE))) {
        return -1;
    }
    if ((flags & OPEN_TRUNCATE) && writable && vfs_handle_truncate(handle) != 0) {
        return -1;
    }
    task_t *self = sched_current();
    int fd = alloc_fd(self);
    if (fd < 0) {
        return -1;
    }
    openfile_t *of = openfile_alloc(handle, writable, path, opening_dir);
    if (!of) {
        return -1;
    }
    if (flags & OPEN_APPEND) {
        of->offset = vfs_handle_size(handle);
    }
    self->fds[fd].type = FD_FILE;
    /* M84: a fresh descriptor is close-on-exec only if the caller asked
     * at open time - which is the race-free way to ask, and the reason
     * O_CLOEXEC exists alongside F_SETFD at all: a fork between the open
     * and the fcntl would otherwise hand the flag's absence to a child. */
    self->fds[fd].cloexec = (flags & OPEN_CLOEXEC) ? 1 : 0;
    self->fds[fd].file = of;
    return fd;
}

static long sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FDS) {
        return -1;
    }
    fd_slot_t *slot = &sched_current()->fds[fd];
    if (slot->type != FD_FILE) {
        return -1; /* a pipe has no position to seek to, and saying so beats pretending */
    }
    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)slot->file->offset; break;
    case SEEK_END: base = (int64_t)vfs_handle_size(slot->file->handle); break;
    default: return -1;
    }
    int64_t target = base + (int64_t)(int32_t)offset;
    if (target < 0 || target > (int64_t)LEANFS_MAX_FILE_SIZE) {
        return -1;
    }
    slot->file->offset = (uint32_t)target;
    return (long)target;
}

static long sys_stat(uint64_t path_ptr, uint64_t out_ptr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0) {
        return -OS_ERR_FAULT;
    }
    leanfs_stat_t st;
    /* M98: -OS_ERR_NOENT rather than -1, and it is a fact rather than a
     * guess - see the constant's note in system_api/include/syscall.h
     * for why a failed stat here means exactly "no such path". */
    if (vfs_stat(path, &st) != 0) {
        return -OS_ERR_NOENT;
    }
    os_stat_t out;
    /* M99: zeroed first. The struct has a pad byte in it and this is
     * copied to user space wholesale; an uninitialised pad byte is a
     * byte of kernel stack handed to a program. */
    k_memset(&out, 0, sizeof(out));
    out.size = st.size;
    out.mtime = st.mtime;
    out.is_dir = st.is_dir;
    out.kind = st.is_dir ? OS_STAT_DIR : OS_STAT_FILE; /* M99 */
    /* M87: always 0 here, and truthfully so - this resolve follows
     * links, so whatever it landed on is by definition not one.
     * SYS_lstat is the call that can say otherwise. */
    out.is_link = 0;
    out.inode = st.inode; /* M89 - see os_stat_t */
    return copy_to_user(out_ptr, &out, sizeof(out));
}

/* ---- M78: memory that can be given back --------------------------------
 *
 * M19 gave this OS one memory primitive - a growth-only sbrk - and its
 * own ABI comment argued the case honestly: "a free-list allocator built
 * on top never needs to give pages back". That was true for as long as
 * the only allocator was one this project wrote. It stops being true the
 * moment a program maps something large, uses it, and is finished with
 * it: a bump allocator has no concept of a hole, so the pages behind a
 * freed 4 MiB buffer stay this process's forever.
 *
 * The arena is a fixed window in the per-process layout (proc.h's
 * USER_MMAP_BASE/LIMIT) and the bookkeeping is a sorted array in task_t.
 * Sorted is what makes first-fit a single forward scan, and first fit
 * over a *sorted* list is what makes "the pages you gave back are the
 * pages you get next" true rather than accidental.
 *
 * Frames are allocated at the moment of the call rather than on a fault,
 * because this kernel has no page-fault handler that could fill one in -
 * a fault in ring 3 kills the task (M52). Lazy allocation would be a
 * genuinely better mmap and it is a different milestone: it needs the
 * fault handler to be able to tell "this address is inside a mapping
 * that has not been backed yet" from "this program dereferenced null",
 * which is exactly the table below plus a decision this project has not
 * had to make.
 */
static void mmap_slot_remove(task_t *t, int index);

/* ---- M98: two mappings that are one mapping ---------------------------
 *
 * Found by the first build this machine ever ran for itself. GCC's
 * garbage collector asks the kernel for memory in half-megabyte chunks
 * and keeps asking; at the hundred and twenty-eighth chunk SYS_mmap
 * started returning -1 and cc1 stopped with `virtual memory exhausted`
 * at about 60 MiB - on a machine with four gigabytes free. The ceiling
 * was never bytes. It was MAX_MMAP_REGIONS, one table entry per call,
 * and a compiler that asks two hundred times gets refused however much
 * memory there is.
 *
 * Two anonymous, private mappings that are adjacent and carry the same
 * protection are indistinguishable, to every path in this kernel, from
 * one mapping that spans both: the fault handler reads `prot` and
 * nothing else, munmap already cuts a region into head and tail, and
 * mprotect already splits one. So they are stored as one, which is what
 * every Unix does with a VMA and for exactly this reason.
 *
 * Deliberately NOT merged: anything file-backed (two adjacent regions of
 * a file are only one region if their file offsets are adjacent too, and
 * getting that wrong silently maps the wrong page of the wrong file) and
 * anything shared (its frames come from and go back to filemap, and the
 * bookkeeping is per-region). Both are refused by returning 0 here
 * rather than by being handled, because the case that pays is the
 * anonymous one and a merge rule nobody needs is a merge rule nobody
 * tests.
 *
 * The alternative was to raise MAX_MMAP_REGIONS, and it is the wrong
 * fix: 128 entries is 4 KiB in every task_t, a build that wants 200
 * would want 2,000 next, and a ceiling raised to fit one program is a
 * ceiling the next program finds again. This makes the number a limit on
 * how *fragmented* an address space is rather than on how many times a
 * program has called mmap.
 */
static int mmap_mergeable(const mmap_region_t *r, uint32_t prot, int handle,
                          int shared) {
    return r->pages != 0 && r->handle == -1 && handle == -1 &&
           !r->shared && !shared && r->prot == prot;
}

/* `merge` is what tells a NEW mapping from a piece of surgery, and it is
 * not an optimisation switch - it is a correctness one. The first
 * version of this merged unconditionally, and mmap_split_for below then
 * could not split: it shrank a region to end at the cut, inserted the
 * remainder starting AT the cut, watched the insert merge the two back
 * into the region it had just taken apart, and rescanned - forever. The
 * boot hung in `vmtest`, which is the self-test that mprotects the
 * middle of a mapping, and it hung there for the whole seven-hundred
 * second ceiling with no output at all.
 *
 * So: a caller adding a mapping asks to merge, and a caller cutting one
 * up says no. The two are different operations that happened to share a
 * function. */
static int mmap_slot_cmp_insert(task_t *t, uint64_t base, uint32_t pages, uint32_t prot,
                                int handle, uint32_t file_page, int shared, int merge) {
    /* Inserts, keeping the array sorted by base with free slots (pages
     * == 0) pushed to the end. Returns 0, or -1 if the table is full. */
    uint64_t end = base + (uint64_t)pages * PAGE_SIZE;
    for (int i = 0; merge && i < MAX_MMAP_REGIONS; i++) {
        if (t->mmaps[i].pages == 0) {
            break; /* sorted: nothing live follows */
        }
        if (!mmap_mergeable(&t->mmaps[i], prot, handle, shared)) {
            continue;
        }
        uint64_t rstart = t->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)t->mmaps[i].pages * PAGE_SIZE;
        if (rend == base) {
            t->mmaps[i].pages += pages;
            /* And the one after it, if this new range has just closed
             * the gap between two live regions - which is what happens
             * when a program frees a chunk out of the middle of its
             * arena and then asks for one the same size. */
            if (i + 1 < MAX_MMAP_REGIONS &&
                mmap_mergeable(&t->mmaps[i + 1], prot, handle, shared) &&
                t->mmaps[i + 1].base == end) {
                t->mmaps[i].pages += t->mmaps[i + 1].pages;
                mmap_slot_remove(t, i + 1);
            }
            return 0;
        }
        if (rstart == end) {
            t->mmaps[i].base = base;
            t->mmaps[i].pages += pages;
            return 0;
        }
    }
    int free_slot = -1;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (t->mmaps[i].pages == 0) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        return -1;
    }
    /* Find where it belongs and shift the tail up by one. The array is
     * 32 entries, so a memmove-shaped loop is cheaper than any structure
     * that would avoid it. */
    int at = free_slot;
    for (int i = 0; i < free_slot; i++) {
        if (t->mmaps[i].base > base) {
            at = i;
            break;
        }
    }
    for (int i = free_slot; i > at; i--) {
        t->mmaps[i] = t->mmaps[i - 1];
    }
    t->mmaps[at].base = base;
    t->mmaps[at].pages = pages;
    t->mmaps[at].prot = prot; /* M82 - the page fault that fills this region reads it */
    /* M91 (second attempt) - see mmap_region_t for what each of these
     * changes and where. */
    t->mmaps[at].handle = handle;
    t->mmaps[at].file_page = file_page;
    t->mmaps[at].shared = (uint8_t)(shared != 0);
    return 0;
}

static void mmap_slot_remove(task_t *t, int index) {
    for (int i = index; i < MAX_MMAP_REGIONS - 1; i++) {
        t->mmaps[i] = t->mmaps[i + 1];
    }
    t->mmaps[MAX_MMAP_REGIONS - 1].base = 0;
    t->mmaps[MAX_MMAP_REGIONS - 1].pages = 0;
    t->mmaps[MAX_MMAP_REGIONS - 1].prot = 0;
    t->mmaps[MAX_MMAP_REGIONS - 1].handle = -1;
    t->mmaps[MAX_MMAP_REGIONS - 1].file_page = 0;
    t->mmaps[MAX_MMAP_REGIONS - 1].shared = 0;
}

/* The first gap in the arena that `pages` will fit into, or 0. THE
 * function this milestone is about: it walks the live mappings in
 * address order and returns the first address with enough room *before*
 * the next one, so a hole left by a munmap is found before the space
 * past everything. A high-water mark would never look at a hole at all,
 * which is precisely the difference the self-test grades. */
static uint64_t mmap_find_gap(task_t *t, uint32_t pages) {
    uint64_t need = (uint64_t)pages * PAGE_SIZE;
    uint64_t candidate = USER_MMAP_BASE;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (t->mmaps[i].pages == 0) {
            break; /* sorted, with free slots at the end - nothing live follows */
        }
        uint64_t start = t->mmaps[i].base;
        uint64_t end = start + (uint64_t)t->mmaps[i].pages * PAGE_SIZE;
        if (candidate + need <= start) {
            return candidate;
        }
        if (end > candidate) {
            candidate = end;
        }
    }
    if (candidate + need <= USER_MMAP_LIMIT) {
        return candidate;
    }
    return 0;
}

/* M91: is [base, base + pages) entirely free inside the arena? Used by
 * the address-hint path, which must not hand back an address that
 * overlaps a live mapping, and by MAP_FIXED, which must know whether it
 * has anything to displace. */
static int mmap_range_is_free(task_t *t, uint64_t base, uint64_t pages) {
    uint64_t end = base + pages * PAGE_SIZE;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (t->mmaps[i].pages == 0) {
            break;
        }
        uint64_t rstart = t->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)t->mmaps[i].pages * PAGE_SIZE;
        if (base < rend && rstart < end) {
            return 0;
        }
    }
    return 1;
}

static long sys_munmap(uint64_t addr, uint64_t len, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6);

/* M91: SYS_mmap grows the three arguments every other Unix has had since
 * 4.2BSD - an address, and (accepted but refused) a descriptor and an
 * offset. M78 left them out because nothing could use them; a dynamic
 * loader is the program that cannot be written without the first, since
 * placing a shared object means choosing where it goes and then placing
 * every other one relative to it. */
static long sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
                      uint64_t fd, uint64_t offset) {
    if (len == 0) {
        return -1;
    }
    /* ---- M91 (second attempt): what may be mapped ---------------------
     *
     * The paragraph that stood here said this call was "still anonymous
     * and private, and still refused by name otherwise". Both halves are
     * gone, and this is the milestone that earns them: a file may back a
     * mapping, and a mapping may be shared.
     *
     * Exactly one of MAP_PRIVATE and MAP_SHARED, which is what POSIX
     * requires and what makes "neither was passed" an error rather than
     * a default somebody has to guess at.
     */
    int shared = (flags & MAP_SHARED) != 0;
    int private_ = (flags & MAP_PRIVATE) != 0;
    if (shared == private_) {
        return -1; /* both, or neither */
    }
    int anon = (flags & MAP_ANONYMOUS) != 0;

    int handle = -1;
    uint32_t file_page = 0;
    if (!anon) {
        /* A file-backed mapping. The descriptor has to be a file this
         * caller has open - not a pipe, not a socket, not a directory -
         * and the offset has to be a whole number of pages, because a
         * mapping's first byte is a page boundary and there is nowhere
         * for a sub-page offset to go. */
        if ((long)fd < 0 || (uint64_t)fd >= MAX_FDS) {
            return -1;
        }
        if ((offset & (PAGE_SIZE - 1)) != 0) {
            return -1;
        }
        task_t *cur = sched_current();
        if (cur->fds[fd].type != FD_FILE || !cur->fds[fd].file) {
            return -1;
        }
        /* A shared writable mapping needs the descriptor to be writable,
         * for the reason a write(2) does: this is a write to the file,
         * arriving later and through a different door. */
        if (shared && (prot & PROT_WRITE) && !cur->fds[fd].file->writable) {
            return -1;
        }
        if (shared && (prot & PROT_WRITE) && !has_cap(CAP_FS_WRITE)) {
            return -1; /* M65: this ends up as bytes on the disk */
        }
        handle = cur->fds[fd].file->handle;
        file_page = (uint32_t)(offset / PAGE_SIZE);
    } else {
        /* Anonymous. Both the descriptor and the offset are meaningless,
         * and a caller who passed either is a caller who thinks they are
         * mapping a file. POSIX says the descriptor is ignored; refusing
         * is louder and is what this kernel did before file mappings
         * existed, so a program that got an error yesterday gets the
         * same one today rather than silently different memory. */
        if (offset != 0 || (long)fd >= 0) {
            return -1;
        }
        /* MAP_SHARED|MAP_ANONYMOUS is memory shared with children, and
         * it is refused here rather than half-built: making it work is
         * fork's job (a child gets the same frame instead of a
         * copy-on-write one) and there is nothing for a process with no
         * children to observe. It is refused rather than quietly given
         * private memory, which is the rule this whole paragraph used to
         * be about - see kernel/ipc/shm.h, which is what two unrelated
         * processes share memory through and has lifetime rules a
         * MAP_SHARED would have to duplicate badly.
         *
         * The condition for building it: something that needs memory
         * shared across a fork and cannot use shm - which is a program,
         * not an argument. */
        if (shared) {
            return -1;
        }
    }
    /* M91: PROT_NONE is now a mapping. M78 refused it because "there is
     * no way to express 'mapped but inaccessible' in a page table entry
     * this kernel sets up" - true then, and demand paging is what changed
     * it: the region exists, no page is ever built for it, and the fault
     * that touches it is fatal. That is exactly a guard page, and
     * sched.c's fill_policy is where it is enforced. */
    if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC)) {
        return -1;
    }
    uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0 || pages > (USER_MMAP_LIMIT - USER_MMAP_BASE) / PAGE_SIZE) {
        return -1;
    }

    /* M79: the arena belongs to the address space, not to the task -
     * two threads with separate mmap tables would hand out the same
     * addresses twice. sched_vm_owner is where that is decided. */
    task_t *self = sched_vm_owner(sched_current());
    if (self->pml4_phys == vmm_kernel_pml4_phys()) {
        return -1; /* a kernel thread has no private region to map into */
    }

    uint64_t base = 0;
    if (flags & MAP_FIXED) {
        /* MAP_FIXED means "here, or fail" - and, per every Unix since
         * SunOS, it *replaces* whatever was already there rather than
         * refusing. That second half is not a detail: it is how a loader
         * lays a second segment of the same object over the tail of the
         * first one's page reservation. Refused outside the arena, which
         * keeps the rule this kernel has always had - a program may
         * arrange its own arena and nothing else. */
        if ((addr & (PAGE_SIZE - 1)) != 0) {
            return -1;
        }
        if (addr < USER_MMAP_BASE || addr + pages * PAGE_SIZE > USER_MMAP_LIMIT) {
            return -1;
        }
        if (!mmap_range_is_free(self, addr, pages) &&
            sys_munmap(addr, pages * PAGE_SIZE, 0, 0, 0, 0) != 0) {
            return -1;
        }
        base = addr;
    } else if (addr != 0) {
        /* An address hint. Honoured when it is page-aligned, inside the
         * arena and free; ignored otherwise, which is what a hint means -
         * a caller that cannot accept an answer elsewhere passes
         * MAP_FIXED. */
        uint64_t want = addr & ~(PAGE_SIZE - 1);
        if (want >= USER_MMAP_BASE && want + pages * PAGE_SIZE <= USER_MMAP_LIMIT &&
            mmap_range_is_free(self, want, pages)) {
            base = want;
        }
    }
    if (base == 0) {
        base = mmap_find_gap(self, (uint32_t)pages);
    }
    if (base == 0) {
        return -1;
    }
    /* The one caller that merges: this is a new mapping, and a new
     * mapping next to an identical one is one mapping. */
    if (mmap_slot_cmp_insert(self, base, (uint32_t)pages, (uint32_t)prot,
                             handle, file_page, shared, 1) != 0) {
        return -1; /* the table is full - see MAX_MMAP_REGIONS */
    }

    /* M82: and that is the whole call.
     *
     * Until now this loop allocated and mapped every page before
     * returning, and M78's comment on SYS_mmap said so: "backed by real
     * frames at the moment of the call, because nothing here fills a page
     * in on a fault." Something does now (sched_fault_fill), so a mapping
     * costs address space and a table slot until it is touched, and a
     * page costs a frame at the moment it is first read or written.
     *
     * What this buys is not speed, it is the ability to reserve more than
     * the machine has - which is what every real program that mmaps
     * assumes, and what M80's unmeasured "memory budget" blocker was
     * actually about. What it costs is that running out of memory now
     * happens at the instruction that touches rather than at the call
     * that reserves; sched_fault_fill's header says so at more length. */
    return (long)base;
}

static long sys_munmap(uint64_t addr, uint64_t len, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if ((addr & (PAGE_SIZE - 1)) != 0 || len == 0) {
        return -1;
    }
    uint64_t end = addr + ((len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (end <= addr) {
        return -1; /* wrapped */
    }
    /* Bounded to the caller's own arena, so this can unmap what SYS_mmap
     * made and nothing else - not its code, not its stack, not the
     * framebuffer. Same rule and the same reason as SYS_shm_unmap's. */
    if (addr < USER_MMAP_BASE || end > USER_MMAP_LIMIT) {
        return -1;
    }
    task_t *self = sched_vm_owner(sched_current()); /* M79 - see sys_mmap */

    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (self->mmaps[i].pages == 0) {
            break;
        }
        uint64_t rstart = self->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)self->mmaps[i].pages * PAGE_SIZE;
        uint64_t cut_start = addr > rstart ? addr : rstart;
        uint64_t cut_end = end < rend ? end : rend;
        if (cut_start >= cut_end) {
            continue; /* no overlap with this mapping */
        }
        /* The frames first, whatever shape the cut turns out to be.
         *
         * M82: by page table rather than by address. This was a loop over
         * every address in the cut, which was exactly right when a
         * mapping was fully backed - and became thirty-six thousand
         * four-level walks to free two frames the moment mappings became
         * sparse. */
        /* M91 (second attempt): a shared mapping's frames are not this
         * address space's to free. They belong to kernel/mm/filemap.c,
         * which holds the one copy every mapper of that file page shares
         * - so the reference is dropped and the frame goes back only if
         * this was the last holder. Unmapping without freeing is the
         * distinction; vmm_unmap_range_free would have handed a frame
         * somebody else is still reading straight back to the pmm. */
        if (self->mmaps[i].shared) {
            sched_release_shared_range(self, cut_start, cut_end);
        } else {
            vmm_unmap_range_free(self->pml4_phys, cut_start, cut_end);
        }
        if (cut_start == rstart && cut_end == rend) {
            mmap_slot_remove(self, i);
            i--; /* the tail shifted down into this index */
        } else if (cut_start == rstart) {
            /* The head went. The file offset moves with the base, or the
             * tail would read the wrong part of the file. */
            self->mmaps[i].file_page += (uint32_t)((cut_end - rstart) / PAGE_SIZE);
            self->mmaps[i].base = cut_end;
            self->mmaps[i].pages = (uint32_t)((rend - cut_end) / PAGE_SIZE);
        } else if (cut_end == rend) {
            self->mmaps[i].pages = (uint32_t)((cut_start - rstart) / PAGE_SIZE);
        } else {
            /* An interior range: one mapping becomes two. Needs a free
             * slot, and a table with none has to refuse - after the
             * frames are already gone, which is the honest ordering: the
             * memory the caller asked to release IS released, and what
             * fails is the bookkeeping for the tail. Reported so the
             * caller knows the tail is no longer reachable. */
            self->mmaps[i].pages = (uint32_t)((cut_start - rstart) / PAGE_SIZE);
            if (mmap_slot_cmp_insert(self, cut_end,
                                      (uint32_t)((rend - cut_end) / PAGE_SIZE),
                                      self->mmaps[i].prot, self->mmaps[i].handle,
                                      self->mmaps[i].file_page +
                                          (uint32_t)((cut_end - rstart) / PAGE_SIZE),
                                      self->mmaps[i].shared, 0) != 0) {
                return -1;
            }
            i = -1; /* the array was re-sorted underneath; rescan from the start */
        }
    }
    /* A range that overlapped nothing is a success: the caller asked for
     * those pages not to be mapped, and they are not. Every Unix answers
     * this the same way and for the same reason. */
    return 0;
}

/* M91 (second attempt): msync - see SYS_msync for what each flag does
 * and which one is refused. */
static long sys_msync(uint64_t addr, uint64_t len, uint64_t flags, uint64_t a4,
                      uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if ((addr & (PAGE_SIZE - 1)) != 0 || len == 0) {
        return -1;
    }
    if (flags & MS_INVALIDATE) {
        return -1;
    }
    if (flags & ~(uint64_t)(MS_ASYNC | MS_SYNC | MS_INVALIDATE)) {
        return -1;
    }
    if (!has_cap(CAP_FS_WRITE)) {
        return -1; /* M65: this ends up as bytes on the disk */
    }
    uint64_t end = addr + ((len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (end <= addr || addr < USER_MMAP_BASE || end > USER_MMAP_LIMIT) {
        return -1;
    }
    task_t *self = sched_vm_owner(sched_current());
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (self->mmaps[i].pages == 0) {
            break;
        }
        uint64_t rstart = self->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)self->mmaps[i].pages * PAGE_SIZE;
        if (end <= rstart || addr >= rend) {
            continue;
        }
        if (self->mmaps[i].shared && self->mmaps[i].handle >= 0) {
            /* Every dirty page of the file, not only the ones in the
             * range: filemap tracks dirtiness per file page and not per
             * mapping, so a finer flush would be a claim this kernel
             * cannot make. Flushing more than was asked is always
             * correct for msync; flushing less is not. */
            filemap_sync(self->mmaps[i].handle);
        }
    }
    return 0;
}

/* ---- M91: mprotect and madvise ---------------------------------------
 *
 * Both walk the caller's own regions and both split them the same way
 * munmap does, which is why the shape below repeats: a range that covers
 * part of a mapping turns one region into two or three, and the
 * bookkeeping has to survive that. The alternative - a region list that
 * stores permissions per page - would be a page table written twice.
 *
 * The rule they share, and the reason mprotect is not simply a page-table
 * rewrite: in a demand-paged address space most of a mapping has no page
 * table entry at all, so the authority on what an untouched page will
 * become is the *region's* prot. Changing only the entries that exist
 * would leave a mapping whose first half is read-only and whose second
 * half becomes writable the moment it is touched.
 */
static int mmap_split_for(task_t *t, uint64_t addr, uint64_t end) {
    /* Ensures no live region straddles either boundary, so that after
     * this call every region is either wholly inside [addr, end) or
     * wholly outside it. Returns -1 if the table has no room for the
     * pieces, which is the only way this can fail. */
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (t->mmaps[i].pages == 0) {
            break;
        }
        uint64_t rstart = t->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)t->mmaps[i].pages * PAGE_SIZE;
        uint64_t cut = 0;
        if (addr > rstart && addr < rend) {
            cut = addr;
        } else if (end > rstart && end < rend) {
            cut = end;
        }
        if (cut == 0) {
            continue;
        }
        uint32_t prot = t->mmaps[i].prot;
        int handle = t->mmaps[i].handle;
        uint32_t fp = t->mmaps[i].file_page + (uint32_t)((cut - rstart) / PAGE_SIZE);
        int shared = t->mmaps[i].shared;
        t->mmaps[i].pages = (uint32_t)((cut - rstart) / PAGE_SIZE);
        /* No merge: this call exists to CREATE the boundary at `cut`, and
         * a merge would put it straight back - see the note on the
         * parameter. */
        if (mmap_slot_cmp_insert(t, cut, (uint32_t)((rend - cut) / PAGE_SIZE), prot,
                                 handle, fp, shared, 0) != 0) {
            /* Put it back rather than leaving a region shorter than the
             * memory it describes - a mapping the caller can still touch
             * with nothing saying what it may become is worse than a
             * refusal. */
            t->mmaps[i].pages = (uint32_t)((rend - rstart) / PAGE_SIZE);
            return -1;
        }
        i = -1; /* the array was re-sorted underneath; rescan */
    }
    return 0;
}

static long sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot, uint64_t a4,
                          uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if ((addr & (PAGE_SIZE - 1)) != 0 || len == 0) {
        return -1;
    }
    if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC)) {
        return -1;
    }
    uint64_t end = addr + ((len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (end <= addr) {
        return -1;
    }
    if (addr < USER_MMAP_BASE || end > USER_MMAP_LIMIT) {
        return -1; /* the caller's own arena, exactly as munmap is bounded */
    }
    task_t *self = sched_vm_owner(sched_current());
    if (self->pml4_phys == vmm_kernel_pml4_phys()) {
        return -1;
    }
    /* Every page of the range has to be inside a mapping. POSIX says
     * ENOMEM otherwise, and this project would rather say no than
     * silently protect the half that exists.
     *
     * Checked BEFORE the split, and measured as overlap rather than as
     * whole regions, which is the same question asked in the order that
     * does not leave damage behind: splitting first and refusing after
     * would consume a region slot every time a program called mprotect on
     * a range it does not own, and a program in a loop would run the
     * table out for a call that never succeeded. */
    uint64_t covered = 0;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (self->mmaps[i].pages == 0) {
            break;
        }
        uint64_t rstart = self->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)self->mmaps[i].pages * PAGE_SIZE;
        uint64_t lo = rstart > addr ? rstart : addr;
        uint64_t hi = rend < end ? rend : end;
        if (lo < hi) {
            covered += hi - lo;
        }
    }
    if (covered != end - addr) {
        return -1;
    }
    if (mmap_split_for(self, addr, end) != 0) {
        return -1;
    }
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (self->mmaps[i].pages == 0) {
            break;
        }
        uint64_t rstart = self->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)self->mmaps[i].pages * PAGE_SIZE;
        if (rstart >= addr && rend <= end) {
            self->mmaps[i].prot = (uint32_t)prot;
        }
    }
    uint64_t flags = VMM_FLAG_USER;
    if (prot & PROT_WRITE) {
        flags |= VMM_FLAG_WRITABLE;
    }
    if (prot & PROT_EXEC) {
        flags |= VMM_FLAG_EXEC;
    }
    vmm_protect_range_in(self->pml4_phys, addr, end, flags);
    return 0;
}

/* MADV_DONTNEED and nothing else, which is the one piece of advice that
 * is not advice: it is an instruction to drop the pages, and the next
 * touch gets zeroes. Everything else in <sys/mman.h>'s advice list is a
 * hint about future access that this kernel has no cache to apply it to,
 * and is accepted as a no-op success - refusing MADV_WILLNEED would make
 * a program fail for asking politely. */
static long sys_madvise(uint64_t addr, uint64_t len, uint64_t advice, uint64_t a4,
                         uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if ((addr & (PAGE_SIZE - 1)) != 0 || len == 0) {
        return -1;
    }
    uint64_t end = addr + ((len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (end <= addr) {
        return -1;
    }
    if (addr < USER_MMAP_BASE || end > USER_MMAP_LIMIT) {
        return -1;
    }
    if (advice != MADV_DONTNEED) {
        return 0;
    }
    task_t *self = sched_vm_owner(sched_current());
    if (self->pml4_phys == vmm_kernel_pml4_phys()) {
        return -1;
    }
    /* The frames go back and the regions stay. That distinction is the
     * whole call: after this the address is still reserved, still has the
     * permissions it had, and reads as zero - which is what a program
     * that has finished with a large buffer but not with the space wants,
     * and what an allocator returning memory to the system without giving
     * up its arena does. */
    vmm_unmap_range_free(self->pml4_phys, addr, end);
    return 0;
}

/* M77: `struct stat` for an open descriptor. The one thing SYS_stat
 * structurally cannot answer, because it takes a name and a descriptor
 * outlives one. Only a file has an answer - see the ABI comment on why a
 * pipe is refused rather than described. */
static long sys_fstat(uint64_t fd, uint64_t out_ptr, uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FDS) {
        return -1;
    }
    fd_slot_t *slot = &sched_current()->fds[fd];
    os_stat_t out;
    k_memset(&out, 0, sizeof(out)); /* M99 - see sys_stat */

    /* ---- M99: every open descriptor has an answer --------------------
     *
     * M77 wrote "only a file has an answer" and refused the rest, and
     * for the callers that existed then that was fine: nothing asked
     * fstat about a pipe or about its own stdout.
     *
     * CPython does, on every start. Its create_stdio() fstats 0, 1 and 2
     * before wrapping them, and a descriptor it cannot stat becomes
     * `None` - so on this machine `print()` wrote nothing and returned
     * successfully, because print with sys.stdout None is defined to do
     * nothing at all. No error anywhere, and an interpreter with no
     * output.
     *
     * What each kind can truthfully say is below. The size of a thing
     * with no length is 0 and its mtime is 0 - those are answers, not
     * placeholders - and `kind` is the field that carries the rest, so
     * <sys/stat.h>'s S_ISCHR and S_ISFIFO mean something here for the
     * first time. */
    switch (slot->type) {
    case FD_FILE: {
        leanfs_stat_t st;
        if (vfs_handle_stat(slot->file->handle, &st) != 0) {
            return -1;
        }
        out.size = st.size;
        out.mtime = st.mtime;
        out.is_dir = st.is_dir;
        out.kind = st.is_dir ? OS_STAT_DIR : OS_STAT_FILE;
        /* M87: a descriptor cannot name a link. Opening one follows it,
         * so what this handle refers to is whatever the link pointed at
         * - which is why fstat has no lstat counterpart anywhere. */
        out.is_link = 0;
        out.inode = st.inode; /* M89 */
        break;
    }
    case FD_STDIN:
    case FD_STDOUT:
        /* The console, and it is a character device in the strict sense
         * the word has: no length, no position, and a read that blocks
         * until somebody types. */
        out.kind = OS_STAT_CHR;
        break;
    case FD_PIPE_READ:
    case FD_PIPE_WRITE:
        out.kind = OS_STAT_FIFO;
        break;
    case FD_SOCKET:
        out.kind = OS_STAT_SOCK;
        break;
    default:
        return -1; /* FD_NONE: not an open descriptor */
    }
    return copy_to_user(out_ptr, &out, sizeof(out)) == 0 ? 0 : -1;
}

static long sys_rmdir(uint64_t path_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_FS_WRITE)) {
        return -1; /* M65 */
    }
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0) {
        return -1;
    }
    return vfs_rmdir(path);
}

static long sys_time(uint64_t out_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    os_datetime_t now;
    rtc_read(&now);
    if (out_ptr != 0 && copy_to_user(out_ptr, &now, sizeof(now)) != 0) {
        return -1;
    }
    return now.valid ? (long)os_unix_time(&now) : 0;
}

static long sys_getcaps(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return (long)sched_current()->caps;
}

static long sys_dropcaps(uint64_t keep, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *self = sched_current();
    self->caps &= (uint32_t)keep; /* the only assignment to caps outside a spawn, and it is an AND */
    return (long)self->caps;
}

/* ---- M64: sockets ----------------------------------------------------
 *
 * Five calls and no new bookkeeping: a socket lives in the fd table
 * beside pipes and open files, is refcounted by the same fd_retain/
 * fd_release M59 wrote, and is closed by the SYS_close that already
 * exists. The only thing these functions do that the file syscalls do
 * not is validate a user pointer for a *variable* length the caller
 * chose, which copy_from_user was already built for in M52.
 */
static struct socket *socket_for_fd(uint64_t fd) {
    task_t *self = sched_current();
    if (fd >= MAX_FDS || self->fds[fd].type != FD_SOCKET) {
        return (struct socket *)0;
    }
    return self->fds[fd].sock;
}

/* M66: hands `s` to a fresh descriptor in the caller's table, or -1 and
 * releases it. Shared by sys_socket and sys_accept, which are the two
 * calls in this kernel that create a descriptor out of nothing. */
static long install_socket_fd(struct socket *s) {
    task_t *self = sched_current();
    int fd = alloc_fd(self);
    if (fd < 0) {
        socket_unref(s);
        return -1;
    }
    self->fds[fd].type = FD_SOCKET;
    self->fds[fd].cloexec = 0; /* M84: a fresh descriptor is not close-on-exec until fcntl says so */
    self->fds[fd].sock = s;
    return fd;
}

static long sys_socket(uint64_t type, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_NETWORK)) {
        return -1; /* M65: gated at socket() rather than at sendto(), so a program without the capability cannot even get a handle to fail with */
    }
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (type != SOCK_DGRAM && type != SOCK_STREAM) {
        return -1;
    }
    struct socket *s = socket_alloc((int)type);
    if (!s) {
        return -1;
    }
    return install_socket_fd(s);
}

/* ---- M66: streams ----------------------------------------------------- */

static long sys_listen(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_fd(fd);
    return s ? socket_listen(s) : -1;
}

static long sys_connect(uint64_t fd, uint64_t ip, uint64_t port, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_fd(fd);
    if (!s || port == 0 || port > 0xFFFF) {
        return -1;
    }
    return socket_connect(s, (uint32_t)ip, (uint16_t)port);
}

static long sys_connstat(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_fd(fd);
    struct tcpcb *tcb = s ? socket_tcb(s) : (struct tcpcb *)0;
    if (!tcb) {
        return -1;
    }
    if (!tcp_connect_settled(tcb)) {
        return 0;
    }
    /* Established, or gone. tcp.c frees the control block on a refused
     * or timed-out connection, so "the tcb is closed" is exactly the
     * failure case - which is why this asks tcp_state rather than
     * consulting a flag that would have been freed with it. */
    return tcp_state(tcb) == TCP_ESTABLISHED ? 1 : -1;
}

static long sys_accept(uint64_t fd, uint64_t from_ptr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct socket *listener = socket_for_fd(fd);
    if (!listener) {
        return -1;
    }
    struct socket *conn = socket_accept(listener);
    if (!conn) {
        return -1;
    }
    if (from_ptr) {
        struct tcpcb *tcb = socket_tcb(conn);
        os_sockaddr_t from = {tcp_remote_ip(tcb), tcp_remote_port(tcb), 0};
        if (copy_to_user(from_ptr, &from, sizeof(from)) != 0) {
            /* The connection is already accepted and cannot be put back,
             * so it is aborted rather than leaked - the peer learns the
             * truth, which is that this machine took the connection and
             * then could not keep it. */
            socket_unref(conn);
            return -1;
        }
    }
    return install_socket_fd(conn);
}

static long sys_send(uint64_t fd, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_fd(fd);
    struct tcpcb *tcb = s ? socket_tcb(s) : (struct tcpcb *)0;
    if (!tcb || len > TCP_MAX_MSS) {
        /* Capped at one segment per call rather than at the buffer size:
         * a partial write is already the contract, so the only thing a
         * larger staging buffer would buy is a larger staging buffer. */
        if (!tcb) {
            return -1;
        }
        len = TCP_MAX_MSS;
    }
    static uint8_t staging[TCP_MAX_MSS];
    if (len && copy_from_user(staging, buf, (size_t)len) != 0) {
        return -1;
    }
    return tcp_send(tcb, staging, (uint16_t)len);
}

static long sys_recv(uint64_t fd, uint64_t buf, uint64_t max, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_fd(fd);
    struct tcpcb *tcb = s ? socket_tcb(s) : (struct tcpcb *)0;
    if (!tcb) {
        return -1;
    }
    if (max > TCP_MAX_MSS) {
        max = TCP_MAX_MSS;
    }
    static uint8_t staging[TCP_MAX_MSS];
    int n = tcp_recv(tcb, staging, (uint16_t)max);
    if (n <= 0) {
        return n; /* 0 = nothing right now, -1 = end of stream; both mean nothing to copy */
    }
    return copy_to_user(buf, staging, (size_t)n) == 0 ? n : -1;
}

static long sys_bind(uint64_t fd, uint64_t port, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_fd(fd);
    if (!s || port > 0xFFFF) {
        return -1;
    }
    return socket_bind(s, (uint16_t)port);
}

static long sys_sendto(uint64_t fd, uint64_t ip, uint64_t port, uint64_t buf, uint64_t len, uint64_t a6) {
    (void)a6;
    struct socket *s = socket_for_fd(fd);
    if (!s || port > 0xFFFF || len > UDP_MAX_PAYLOAD) {
        return -1;
    }
    static uint8_t staging[UDP_MAX_PAYLOAD];
    if (len && copy_from_user(staging, buf, (size_t)len) != 0) {
        return -1;
    }
    return socket_sendto(s, (uint32_t)ip, (uint16_t)port, staging, (uint16_t)len);
}

static long sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t max, uint64_t from_ptr, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    struct socket *s = socket_for_fd(fd);
    if (!s || max > SOCKET_MAX_DATAGRAM) {
        return -1;
    }
    static uint8_t staging[SOCKET_MAX_DATAGRAM];
    os_sockaddr_t from = {0, 0, 0};
    int n = socket_recvfrom(s, staging, (uint16_t)max, &from.ip, &from.port);
    if (n < 0) {
        return -1;
    }
    /* The datagram is already off the queue by the time the copy can
     * fail, and there is nowhere to put it back. A caller that passes a
     * bad pointer loses that one datagram, which is a strictly better
     * outcome than a kernel that keeps a partial copy around to hand
     * out twice - and is the same trade sys_read already makes. */
    if (n && copy_to_user(buf, staging, (size_t)n) != 0) {
        return -1;
    }
    if (from_ptr && copy_to_user(from_ptr, &from, sizeof(from)) != 0) {
        return -1;
    }
    return n;
}

static long sys_sockpoll(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_fd(fd);
    return s ? socket_pending(s) : -1;
}

static long sys_netconf(uint64_t out_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!net_have_nic()) {
        return -1;
    }
    os_netconf_t conf;
    conf.ip = net_local_ip();
    conf.mask = net_subnet_mask();
    conf.gateway = net_gateway_ip();
    conf.dns = net_dns_ip();
    conf.leased = net_config_is_leased();
    return copy_to_user(out_ptr, &conf, sizeof(conf)) == 0 ? 0 : -1;
}

static long sys_settime(uint64_t seconds, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_SET_TIME)) {
        return -1; /* M65 */
    }
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (seconds > 0xFFFFFFFFu) {
        return -1;
    }
    return rtc_set_unix((uint32_t)seconds);
}

/* ---- M62: sound ------------------------------------------------------
 *
 * One owner, enforced. See SYS_audio_claim's own note for why this is a
 * claim rather than a comment asking programs to be polite: there is one
 * speaker, and this project has no permission model to lean on - but
 * "the first process to ask holds it until it exits" needs none. */
static int audio_owner = -1;

static int audio_owner_is_caller(void) {
    task_t *self = sched_current();
    if (audio_owner < 0) {
        return 0;
    }
    if (audio_owner == self->id) {
        return 1;
    }
    /* The owner may have exited without anything noticing - the same
     * "ask about a pid rather than infer from silence" check M29's
     * reap_dead_clients uses. A dead owner owns nothing. */
    task_t *owner = sched_task_by_id(audio_owner);
    if (!owner || owner->state == TASK_TERMINATED) {
        audio_owner = -1;
    }
    return 0;
}

static long sys_audio_claim(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_AUDIO)) {
        return -1; /* M65 */
    }
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = sched_current();
    if (audio_owner_is_caller()) {
        return 0;
    }
    if (audio_owner >= 0) {
        return -1;
    }
    audio_owner = self->id;
    return 0;
}

static long sys_audio_release(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!audio_owner_is_caller()) {
        return -1;
    }
    /* Quiet on the way out, so a release cannot leave a tone playing
     * that nobody now owns the means to stop. */
    pcspk_off();
    audio_owner = -1;
    return 0;
}

static long sys_beep(uint64_t freq_hz, uint64_t ms, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!audio_owner_is_caller()) {
        return -1;
    }
    /* Bounded here rather than trusted: a caller asking for a ten-second
     * tone is a caller making the machine unusable, which is the exact
     * thing ownership exists to prevent - and the owner is a program too,
     * with its own bugs. */
    if (ms > 1000) {
        ms = 1000;
    }
    pcspk_tone((uint32_t)freq_hz, (uint32_t)ms);
    return 0;
}

static long sys_audio_volume(uint64_t percent, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!audio_owner_is_caller()) {
        return -1;
    }
    if (percent > 100) {
        percent = 100;
    }
    ac97_set_volume((uint32_t)percent);
    /* The speaker has no volume - see pcspk.h. Mute is the only part of
     * this it can honour, and honouring it is what makes a mute switch
     * mean one thing rather than two. */
    pcspk_set_muted(percent == 0);
    return 0;
}

static long sys_audio_play(uint64_t buf, uint64_t frames, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!audio_owner_is_caller()) {
        return -1;
    }
    if (frames == 0 || frames > ac97_max_frames()) {
        return -1;
    }
    /* Stereo 16-bit: four bytes a frame, and the range is validated
     * before a single byte is read - M52's rule, and this is a buffer a
     * user process supplies. */
    if (!user_range_ok(buf, frames * 4, 0)) {
        return -1;
    }
    return ac97_play((const int16_t *)buf, (uint32_t)frames);
}

/* M58: the display-mode pair. The list is built once at boot by
 * dispi_init (which validates every candidate against the device's own
 * limits and its reported video memory) and simply copied out here -
 * validation at the moment somebody clicks would be validation in the one
 * place a mistake costs them their desktop. */
static long sys_display_modes(uint64_t out_ptr, uint64_t max_entries, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (max_entries > DISPLAY_MAX_MODES) {
        max_entries = DISPLAY_MAX_MODES;
    }
    display_mode_t modes[DISPLAY_MAX_MODES];
    int total = dispi_get_modes(modes, (int)max_entries);
    int n = total < (int)max_entries ? total : (int)max_entries;
    if (n > 0 && copy_to_user(out_ptr, modes, (size_t)n * sizeof(modes[0])) != 0) {
        return -1;
    }
    return total;
}

static long sys_display_set_mode(uint64_t width, uint64_t height, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_DISPLAY_MODE)) {
        return -1; /* M65: reconfiguring the display hardware is authority over every other program on the machine, not just this one */
    }
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    uint32_t pitch = 0;
    if (dispi_set_mode((uint32_t)width, (uint32_t)height, &pitch) != 0) {
        return -1;
    }
    /* fb_remap re-derives the text console itself, before it logs
     * anything - see its own comment for why that ordering is
     * load-bearing rather than tidy. */
    fb_remap(pitch, (uint32_t)width, (uint32_t)height);
    return 0;
}

/* ---- M68: SYS_waitfds -------------------------------------------------
 *
 * "Is this descriptor ready right now", with no waiting and no side
 * effects. Split out from the syscall itself because the wait loop below
 * has to ask it twice per pass - once before parking and once after
 * waking - and the two answers have to be produced by the same code or
 * the second one is a different question. */
static int fd_is_ready(task_t *self, int fd) {
    if (fd < 0 || fd >= MAX_FDS) {
        return 0;
    }
    fd_slot_t *slot = &self->fds[fd];
    switch (slot->type) {
    case FD_STDIN:
        return keyboard_peek() ? 1 : 0;
    case FD_PIPE_READ:
        /* End of stream counts as ready, and this is the line that stops
         * a blocking wait from becoming a hang. A reader whose writer has
         * gone away will never get bytes; if that did not wake it, every
         * pipeline on this machine would stop at its last read instead of
         * returning 0. */
        return (pipe_buffered(slot->pipe) > 0 || pipe_write_closed(slot->pipe)) ? 1 : 0;
    case FD_SOCKET:
        return socket_pending(slot->sock) > 0 ? 1 : 0;
    case FD_FILE:
        /* A regular file is always readable - it is never a reason to
         * wait. Saying so beats refusing the whole call because one
         * descriptor in the set happens to be a file.
         *
         * M85: and a pty is not a regular file. vfs_handle_readable is
         * the one place that knows the difference, and it answers 1 for
         * everything that is - so this stayed a one-liner. */
        return vfs_handle_readable(slot->file->handle);
    default:
        return 0;
    }
}

static long sys_waitfds(uint64_t fds_ptr, uint64_t count, uint64_t timeout_ms,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (count > MAX_FDS) {
        return -1;
    }
    /* M88: a count of zero is allowed, and is a sleep.
     *
     * It used to be refused alongside an over-long count, which was
     * reasonable when every caller had descriptors to watch. `poll` has
     * one that does not: POSIX says an empty set with a timeout is a
     * sleep, and every implementation honours it. This call already has
     * a deadline and a park - a wait with nothing that could satisfy it
     * early IS a sleep - so allowing zero gives user space the sleep
     * primitive it did not otherwise have, rather than making libc spin
     * on SYS_yield to imitate one.
     *
     * The scan below runs zero times, the deadline check still applies,
     * and a negative timeout with no descriptors parks until a signal -
     * which is `pause()`, and is also correct. */
    if (count > 0 && !user_range_ok(fds_ptr, count * sizeof(int), 0)) {
        return -1;
    }
    const int *fds = (const int *)fds_ptr;
    task_t *self = sched_current();

    long timeout = (long)timeout_ms;
    uint64_t now = pit_get_ticks() * (1000 / PIT_HZ);
    /* A negative timeout is "no deadline"; 0 is "poll and return". The
     * three cases are one expression so that the deadline is computed
     * once, before the first scan - otherwise a slow scan would extend
     * its own timeout. */
    uint64_t deadline = (timeout < 0) ? 0 : now + (uint64_t)timeout;

    for (;;) {
        /* Sampled before the scan, handed back at the park. Anything that
         * happens during the scan - including a mouse event, which has no
         * descriptor to be scanned - moves the counter and stops this
         * task from sleeping through it. */
        uint64_t seq = sched_event_seq();
        for (uint64_t i = 0; i < count; i++) {
            if (fd_is_ready(self, fds[i])) {
                return (long)i;
            }
        }
        if (timeout == 0) {
            return -2; /* a pure poll: nothing ready, and no waiting asked for */
        }
        if (deadline != 0 && pit_get_ticks() * (1000 / PIT_HZ) >= deadline) {
            return -2;
        }

        /* Park on the shared poll channel. Every waker in the kernel -
         * pipe_write, pipe_close_*, the keyboard and mouse IRQs,
         * socket_deliver, task_exit - wakes it, so this task is woken by
         * anything that could possibly have made one of its descriptors
         * ready, plus a few things that could not. The re-scan at the top
         * of this loop is what makes those spurious wakes free, and it is
         * the same re-test sched_block_on's contract requires anyway.
         *
         * The deadline is passed down so the timer can wake this task
         * even if no event ever arrives - which is what makes a frame
         * clock out of the same call. */
        sched_block_on_seq(SCHED_POLL_CHAN, deadline, seq);
        /* M76: a caught signal ends the park. -2 is "nothing was ready",
         * which every caller already handles by looping - and the
         * handler runs before the next call, which is the entire point.
         * A separate return code would be a third case for every one of
         * those callers to get right in exchange for information none of
         * them wants. */
        if (sched_signal_pending()) {
            return -2;
        }
    }
}

/* M68: see SYS_idle_ticks. Not capability-gated, for the reason M65 gave
 * about SYS_fb_info and SYS_netconf: how busy this machine is, is a fact
 * about the machine rather than authority over it. */
static long sys_idle_ticks(uint64_t cpu, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (cpu >= MAX_CPUS) {
        return -1;
    }
    return (long)sched_idle_ticks((int)cpu);
}

/* ---- M70: reading the kernel's own account of itself ------------------- */

static long sys_klog(uint64_t from, uint64_t buf, uint64_t max, uint64_t next_out,
                      uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    if (!has_cap(CAP_SYSLOG)) {
        return -1;
    }
    if (max == 0 || !user_range_ok(buf, max, 1)) {
        return -1;
    }
    if (next_out && !user_range_ok(next_out, sizeof(uint64_t), 1)) {
        return -1;
    }
    uint64_t next = 0;
    size_t n = klog_read(from, (char *)buf, (size_t)max, &next);
    if (next_out) {
        *(uint64_t *)next_out = next;
    }
    return (long)n;
}

static long sys_klog_total(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    /* Not gated - see SYS_klog_total's own comment. How much has been
     * logged is a length, and gating a length is the kind of check M65
     * warned looks like security and is not. */
    return (long)klog_written_total();
}

/* M71: see SYS_rename_replace. Same shape as sys_rename, and gated the
 * same way - it writes, so it needs CAP_FS_WRITE. */
/* ---- M76: signals a program can catch ---------------------------------
 *
 * Three calls and one delivery point. The delivery point is the
 * interesting one and it is at the bottom of this file, in
 * syscall_handler, because that is the only place in this kernel that
 * holds a ring-3 iretq frame it is about to return through *and* can
 * modify it. An interrupt's return path could do the same and does not:
 * see SYS_kill's ABI comment for what that costs and why it was not
 * done here.
 */
static long sys_sigaction(uint64_t signo, uint64_t handler, uint64_t restorer,
                           uint64_t flags, uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    if (!SIG_IS_CATCHABLE((int)signo)) {
        return -1;
    }
    task_t *self = sched_current();
    /* A handler address must be inside the caller's own image, or the
     * kernel would be pointing ring 3 at an address the caller cannot
     * execute - which faults immediately and looks like a kernel bug.
     * SIG_DFL_ADDR and SIG_IGN_ADDR are the two values that are not
     * addresses at all. */
    if (handler != SIG_DFL_ADDR && handler != SIG_IGN_ADDR) {
        if (handler < USER_REGION_BASE || handler >= USER_REGION_LIMIT) {
            return -1;
        }
        /* And a restorer, since without one the handler returns to
         * whatever happens to be on the stack. Required rather than
         * defaulted: there is no address the kernel could default it to
         * that would be right for a program it did not link. */
        if (restorer < USER_REGION_BASE || restorer >= USER_REGION_LIMIT) {
            return -1;
        }
        self->sig_restorer = restorer;
    }
    long prev = (long)self->sig_handler[signo];
    self->sig_handler[signo] = handler;
    /* M99: whether this handler wants three arguments. It is a calling
     * convention rather than a feature flag - see sched.h's sig_siginfo
     * and the header note in system_api/include/signal.h for the program
     * that faulted every time because the two disagreed. */
    if (flags & SA_SIGINFO) {
        self->sig_siginfo |= (1u << signo);
    } else {
        self->sig_siginfo &= ~(1u << signo);
    }
    if (handler == SIG_IGN_ADDR || handler == SIG_DFL_ADDR) {
        /* Anything already queued for a signal that has just stopped
         * being caught would otherwise be delivered to an address that
         * is no longer a handler. */
        self->sig_pending &= ~(1u << signo);
    }
    return prev;
}

/* The mask the kernel will let a ring-3 program put back into RFLAGS.
 * Carry/parity/adjust/zero/sign/direction/overflow, plus bit 1 which is
 * architecturally always set, plus IF - which is not negotiable, so it is
 * forced on below rather than taken from the frame. Everything else
 * (IOPL, NT, RF, VM, AC, the virtual-interrupt bits) is either privileged
 * or a way to make the next instruction behave in a way the caller could
 * not otherwise ask for, and a sigreturn is not the place to grant it. */
#define USER_RFLAGS_MASK 0x0000000000000CD5ULL
#define RFLAGS_IF        0x0000000000000200ULL

static long sys_sigreturn(uint64_t frame_ptr, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6);

static long sys_sigprocmask(uint64_t how, uint64_t mask, uint64_t old_out,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = sched_current();
    uint32_t previous = self->sig_blocked;
    uint32_t want = (uint32_t)mask;
    /* Silently cleared rather than refused - a program that asks to block
     * "everything" is asking for a thing it can have almost all of, and
     * failing the whole call would leave it with no idea which bit was
     * the problem.
     *
     * M99: and NOT cleared for SIG_UNBLOCK, which is the direction that
     * has to work. signal_deliver blocks a signal while its own handler
     * runs, and a handler that leaves by siglongjmp rather than by
     * returning never reaches sigreturn to put the mask back - so
     * unblocking it by hand is the only way out, and it is exactly what
     * siglongjmp does. Filtering the unblock mask made SIGSEGV blocked
     * for the rest of the process's life after the first fault it
     * caught, which is a program that can survive one fault and not
     * two. */
    /* M99: SIGSEGV is catchable now (see signal.h) and it is still not
     * blockable, which is not an inconsistency. Blocking a signal the
     * MMU is about to raise does not postpone it - the faulting
     * instruction is still there and still cannot execute - so a
     * process that successfully blocked it would fault forever with
     * nothing able to happen. Linux resolves this by forcing the default
     * action on a blocked synchronous SIGSEGV, which is the same outcome
     * by a longer road; this refuses the block instead and the task is
     * terminated with the fault it had. What still blocks it, and must,
     * is signal_deliver's automatic block for the duration of its own
     * handler. */
    if (how != SIG_UNBLOCK) {
        want &= ~(1u << SIGKILL);
        want &= ~(1u << SIGSEGV);
    }
    switch (how) {
    case SIG_BLOCK:
        self->sig_blocked |= want;
        break;
    case SIG_UNBLOCK:
        self->sig_blocked &= ~want;
        break;
    case SIG_SETMASK:
        self->sig_blocked = want;
        break;
    default:
        return -1;
    }
    if (old_out && copy_to_user(old_out, &previous, sizeof(previous)) != 0) {
        return -1;
    }
    return 0;
}

/* ---- M75: the working directory ---------------------------------------
 *
 * Not gated on a capability, deliberately. A process's own directory is
 * a fact about itself, not authority over anything else - the same
 * argument caps.h makes about SYS_fb_info and SYS_netconf, and the
 * opposite of the one it makes about SYS_klog. Everything a chdir can
 * then *reach* is still gated exactly as it was: a relative path
 * resolves to an absolute one and meets CAP_FS_WRITE at the same door
 * an absolute one always did.
 */
static long sys_chdir(uint64_t path_ptr, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0) {
        return -1;
    }
    /* Refused unless it is a directory that exists. A cd into a regular
     * file, or into nothing, has to be an error rather than a quiet
     * success: every relative path afterwards would resolve against a
     * place that is not there, and the failure would surface as some
     * unrelated open failing later. */
    if (!vfs_is_dir(path)) {
        return -1;
    }
    task_t *self = sched_vm_owner(sched_current()); /* M79 - see copy_path_from_user */
    int i = 0;
    for (; path[i] && i < PATH_MAX_LEN - 1; i++) {
        self->cwd[i] = path[i];
    }
    self->cwd[i] = '\0';
    return 0;
}

static long sys_getcwd(uint64_t buf, uint64_t maxlen, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = sched_vm_owner(sched_current()); /* M79 */
    const char *cwd = (self->cwd[0] == '/') ? self->cwd : "/";
    uint64_t len = 0;
    while (cwd[len]) {
        len++;
    }
    if (maxlen < len + 1) {
        return -1; /* refused, not truncated - a short path names a different directory */
    }
    if (copy_to_user(buf, cwd, len + 1) != 0) {
        return -1;
    }
    return (long)len;
}

static long sys_rename_replace(uint64_t old_ptr, uint64_t new_ptr, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!has_cap(CAP_FS_WRITE)) {
        return -1;
    }
    char old_path[LEANFS_MAX_PATH];
    char new_path[LEANFS_MAX_PATH];
    if (copy_path_from_user(old_path, old_ptr) != 0 ||
        copy_path_from_user(new_path, new_ptr) != 0) {
        return -1;
    }
    return vfs_rename_replace(old_path, new_path);
}

/* ---- M83: fork ---------------------------------------------------------
 *
 * The ordering here is the whole of the error handling, and it is chosen
 * so that every failure leaves the parent exactly as it was:
 *
 *   1. clone the address space. This is the expensive step and the one
 *      most likely to fail, so it goes first - and it is undoable by
 *      itself, because a clone nobody has been given is just an address
 *      space to destroy.
 *   2. take a task slot. If the table is full, destroy the clone.
 *   3. copy the environment. This allocates, so it can fail too - and by
 *      now there is a live child in the table, which is why the failure
 *      path has to kill it rather than just returning. A child with no
 *      environment would be a child that silently lost its parent's, and
 *      M75 built that environment precisely so it would survive.
 *
 * Note what is NOT here: nothing copies the parent's memory. That is the
 * milestone. vmm_fork_address_space marks both sides read-only and hands
 * the page back to whoever writes to it first.
 */
static long sys_fork(isr_regs_t *regs) {
    task_t *parent = sched_current();
    if (!parent || parent->pml4_phys == vmm_kernel_pml4_phys()) {
        return -1; /* a kernel thread has no address space of its own to copy */
    }

    /* ---- refused from a process with more than one thread, and why ----
     *
     * This is an SMP correctness limit, not a policy one, and it is worth
     * writing down rather than discovering.
     *
     * Making a page copy-on-write means clearing its writable bit in the
     * parent's page tables. The `invlpg` that follows only flushes the
     * translation on *this* CPU. A second thread of the same process
     * running on another core still holds the old writable entry in its
     * own TLB, and would go on writing to a page this call has just
     * promised the child is a private copy of - so the child would see
     * the parent's later writes, intermittently, on a multi-core machine
     * only, in a way no self-test on one core would ever reproduce.
     *
     * The real fix is a TLB shootdown: an IPI to every CPU running this
     * address space, and a wait for each to acknowledge. That is a piece
     * of machinery this kernel does not have and M83 is not the milestone
     * to build it in - it is its own work, with its own failure modes,
     * and it is needed by more than fork.
     *
     * So a threaded process cannot fork here, and gets a clean -1 rather
     * than a page that is sometimes shared. POSIX makes fork-from-a-thread
     * nearly unusable anyway (only async-signal-safe calls are legal in
     * the child), so a program that needs both is already in territory
     * this OS has no business pretending to support. */
    if (sched_count_sharing_address_space(parent->pml4_phys) > 1) {
        return -1;
    }

    /* M91 (second attempt): a shared file mapping must not be cloned
     * copy-on-write.
     *
     * COW's whole promise is that a write separates the two copies,
     * which is the opposite of what MAP_SHARED means. So the parent's
     * entries for those pages are dropped first - references and all -
     * and both sides fault them back in through filemap afterwards, each
     * taking its own reference to the one frame they now share. The cost
     * is a re-fault per page in the parent; the alternative is a fork
     * that silently turns a shared mapping into two private ones. */
    sched_release_shared_range(parent, USER_MMAP_BASE, USER_MMAP_LIMIT);

    uint64_t child_pml4 = process_fork_address_space(parent->pml4_phys);
    if (child_pml4 == 0) {
        return -1;
    }

    task_t *child = task_fork(child_pml4, regs);
    if (!child) {
        process_destroy_address_space(child_pml4);
        return -1;
    }

    /* M79: the environment belongs to the address space's owner, so a
     * thread that forks passes on the environment of the process it is
     * part of rather than the nothing it holds itself. */
    task_t *owner = sched_vm_owner(parent);
    if (owner->env_block && owner->env_len && owner->env_count) {
        if (sched_set_env(child, owner->env_block, owner->env_len, owner->env_count) != 0) {
            /* The child exists and is schedulable, so this cannot be
             * unwound by freeing things - it has to be killed the way any
             * other doomed task is, and reaped by whoever waits for it.
             * The parent is told the fork failed, which is true. */
            sched_raise_signal(child, SIGKILL);
            return -1;
        }
    }

    return (long)child->id;
}

/* M84: the one descriptor flag this machine has. See SYS_fcntl in
 * system_api/include/syscall.h for why the rest of fcntl stays in libc
 * answering honestly rather than arriving here to be refused. */
static long sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t a4,
                      uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = sched_current();
    if (fd >= MAX_FDS || self->fds[fd].type == FD_NONE) {
        return -1;
    }
    switch (cmd) {
    case F_GETFD_CMD:
        return self->fds[fd].cloexec ? FD_CLOEXEC_BIT : 0;
    case F_SETFD_CMD:
        self->fds[fd].cloexec = (arg & FD_CLOEXEC_BIT) ? 1 : 0;
        return 0;
    /* M98: the access mode, which only this table knows. See the note
     * at F_GETFL_CMD in system_api/include/syscall.h for who asked. A
     * disk file is always readable here - openfile_t records `writable`
     * and nothing else because SYS_read has never refused one - so the
     * answer for FD_FILE says so rather than inventing a distinction
     * the kernel does not enforce. */
    case F_GETFL_CMD:
        switch (self->fds[fd].type) {
        case FD_STDIN:      return OPEN_READ;
        case FD_STDOUT:     return OPEN_WRITE;
        case FD_PIPE_READ:  return OPEN_READ;
        case FD_PIPE_WRITE: return OPEN_WRITE;
        case FD_FILE:
            return OPEN_READ |
                   (self->fds[fd].file->writable ? OPEN_WRITE : 0);
        case FD_SOCKET:     return OPEN_READ | OPEN_WRITE;
        default:            return -1;
        }
    default:
        return -1;
    }
}

/* ---- M84: waitpid -----------------------------------------------------
 *
 * What SYS_wait could not say. It returns an exit code, and an exit code
 * cannot distinguish a program that called exit(139) from one that died
 * on SIGSEGV - both are 139, because 128 + signal is the shell convention
 * that makes a signal death *readable* and therefore indistinguishable to
 * a program. SYS_task_alive's own comment documents that squash. This
 * returns the pid and writes a status that decodes into either answer.
 *
 * The encoding is the familiar one, because every W-macro anyone has ever
 * written assumes it: a normal exit is the code in bits 8-15 with the low
 * seven bits clear; a signal death is the signal in the low seven bits.
 * <sys/wait.h> is the other half.
 *
 * `pid` is -1 for any child or a specific pid. Process groups are not a
 * thing this OS has yet, so a negative pid other than -1 is refused
 * rather than silently treated as -1 - M85 is where groups arrive and
 * where that case gets a meaning.
 *
 * The blocking is M68's, unchanged from sys_wait: a parent waiting on one
 * child parks on that child and is woken by it; a parent waiting on any
 * child parks on the poll channel with a short deadline, so a lost wake
 * costs 50 ms rather than forever.
 */
static int wait_status_of(const task_t *t) {
    if (t->exit_signal) {
        return t->exit_signal & 0x7F;
    }
    return (t->exit_code & 0xFF) << 8;
}

/* M85: the third status a wait can report, and the only one that is not
 * about a dead process.
 *
 * 0x7f in the low byte is the encoding every Unix uses and the reason
 * WIFSTOPPED reads the way it does: 0 means exited, a signal number
 * means killed, and 0x7f is the one value that cannot be either. The
 * layout is what makes the macros a ported program was compiled against
 * work, which is the argument <sys/wait.h> already makes about the other
 * two. */
static int stop_status_of(const task_t *t) {
    return ((t->stopped_sig & 0xFF) << 8) | 0x7F;
}

/* Has this child a stop that a WUNTRACED wait should report? Reported at
 * most once - see task_t.stop_reported for why a shell depends on that. */
static int stop_to_report(task_t *t, uint64_t options) {
    return (options & WUNTRACED) && t->state == TASK_STOPPED && !t->stop_reported;
}

static long sys_waitpid(uint64_t pid_arg, uint64_t status_ptr, uint64_t options,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = sched_current();
    int64_t want = (int64_t)pid_arg;
    if (want < -1) {
        return -1; /* process groups arrive with M85 */
    }
    if (status_ptr && !user_range_ok(status_ptr, sizeof(int), 1)) {
        return -1;
    }

    for (;;) {
        int any_children = 0;
        uint64_t seq = sched_event_seq(); /* M68: sampled before the scan */
        task_t *only = (task_t *)0;

        if (want > 0) {
            task_t *t = sched_task_by_id((int)want);
            if (!t || t->reaped || t->parent_id != self->id) {
                return -1;
            }
            any_children = 1;
            only = t;
            if (t->state == TASK_TERMINATED) {
                int pid = t->id;
                int status = wait_status_of(t);
                t->reaped = 1;
                sched_reap_slot(t);
                if (status_ptr) {
                    (void)copy_to_user(status_ptr, &status, sizeof(status));
                }
                return pid;
            }
            /* M85: and a stop. Deliberately NOT reaped - the child is
             * alive and will be waited for again when it finally exits,
             * which is the difference between this and every other
             * return from this call. */
            if (stop_to_report(t, options)) {
                int status = stop_status_of(t);
                t->stop_reported = 1;
                if (status_ptr) {
                    (void)copy_to_user(status_ptr, &status, sizeof(status));
                }
                return t->id;
            }
        } else {
            int total = sched_task_count();
            for (int i = 0; i < total; i++) {
                /* M54: by slot, not by id - a pid is not an index, and a
                 * lookup would be answered by a recycled generation. */
                task_t *t = sched_task_by_slot(i);
                if (!t || t->parent_id != self->id || t->reaped) {
                    continue;
                }
                any_children = 1;
                if (t->state == TASK_TERMINATED) {
                    int pid = t->id;
                    int status = wait_status_of(t);
                    t->reaped = 1;
                    sched_reap_slot(t);
                    if (status_ptr) {
                        (void)copy_to_user(status_ptr, &status, sizeof(status));
                    }
                    return pid;
                }
                if (stop_to_report(t, options)) {
                    int status = stop_status_of(t);
                    t->stop_reported = 1;
                    if (status_ptr) {
                        (void)copy_to_user(status_ptr, &status, sizeof(status));
                    }
                    return t->id;
                }
            }
        }

        if (!any_children) {
            return -1;
        }
        if (options & WNOHANG) {
            return 0; /* alive, and the caller said not to wait */
        }
        if (only) {
            sched_block_on_seq((const void *)only,
                               pit_get_ticks() * (1000 / PIT_HZ) + 200, seq);
        } else {
            sched_block_on_seq(SCHED_POLL_CHAN,
                               pit_get_ticks() * (1000 / PIT_HZ) + 50, seq);
        }
    }
}

/* ---- M84: execve ------------------------------------------------------
 *
 * Replaces the calling task's memory with a different program's, and
 * changes nothing else about it: same pid, same parent, same process
 * group, same working directory, same descriptors except the ones marked
 * close-on-exec.
 *
 * The order below is the whole of the correctness, because there is a
 * point in the middle after which nothing can fail:
 *
 *   1. everything that reads the CALLER's address space happens first -
 *      the path, argv, envp. They are copied into kernel buffers because
 *      the address space they point into is about to stop existing.
 *   2. the image is read and the new address space is built. Both can
 *      fail, and both fail harmlessly: the caller still has its own
 *      memory and gets a -1 it can act on.
 *   3. the swap. From here there is no way back - the old address space
 *      is gone and the program that called this no longer exists - so
 *      everything after this point must be incapable of failing.
 *
 * `regs` is rewritten rather than returned through: an exec does not
 * return, it arrives. Setting rip, rsp and rdi and letting
 * syscall_common_stub's own `iretq` do the rest is the same mechanism
 * enter_user_mode uses for a fresh process, minus the fresh process.
 */
static long sys_execve(isr_regs_t *regs) {
    task_t *self = sched_current();
    if (!self || self->pml4_phys == vmm_kernel_pml4_phys()) {
        return -1; /* a kernel thread has no image to replace */
    }
    /* Refused from a threaded process, for a plainer reason than fork's:
     * the other threads are running on the address space this is about to
     * destroy. POSIX says exec keeps only the calling thread and stops
     * the rest, which needs a way to stop a thread that may be running on
     * another core - the same machinery M83 said fork needs and does not
     * have. Refusing is the honest version. */
    if (sched_count_sharing_address_space(self->pml4_phys) > 1) {
        return -1;
    }

    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, regs->rdi) != 0) {
        return -1;
    }

    user_vectors_t v;
    if (copy_vectors_from_user(path, regs->rsi, regs->rdx, 0, &v) != 0) {
        return -1;
    }
    if (v.argc == 0) {
        /* A caller that passed no argv at all. POSIX leaves this
         * undefined and every program in the world reads argv[0], so the
         * path is supplied rather than handing over an empty vector -
         * which is the one place exec borrows spawn's rule, and only
         * because the alternative is a program that crashes on its first
         * line. */
        size_t len = k_strlen(path) + 1;
        k_memcpy(v.argbuf, path, len);
        v.argv[0] = v.argbuf;
        v.argv[1] = (const char *)0;
        v.argc = 1;
    }

    /* A `#!` script is refused here rather than resolved, and M84's own
     * milestone note says why: shebang handling belongs to whoever has a
     * path and an opinion about interpreters, which is the shell (M72),
     * not a kernel. SYS_spawn resolves one because it is the call every
     * launcher on this desktop already goes through; exec is the call a
     * program makes about itself, and a program that wants to run a
     * script can run its interpreter. */
    leanfs_stat_t st;
    if (vfs_stat(path, &st) != 0 || st.is_dir) {
        free_vectors(&v);
        return -1;
    }
    uint8_t *image = (uint8_t *)kmalloc(st.size ? st.size : 1);
    if (!image) {
        free_vectors(&v);
        return -1;
    }
    int64_t size = vfs_read(path, image, st.size);
    if (size < 2 || (image[0] == '#' && image[1] == '!') ||
        !elf_validate(image, (size_t)size)) {
        kfree(image);
        free_vectors(&v);
        return -1;
    }

    /* The inherit case, built before anything is torn down. env_block
     * lives in the kernel heap, so these pointers stay valid across the
     * swap - which is exactly why this can be built now and used after. */
    const char *inherited[USER_ENV_MAX_VARS + 1];
    const char *const *effective = v.envp;
    task_t *owner = sched_vm_owner(self);
    if (!effective && owner && owner->env_block && owner->env_count) {
        uint32_t n = 0;
        uint32_t off = 0;
        while (off < owner->env_len && n < USER_ENV_MAX_VARS) {
            inherited[n++] = owner->env_block + off;
            while (off < owner->env_len && owner->env_block[off]) {
                off++;
            }
            off++;
        }
        inherited[n] = (const char *)0;
        effective = inherited;
    }

    uint64_t entry = 0;
    uint64_t new_pml4 = process_build_address_space(image, (size_t)size, v.argv,
                                                    effective, &entry);
    kfree(image);
    if (new_pml4 == 0) {
        free_vectors(&v);
        return -1;
    }

    /* ---- the point of no return -------------------------------------- */

    uint64_t old_pml4 = self->pml4_phys;
    /* M98: the peak of the image being replaced, taken before the
     * address space that holds it is destroyed two lines down. A process
     * that execs is still the same process, so its high-water mark spans
     * the exec - which matters here more than anywhere, because every
     * compile on this machine is a shell that exec'd a driver that
     * spawned a cc1. */
    uint64_t peak = vmm_rss_peak_pages(old_pml4);
    if (peak > self->max_rss_pages) {
        self->max_rss_pages = peak;
    }
    self->pml4_phys = new_pml4;
    /* Switched before the old one is destroyed, and this CPU is running
     * on a kernel stack in PML4[0] - shared by every address space - so
     * there is no instant at which the code doing this is unmapped. */
    vmm_switch_address_space(new_pml4);
    process_destroy_address_space(old_pml4);

    /* Everything that described the old image's memory. Left set, each of
     * these would be a cursor into an address space that no longer
     * exists: heap_brk is the one that would bite first, since the new
     * program's first malloc would extend a heap it does not have. */
    self->heap_brk = USER_HEAP_START;
    self->heap_mapped_end = USER_HEAP_START;
    self->shm_next_vaddr = USER_SHM_BASE;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        self->mmaps[i].base = 0;
        self->mmaps[i].pages = 0;
        self->mmaps[i].prot = 0;
        self->mmaps[i].handle = -1; /* M91 */
        self->mmaps[i].file_page = 0;
        self->mmaps[i].shared = 0;
    }
    /* M96: and the thread pointer. It points into the address space
     * that has just been replaced, so carrying it across an exec would
     * hand the new program a %fs into memory that is gone - which is the
     * same failure a recycled task slot had, in a different place. */
    self->fs_base = 0;

    /* M84: the descriptors that said they should not survive this. */
    for (int i = 0; i < MAX_FDS; i++) {
        if (self->fds[i].cloexec) {
            fd_release(&self->fds[i]);
            self->fds[i].type = FD_NONE;
        }
    }

    /* POSIX's rule, and it is not the obvious one: a signal the old image
     * had a HANDLER for goes back to the default, because that handler's
     * address is in memory that no longer exists - but one it had chosen
     * to IGNORE stays ignored, because "ignore this" is a decision about
     * the process rather than about the image. The blocked mask survives
     * for the same reason. */
    for (int i = 0; i <= SIG_MAX; i++) {
        if (self->sig_handler[i] != SIG_IGN_ADDR) {
            self->sig_handler[i] = SIG_DFL_ADDR;
        }
    }
    self->sig_restorer = 0;
    /* M99: and every SA_SIGINFO bit, which describes a handler that has
     * just gone back to the default. A bit left set here would mean the
     * next handler the new image installs with plain signal() is entered
     * with three arguments - a calling convention inherited from a
     * program that is no longer running. */
    self->sig_siginfo = 0;
    self->si_pid = 0;
    self->si_status = 0;
    self->si_addr = 0;

    /* The name a person sees in the task manager should be the program
     * that is actually running. */
    const char *base = path;
    for (const char *c = path; *c; c++) {
        if (*c == '/') {
            base = c + 1;
        }
    }
    sched_set_task_name(self, base);

    /* M65: capabilities can only ever shrink. The new image gets what its
     * manifest allows AND what this process already held - so exec can
     * never be a way to gain a capability the caller did not have, which
     * is the property the whole model rests on. */
    self->caps &= caps_for_program(base);

    /* The environment the new image was actually given, recorded so that
     * *its* children inherit in turn. Best-effort: a failure here leaves
     * the process running with the environment it had, which is wrong in
     * a small way and better than not running at all - there is nothing
     * left to return an error to. */
    if (v.envp) {
        char *packed = (char *)kmalloc(USER_ENV_MAX_BYTES);
        if (packed) {
            uint32_t len = 0;
            uint32_t count = 0;
            for (int i = 0; v.envv[i] && count < USER_ENV_MAX_VARS; i++) {
                uint32_t n = (uint32_t)k_strlen(v.envv[i]) + 1;
                if (len + n > USER_ENV_MAX_BYTES) {
                    break;
                }
                k_memcpy(packed + len, v.envv[i], n);
                len += n;
                count++;
            }
            sched_set_env(self, packed, len, count);
            kfree(packed);
        }
    }
    free_vectors(&v);

    /* And the frame the `iretq` at the end of syscall_common_stub will
     * return through. Every register is cleared rather than left as the
     * caller had it: the new program is entitled to assume it starts from
     * a known state, and leaking the old image's registers into it would
     * be a small, permanent source of nondeterminism. */
    uint64_t cs = regs->cs;
    uint64_t ss = regs->ss;
    k_memset(regs, 0, sizeof(*regs));
    regs->rip = entry;
    regs->rsp = USER_STACK_TOP;
    regs->rdi = USER_ARG_ADDR; /* crt0's argument, as enter_user_mode passes it */
    regs->cs = cs;
    regs->ss = ss;
    regs->rflags = 0x202; /* bit 1 always set, bit 9 IF - ring 3 runs with interrupts on */
    regs->vector = 0x80;
    return 0;
}

static const syscall_fn_t syscall_table[SYSCALL_COUNT] = {
    [SYS_write] = sys_write,
    [SYS_exit] = sys_exit,
    [SYS_getpid] = sys_getpid,
    [SYS_spawn] = sys_spawn,
    [SYS_wait] = sys_wait,
    [SYS_read] = sys_read,
    [SYS_readfile] = sys_readfile,
    [SYS_listdir] = sys_listdir,
    [SYS_kill] = sys_kill,
    [SYS_pipe] = sys_pipe,
    [SYS_getpgid] = sys_getpgid,
    [SYS_sbrk] = sys_sbrk,
    [SYS_shm_create] = sys_shm_create,
    [SYS_shm_map] = sys_shm_map,
    [SYS_fb_info] = sys_fb_info,
    [SYS_fb_map] = sys_fb_map,
    [SYS_mouse_read] = sys_mouse_read,
    [SYS_pipe_open] = sys_pipe_open,
    [SYS_kbd_read] = sys_kbd_read,
    [SYS_pipe_poll] = sys_pipe_poll,
    [SYS_uptime_ms] = sys_uptime_ms,
    [SYS_dup2] = sys_dup2,
    [SYS_wait_nb] = sys_wait_nb,
    [SYS_yield] = sys_yield,
    [SYS_task_alive] = sys_task_alive,
    [SYS_pipe_reset] = sys_pipe_reset,
    [SYS_kbd_modifiers] = sys_kbd_modifiers,
    [SYS_clipboard_set] = sys_clipboard_set,
    [SYS_clipboard_get] = sys_clipboard_get,
    [SYS_writefile] = sys_writefile,
    [SYS_taskinfo] = sys_taskinfo,
    [SYS_shutdown] = sys_shutdown,
    [SYS_close] = sys_close,
    [SYS_shm_free] = sys_shm_free,
    [SYS_mkdir] = sys_mkdir,
    [SYS_shm_unmap] = sys_shm_unmap,
    [SYS_unlink] = sys_unlink,
    [SYS_rename] = sys_rename,
    [SYS_open] = sys_open,
    [SYS_lseek] = sys_lseek,
    [SYS_stat] = sys_stat,
    [SYS_rmdir] = sys_rmdir,
    [SYS_time] = sys_time,
    [SYS_audio_claim] = sys_audio_claim,
    [SYS_audio_release] = sys_audio_release,
    [SYS_beep] = sys_beep,
    [SYS_audio_volume] = sys_audio_volume,
    [SYS_audio_play] = sys_audio_play,
    [SYS_display_modes] = sys_display_modes,
    [SYS_display_set_mode] = sys_display_set_mode,
    [SYS_socket] = sys_socket,
    [SYS_bind] = sys_bind,
    [SYS_sendto] = sys_sendto,
    [SYS_recvfrom] = sys_recvfrom,
    [SYS_sockpoll] = sys_sockpoll,
    [SYS_netconf] = sys_netconf,
    [SYS_settime] = sys_settime,
    [SYS_getcaps] = sys_getcaps,
    [SYS_dropcaps] = sys_dropcaps,
    [SYS_listen] = sys_listen,
    [SYS_connect] = sys_connect,
    [SYS_connstat] = sys_connstat,
    [SYS_accept] = sys_accept,
    [SYS_send] = sys_send,
    [SYS_recv] = sys_recv,
    [SYS_klog] = sys_klog,
    [SYS_klog_total] = sys_klog_total,
    [SYS_rename_replace] = sys_rename_replace,
    [SYS_waitfds] = sys_waitfds,
    [SYS_idle_ticks] = sys_idle_ticks,
    [SYS_chdir] = sys_chdir,
    [SYS_getcwd] = sys_getcwd,
    [SYS_sigaction] = sys_sigaction,
    [SYS_sigreturn] = sys_sigreturn,
    [SYS_sigprocmask] = sys_sigprocmask,
    [SYS_fstat] = sys_fstat,
    [SYS_mmap] = sys_mmap,
    [SYS_mprotect] = sys_mprotect,
    [SYS_madvise] = sys_madvise,
    [SYS_munmap] = sys_munmap,
    [SYS_thread_create] = sys_thread_create,
    [SYS_thread_exit] = sys_thread_exit,
    [SYS_gettid] = sys_gettid,
    [SYS_getdents] = sys_getdents,
    [SYS_waitpid] = sys_waitpid,
    [SYS_fcntl] = sys_fcntl,
    [SYS_setpgid] = sys_setpgid,
    [SYS_setsid] = sys_setsid,
    [SYS_getsid] = sys_getsid,
    [SYS_ioctl] = sys_ioctl,
    [SYS_ftruncate] = sys_ftruncate,
    [SYS_symlink] = sys_symlink,
    [SYS_link] = sys_link,
    [SYS_fsync] = sys_fsync,
    [SYS_profile] = sys_profile,
    [SYS_readlink] = sys_readlink,
    [SYS_lstat] = sys_lstat,
    [SYS_rusage] = sys_rusage,
    [SYS_statvfs] = sys_statvfs,
    [SYS_utime] = sys_utime,
    [SYS_fdpath] = sys_fdpath,
    [SYS_getppid] = sys_getppid,
    [SYS_sync] = sys_sync,
    [SYS_meminfo] = sys_meminfo,
    [SYS_alarm] = sys_alarm,
    [SYS_msync] = sys_msync,
    [SYS_arch_prctl] = sys_arch_prctl,
    [SYS_futex] = sys_futex,
};

/* M67: which syscall numbers reach kernel/net. Enumerated rather than
 * derived from a range, because the numbers are not contiguous (M64 took
 * 50-56, M66 took 59-64, and 57/58 are the capability calls in between)
 * and a range that silently grew to include the wrong neighbour would be
 * a lock held over something that does not need it or - far worse - not
 * held over something that does. */
static int syscall_touches_net(uint64_t num) {
    switch (num) {
    case SYS_socket:
    case SYS_bind:
    case SYS_sendto:
    case SYS_recvfrom:
    case SYS_sockpoll:
    case SYS_netconf:
    case SYS_listen:
    case SYS_connect:
    case SYS_connstat:
    case SYS_accept:
    case SYS_send:
    case SYS_recv:
        return 1;
    default:
        return 0;
    }
}

/* ---- M76: getting into and out of a handler ---------------------------
 *
 * `regs` is the ring-3 frame this syscall is about to `iretq` back
 * through. Both halves of a signal are edits to it:
 *
 *   deliver: save it onto the process's own stack, then point RIP at the
 *            handler and RSP just below the saved copy - at a word
 *            holding the restorer's address, so an ordinary `ret` out of
 *            an ordinary C function lands there.
 *   return:  read it back and put every field where it was.
 *
 * On the *process's* stack, not the kernel's, for the reason every Unix
 * does it that way: a process may already be inside a handler when the
 * next signal arrives, and one kernel-side saved context would be
 * overwritten by it. A stack nests for free.
 */
static long sys_sigreturn(uint64_t frame_ptr, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)frame_ptr;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    /* Never reached: syscall_handler intercepts SYS_sigreturn before
     * dispatch, because restoring a context means writing every field of
     * `regs` and the dispatch table's signature has no `regs` in it. The
     * entry exists so the table has no hole at this number - a NULL entry
     * would be a jump through a null pointer for a caller that guessed
     * the number. */
    return -1;
}

/* Returns 1 if a frame was built and `regs` now points at a handler. */
static int signal_deliver(isr_regs_t *regs) {
    task_t *self = sched_current();
    uint32_t ready = self->sig_pending & ~self->sig_blocked;
    if (ready == 0) {
        return 0;
    }
    /* Only on the way back to ring 3. A kernel thread that called a
     * syscall directly (kernel.c's self-tests do it constantly) has no
     * ring-3 frame to build one on, and its "stack" is kernel heap. */
    if ((regs->cs & 3) != 3) {
        return 0;
    }

    int signo = 0;
    for (int i = 1; i <= SIG_MAX; i++) {
        if (ready & (1u << i)) {
            signo = i;
            break;
        }
    }
    uint64_t handler = self->sig_handler[signo];
    if (handler == SIG_DFL_ADDR || handler == SIG_IGN_ADDR) {
        /* The disposition changed between raise and delivery. Drop it -
         * sched_raise_signal is where a default action is decided, and
         * re-deciding it here would apply it twice. */
        self->sig_pending &= ~(1u << signo);
        return 0;
    }
    if (self->sig_restorer == 0) {
        self->sig_pending &= ~(1u << signo);
        return 0;
    }

    /* M99: does this handler want a siginfo_t, and if so, build one.
     *
     * Filled here rather than at raise time because only some of it is
     * known at raise time: si_pid and si_status are recorded on the task
     * by whoever raised SIGCHLD, si_addr by the fault path, and si_code
     * follows from which of those it was. Everything not known is zero,
     * and zero is the answer rather than a gap - see the struct's own
     * note in system_api/include/signal.h. */
    int want_info = (self->sig_siginfo & (1u << signo)) != 0;
    siginfo_t info;
    if (want_info) {
        k_memset(&info, 0, sizeof(info));
        info.si_signo = signo;
        if (signo == SIGCHLD) {
            info.si_pid = self->si_pid;
            info.si_status = self->si_status;
            info.si_code = CLD_EXITED;
        } else if (signo == SIGSEGV || signo == SIGBUS) {
            info.si_addr = (void *)self->si_addr;
            info.si_code = SI_KERNEL;
        } else if (signo == SIGFPE || signo == SIGILL) {
            info.si_code = SI_KERNEL;
        } else {
            info.si_code = SI_USER;
        }
    }

    /* 128 bytes of clearance below the interrupted RSP before the frame,
     * then 16-byte alignment for the frame itself, then one word below it
     * for the return address - which leaves RSP % 16 == 8 at the handler's
     * first instruction, exactly as it is after a `call`. Getting that
     * wrong does not fault; it makes any SSE spill inside the handler
     * fault instead, some arbitrary distance away.
     *
     * M99: the siginfo_t goes below the frame in the same reservation,
     * so a handler that keeps the pointer past its own return is looking
     * at stack the sigreturn has released - which is exactly as true of
     * it on Linux. */
    uint64_t sp = regs->rsp;
    sp -= 128;
    if (want_info) {
        sp -= sizeof(siginfo_t);
        sp &= ~15ULL;
    }
    uint64_t info_addr = sp;
    sp -= sizeof(sig_frame_t);
    sp &= ~15ULL;
    uint64_t frame_addr = sp;
    uint64_t new_rsp = sp - 8;

    sig_frame_t frame;
    frame.rax = regs->rax;
    frame.rbx = regs->rbx;
    frame.rcx = regs->rcx;
    frame.rdx = regs->rdx;
    frame.rsi = regs->rsi;
    frame.rdi = regs->rdi;
    frame.rbp = regs->rbp;
    frame.r8 = regs->r8;
    frame.r9 = regs->r9;
    frame.r10 = regs->r10;
    frame.r11 = regs->r11;
    frame.r12 = regs->r12;
    frame.r13 = regs->r13;
    frame.r14 = regs->r14;
    frame.r15 = regs->r15;
    frame.rip = regs->rip;
    frame.rflags = regs->rflags;
    frame.rsp = regs->rsp;
    frame.saved_blocked = self->sig_blocked;
    frame.signo = (uint32_t)signo;

    /* Written through copy_to_user, so a process whose stack has no room
     * left gets its signal dropped rather than the kernel taking a fault
     * writing into it. Dropped and *not* delivered as a default action:
     * turning "your stack is full" into "you are killed by SIGINT" would
     * report the wrong problem. */
    if (copy_to_user(frame_addr, &frame, sizeof(frame)) != 0) {
        self->sig_pending &= ~(1u << signo);
        return 0;
    }
    uint64_t ret_addr = self->sig_restorer;
    if (copy_to_user(new_rsp, &ret_addr, sizeof(ret_addr)) != 0) {
        self->sig_pending &= ~(1u << signo);
        return 0;
    }

    if (want_info && copy_to_user(info_addr, &info, sizeof(info)) != 0) {
        self->sig_pending &= ~(1u << signo);
        return 0;
    }

    self->sig_pending &= ~(1u << signo);
    /* The handler's own signal is blocked while it runs, and put back by
     * sigreturn. Without this a signal arriving during its own handler
     * re-enters it, which is a stack that grows until it does not. */
    self->sig_blocked |= (1u << signo);

    regs->rsp = new_rsp;
    regs->rip = handler;
    regs->rdi = (uint64_t)signo; /* void handler(int) - SysV's first argument */
    /* M99: and the other two, when the handler was installed asking for
     * them. %rdx is the ucontext_t argument every other Unix passes and
     * this one does not have - NULL rather than a pointer to something
     * invented, so a program that reads it faults at once instead of
     * believing a fiction. */
    regs->rsi = want_info ? info_addr : 0;
    regs->rdx = 0;
    regs->rax = 0;
    return 1;
}

/* M99 - see syscall_entry.h. A synchronous fault, offered to the
 * handler the program installed for it.
 *
 * The signal is raised directly into sig_pending rather than through
 * sched_raise_signal, and the difference matters: sched_raise_signal
 * applies the DEFAULT action when there is no handler, and the default
 * here is already the caller's business - it is the terminate-with-this-
 * signal path that has existed since M52 and that reports the fault. A
 * second opinion about it would be a task that exits twice. */
int signal_deliver_fault(isr_regs_t *regs, int signo, uint64_t fault_addr) {
    task_t *self = sched_current();
    if (!self || (regs->cs & 3) != 3) {
        return 0;
    }
    if (signo <= 0 || signo > SIG_MAX || !SIG_IS_CATCHABLE(signo)) {
        return 0;
    }
    uint64_t handler = self->sig_handler[signo];
    if (handler == SIG_DFL_ADDR || handler == SIG_IGN_ADDR) {
        /* SIG_IGN on a fault is not "carry on" - carrying on re-executes
         * the instruction that faulted, forever. Ignoring a synchronous
         * fault means the same thing it means on every other Unix: the
         * default action happens anyway. */
        return 0;
    }
    if (self->sig_blocked & (1u << signo)) {
        /* Blocked, which is precisely the state inside its own handler.
         * This is where a handler that faults stops being a loop. */
        return 0;
    }
    /* M99: the address the CPU faulted on, which is CR2 and is the one
     * fact a SIGSEGV handler actually wants. Recorded before the raise
     * so signal_deliver can put it in si_addr. */
    self->si_addr = fault_addr;
    self->sig_pending |= (1u << signo);
    return signal_deliver(regs);
}

static int signal_return(isr_regs_t *regs) {
    task_t *self = sched_current();
    uint64_t frame_addr = regs->rdi;
    sig_frame_t frame;
    if ((regs->cs & 3) != 3) {
        return -1;
    }
    if (copy_from_user(&frame, frame_addr, sizeof(frame)) != 0) {
        return -1;
    }
    if (frame.rip < USER_REGION_BASE || frame.rip >= USER_REGION_LIMIT) {
        return -1;
    }
    if (frame.rsp < USER_REGION_BASE || frame.rsp >= USER_REGION_LIMIT) {
        return -1;
    }
    regs->rax = frame.rax;
    regs->rbx = frame.rbx;
    regs->rcx = frame.rcx;
    regs->rdx = frame.rdx;
    regs->rsi = frame.rsi;
    regs->rdi = frame.rdi;
    regs->rbp = frame.rbp;
    regs->r8 = frame.r8;
    regs->r9 = frame.r9;
    regs->r10 = frame.r10;
    regs->r11 = frame.r11;
    regs->r12 = frame.r12;
    regs->r13 = frame.r13;
    regs->r14 = frame.r14;
    regs->r15 = frame.r15;
    regs->rip = frame.rip;
    regs->rsp = frame.rsp;
    /* Only the bits a ring-3 program could have set for itself anyway,
     * plus IF forced on. A sigreturn is the one call whose argument is a
     * register file, so this is the one place where "the caller chose
     * this value" and "the CPU will load it" meet. */
    regs->rflags = (frame.rflags & USER_RFLAGS_MASK) | RFLAGS_IF;
    self->sig_blocked = frame.saved_blocked & ~(1u << SIGKILL) & ~(1u << SIGSEGV);
    return 0;
}

/* M101: the accounting bracket, and the reason dispatch became an inner
 * function rather than growing a counter at each of its five exits.
 *
 * There are five places below that return, and one of them (SYS_execve on
 * success) has already rewritten `regs` by the time it does. A counter
 * added at each exit is five chances to add it in four of them, which is
 * the shape of bug this project keeps finding in code that grew a special
 * case at a time. One bracket has no such choice to get wrong.
 *
 * What this deliberately does NOT record is a syscall that never comes
 * back: SYS_exit, a fatal signal delivered at entry, and an `execve` that
 * replaced the program all leave through task_exit_with_code or the new
 * image's entry point, and nothing here runs afterwards. A count of
 * "calls that returned" is the honest name for what this measures, and it
 * differs from "calls made" by at most one per process. */
static void syscall_dispatch(isr_regs_t *regs);

void syscall_handler(isr_regs_t *regs) {
    uint64_t num = regs->rax;
    int timing = syscount_timing_enabled();
    uint64_t started = timing ? tsc_read() : 0;

    syscall_dispatch(regs);

    syscount_record((int)num, timing ? (tsc_read() - started) : 0);
}

static void syscall_dispatch(isr_regs_t *regs) {
    /* Deliver a pending fatal signal before servicing the syscall the
     * caller actually asked for - this is the "next syscall entry"
     * checkpoint sys_kill's doc comment promises. A task that never
     * syscalls gets caught by sched.c's scheduler_tick instead. */
    task_t *self = sched_current();
    if (self->pending_signal != 0) { /* M76 - see sched_deliver_pending_signal */
        int sig = self->pending_signal;
        self->pending_signal = 0;
        task_exit_with_code(128 + sig); /* noreturn */
    }
    /* M85 (second attempt): and a pending STOP, at the same checkpoint.
     *
     * take_pending_stop's own comment has said since M85 that it is
     * "called from the same two places a fatal pending signal is - the
     * scheduler tick and the syscall boundary". It was called from one:
     * the tick. That is not a missing optimisation, it is why job
     * control did not work, and the failure is subtle enough to be worth
     * writing down.
     *
     * The tick takes the stop from whichever task is CURRENT when the
     * timer fires. A program waiting to be stopped is, almost by
     * definition, a program sitting in a blocking read - it wakes for a
     * few microseconds, finds nothing, and parks again. The chance that
     * a 10 ms tick lands inside one of those windows is small, so a ^Z
     * typed at a terminal reached a task that was READY with
     * `pending_stop` set and stayed that way indefinitely. It looked
     * exactly like "the stop machinery does not work", which is what
     * M85's first attempt concluded.
     *
     * Here it is taken on the way *into* the next syscall - and the
     * blocking read the task is about to make is a syscall. After the
     * death check, for the reason take_pending_stop already gives: a
     * task with both pending has been killed, and stopping it first
     * would suspend it with a SIGKILL it can never take. */
    sched_take_pending_stop_if_any(self);

    uint64_t num = regs->rax;
    if (num >= SYSCALL_COUNT) {
        regs->rax = (uint64_t)-1;
        return;
    }

    /* M67 boundary 3 of 3: the net syscalls run under net_lock. See
     * kernel/net/net.h for what it protects and why the bracket is here,
     * at dispatch, rather than inside each of the thirteen handlers -
     * one place with no early-return paths to get wrong, and it makes the
     * whole compound operation atomic (socket_for_fd followed by tcp_send
     * is two lookups that have to agree about the same socket).
     *
     * Every one of these is non-blocking by construction, so nothing
     * under this bracket calls schedule() and the interrupts-off window
     * is bounded by a buffer copy. SYS_settime is deliberately not in the
     * set: it is the clock, reached from a network program but not itself
     * touching a byte of kernel/net. */
    /* M76: intercepted before dispatch, because what it does is write
     * every field of `regs` - including rax, which the dispatch below
     * would then overwrite with a return value there is nowhere to put.
     * A failed restore is -1 in rax and the caller keeps running, which
     * is the only answer available: there is no context left to return
     * to and killing the process for a malformed frame it did not
     * construct would blame the wrong party. */
    /* M83: intercepted before dispatch for the same reason SYS_sigreturn
     * is - it needs `regs`, which the six-argument dispatch below cannot
     * hand it. A fork's whole observable difference is the value in rax
     * on two different stacks, so the frame is the argument. */
    if (num == SYS_fork) {
        regs->rax = (uint64_t)sys_fork(regs);
        signal_deliver(regs);
        return;
    }

    /* M84: intercepted for the same reason, and with one extra rule - on
     * success it has already written every field of `regs`, including
     * rax, so the return value must not be stored over it. An exec that
     * worked has nowhere to return a value to; an exec that failed is an
     * ordinary -1 to a caller that is still there. */
    if (num == SYS_execve) {
        long rc = sys_execve(regs);
        if (rc != 0) {
            regs->rax = (uint64_t)rc;
        }
        signal_deliver(regs);
        return;
    }

    if (num == SYS_sigreturn) {
        if (signal_return(regs) != 0) {
            regs->rax = (uint64_t)-1;
        }
        return;
    }

    if (syscall_touches_net(num)) {
        net_lock_acquire();
        regs->rax = (uint64_t)syscall_table[num](regs->rdi, regs->rsi, regs->rdx,
                                                  regs->rcx, regs->r8, regs->r9);
        net_lock_release();
        signal_deliver(regs);
        return;
    }

    regs->rax = (uint64_t)syscall_table[num](regs->rdi, regs->rsi, regs->rdx,
                                              regs->rcx, regs->r8, regs->r9);
    /* M76: the delivery point. After the syscall the caller asked for has
     * produced its answer, and while `regs` still describes the ring-3
     * context this is about to return to. A frame built here means the
     * `iretq` at the end of syscall_common_stub lands in the handler
     * instead of at the instruction after the caller's `int 0x80` - and
     * the saved copy is what puts it back. */
    signal_deliver(regs);
}
