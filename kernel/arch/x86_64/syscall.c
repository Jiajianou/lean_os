#include "syscall_entry.h"

#include <stdint.h>

#include "drivers/fb.h"
#include "drivers/keyboard.h"
#include "drivers/klog.h"
#include "drivers/mouse.h"
#include "drivers/pit.h"
#include "fs/leanfs.h"
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
#include "wm.h"      /* system_api/include/wm.h - wm_fb_info_t, M20 */

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
        return SPAWN_ERR_NOT_FOUND;
    }
    const char *path = (const char *)path_ptr;
    const char *arg = arg_ptr ? (const char *)arg_ptr : "";

    uint8_t *image = (uint8_t *)kmalloc(LEANFS_MAX_FILE_SIZE);
    if (!image) {
        return SPAWN_ERR_NO_MEMORY;
    }
    int64_t size = vfs_read(path, image, LEANFS_MAX_FILE_SIZE);
    if (size < 0) {
        kfree(image);
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
        return SPAWN_ERR_BAD_IMAGE;
    }
    if (!sched_has_free_task_slot()) {
        kfree(image);
        return SPAWN_ERR_NO_TASK_SLOT;
    }

    /* process_spawn's elf_load synchronously copies every byte it needs
     * into fresh physical frames before returning, so freeing this
     * buffer right away is safe - nothing keeps pointing at it. */
    /* M45: `path` is what this process gets listed as - the only
     * human-readable identity anything at this layer has for it. */
    task_t *t = process_spawn(path, image, (size_t)size, arg);
    kfree(image);
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

/* M33: mirrors sys_readfile's shape exactly - see SYS_writefile's own doc
 * comment (system_api/include/syscall.h) for why this hadn't been needed
 * until now. */
static long sys_writefile(uint64_t name_ptr, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (name_ptr == 0 || buf == 0) {
        return -1;
    }
    return vfs_write((const char *)name_ptr, (const void *)buf, (size_t)len);
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
static long sys_fb_info(uint64_t out_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (out_ptr == 0) {
        return -1;
    }
    wm_fb_info_t *out = (wm_fb_info_t *)out_ptr;
    out->width = fb_width();
    out->height = fb_height();
    out->pitch = fb_pitch_bytes();
    out->bpp = 32;
    return 0;
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
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *cur = sched_current();
    uint64_t phys_base = fb_phys_addr();
    uint64_t size = (uint64_t)fb_pitch_bytes() * fb_height();
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
    if (out_ptr == 0) {
        return -1;
    }
    return mouse_read((mouse_event_t *)out_ptr);
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
    if (name_ptr == 0 || fds_out_ptr == 0) {
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

    pipe_t *p = pipe_named((const char *)name_ptr);
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
    if (out_ptr == 0) {
        return -1;
    }
    int c = keyboard_read();
    if (c == -1) {
        return 0;
    }
    *(char *)out_ptr = (char)c;
    return 1;
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
    return (long)slot->pipe->count;
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
    self->fds[newfd] = self->fds[oldfd];
    return (long)newfd;
}

/* Non-blocking counterpart to SYS_wait: never yields, just reports
 * whether pid has already terminated - -2 (not -1, which already means
 * "no such task") is "still running", so a caller with its own event
 * loop to keep servicing (a GUI terminal, spawning a child while it
 * keeps redrawing/routing input) can poll this every iteration instead
 * of blocking. Reaps pid the same way SYS_wait does once it does report
 * a real exit code. */
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
    return t->exit_code;
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
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (buf == 0 || max_entries == 0 || max_entries > 4096) {
        return -1;
    }
    task_info_t *out = (task_info_t *)buf;
    int total = sched_task_count();
    uint64_t written = 0;
    for (int i = 0; i < total && written < max_entries; i++) {
        task_t *t = sched_task_by_id(i);
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
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (buf == 0) {
        return -1;
    }
    clipboard_set((const void *)buf, (size_t)len);
    return 0;
}

static long sys_clipboard_get(uint64_t buf, uint64_t maxlen, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (buf == 0) {
        return -1;
    }
    return (long)clipboard_get((void *)buf, (size_t)maxlen);
}

/* M47: see SYS_shutdown's own doc comment. Returns only for a mode this
 * kernel doesn't recognize; on POWER_OFF/POWER_REBOOT power_shutdown
 * never comes back, so the caller's own return value is unobservable. */
static long sys_shutdown(uint64_t mode, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
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
