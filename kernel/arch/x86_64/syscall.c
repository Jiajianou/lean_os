#include "syscall_entry.h"

#include <stdint.h>

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
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "power/power.h" /* M47 - power_shutdown, and power_mode.h's POWER_OFF/POWER_REBOOT through it */
#include "proc.h"      /* system_api/include/proc.h - task_info_t, M45. Resolves to the system_api one, not kernel/proc/proc.h below: a quoted include searches the *including* file's own directory first (kernel/arch/x86_64/, which has no proc.h), then -Ikernel (no kernel/proc.h either), then -Isystem_api/include. */
#include "proc/proc.h"
#include "proc/elf.h"   /* M48 - elf_validate, to tell "not a program" apart from "no such file" */
#include "sched/sched.h"
#include "signal.h"  /* system_api/include/signal.h - SIGKILL/SIGTERM */
#include "spawn_error.h" /* system_api/include/spawn_error.h - M48's distinct SYS_spawn failure codes */
#include "syscall.h" /* system_api/include/syscall.h - the shared ABI, on the include path via Makefile's -Isystem_api/include */
#include "display.h" /* system_api/include/display.h - display_mode_t, M58 */
#include "os_time.h" /* system_api/include/os_time.h - os_datetime_t, M59 */
#include "os_net.h"  /* system_api/include/os_net.h - os_sockaddr_t/os_netconf_t, M64 */
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
 * There is deliberately no copy_from_user counterpart to copy_to_user:
 * no syscall in this project takes a fixed-size struct *from* the caller
 * (the window-manager protocol carries its structs over pipes, not
 * arguments), so it would be a primitive with no user - which this
 * project has refused to write before, for the reasons kernel/ipc/pipe.h
 * gives about pipe_named. The day one is needed it is four lines, and
 * the rule it enforces is already written down here.
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
            int c = keyboard_read();
            if (c == -1) {
                if (n > 0) {
                    break;
                }
                /* M42: a shell parked on an empty keyboard buffer is past
                 * this handler's own signal check and gets no timer tick
                 * while it's current - see sched_deliver_pending_signal. */
                sched_deliver_pending_signal();
                schedule();
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
    task_exit_with_code((int)code); /* noreturn */
}

/* Task id doubles as pid - there's no process concept distinct from a
 * bare kernel thread. */
static long sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return sched_current()->id;
}

/* Combines fork+exec into one call: reads `path` from the filesystem and
 * loads it as a brand-new process via process_spawn (M9) - a fork/exec
 * "equivalent" (M13's own wording), not literal fork() semantics (no
 * address-space duplication). The child inherits the caller's whole fd
 * table (sched.c's task_spawn_common), so a pipe set up beforehand
 * carries over. */
static long sys_spawn(uint64_t path_ptr, uint64_t arg_ptr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
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
    if (copy_str_from_user(path, path_ptr, sizeof(path)) != 0) {
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
    char *arg = (char *)kmalloc(PAGE_SIZE);
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
            size_t room = PAGE_SIZE - used;
            if (room < 2 || copy_str_from_user(dst, slot, room) != 0) {
                break;
            }
            argv[argc++] = dst;
            used += k_strlen(dst) + 1;
        }
    }
    argv[argc] = (const char *)0;

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
        return SPAWN_ERR_NOT_FOUND;
    }
    uint8_t *image = (uint8_t *)kmalloc(st.size ? st.size : 1);
    if (!image) {
        kfree(arg);
        return SPAWN_ERR_NO_MEMORY;
    }
    int64_t size = vfs_read(path, image, st.size);
    if (size < 0) {
        kfree(image);
        kfree(arg);
        return SPAWN_ERR_NOT_FOUND;
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
        return SPAWN_ERR_BAD_IMAGE;
    }
    if (!sched_has_free_task_slot()) {
        kfree(image);
        kfree(arg);
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
    task_t *t = process_spawnv(name, image, (size_t)size, argv);
    kfree(image);
    kfree(arg);
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
            sched_deliver_pending_signal();
            schedule();
        }
    }

    task_t *t = sched_task_by_id((int)pid_arg);
    if (!t) {
        return -1;
    }
    while (t->state != TASK_TERMINATED) {
        sched_deliver_pending_signal();
        schedule();
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
    if (copy_str_from_user(path, name_ptr, sizeof(path)) != 0 ||
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
    if (copy_str_from_user(path, name_ptr, sizeof(path)) != 0 ||
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
    if (copy_str_from_user(path, path_ptr, sizeof(path)) != 0) {
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
    if (copy_str_from_user(old_path, old_ptr, sizeof(old_path)) != 0 ||
        copy_str_from_user(new_path, new_ptr, sizeof(new_path)) != 0) {
        return -1;
    }
    return vfs_rename(old_path, new_path);
}

static long sys_listdir(uint64_t path_ptr, uint64_t buf, uint64_t maxlen, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_str_from_user(path, path_ptr, sizeof(path)) != 0 ||
        !user_range_ok(buf, maxlen, 1)) {
        return -1;
    }
    if (!vfs_is_dir(path)) {
        return -1; /* an ordinary file, or nothing at all - either way not something with contents to list */
    }
    return (long)vfs_list(path, (char *)buf, (size_t)maxlen);
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
    if (copy_str_from_user(path, path_ptr, sizeof(path)) != 0) {
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
static long sys_kill(uint64_t pid, uint64_t sig, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (sig != SIGKILL && sig != SIGTERM) {
        return -1;
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
    int mine = 0;
    task_t *up = t;
    for (int depth = 0; up && depth < MAX_TASKS; depth++) {
        if (up->parent_id == self->id) {
            mine = 1;
            break;
        }
        up = up->parent_id >= 0 ? sched_task_by_id(up->parent_id) : (task_t *)0;
    }
    if (!mine && !has_cap(CAP_KILL_ANY)) {
        return -1;
    }
    t->pending_signal = (int)sig;
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
    self->fds[read_fd].pipe = p;
    self->fds[write_fd].type = FD_PIPE_WRITE;
    self->fds[write_fd].pipe = p;

    out[0] = read_fd;
    out[1] = write_fd;
    return copy_to_user(fds_out_ptr, out, sizeof(out));
}

/* Read-only: nothing needs to *change* a process's group yet (no job
 * control in this shell), so there's no setpgid to go with it. */
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
    task_t *cur = sched_current();
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
        vmm_map_page_in(cur->pml4_phys, cur->heap_mapped_end, phys, VMM_FLAG_WRITABLE | VMM_FLAG_USER);
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
    task_t *cur = sched_current();
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
        vmm_map_page_in(cur->pml4_phys, USER_FB_BASE + i * PAGE_SIZE, phys_base + i * PAGE_SIZE,
                         VMM_FLAG_WRITABLE | VMM_FLAG_USER);
    }
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
    self->fds[read_fd].pipe = p;
    self->fds[write_fd].type = FD_PIPE_WRITE;
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
        e->state = (t->state == TASK_TERMINATED) ? TASK_INFO_TERMINATED
                                                  : (t->state == TASK_RUNNING ? TASK_INFO_RUNNING : TASK_INFO_READY);
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
    if (copy_str_from_user(path, path_ptr, sizeof(path)) != 0) {
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
    int handle = vfs_open(path, (flags & OPEN_CREATE) != 0);
    if (handle < 0) {
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
    openfile_t *of = openfile_alloc(handle, writable);
    if (!of) {
        return -1;
    }
    if (flags & OPEN_APPEND) {
        of->offset = vfs_handle_size(handle);
    }
    self->fds[fd].type = FD_FILE;
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
    if (copy_str_from_user(path, path_ptr, sizeof(path)) != 0) {
        return -1;
    }
    leanfs_stat_t st;
    if (vfs_stat(path, &st) != 0) {
        return -1;
    }
    os_stat_t out;
    out.size = st.size;
    out.mtime = st.mtime;
    out.is_dir = st.is_dir;
    out.reserved = 0;
    return copy_to_user(out_ptr, &out, sizeof(out));
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
    if (copy_str_from_user(path, path_ptr, sizeof(path)) != 0) {
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

void syscall_handler(isr_regs_t *regs) {
    /* Deliver a pending fatal signal before servicing the syscall the
     * caller actually asked for - this is the "next syscall entry"
     * checkpoint sys_kill's doc comment promises. A task that never
     * syscalls gets caught by sched.c's scheduler_tick instead. */
    task_t *self = sched_current();
    if (self->pending_signal == SIGKILL || self->pending_signal == SIGTERM) {
        int sig = self->pending_signal;
        self->pending_signal = 0;
        task_exit_with_code(128 + sig); /* noreturn */
    }

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
    if (syscall_touches_net(num)) {
        net_lock_acquire();
        regs->rax = (uint64_t)syscall_table[num](regs->rdi, regs->rsi, regs->rdx,
                                                  regs->rcx, regs->r8, regs->r9);
        net_lock_release();
        return;
    }

    regs->rax = (uint64_t)syscall_table[num](regs->rdi, regs->rsi, regs->rdx,
                                              regs->rcx, regs->r8, regs->r9);
}
