#include "syscall_entry.h"

#include <stdint.h>

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/tsc.h"
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
#include "dev/random.h"
#include "fs/flock.h"
#include "fs/openfile.h"
#include "fs/vfs.h"
#include "ipc/clipboard.h"
#include "ipc/pipe.h"
#include "ipc/shm.h"
#include "ipc/unixsock.h"
#include "ipc/eventfd.h"
#include "ipc/timerfd.h"
#include "ipc/epoll.h"
#include "ipc/memfd.h"
#include "os_poll.h"
#include "drivers/blk.h"
#include "mm/filemap.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "power/power.h"
#include "os_fs.h"
#include "proc.h"
#include "proc/proc.h"
#include "proc/pkgcaps.h"
#include "proc/elf.h"
#include "profile/sampler.h"
#include "profile/syscount.h"
#include "sched/sched.h"
#include "signal.h"
#include "spawn_error.h"
#include "syscall.h"
#include "display.h"
#include "os_time.h"
#include "os_net.h"
#include "dev/tty.h"
#include "mman.h"
#include "caps.h"
#include "net/net.h"
#include "net/socket.h"
#include "net/tcp.h"
#include "wm.h"

typedef long (*syscall_fn_t)(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6);

static int user_range_ok(uint64_t addr, uint64_t len, int need_write) {
    if (addr == 0) {
        return 0;
    }
    if (sched_current()->pml4_phys == vmm_kernel_pml4_phys()) {
        return 1;
    }
    if (len == 0) {
        return 1;
    }
    if (addr < USER_REGION_BASE || addr >= USER_REGION_LIMIT) {
        return 0;
    }
    uint64_t end = addr + len;
    if (end < addr || end > USER_REGION_LIMIT) {
        return 0;
    }
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

static int copy_str_from_user(char *dst, uint64_t src, uint64_t max) {
    uint64_t checked_to = 0;
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

#define PATH_MAX_DEPTH 128

static int path_normalize(char *out, const char *in) {
    int starts[PATH_MAX_DEPTH];
    int depth = 0;
    int n = 0;

    if (in[0] != '/') {
        return -1;
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
        int c_start = i;
        while (in[i] && in[i] != '/') {
            i++;
        }
        int c_len = i - c_start;

        if (c_len == 1 && in[c_start] == '.') {
            continue;
        }
        if (c_len == 2 && in[c_start] == '.' && in[c_start + 1] == '.') {
            if (depth > 0) {
                n = starts[--depth];
                if (n > 1) {
                    n--;
                }
            }
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

static int copy_path_from_user(char *out, uint64_t src) {
    char raw[LEANFS_MAX_PATH];
    if (copy_str_from_user(raw, src, sizeof(raw)) != 0) {
        return -1;
    }
    if (raw[0] == '/') {
        return path_normalize(out, raw);
    }
    char joined[LEANFS_MAX_PATH];
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

static int may_write_path(const char *path) {
    if (!has_cap(CAP_FS_WRITE)) {
        return 0;
    }
    if (path_is_under_pkg(path) && !has_cap(CAP_PKG_ADMIN)) {
        return 0;
    }
    return 1;
}

static int copy_write_path_from_user(char *out, uint64_t src) {
    if (copy_path_from_user(out, src) != 0) {
        return -1;
    }
    return may_write_path(out) ? 0 : -1;
}

static void pkg_note_write(const char *path, long result) {
    if (result >= 0 && path_is_under_pkg(path)) {
        pkg_registry_invalidate();
    }
}

static uint64_t clock_now_ns(void) {
    return pit_get_ticks() * (1000ULL / PIT_HZ) * 1000000ULL;
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
        return pipe_write(slot->pipe, s, (size_t)len, slot->nonblock);
    }
    if (slot->type == FD_EVENT) {
        if (len < sizeof(uint64_t)) {
            return -1;
        }
        uint64_t v = 0;
        if (copy_from_user(&v, buf, sizeof(v)) != 0) {
            return -1;
        }
        for (;;) {
            uint64_t seq = sched_event_seq();
            int rc = eventfd_write(slot->event, v);
            if (rc == 0) {
                return (long)sizeof(v);
            }
            if (rc == -2) {
                return -1;
            }
            if (slot->nonblock) {
                return -OS_ERR_AGAIN;
            }
            if (sched_signal_pending()) {
                return -OS_ERR_INTR;
            }
            sched_block_on_seq(SCHED_POLL_CHAN, 0, seq);
        }
    }
    if (slot->type == FD_UNIX) {
        uint64_t sent = 0;
        while (sent < len) {
            uint32_t chunk = (len - sent) > UNIX_BUF_SIZE ? UNIX_BUF_SIZE : (uint32_t)(len - sent);
            uint8_t staging[UNIX_BUF_SIZE];
            if (copy_from_user(staging, buf + sent, chunk) != 0) {
                return sent ? (long)sent : -1;
            }
            uint64_t seq = sched_event_seq();
            long m = unixsock_send(slot->un, staging, chunk, (const fd_slot_t *)0, 0);
            if (m < 0) {
                return sent ? (long)sent : -1;
            }
            if (m == 0) {
                if (slot->nonblock) {
                    return sent ? (long)sent : -OS_ERR_AGAIN;
                }
                if (sched_signal_pending()) {
                    return sent ? (long)sent : -OS_ERR_INTR;
                }
                sched_block_on_seq(SCHED_POLL_CHAN, 0, seq);
                continue;
            }
            sent += (uint64_t)m;
        }
        return (long)sent;
    }
    if (slot->type == FD_SOCKET) {
        struct tcpcb *tcb = socket_tcb(slot->sock);
        if (!tcb) {
            return -1;
        }
        uint64_t sent = 0;
        while (sent < len) {
            uint16_t chunk = (len - sent) > TCP_MAX_MSS ? TCP_MAX_MSS : (uint16_t)(len - sent);
            uint8_t staging[TCP_MAX_MSS];
            if (copy_from_user(staging, buf + sent, chunk) != 0) {
                return sent ? (long)sent : -1;
            }
            uint64_t seq = sched_event_seq();
            net_lock_acquire();
            int m = tcp_send(tcb, staging, chunk);
            net_lock_release();
            if (m < 0) {
                return sent ? (long)sent : -1;
            }
            if (m == 0) {
                if (slot->nonblock) {
                    return sent ? (long)sent : -OS_ERR_AGAIN;
                }
                if (sched_signal_pending()) {
                    return sent ? (long)sent : -OS_ERR_INTR;
                }
                sched_block_on_seq(SCHED_POLL_CHAN, pit_get_ticks() * (1000 / PIT_HZ) + 10, seq);
                continue;
            }
            sent += (uint64_t)m;
        }
        return (long)sent;
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
            uint64_t seq = sched_event_seq();
            int c = keyboard_read();
            if (c == -1) {
                if (n > 0) {
                    break;
                }
                sched_block_on_seq(SCHED_KEYBOARD_CHAN, 0, seq);
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
        return pipe_read(slot->pipe, dst, (size_t)len, slot->nonblock);
    }
    if (slot->type == FD_EVENT || slot->type == FD_TIMER) {
        if (len < sizeof(uint64_t)) {
            return -1;
        }
        for (;;) {
            uint64_t seq = sched_event_seq();
            uint64_t value = 0;
            int got = (slot->type == FD_EVENT)
                          ? eventfd_read(slot->event, &value)
                          : timerfd_read(slot->timer, clock_now_ns(), &value);
            if (got == 0) {
                return copy_to_user(buf, &value, sizeof(value)) == 0
                           ? (long)sizeof(value)
                           : -1;
            }
            if (slot->nonblock) {
                return -OS_ERR_AGAIN;
            }
            if (sched_signal_pending()) {
                return -OS_ERR_INTR;
            }
            uint64_t deadline = 0;
            if (slot->type == FD_TIMER) {
                long ms = timerfd_next_ms(slot->timer, clock_now_ns());
                if (ms < 0) {
                    deadline = 0;
                } else {
                    deadline = pit_get_ticks() * (1000 / PIT_HZ) + (uint64_t)ms;
                }
            }
            sched_block_on_seq(SCHED_POLL_CHAN, deadline, seq);
        }
    }
    if (slot->type == FD_UNIX) {
        uint32_t want = len > UNIX_BUF_SIZE ? UNIX_BUF_SIZE : (uint32_t)len;
        uint8_t staging[UNIX_BUF_SIZE];
        for (;;) {
            uint64_t seq = sched_event_seq();
            long n = unixsock_recv(slot->un, staging, want, (fd_slot_t *)0, 0,
                                   (int *)0, (int *)0);
            if (n > 0) {
                return copy_to_user(buf, staging, (size_t)n) == 0 ? n : -1;
            }
            if (n < 0) {
                return 0;
            }
            if (slot->nonblock) {
                return -OS_ERR_AGAIN;
            }
            if (sched_signal_pending()) {
                return -OS_ERR_INTR;
            }
            sched_block_on_seq(SCHED_POLL_CHAN, 0, seq);
        }
    }
    if (slot->type == FD_SOCKET) {
        struct tcpcb *tcb = socket_tcb(slot->sock);
        if (!tcb) {
            return -1;
        }
        uint16_t want = len > TCP_MAX_MSS ? TCP_MAX_MSS : (uint16_t)len;
        uint8_t staging[TCP_MAX_MSS];
        for (;;) {
            uint64_t seq = sched_event_seq();
            net_lock_acquire();
            int n = tcp_recv(tcb, staging, want);
            net_lock_release();
            if (n > 0) {
                return copy_to_user(buf, staging, (size_t)n) == 0 ? n : -1;
            }
            if (n < 0) {
                return 0;
            }
            if (slot->nonblock) {
                return -OS_ERR_AGAIN;
            }
            if (sched_signal_pending()) {
                return -OS_ERR_INTR;
            }
            sched_block_on_seq(SCHED_POLL_CHAN, 0, seq);
        }
    }
    if (slot->type == FD_FILE) {
        if (slot->file->is_dir) {
            return -1;
        }
        while (!vfs_handle_readable(slot->file->handle)) {
            uint64_t seq = sched_event_seq();
            if (vfs_handle_readable(slot->file->handle)) {
                break;
            }
            sched_block_on_seq(SCHED_POLL_CHAN, 0, seq);
            if (sched_signal_pending()) {
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

static long pfile_slot(uint64_t fd, uint64_t buf, uint64_t len,
                       int64_t offset, int write, fd_slot_t **out_slot) {
    if (fd >= MAX_FDS || !user_range_ok(buf, len, write ? 0 : 1)) {
        return -1;
    }
    if (offset < 0) {
        return -1;
    }
    fd_slot_t *slot = &sched_current()->fds[fd];
    if (slot->type != FD_FILE) {
        return slot->type == FD_NONE ? -1 : -OS_ERR_SPIPE;
    }
    if (slot->file->is_dir) {
        return -1;
    }
    *out_slot = slot;
    return 0;
}

static long sys_pread(uint64_t fd, uint64_t buf, uint64_t len, uint64_t offset,
                      uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    fd_slot_t *slot = NULL;
    long err = pfile_slot(fd, buf, len, (int64_t)offset, 0, &slot);
    if (err != 0) {
        return err;
    }
    int64_t n = vfs_handle_read(slot->file->handle, (char *)buf, (size_t)len,
                                (uint32_t)offset);
    return n < 0 ? -1 : (long)n;
}

static long sys_pwrite(uint64_t fd, uint64_t buf, uint64_t len, uint64_t offset,
                       uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    fd_slot_t *slot = NULL;
    long err = pfile_slot(fd, buf, len, (int64_t)offset, 1, &slot);
    if (err != 0) {
        return err;
    }
    if (!slot->file->writable) {
        return -1;
    }
    int64_t n = vfs_handle_write(slot->file->handle, (const char *)buf,
                                 (size_t)len, (uint32_t)offset);
    return n < 0 ? -1 : (long)n;
}

static long sys_exit(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    sched_kill_thread_group(sched_current());
    task_exit_with_code((int)code);
}

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

static long sys_thread_create(uint64_t entry, uint64_t arg, uint64_t stack_top,
                               uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (entry < USER_REGION_BASE || entry >= USER_REGION_LIMIT) {
        return -1;
    }
    if (stack_top < USER_REGION_BASE || stack_top >= USER_REGION_LIMIT) {
        return -1;
    }
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
    task_exit_with_code((int)value);
}

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
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_ptr) != 0) {
        return SPAWN_ERR_NOT_FOUND;
    }
    char *arg = (char *)kmalloc(USER_ARG_BYTES);
    if (!arg) {
        return SPAWN_ERR_NO_MEMORY;
    }
    const char *argv[SPAWN_MAX_ARGS + 1];
    int argc = 0;
    size_t used = 0;
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
                break;
            }
            envv[envc++] = dst;
            eused += k_strlen(dst) + 1;
        }
        envv[envc] = (const char *)0;
        envp = envv;
    }

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
            return SPAWN_ERR_BAD_IMAGE;
        }

        const char *shifted[SPAWN_MAX_ARGS + 2];
        int sc = 0;
        shifted[sc++] = interp;
        shifted[sc++] = path;
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
        uint32_t icaps = caps_for_spawn_path(interp);
        if (path_is_under_pkg(path)) {
            icaps &= caps_for_spawn_path(path);
        }
        task_t *it = process_spawnve_capped(iname, iimage, (size_t)isize, shifted,
                                            envp, icaps);
        kfree(iimage);
        kfree(arg);
        kfree(envbuf);
        return it ? (long)it->id : SPAWN_ERR_NO_MEMORY;
    }

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

    const char *name = path;
    for (const char *c = path; *c; c++) {
        if (*c == '/') {
            name = c + 1;
        }
    }
    task_t *t = process_spawnve_capped(name, image, (size_t)size, argv, envp,
                                       caps_for_spawn_path(path));
    kfree(image);
    kfree(arg);
    kfree(envbuf);
    if (!t) {
        return SPAWN_ERR_NO_MEMORY;
    }
    return t->id;
}

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
            uint64_t seq = sched_event_seq();
            int total = sched_task_count();
            for (int i = 0; i < total; i++) {
                task_t *t = sched_task_by_slot(i);
                if (!t || t->parent_id != self->id || t->reaped) {
                    continue;
                }
                any_children = 1;
                if (t->state == TASK_TERMINATED) {
                    t->reaped = 1;
                    int pid = t->id;
                    sched_reap_slot(t);
                    return pid;
                }
            }
            if (!any_children) {
                return -1;
            }
            sched_block_on_seq(SCHED_POLL_CHAN, pit_get_ticks() * (1000 / PIT_HZ) + 50, seq);
        }
    }

    task_t *t = sched_task_by_id((int)pid_arg);
    if (!t) {
        return -1;
    }
    while (t->state != TASK_TERMINATED) {
        uint64_t seq = sched_event_seq();
        if (t->state == TASK_TERMINATED) {
            break;
        }
        sched_block_on_seq((const void *)t, pit_get_ticks() * (1000 / PIT_HZ) + 200, seq);
    }
    t->reaped = 1;
    int code = t->exit_code;
    sched_reap_slot(t);
    return code;
}

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

static long sys_writefile(uint64_t name_ptr, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(path, name_ptr) != 0 ||
        !user_range_ok(buf, len, 0)) {
        return -1;
    }
    long r = vfs_write(path, (const void *)buf, (size_t)len);
    pkg_note_write(path, r);
    return r;
}

static long sys_unlink(uint64_t path_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(path, path_ptr) != 0) {
        return -1;
    }
    long r = vfs_unlink(path);
    pkg_note_write(path, r);
    return r;
}

static long sys_rename(uint64_t old_ptr, uint64_t new_ptr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char old_path[LEANFS_MAX_PATH];
    char new_path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(old_path, old_ptr) != 0 ||
        copy_write_path_from_user(new_path, new_ptr) != 0) {
        return -1;
    }
    long r = vfs_rename(old_path, new_path);
    pkg_note_write(old_path, r);
    pkg_note_write(new_path, r);
    return r;
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
        return -1;
    }
    return (long)vfs_list(path, (char *)buf, (size_t)maxlen);
}

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
            break;
        }

        size_t name_len = k_strlen(e.name);
        size_t need = (sizeof(os_dirent_t) + name_len + 1 + 7) & ~(size_t)7;
        if (written + need > buflen) {
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

static long sys_mkdir(uint64_t path_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(path, path_ptr) != 0) {
        return -1;
    }
    long r = vfs_mkdir(path);
    pkg_note_write(path, r);
    return r;
}

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
    if ((long)sig < 0 || sig > SIG_MAX) {
        return -1;
    }

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
        return delivered > 0 ? 0 : -1;
    }

    task_t *t = sched_task_by_id((int)pid);
    if (!t || t->state == TASK_TERMINATED) {
        return -1;
    }
    task_t *self = sched_current();
    if (!may_signal(self, t)) {
        return -1;
    }
    if (sig == 0) {
        return 0;
    }
    sched_raise_signal(t, (int)sig);
    return 0;
}

static long sys_pipe(uint64_t fds_out_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
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
    self->fds[read_fd].cloexec = 0;
    self->fds[read_fd].pipe = p;
    self->fds[write_fd].type = FD_PIPE_WRITE;
    self->fds[write_fd].cloexec = 0;
    self->fds[write_fd].pipe = p;

    out[0] = read_fd;
    out[1] = write_fd;
    return copy_to_user(fds_out_ptr, out, sizeof(out));
}

static long sys_symlink(uint64_t target_ptr, uint64_t path_ptr, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(path, path_ptr) != 0) {
        return -1;
    }
    char target[LEANFS_MAX_PATH];
    if (copy_str_from_user(target, target_ptr, sizeof(target)) != 0) {
        return -1;
    }
    long r = vfs_symlink(path, target);
    pkg_note_write(path, r);
    return r;
}

static long sys_link(uint64_t old_ptr, uint64_t new_ptr, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char old_path[LEANFS_MAX_PATH];
    char new_path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(old_path, old_ptr) != 0 ||
        copy_write_path_from_user(new_path, new_ptr) != 0) {
        return -1;
    }
    long r = vfs_link(old_path, new_path);
    pkg_note_write(new_path, r);
    return r;
}

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
    vfs_sync();
    blk_flush();
    return 0;
}

static long sys_alarm(uint64_t seconds, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (seconds > 0xffffffffu) {
        seconds = 0xffffffffu;
    }
    return (long)sched_set_alarm(sched_current(), (unsigned int)seconds);
}

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

static long sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = sched_current();
    switch (code) {
    case ARCH_SET_FS:
        if (addr != 0 && !user_range_ok(addr, 1, 0)) {
            return -1;
        }
        self->fs_base = addr;
        cpu_write_msr(MSR_FS_BASE, addr);
        return 0;
    case ARCH_GET_FS:
        if (!user_range_ok(addr, sizeof(uint64_t), 1)) {
            return -1;
        }
        return copy_to_user(addr, &self->fs_base, sizeof(self->fs_base));
    default:
        return -1;
    }
}

static spinlock_t futex_lock;

static long sys_futex(uint64_t addr, uint64_t op, uint64_t val,
                      uint64_t timeout_ms, uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    if ((addr & 3u) != 0) {
        return -1;
    }
    if (!user_range_ok(addr, sizeof(uint32_t), 0)) {
        return -1;
    }
    if (op == FUTEX_WAKE) {
        int max = (val > (uint64_t)(unsigned)MAX_TASKS) ? MAX_TASKS : (int)val;
        return sched_wake_n((const void *)addr, max);
    }
    if (op != FUTEX_WAIT) {
        return -1;
    }

    uint64_t flags = spin_lock_irqsave(&futex_lock);
    uint32_t seen = *(const volatile uint32_t *)addr;
    if (seen != (uint32_t)val) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return -1;
    }
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = pit_get_ticks() * (1000 / PIT_HZ) + timeout_ms;
    }
    sched_block_on((const void *)addr, deadline, &futex_lock, &flags);
    spin_unlock_irqrestore(&futex_lock, flags);
    if (deadline != 0 && pit_get_ticks() * (1000 / PIT_HZ) >= deadline) {
        return -2;
    }
    return 0;
}

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
        return -1;
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
        uint64_t peak = vmm_rss_peak_pages(self->pml4_phys);
        r.max_rss_pages = (peak > self->max_rss_pages) ? peak : self->max_rss_pages;
    } else if (who == OS_RUSAGE_CHILDREN) {
        r.user_ticks = self->child_user_ticks;
        r.sys_ticks = self->child_sys_ticks;
        r.max_rss_pages = self->child_max_rss_pages;
    } else {
        return -1;
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
    char path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(path, path_ptr) != 0) {
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
        return -OS_ERR_NOENT;
    }
    os_stat_t out;
    k_memset(&out, 0, sizeof(out));
    out.size = st.size;
    out.mtime = st.mtime;
    out.is_dir = st.is_dir;
    out.is_link = st.is_link;
    out.kind = st.is_dir ? OS_STAT_DIR : OS_STAT_FILE;
    out.inode = st.inode;
    return copy_to_user(out_ptr, &out, sizeof(out));
}

static long sys_ftruncate(uint64_t fd, uint64_t length, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = sched_current();
    if (fd < MAX_FDS && self->fds[fd].type == FD_MEMFD) {
        return memfd_truncate(self->fds[fd].memfd, length);
    }
    if (!has_cap(CAP_FS_WRITE)) {
        return -1;
    }
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

static tty_t *tty_for_fd(task_t *self, uint64_t fd, int *pty_number) {
    *pty_number = -1;
    if (fd >= MAX_FDS) {
        return NULL;
    }
    fd_slot_t *slot = &self->fds[fd];
    if (slot->type == FD_STDIN || slot->type == FD_STDOUT) {
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
        return -1;
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
        return 0;
    }
    case TIOCGPTN: {
        if (pty_number < 0) {
            return -1;
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
        if (t->sid == 0) {
            t->sid = self->sid;
        }
        if (t->sid != self->sid) {
            return -1;
        }
        t->fg_pgid = pgid;
        return 0;
    }
    case TIOCSCTTY:
        if (self->sid != self->id) {
            return -1;
        }
        if (t->sid != 0 && t->sid != self->sid) {
            return -1;
        }
        t->sid = self->sid;
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
        return -1;
    }
    if (pgid == 0) {
        pgid = t->id;
    }
    if (pgid != t->id) {
        task_t *leader = sched_task_by_id(pgid);
        if (!leader || leader->sid != t->sid) {
            return -1;
        }
    }
    t->pgid = pgid;
    return 0;
}

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
    task_t *cur = sched_vm_owner(sched_current());
    uint64_t old_brk = cur->heap_brk;
    uint64_t new_brk = old_brk + (uint64_t)increment;
    if (new_brk > USER_HEAP_LIMIT || new_brk < old_brk  ) {
        return -1;
    }
    while (cur->heap_mapped_end < new_brk) {
        uint64_t phys = pmm_try_alloc_frame();
        if (phys == 0) {
            return -1;
        }
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

static long sys_shm_unmap(uint64_t vaddr, uint64_t bytes, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if ((vaddr & (PAGE_SIZE - 1)) != 0 || bytes == 0) {
        return -1;
    }
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t end = vaddr + pages * PAGE_SIZE;
    if (end < vaddr || vaddr < USER_SHM_BASE || end > USER_FB_BASE) {
        return -1;
    }
    uint64_t pml4 = sched_current()->pml4_phys;
    for (uint64_t i = 0; i < pages; i++) {
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
    task_t *cur = sched_vm_owner(sched_current());
    uint64_t vaddr = cur->shm_next_vaddr;
    if (shm_map_into((int)id, cur->pml4_phys, vaddr, VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
        return -1;
    }
    uint64_t pages = ((uint64_t)size + PAGE_SIZE - 1) / PAGE_SIZE;
    cur->shm_next_vaddr += pages * PAGE_SIZE;
    return (long)vaddr;
}

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
            return -1;
        }
        uint64_t last = vaddr + (uint64_t)pages * PAGE_SIZE;
        if (vaddr < USER_REGION_BASE || last > USER_REGION_LIMIT || last < vaddr) {
            return -1;
        }
        uint64_t pml4 = sched_current()->pml4_phys;
        for (int64_t i = 0; i < pages; i++) {
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

static long sys_fb_map(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_FRAMEBUFFER)) {
        return (long)(uint64_t)-1;
    }
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *cur = sched_current();
    uint64_t phys_base = fb_phys_addr();
    uint64_t size = fb_mapped_bytes();
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        if (vmm_try_map_page_in(cur->pml4_phys, USER_FB_BASE + i * PAGE_SIZE,
                                phys_base + i * PAGE_SIZE,
                                VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
            return (long)(uint64_t)-1;
        }
    }
    klog_release_console();
    return (long)USER_FB_BASE;
}

static long sys_mouse_read(uint64_t out_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    mouse_event_t ev;
    if (!user_range_ok(out_ptr, sizeof(ev), 1)) {
        return -1;
    }
    if (!mouse_read(&ev)) {
        return 0;
    }
    return copy_to_user(out_ptr, &ev, sizeof(ev)) == 0 ? 1 : -1;
}

static long sys_pipe_open(uint64_t name_ptr, uint64_t fds_out_ptr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
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
    self->fds[read_fd].cloexec = 0;
    self->fds[read_fd].pipe = p;
    self->fds[write_fd].type = FD_PIPE_WRITE;
    self->fds[write_fd].cloexec = 0;
    self->fds[write_fd].pipe = p;

    out[0] = read_fd;
    out[1] = write_fd;
    return copy_to_user(fds_out_ptr, out, sizeof(out));
}

static long sys_kbd_read(uint64_t out_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
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
    return (long)pipe_buffered(slot->pipe);
}

static long sys_uptime_ms(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return (long)(pit_get_ticks() * (1000 / PIT_HZ));
}

static uint32_t slot_inode(const fd_slot_t *slot) {
    leanfs_stat_t st;
    if (slot->type != FD_FILE || vfs_handle_stat(slot->file->handle, &st) != 0) {
        return 0;
    }
    return st.inode;
}

static void drop_record_locks(task_t *self, const fd_slot_t *slot) {
    if (slot->type != FD_FILE || flock_count() == 0) {
        return;
    }
    uint32_t ino = slot_inode(slot);
    if (ino && flock_release_file(ino, self->id) > 0) {
        sched_wake_all(FLOCK_CHAN);
    }
}

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
        return (long)newfd;
    }
    drop_record_locks(self, &self->fds[newfd]);
    fd_release(&self->fds[newfd]);
    self->fds[newfd] = self->fds[oldfd];
    fd_retain(&self->fds[newfd]);
    return (long)newfd;
}

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
        return -1;
    }
    drop_record_locks(self, &self->fds[fd]);
    fd_release(&self->fds[fd]);
    return 0;
}

static long sys_profile(uint64_t op, uint64_t arg1, uint64_t arg2, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (!has_cap(CAP_PROCESS_LIST)) {
        return -1;
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
        return profile_snapshot((prof_sample_t *)arg1, (int)arg2);
    }
    case PROFILE_OP_SYSCALLS: {
        if (arg2 == 0 || arg2 > SYSCALL_COUNT ||
            !user_range_ok(arg1, arg2 * sizeof(prof_syscount_t), 1)) {
            return -1;
        }
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
    sched_reap_slot(t);
    return code;
}

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

static long sys_kbd_modifiers(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return keyboard_modifiers();
}

static long sys_taskinfo(uint64_t buf, uint64_t max_entries, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_PROCESS_LIST)) {
        return -1;
    }
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (max_entries == 0 || max_entries > TASK_INFO_MAX ||
        !user_range_ok(buf, max_entries * sizeof(task_info_t), 1)) {
        return -1;
    }
    task_info_t *out = (task_info_t *)buf;
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
        return -1;
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
        return -1;
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

static long sys_shutdown(uint64_t mode, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_POWER)) {
        return -1;
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
    if ((flags & (OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE)) && !may_write_path(path)) {
        return -1;
    }
    int create_flags = ((flags & OPEN_CREATE) ? LEANFS_OPEN_CREATE : 0) |
                       ((flags & OPEN_EXCL) ? LEANFS_OPEN_EXCL : 0);
    if ((flags & OPEN_EXCL) && !(flags & OPEN_CREATE)) {
        return -1;
    }
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
    if (flags & (OPEN_CREATE | OPEN_TRUNCATE | OPEN_WRITE)) {
        pkg_note_write(path, 0);
    }
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
        return -1;
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
    if (vfs_stat(path, &st) != 0) {
        return -OS_ERR_NOENT;
    }
    os_stat_t out;
    k_memset(&out, 0, sizeof(out));
    out.size = st.size;
    out.mtime = st.mtime;
    out.is_dir = st.is_dir;
    out.kind = st.is_dir ? OS_STAT_DIR : OS_STAT_FILE;
    out.is_link = 0;
    out.inode = st.inode;
    return copy_to_user(out_ptr, &out, sizeof(out));
}

static void mmap_slot_remove(task_t *t, int index);

static int mmap_mergeable(const mmap_region_t *r, uint32_t prot, int handle,
                          int shared) {
    return r->pages != 0 && r->handle == -1 && handle == -1 &&
           !r->shared && !shared && r->prot == prot;
}

static void region_tag_ref(uint8_t memfd_id, uint16_t memfd_gen) {
    if (!memfd_id) {
        return;
    }
    memfd_region_ref(memfd_by_tag((uint8_t)(memfd_id - 1), memfd_gen));
}

static int mmap_slot_cmp_insert(task_t *t, uint64_t base, uint32_t pages, uint32_t prot,
                                int handle, uint32_t file_page, int shared, int merge,
                                uint8_t memfd_id, uint16_t memfd_gen) {
    uint64_t end = base + (uint64_t)pages * PAGE_SIZE;
    for (int i = 0; merge && i < MAX_MMAP_REGIONS; i++) {
        if (t->mmaps[i].pages == 0) {
            break;
        }
        if (!mmap_mergeable(&t->mmaps[i], prot, handle, shared)) {
            continue;
        }
        uint64_t rstart = t->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)t->mmaps[i].pages * PAGE_SIZE;
        if (rend == base) {
            t->mmaps[i].pages += pages;
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
    t->mmaps[at].prot = prot;
    t->mmaps[at].handle = handle;
    t->mmaps[at].file_page = file_page;
    t->mmaps[at].shared = (uint8_t)(shared != 0);
    t->mmaps[at].memfd_id = memfd_id;
    t->mmaps[at].memfd_gen = memfd_gen;
    return 0;
}

static void mmap_slot_remove(task_t *t, int index) {
    sched_region_forget_memfd(&t->mmaps[index]);
    for (int i = index; i < MAX_MMAP_REGIONS - 1; i++) {
        t->mmaps[i] = t->mmaps[i + 1];
    }
    t->mmaps[MAX_MMAP_REGIONS - 1].base = 0;
    t->mmaps[MAX_MMAP_REGIONS - 1].pages = 0;
    t->mmaps[MAX_MMAP_REGIONS - 1].prot = 0;
    t->mmaps[MAX_MMAP_REGIONS - 1].handle = -1;
    t->mmaps[MAX_MMAP_REGIONS - 1].file_page = 0;
    t->mmaps[MAX_MMAP_REGIONS - 1].memfd_id = 0;
    t->mmaps[MAX_MMAP_REGIONS - 1].memfd_gen = 0;
    t->mmaps[MAX_MMAP_REGIONS - 1].shared = 0;
}

static uint64_t mmap_find_gap(task_t *t, uint32_t pages) {
    uint64_t need = (uint64_t)pages * PAGE_SIZE;
    uint64_t candidate = USER_MMAP_BASE;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (t->mmaps[i].pages == 0) {
            break;
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

static long sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
                      uint64_t fd, uint64_t offset) {
    if (len == 0) {
        return -1;
    }
    int shared = (flags & MAP_SHARED) != 0;
    int private_ = (flags & MAP_PRIVATE) != 0;
    if (shared == private_) {
        return -1;
    }
    int anon = (flags & MAP_ANONYMOUS) != 0;

    int handle = -1;
    uint32_t file_page = 0;
    uint8_t memfd_id = 0;
    uint16_t memfd_gen = 0;
    if (!anon) {
        if ((long)fd < 0 || (uint64_t)fd >= MAX_FDS) {
            return -1;
        }
        if ((offset & (PAGE_SIZE - 1)) != 0) {
            return -1;
        }
        task_t *cur = sched_current();
        if (cur->fds[fd].type == FD_MEMFD) {
            struct memfd *m = cur->fds[fd].memfd;
            if (!shared) {
                return -1;
            }
            uint64_t size = memfd_size(m);
            uint64_t want_end = offset + (uint64_t)len;
            if (size == 0 || want_end < offset || want_end > size) {
                return -1;
            }
            if ((prot & PROT_WRITE) && !memfd_may_write(m)) {
                return -1;
            }
            memfd_region_ref(m);
            memfd_id = (uint8_t)(memfd_slot(m) + 1);
            memfd_gen = memfd_generation(m);
            file_page = (uint32_t)(offset / PAGE_SIZE);
            goto have_backing;
        }
        if (cur->fds[fd].type != FD_FILE || !cur->fds[fd].file) {
            return -1;
        }
        if (shared && (prot & PROT_WRITE) && !cur->fds[fd].file->writable) {
            return -1;
        }
        if (shared && (prot & PROT_WRITE) && !has_cap(CAP_FS_WRITE)) {
            return -1;
        }
        handle = cur->fds[fd].file->handle;
        file_page = (uint32_t)(offset / PAGE_SIZE);
    } else {
        if (offset != 0 || (long)fd >= 0) {
            return -1;
        }
        if (shared) {
            return -1;
        }
    }
have_backing:
    if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC)) {
        return -1;
    }
    uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0 || pages > (USER_MMAP_LIMIT - USER_MMAP_BASE) / PAGE_SIZE) {
        return -1;
    }

    task_t *self = sched_vm_owner(sched_current());
    if (self->pml4_phys == vmm_kernel_pml4_phys()) {
        return -1;
    }

    uint64_t base = 0;
    if (flags & MAP_FIXED) {
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
    if (mmap_slot_cmp_insert(self, base, (uint32_t)pages, (uint32_t)prot,
                             handle, file_page, shared, 1, memfd_id, memfd_gen) != 0) {
        if (memfd_id) {
            memfd_region_unref(memfd_by_tag((uint8_t)(memfd_id - 1), memfd_gen));
        }
        return -1;
    }

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
        return -1;
    }
    if (addr < USER_MMAP_BASE || end > USER_MMAP_LIMIT) {
        return -1;
    }
    task_t *self = sched_vm_owner(sched_current());

    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (self->mmaps[i].pages == 0) {
            break;
        }
        uint64_t rstart = self->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)self->mmaps[i].pages * PAGE_SIZE;
        uint64_t cut_start = addr > rstart ? addr : rstart;
        uint64_t cut_end = end < rend ? end : rend;
        if (cut_start >= cut_end) {
            continue;
        }
        if (self->mmaps[i].shared) {
            sched_release_shared_range(self, cut_start, cut_end);
        } else {
            vmm_unmap_range_free(self->pml4_phys, cut_start, cut_end);
        }
        if (cut_start == rstart && cut_end == rend) {
            mmap_slot_remove(self, i);
            i--;
        } else if (cut_start == rstart) {
            self->mmaps[i].file_page += (uint32_t)((cut_end - rstart) / PAGE_SIZE);
            self->mmaps[i].base = cut_end;
            self->mmaps[i].pages = (uint32_t)((rend - cut_end) / PAGE_SIZE);
        } else if (cut_end == rend) {
            self->mmaps[i].pages = (uint32_t)((cut_start - rstart) / PAGE_SIZE);
        } else {
            self->mmaps[i].pages = (uint32_t)((cut_start - rstart) / PAGE_SIZE);
            region_tag_ref(self->mmaps[i].memfd_id, self->mmaps[i].memfd_gen);
            if (mmap_slot_cmp_insert(self, cut_end,
                                      (uint32_t)((rend - cut_end) / PAGE_SIZE),
                                      self->mmaps[i].prot, self->mmaps[i].handle,
                                      self->mmaps[i].file_page +
                                          (uint32_t)((cut_end - rstart) / PAGE_SIZE),
                                      self->mmaps[i].shared, 0,
                                      self->mmaps[i].memfd_id,
                                      self->mmaps[i].memfd_gen) != 0) {
                sched_region_forget_memfd(&self->mmaps[i]);
                return -1;
            }
            i = -1;
        }
    }
    return 0;
}

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
        return -1;
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
            filemap_sync(self->mmaps[i].handle);
        }
    }
    return 0;
}

static int mmap_split_for(task_t *t, uint64_t addr, uint64_t end) {
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
        uint8_t mid = t->mmaps[i].memfd_id;
        uint16_t mgen = t->mmaps[i].memfd_gen;
        t->mmaps[i].pages = (uint32_t)((cut - rstart) / PAGE_SIZE);
        region_tag_ref(mid, mgen);
        if (mmap_slot_cmp_insert(t, cut, (uint32_t)((rend - cut) / PAGE_SIZE), prot,
                                 handle, fp, shared, 0, mid, mgen) != 0) {
            t->mmaps[i].pages = (uint32_t)((rend - rstart) / PAGE_SIZE);
            if (mid) {
                memfd_region_unref(memfd_by_tag((uint8_t)(mid - 1), mgen));
            }
            return -1;
        }
        i = -1;
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
        return -1;
    }
    task_t *self = sched_vm_owner(sched_current());
    if (self->pml4_phys == vmm_kernel_pml4_phys()) {
        return -1;
    }
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
    vmm_unmap_range_free(self->pml4_phys, addr, end);
    return 0;
}

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
    k_memset(&out, 0, sizeof(out));

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
        out.is_link = 0;
        out.inode = st.inode;
        break;
    }
    case FD_STDIN:
    case FD_STDOUT:
        out.kind = OS_STAT_CHR;
        break;
    case FD_PIPE_READ:
    case FD_PIPE_WRITE:
        out.kind = OS_STAT_FIFO;
        break;
    case FD_SOCKET:
    case FD_UNIX:
        out.kind = OS_STAT_SOCK;
        break;
    case FD_MEMFD:
        out.kind = OS_STAT_FILE;
        out.size = (uint32_t)memfd_size(slot->memfd);
        break;
    case FD_EVENT:
    case FD_TIMER:
    case FD_EPOLL:
        out.kind = OS_STAT_CHR;
        break;
    default:
        return -1;
    }
    return copy_to_user(out_ptr, &out, sizeof(out)) == 0 ? 0 : -1;
}

static long sys_rmdir(uint64_t path_ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(path, path_ptr) != 0) {
        return -1;
    }
    long r = vfs_rmdir(path);
    pkg_note_write(path, r);
    return r;
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
    self->caps &= (uint32_t)keep;
    return (long)self->caps;
}

static struct socket *socket_for_fd(uint64_t fd) {
    task_t *self = sched_current();
    if (fd >= MAX_FDS || self->fds[fd].type != FD_SOCKET) {
        return (struct socket *)0;
    }
    return self->fds[fd].sock;
}

static long install_socket_fd(struct socket *s) {
    task_t *self = sched_current();
    int fd = alloc_fd(self);
    if (fd < 0) {
        socket_unref(s);
        return -1;
    }
    self->fds[fd].type = FD_SOCKET;
    self->fds[fd].cloexec = 0;
    self->fds[fd].sock = s;
    return fd;
}

static long install_unix_fd(struct unixsock *u) {
    task_t *self = sched_current();
    int fd = alloc_fd(self);
    if (fd < 0) {
        unixsock_unref(u);
        return -1;
    }
    self->fds[fd].type = FD_UNIX;
    self->fds[fd].cloexec = 0;
    self->fds[fd].nonblock = 0;
    self->fds[fd].un = u;
    return fd;
}

static struct unixsock *unix_for_fd(uint64_t fd) {
    task_t *self = sched_current();
    if (fd >= MAX_FDS || self->fds[fd].type != FD_UNIX) {
        return (struct unixsock *)0;
    }
    return self->fds[fd].un;
}

static long sys_socket(uint64_t type, uint64_t domain, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (domain == OS_AF_UNIX) {
        if (type != UNIX_SOCK_STREAM && type != UNIX_SOCK_SEQPACKET) {
            return -1;
        }
        struct unixsock *u = unixsock_alloc((int)type);
        return u ? install_unix_fd(u) : -1;
    }
    if (domain != OS_AF_INET) {
        return -1;
    }
    if (!has_cap(CAP_NETWORK)) {
        return -1;
    }
    if (type != SOCK_DGRAM && type != SOCK_STREAM) {
        return -1;
    }
    struct socket *s = socket_alloc((int)type);
    if (!s) {
        return -1;
    }
    return install_socket_fd(s);
}

static long sys_socketpair(uint64_t type, uint64_t fds_ptr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!user_range_ok(fds_ptr, 2 * sizeof(int), 1)) {
        return -1;
    }
    struct unixsock *a = (struct unixsock *)0;
    struct unixsock *b = (struct unixsock *)0;
    if (unixsock_pair((int)type, &a, &b) != 0) {
        return -1;
    }
    task_t *self = sched_current();
    int fa = alloc_fd(self);
    if (fa >= 0) {
        self->fds[fa].type = FD_UNIX;
        self->fds[fa].cloexec = 0;
        self->fds[fa].nonblock = 0;
        self->fds[fa].un = a;
    }
    int fb = fa >= 0 ? alloc_fd(self) : -1;
    if (fb < 0) {
        if (fa >= 0) {
            fd_release(&self->fds[fa]);
        } else {
            unixsock_unref(a);
        }
        unixsock_unref(b);
        return -1;
    }
    self->fds[fb].type = FD_UNIX;
    self->fds[fb].cloexec = 0;
    self->fds[fb].nonblock = 0;
    self->fds[fb].un = b;
    int out[2] = {fa, fb};
    if (copy_to_user(fds_ptr, out, sizeof(out)) != 0) {
        fd_release(&self->fds[fa]);
        fd_release(&self->fds[fb]);
        return -1;
    }
    return 0;
}

static int copy_un_name(char *out, uint64_t src, uint64_t len) {
    if (len == 0 || len > UNIX_PATH_MAX) {
        return -1;
    }
    return copy_from_user(out, src, (size_t)len) == 0 ? 0 : -1;
}

static long sys_bindun(uint64_t fd, uint64_t name_ptr, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    char name[UNIX_PATH_MAX];
    if (copy_un_name(name, name_ptr, len) != 0) {
        return -1;
    }
    struct unixsock *u = unix_for_fd(fd);
    return u ? unixsock_bind(u, name, (int)len) : -1;
}

static long sys_connectun(uint64_t fd, uint64_t name_ptr, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    char name[UNIX_PATH_MAX];
    if (copy_un_name(name, name_ptr, len) != 0) {
        return -1;
    }
    struct unixsock *u = unix_for_fd(fd);
    return u ? unixsock_connect(u, name, (int)len) : -1;
}

#define UNIX_MSG_STAGING UNIX_BUF_SIZE

static long sys_sendmsg(uint64_t fd, uint64_t msg_ptr, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)flags; (void)a4; (void)a5; (void)a6;
    task_t *self = sched_current();
    os_msg_t msg;
    if (copy_from_user(&msg, msg_ptr, sizeof(msg)) != 0) {
        return -1;
    }
    struct unixsock *u = unix_for_fd(fd);
    if (!u) {
        return -1;
    }
    if (msg.nfds > UNIX_MAX_FDS) {
        return -1;
    }
    uint32_t len = msg.len;
    if (len > UNIX_MSG_STAGING) {
        if (unixsock_type(u) == UNIX_SOCK_SEQPACKET) {
            return -1;
        }
        len = UNIX_MSG_STAGING;
    }
    fd_slot_t slots[UNIX_MAX_FDS];
    int nfds = (int)msg.nfds;
    if (nfds > 0) {
        int nums[UNIX_MAX_FDS];
        if (copy_from_user(nums, msg.fds, (size_t)nfds * sizeof(int)) != 0) {
            return -1;
        }
        for (int i = 0; i < nfds; i++) {
            if (nums[i] < 0 || nums[i] >= MAX_FDS ||
                self->fds[nums[i]].type == FD_NONE) {
                return -1;
            }
            slots[i] = self->fds[nums[i]];
        }
    }
    uint8_t staging[UNIX_MSG_STAGING];
    if (len && copy_from_user(staging, msg.data, (size_t)len) != 0) {
        return -1;
    }
    for (;;) {
        uint64_t seq = sched_event_seq();
        long n = unixsock_send(u, staging, len, slots, nfds);
        if (n != 0 || (len == 0 && nfds == 0)) {
            return n;
        }
        if (self->fds[fd].nonblock) {
            return -OS_ERR_AGAIN;
        }
        if (sched_signal_pending()) {
            return -OS_ERR_INTR;
        }
        sched_block_on_seq(SCHED_POLL_CHAN, 0, seq);
    }
}

static long sys_recvmsg(uint64_t fd, uint64_t msg_ptr, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)flags; (void)a4; (void)a5; (void)a6;
    task_t *self = sched_current();
    os_msg_t msg;
    if (copy_from_user(&msg, msg_ptr, sizeof(msg)) != 0) {
        return -1;
    }
    struct unixsock *u = unix_for_fd(fd);
    if (!u) {
        return -1;
    }
    if (msg.nfds > UNIX_MAX_FDS) {
        return -1;
    }
    if (msg.len && !user_range_ok(msg.data, msg.len, 1)) {
        return -1;
    }
    uint32_t want = msg.len > UNIX_MSG_STAGING ? UNIX_MSG_STAGING : msg.len;
    uint8_t staging[UNIX_MSG_STAGING];
    fd_slot_t slots[UNIX_MAX_FDS];
    for (;;) {
        uint64_t seq = sched_event_seq();
        int nfds = 0;
        int rflags = 0;
        long n = unixsock_recv(u, staging, want, slots, (int)msg.nfds, &nfds, &rflags);
        if (n < 0) {
            msg.nfds = 0;
            msg.flags = 0;
            copy_to_user(msg_ptr, &msg, sizeof(msg));
            return 0;
        }
        if (n > 0 || nfds > 0) {
            int nums[UNIX_MAX_FDS];
            int installed = 0;
            for (int i = 0; i < nfds; i++) {
                int nfd = alloc_fd(self);
                if (nfd < 0) {
                    fd_release(&slots[i]);
                    rflags |= OS_MSG_CTRUNC;
                    continue;
                }
                self->fds[nfd] = slots[i];
                self->fds[nfd].cloexec = 0;
                self->fds[nfd].nonblock = 0;
                nums[installed++] = nfd;
            }
            msg.nfds = (uint32_t)installed;
            msg.flags = (uint32_t)rflags;
            int copied =
                (installed == 0 ||
                 copy_to_user(msg.fds, nums, (size_t)installed * sizeof(int)) == 0) &&
                (n == 0 || copy_to_user(msg.data, staging, (size_t)n) == 0) &&
                copy_to_user(msg_ptr, &msg, sizeof(msg)) == 0;
            if (!copied) {
                for (int i = 0; i < installed; i++) {
                    fd_release(&self->fds[nums[i]]);
                }
                return -1;
            }
            return n;
        }
        if (self->fds[fd].nonblock) {
            return -OS_ERR_AGAIN;
        }
        if (sched_signal_pending()) {
            return -OS_ERR_INTR;
        }
        sched_block_on_seq(SCHED_POLL_CHAN, 0, seq);
    }
}

static long sys_memfd_create(uint64_t name_ptr, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (flags & ~(uint64_t)(OS_MFD_CLOEXEC | OS_MFD_ALLOW_SEALING)) {
        return -1;
    }
    char name[MEMFD_NAME_MAX];
    name[0] = '\0';
    if (name_ptr) {
        if (copy_from_user(name, name_ptr, sizeof(name)) != 0) {
            return -1;
        }
        name[sizeof(name) - 1] = '\0';
    }
    struct memfd *m = memfd_create_obj(name_ptr ? name : (const char *)0);
    if (!m) {
        return -1;
    }
    if (!(flags & OS_MFD_ALLOW_SEALING)) {
        memfd_add_seals(m, MEMFD_SEAL_SEAL);
    }
    task_t *self = sched_current();
    int fd = alloc_fd(self);
    if (fd < 0) {
        memfd_unref(m);
        return -1;
    }
    self->fds[fd].type = FD_MEMFD;
    self->fds[fd].memfd = m;
    self->fds[fd].cloexec = (flags & OS_MFD_CLOEXEC) ? 1 : 0;
    self->fds[fd].nonblock = 0;
    return fd;
}

static long sys_memfd_seal(uint64_t fd, uint64_t add, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *self = sched_current();
    if (fd >= MAX_FDS || self->fds[fd].type != FD_MEMFD) {
        return -1;
    }
    struct memfd *m = self->fds[fd].memfd;
    if (add != 0 && memfd_add_seals(m, (uint32_t)add) != 0) {
        return -1;
    }
    return (long)memfd_get_seals(m);
}

static long sys_sockshut(uint64_t fd, uint64_t how, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct unixsock *u = unix_for_fd(fd);
    if (!u) {
        return -1;
    }
    return unixsock_shutdown(u, (int)how);
}

static long sys_listen(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct unixsock *u = unix_for_fd(fd);
    if (u) {
        return unixsock_listen(u);
    }
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
    return tcp_state(tcb) == TCP_ESTABLISHED ? 1 : -1;
}

static long sys_accept(uint64_t fd, uint64_t from_ptr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct unixsock *ulistener = unix_for_fd(fd);
    if (ulistener) {
        struct unixsock *uconn = unixsock_accept(ulistener);
        if (!uconn) {
            return -1;
        }
        if (from_ptr) {
            os_sockaddr_t none = {0, 0, 0};
            if (copy_to_user(from_ptr, &none, sizeof(none)) != 0) {
                unixsock_unref(uconn);
                return -1;
            }
        }
        return install_unix_fd(uconn);
    }
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
        return n;
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
        return -1;
    }
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (seconds > 0xFFFFFFFFu) {
        return -1;
    }
    return rtc_set_unix((uint32_t)seconds);
}

static int audio_owner = -1;

static int audio_owner_is_caller(void) {
    task_t *self = sched_current();
    if (audio_owner < 0) {
        return 0;
    }
    if (audio_owner == self->id) {
        return 1;
    }
    task_t *owner = sched_task_by_id(audio_owner);
    if (!owner || owner->state == TASK_TERMINATED) {
        audio_owner = -1;
    }
    return 0;
}

static long sys_audio_claim(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_AUDIO)) {
        return -1;
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
    if (!user_range_ok(buf, frames * 4, 0)) {
        return -1;
    }
    return ac97_play((const int16_t *)buf, (uint32_t)frames);
}

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
        return -1;
    }
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    uint32_t pitch = 0;
    if (dispi_set_mode((uint32_t)width, (uint32_t)height, &pitch) != 0) {
        return -1;
    }
    fb_remap(pitch, (uint32_t)width, (uint32_t)height);
    return 0;
}

static int fd_is_ready(task_t *self, int fd) {
    if (fd < 0 || fd >= MAX_FDS) {
        return 0;
    }
    fd_slot_t *slot = &self->fds[fd];
    switch (slot->type) {
    case FD_STDIN:
        return keyboard_peek() ? 1 : 0;
    case FD_PIPE_READ:
        return (pipe_buffered(slot->pipe) > 0 || pipe_write_closed(slot->pipe)) ? 1 : 0;
    case FD_SOCKET:
        return socket_pending(slot->sock) > 0 ? 1 : 0;
    case FD_UNIX:
        return unixsock_pending(slot->un);
    case FD_EVENT:
        return eventfd_readable(slot->event) ? 1 : 0;
    case FD_TIMER:
        return timerfd_readable(slot->timer, clock_now_ns()) ? 1 : 0;
    case FD_EPOLL:
        return 0;
    case FD_FILE:
        return vfs_handle_readable(slot->file->handle);
    default:
        return 0;
    }
}

static uint32_t fd_epoll_mask_for(task_t *self, int fd, const void *obj) {
    if (fd < 0 || fd >= MAX_FDS) {
        return EPOLL_STALE;
    }
    fd_slot_t *slot = &self->fds[fd];
    if (slot->type == FD_NONE) {
        return EPOLL_STALE;
    }
    if (obj && slot->pipe != (struct pipe *)obj) {
        return EPOLL_STALE;
    }
    uint32_t m = 0;
    if (fd_is_ready(self, fd)) {
        m |= EPOLLIN;
    }
    switch (slot->type) {
    case FD_STDOUT:
    case FD_FILE:
        m |= EPOLLOUT;
        break;
    case FD_STDIN:
        break;
    case FD_PIPE_WRITE:
        if (pipe_writable(slot->pipe)) {
            m |= EPOLLOUT;
        }
        if (pipe_read_closed(slot->pipe)) {
            m |= EPOLLERR;
        }
        break;
    case FD_PIPE_READ:
        if (pipe_write_closed(slot->pipe) && pipe_buffered(slot->pipe) <= 0) {
            m |= EPOLLHUP;
        }
        break;
    case FD_SOCKET: {
        struct tcpcb *tcb = socket_tcb(slot->sock);
        if (!tcb) {
            m |= EPOLLOUT;
            break;
        }
        if (tcp_send_space(tcb) > 0) {
            m |= EPOLLOUT;
        }
        tcp_state_t st = tcp_state(tcb);
        if (st == TCP_CLOSED || st == TCP_TIME_WAIT) {
            m |= EPOLLHUP;
        } else if (st == TCP_CLOSE_WAIT) {
            m |= EPOLLRDHUP;
        }
        break;
    }
    case FD_UNIX:
        if (unixsock_writable(slot->un)) {
            m |= EPOLLOUT;
        }
        if (unixsock_hup(slot->un)) {
            m |= EPOLLHUP;
        } else if (unixsock_rdhup(slot->un)) {
            m |= EPOLLRDHUP;
        }
        break;
    case FD_EVENT:
        if (eventfd_writable(slot->event)) {
            m |= EPOLLOUT;
        }
        break;
    case FD_TIMER:
    case FD_EPOLL:
        break;
    default:
        break;
    }
    return m;
}

static uint32_t epoll_mask_cb(void *ctx, int fd, const void *obj) {
    return fd_epoll_mask_for((task_t *)ctx, fd, obj);
}

static long install_fd_of(fd_type_t type, void *obj, uint64_t flags) {
    task_t *self = sched_current();
    int fd = alloc_fd(self);
    if (fd < 0) {
        switch (type) {
        case FD_EVENT: eventfd_unref((struct eventfd *)obj); break;
        case FD_TIMER: timerfd_unref((struct timerfd *)obj); break;
        case FD_EPOLL: epoll_unref((struct epoll *)obj); break;
        default: break;
        }
        return -1;
    }
    self->fds[fd].type = type;
    self->fds[fd].event = (struct eventfd *)obj;
    self->fds[fd].cloexec = (flags & OS_FD_CLOEXEC) ? 1 : 0;
    self->fds[fd].nonblock = (flags & OS_FD_NONBLOCK) ? 1 : 0;
    return fd;
}

static long sys_eventfd(uint64_t initval, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (flags & ~(uint64_t)(OS_EFD_SEMAPHORE | OS_FD_NONBLOCK | OS_FD_CLOEXEC)) {
        return -1;
    }
    struct eventfd *e = eventfd_create(initval, (flags & OS_EFD_SEMAPHORE) != 0);
    return e ? install_fd_of(FD_EVENT, e, flags) : -1;
}

static long sys_timerfd_create(uint64_t clockid, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (flags & ~(uint64_t)(OS_FD_NONBLOCK | OS_FD_CLOEXEC)) {
        return -1;
    }
    struct timerfd *t = timerfd_create((int)clockid);
    return t ? install_fd_of(FD_TIMER, t, flags) : -1;
}

static uint64_t realtime_now_ns(void) {
    os_datetime_t now;
    rtc_read(&now);
    if (!now.valid) {
        return 0;
    }
    return (uint64_t)os_unix_time(&now) * 1000000000ULL;
}

static uint64_t timer_clock_ns(struct timerfd *t, int absolute) {
    if (absolute && timerfd_clock(t) == TIMERFD_CLOCK_REALTIME) {
        return realtime_now_ns();
    }
    return clock_now_ns();
}

static long sys_timerfd_settime(uint64_t fd, uint64_t flags, uint64_t new_ptr, uint64_t old_ptr, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    os_itimer_t want;
    if (copy_from_user(&want, new_ptr, sizeof(want)) != 0) {
        return -1;
    }
    task_t *self = sched_current();
    if (fd >= MAX_FDS || self->fds[fd].type != FD_TIMER) {
        return -1;
    }
    if (flags & ~(uint64_t)OS_TFD_ABSTIME) {
        return -1;
    }
    struct timerfd *t = self->fds[fd].timer;
    int absolute = (flags & OS_TFD_ABSTIME) != 0;
    os_itimer_t had = {0, 0};
    if (timerfd_settime(t, timer_clock_ns(t, absolute), absolute, want.value_ns,
                        want.interval_ns, &had.value_ns, &had.interval_ns) != 0) {
        return -1;
    }
    if (old_ptr && copy_to_user(old_ptr, &had, sizeof(had)) != 0) {
        return -1;
    }
    return 0;
}

static long sys_timerfd_gettime(uint64_t fd, uint64_t out_ptr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!user_range_ok(out_ptr, sizeof(os_itimer_t), 1)) {
        return -1;
    }
    task_t *self = sched_current();
    if (fd >= MAX_FDS || self->fds[fd].type != FD_TIMER) {
        return -1;
    }
    os_itimer_t out = {0, 0};
    timerfd_gettime(self->fds[fd].timer, clock_now_ns(), &out.value_ns, &out.interval_ns);
    return copy_to_user(out_ptr, &out, sizeof(out)) == 0 ? 0 : -1;
}

static long sys_epoll_create(uint64_t flags, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (flags & ~(uint64_t)OS_FD_CLOEXEC) {
        return -1;
    }
    struct epoll *ep = epoll_create_set();
    return ep ? install_fd_of(FD_EPOLL, ep, flags) : -1;
}

static long sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t ev_ptr, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    task_t *self = sched_current();
    os_epoll_event_t ev = {0, 0, 0};
    if (op != EPOLL_CTL_DEL && copy_from_user(&ev, ev_ptr, sizeof(ev)) != 0) {
        return -1;
    }
    if (epfd >= MAX_FDS || self->fds[epfd].type != FD_EPOLL) {
        return -1;
    }
    if (fd >= MAX_FDS || self->fds[fd].type == FD_NONE) {
        return -1;
    }
    if (self->fds[fd].type == FD_EPOLL) {
        return -1;
    }
    const void *obj = (const void *)self->fds[fd].pipe;
    return epoll_ctl_set(self->fds[epfd].epoll, (int)op, (int)fd, obj,
                         ev.events, ev.data);
}

static long sys_epoll_wait(uint64_t epfd, uint64_t out_ptr, uint64_t maxevents, uint64_t timeout_ms, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    task_t *self = sched_current();
    if (maxevents == 0 || maxevents > EPOLL_MAX_WATCH) {
        return -1;
    }
    if (!user_range_ok(out_ptr, maxevents * sizeof(os_epoll_event_t), 1)) {
        return -1;
    }
    if (epfd >= MAX_FDS || self->fds[epfd].type != FD_EPOLL) {
        return -1;
    }
    struct epoll *ep = self->fds[epfd].epoll;
    long timeout = (long)timeout_ms;
    uint64_t now = pit_get_ticks() * (1000 / PIT_HZ);
    uint64_t deadline = (timeout < 0) ? 0 : now + (uint64_t)timeout;
    epoll_ev_t evs[EPOLL_MAX_WATCH];
    for (;;) {
        uint64_t seq = sched_event_seq();
        int n = epoll_scan(ep, epoll_mask_cb, self, evs, (int)maxevents);
        if (n > 0) {
            if (copy_to_user(out_ptr, evs, (size_t)n * sizeof(epoll_ev_t)) != 0) {
                return -1;
            }
            return n;
        }
        if (timeout == 0) {
            return 0;
        }
        if (sched_signal_pending()) {
            return -OS_ERR_INTR;
        }
        now = pit_get_ticks() * (1000 / PIT_HZ);
        if (timeout > 0 && now >= deadline) {
            return 0;
        }
        uint64_t park_until = deadline;
        for (int i = 0; i < MAX_FDS; i++) {
            if (self->fds[i].type != FD_TIMER) {
                continue;
            }
            long ms = timerfd_next_ms(self->fds[i].timer, clock_now_ns());
            if (ms < 0) {
                continue;
            }
            uint64_t when = now + (uint64_t)ms;
            if (park_until == 0 || when < park_until) {
                park_until = when;
            }
        }
        sched_block_on_seq(SCHED_POLL_CHAN, park_until, seq);
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
    if (count > 0 && !user_range_ok(fds_ptr, count * sizeof(int), 0)) {
        return -1;
    }
    const int *fds = (const int *)fds_ptr;
    task_t *self = sched_current();

    long timeout = (long)timeout_ms;
    uint64_t now = pit_get_ticks() * (1000 / PIT_HZ);
    uint64_t deadline = (timeout < 0) ? 0 : now + (uint64_t)timeout;

    for (;;) {
        uint64_t seq = sched_event_seq();
        for (uint64_t i = 0; i < count; i++) {
            if (fd_is_ready(self, fds[i])) {
                return (long)i;
            }
        }
        if (timeout == 0) {
            return -2;
        }
        if (deadline != 0 && pit_get_ticks() * (1000 / PIT_HZ) >= deadline) {
            return -2;
        }

        sched_block_on_seq(SCHED_POLL_CHAN, deadline, seq);
        if (sched_signal_pending()) {
            return -2;
        }
    }
}

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
    return (long)klog_written_total();
}

static long sys_sigaction(uint64_t signo, uint64_t handler, uint64_t restorer,
                           uint64_t flags, uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    if (!SIG_IS_CATCHABLE((int)signo)) {
        return -1;
    }
    task_t *self = sched_current();
    if (handler != SIG_DFL_ADDR && handler != SIG_IGN_ADDR) {
        if (handler < USER_REGION_BASE || handler >= USER_REGION_LIMIT) {
            return -1;
        }
        if (restorer < USER_REGION_BASE || restorer >= USER_REGION_LIMIT) {
            return -1;
        }
        self->sig_restorer = restorer;
    }
    long prev = (long)self->sig_handler[signo];
    self->sig_handler[signo] = handler;
    if (flags & SA_SIGINFO) {
        self->sig_siginfo |= (1u << signo);
    } else {
        self->sig_siginfo &= ~(1u << signo);
    }
    if (handler == SIG_IGN_ADDR || handler == SIG_DFL_ADDR) {
        self->sig_pending &= ~(1u << signo);
    }
    return prev;
}

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
    if (!vfs_is_dir(path)) {
        return -1;
    }
    task_t *self = sched_vm_owner(sched_current());
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
    task_t *self = sched_vm_owner(sched_current());
    const char *cwd = (self->cwd[0] == '/') ? self->cwd : "/";
    uint64_t len = 0;
    while (cwd[len]) {
        len++;
    }
    if (maxlen < len + 1) {
        return -1;
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
    char old_path[LEANFS_MAX_PATH];
    char new_path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(old_path, old_ptr) != 0 ||
        copy_write_path_from_user(new_path, new_ptr) != 0) {
        return -1;
    }
    long r = vfs_rename_replace(old_path, new_path);
    pkg_note_write(old_path, r);
    pkg_note_write(new_path, r);
    return r;
}

static long sys_fork(isr_regs_t *regs) {
    task_t *parent = sched_current();
    if (!parent || parent->pml4_phys == vmm_kernel_pml4_phys()) {
        return -1;
    }

    if (sched_count_sharing_address_space(parent->pml4_phys) > 1) {
        return -1;
    }

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

    task_t *owner = sched_vm_owner(parent);
    if (owner->env_block && owner->env_len && owner->env_count) {
        if (sched_set_env(child, owner->env_block, owner->env_len, owner->env_count) != 0) {
            sched_raise_signal(child, SIGKILL);
            return -1;
        }
    }

    return (long)child->id;
}

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
    case F_GETFL_CMD: {
        long access;
        switch (self->fds[fd].type) {
        case FD_STDIN:      access = OPEN_READ; break;
        case FD_STDOUT:     access = OPEN_WRITE; break;
        case FD_PIPE_READ:  access = OPEN_READ; break;
        case FD_PIPE_WRITE: access = OPEN_WRITE; break;
        case FD_FILE:
            access = OPEN_READ | (self->fds[fd].file->writable ? OPEN_WRITE : 0);
            break;
        case FD_SOCKET:     access = OPEN_READ | OPEN_WRITE; break;
        case FD_UNIX:       access = OPEN_READ | OPEN_WRITE; break;
        case FD_EVENT:      access = OPEN_READ | OPEN_WRITE; break;
        case FD_TIMER:      access = OPEN_READ; break;
        case FD_EPOLL:      access = OPEN_READ; break;
        case FD_MEMFD:      access = OPEN_READ | OPEN_WRITE; break;
        default:            return -1;
        }
        return access | (self->fds[fd].nonblock ? OS_NONBLOCK_BIT : 0);
    }
    case F_SETFL_CMD:
        self->fds[fd].nonblock = (arg & OS_NONBLOCK_BIT) ? 1 : 0;
        return 0;
    case F_GETLK_CMD:
    case F_SETLK_CMD:
    case F_SETLKW_CMD: {
        os_flock_t req;
        if (self->fds[fd].type != FD_FILE ||
            copy_from_user(&req, arg, sizeof(req)) != 0) {
            return -1;
        }
        uint32_t ino = slot_inode(&self->fds[fd]);
        if (ino == 0) {
            return -1;
        }
        int64_t start = req.start;
        int64_t len = req.len;
        if (req.whence == 1) {
            start += (int64_t)self->fds[fd].file->offset;
        } else if (req.whence == 2) {
            start += (int64_t)vfs_handle_size(self->fds[fd].file->handle);
        } else if (req.whence != 0) {
            return -1;
        }
        if (len < 0) {
            start += len;
            len = -len;
        }
        if (start < 0 || (req.type != OS_FLOCK_RD && req.type != OS_FLOCK_WR &&
                          req.type != OS_FLOCK_UNLCK)) {
            return -1;
        }
        if (cmd == F_GETLK_CMD) {
            os_flock_t ans;
            flock_test(ino, self->id, req.type, (uint64_t)start, (uint64_t)len, &ans);
            return copy_to_user(arg, &ans, sizeof(ans)) == 0 ? 0 : -1;
        }
        for (;;) {
            uint64_t seq = sched_event_seq();
            int r = flock_set(ino, self->id, req.type, (uint64_t)start, (uint64_t)len);
            if (r == 0) {
                if (req.type == OS_FLOCK_UNLCK) {
                    sched_wake_all(FLOCK_CHAN);
                }
                return 0;
            }
            if (r != FLOCK_CONFLICT || cmd == F_SETLK_CMD) {
                return r;
            }
            sched_block_on_seq(FLOCK_CHAN, 0, seq);
        }
    }
    default:
        return -1;
    }
}

static int wait_status_of(const task_t *t) {
    if (t->exit_signal) {
        return t->exit_signal & 0x7F;
    }
    return (t->exit_code & 0xFF) << 8;
}

static int stop_status_of(const task_t *t) {
    return ((t->stopped_sig & 0xFF) << 8) | 0x7F;
}

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
    int want_pgid = 0;
    if (want < -1) {
        want_pgid = (int)(-want);
    } else if (want == 0) {
        want_pgid = self->pgid;
    }
    if (status_ptr && !user_range_ok(status_ptr, sizeof(int), 1)) {
        return -1;
    }

    for (;;) {
        int any_children = 0;
        uint64_t seq = sched_event_seq();
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
                task_t *t = sched_task_by_slot(i);
                if (!t || t->parent_id != self->id || t->reaped) {
                    continue;
                }
                if (want_pgid && t->pgid != want_pgid) {
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
            return 0;
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

static long sys_execve(isr_regs_t *regs) {
    task_t *self = sched_current();
    if (!self || self->pml4_phys == vmm_kernel_pml4_phys()) {
        return -1;
    }
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
        size_t len = k_strlen(path) + 1;
        k_memcpy(v.argbuf, path, len);
        v.argv[0] = v.argbuf;
        v.argv[1] = (const char *)0;
        v.argc = 1;
    }

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

    uint64_t old_pml4 = self->pml4_phys;
    uint64_t peak = vmm_rss_peak_pages(old_pml4);
    if (peak > self->max_rss_pages) {
        self->max_rss_pages = peak;
    }
    self->pml4_phys = new_pml4;
    vmm_switch_address_space(new_pml4);
    process_destroy_address_space(old_pml4);

    self->heap_brk = USER_HEAP_START;
    self->heap_mapped_end = USER_HEAP_START;
    self->shm_next_vaddr = USER_SHM_BASE;
    sched_regions_forget_memfds(self);
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        self->mmaps[i].base = 0;
        self->mmaps[i].pages = 0;
        self->mmaps[i].prot = 0;
        self->mmaps[i].handle = -1;
        self->mmaps[i].file_page = 0;
        self->mmaps[i].shared = 0;
        self->mmaps[i].memfd_id = 0;
        self->mmaps[i].memfd_gen = 0;
    }
    self->fs_base = 0;

    for (int i = 0; i < MAX_FDS; i++) {
        if (self->fds[i].cloexec) {
            drop_record_locks(self, &self->fds[i]);
            fd_release(&self->fds[i]);
            self->fds[i].type = FD_NONE;
        }
    }

    for (int i = 0; i <= SIG_MAX; i++) {
        if (self->sig_handler[i] != SIG_IGN_ADDR) {
            self->sig_handler[i] = SIG_DFL_ADDR;
        }
    }
    self->sig_restorer = 0;
    self->sig_siginfo = 0;
    self->si_pid = 0;
    self->si_status = 0;
    self->si_addr = 0;

    const char *base = path;
    for (const char *c = path; *c; c++) {
        if (*c == '/') {
            base = c + 1;
        }
    }
    sched_set_task_name(self, base);

    self->caps &= caps_for_spawn_path(path);

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

    uint64_t cs = regs->cs;
    uint64_t ss = regs->ss;
    k_memset(regs, 0, sizeof(*regs));
    regs->rip = entry;
    regs->rsp = USER_STACK_TOP;
    regs->rdi = USER_ARG_ADDR;
    regs->cs = cs;
    regs->ss = ss;
    regs->rflags = 0x202;
    regs->vector = 0x80;
    return 0;
}

static long sys_getrandom(uint64_t buf, uint64_t len, uint64_t flags,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (flags & ~(uint64_t)(GRND_NONBLOCK_BIT | GRND_RANDOM_BIT)) {
        return -1;
    }
    if (!user_range_ok(buf, len, 1)) {
        return -1;
    }
    uint8_t chunk[256];
    uint64_t done = 0;
    while (done < len) {
        size_t n = len - done > sizeof(chunk) ? sizeof(chunk) : (size_t)(len - done);
        random_bytes(chunk, n);
        if (copy_to_user(buf + done, chunk, n) != 0) {
            return -1;
        }
        done += n;
    }
    k_memset(chunk, 0, sizeof(chunk));
    return (long)done;
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
    [SYS_getrandom] = sys_getrandom,
    [SYS_pread] = sys_pread,
    [SYS_pwrite] = sys_pwrite,
    [SYS_socketpair] = sys_socketpair,
    [SYS_bindun] = sys_bindun,
    [SYS_connectun] = sys_connectun,
    [SYS_sendmsg] = sys_sendmsg,
    [SYS_recvmsg] = sys_recvmsg,
    [SYS_sockshut] = sys_sockshut,
    [SYS_epoll_create] = sys_epoll_create,
    [SYS_epoll_ctl] = sys_epoll_ctl,
    [SYS_epoll_wait] = sys_epoll_wait,
    [SYS_eventfd] = sys_eventfd,
    [SYS_timerfd_create] = sys_timerfd_create,
    [SYS_timerfd_settime] = sys_timerfd_settime,
    [SYS_timerfd_gettime] = sys_timerfd_gettime,
    [SYS_memfd_create] = sys_memfd_create,
    [SYS_memfd_seal] = sys_memfd_seal,
};

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

static long sys_sigreturn(uint64_t frame_ptr, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)frame_ptr;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return -1;
}

static int signal_deliver(isr_regs_t *regs) {
    task_t *self = sched_current();
    uint32_t ready = self->sig_pending & ~self->sig_blocked;
    if (ready == 0) {
        return 0;
    }
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
        self->sig_pending &= ~(1u << signo);
        return 0;
    }
    if (self->sig_restorer == 0) {
        self->sig_pending &= ~(1u << signo);
        return 0;
    }

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
    self->sig_blocked |= (1u << signo);

    regs->rsp = new_rsp;
    regs->rip = handler;
    regs->rdi = (uint64_t)signo;
    regs->rsi = want_info ? info_addr : 0;
    regs->rdx = 0;
    regs->rax = 0;
    return 1;
}

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
        return 0;
    }
    if (self->sig_blocked & (1u << signo)) {
        return 0;
    }
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
    regs->rflags = (frame.rflags & USER_RFLAGS_MASK) | RFLAGS_IF;
    self->sig_blocked = frame.saved_blocked & ~(1u << SIGKILL) & ~(1u << SIGSEGV);
    return 0;
}

static void syscall_dispatch(isr_regs_t *regs);

void syscall_handler(isr_regs_t *regs) {
    uint64_t num = regs->rax;
    int timing = syscount_timing_enabled();
    uint64_t started = timing ? tsc_read() : 0;

    syscall_dispatch(regs);

    syscount_record((int)num, timing ? (tsc_read() - started) : 0);
}

static void syscall_dispatch(isr_regs_t *regs) {
    task_t *self = sched_current();
    if (self->pending_signal != 0) {
        int sig = self->pending_signal;
        self->pending_signal = 0;
        task_exit_with_code(128 + sig);
    }
    sched_take_pending_stop_if_any(self);

    uint64_t num = regs->rax;
    if (num >= SYSCALL_COUNT) {
        regs->rax = (uint64_t)-1;
        return;
    }

    if (num == SYS_fork) {
        regs->rax = (uint64_t)sys_fork(regs);
        signal_deliver(regs);
        return;
    }

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
    signal_deliver(regs);
}
