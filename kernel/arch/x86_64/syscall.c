#include "syscall_entry.h"

#include <stdint.h>

#include "drivers/keyboard.h"
#include "drivers/klog.h"
#include "fs/leanfs.h"
#include "fs/vfs.h"
#include "ipc/pipe.h"
#include "mm/heap.h"
#include "proc/proc.h"
#include "sched/sched.h"
#include "signal.h"  /* system_api/include/signal.h - SIGKILL/SIGTERM */
#include "syscall.h" /* system_api/include/syscall.h - the shared ABI, on the include path via Makefile's -Isystem_api/include */

/* Every syscall implementation shares one signature regardless of how
 * many of its six argument slots it actually uses - keeps the dispatch
 * table trivial. Unused args are silently ignored by whichever function
 * doesn't need them. */
typedef long (*syscall_fn_t)(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6);

/* buf is trusted as-is for now; validating that a ring-3 pointer is
 * actually mapped and owned by the caller is left for whenever a
 * genuinely untrusted program needs to run here. */
static long sys_write(uint64_t fd, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (buf == 0 || fd >= MAX_FDS) {
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
    if (buf == 0 || fd >= MAX_FDS) {
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
    if (path_ptr == 0) {
        return -1;
    }
    const char *path = (const char *)path_ptr;
    const char *arg = arg_ptr ? (const char *)arg_ptr : "";

    uint8_t *image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
    if (!image) {
        return -1;
    }
    int64_t size = vfs_read(path, image, LEANFS_MAX_FILE_SIZE);
    if (size < 0) {
        kfree(image);
        return -1;
    }

    /* process_spawn's elf_load synchronously copies every byte it needs
     * into fresh physical frames before returning, so freeing this
     * buffer right away is safe - nothing keeps pointing at it. */
    task_t *t = process_spawn(image, (size_t)size, arg);
    kfree(image);
    if (!t) {
        return -1;
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
            int total = sched_task_count();
            for (int i = 0; i < total; i++) {
                task_t *t = sched_task_by_id(i);
                if (!t || t->parent_id != self->id || t->reaped) {
                    continue;
                }
                any_children = 1;
                if (t->state == TASK_TERMINATED) {
                    t->reaped = 1;
                    return t->id;
                }
            }
            if (!any_children) {
                return -1;
            }
            schedule();
        }
    }

    task_t *t = sched_task_by_id((int)pid_arg);
    if (!t) {
        return -1;
    }
    while (t->state != TASK_TERMINATED) {
        schedule();
    }
    t->reaped = 1;
    return t->exit_code;
}

/* Whole-file read by name - no open/close/lseek yet, matching how few
 * programs actually need file I/O so far (cat is the one). */
static long sys_readfile(uint64_t name_ptr, uint64_t buf, uint64_t maxlen, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (name_ptr == 0 || buf == 0) {
        return -1;
    }
    return (long)vfs_read((const char *)name_ptr, (void *)buf, (size_t)maxlen);
}

static long sys_listfiles(uint64_t buf, uint64_t maxlen, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (buf == 0) {
        return -1;
    }
    return (long)vfs_list((char *)buf, (size_t)maxlen);
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
    if (fds_out_ptr == 0) {
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

    int *out = (int *)fds_out_ptr;
    out[0] = read_fd;
    out[1] = write_fd;
    return 0;
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

static const syscall_fn_t syscall_table[SYSCALL_COUNT] = {
    [SYS_write] = sys_write,
    [SYS_exit] = sys_exit,
    [SYS_getpid] = sys_getpid,
    [SYS_spawn] = sys_spawn,
    [SYS_wait] = sys_wait,
    [SYS_read] = sys_read,
    [SYS_readfile] = sys_readfile,
    [SYS_listfiles] = sys_listfiles,
    [SYS_kill] = sys_kill,
    [SYS_pipe] = sys_pipe,
    [SYS_getpgid] = sys_getpgid,
};

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
    regs->rax = (uint64_t)syscall_table[num](regs->rdi, regs->rsi, regs->rdx,
                                              regs->rcx, regs->r8, regs->r9);
}
