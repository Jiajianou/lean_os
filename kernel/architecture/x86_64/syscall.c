#include "syscall_entry.h"

#include <stdint.h>

#include "architecture/x86_64/cpu.h"
#include "architecture/x86_64/symmetric_multiprocessing.h"
#include "architecture/x86_64/timestamp_counter.h"
#include "drivers/ac97.h"
#include "drivers/dispi.h"
#include "drivers/pc_speaker.h"
#include "drivers/framebuffer.h"
#include "drivers/keyboard.h"
#include "drivers/kernel_log.h"
#include "drivers/mouse.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "library/kernel_library.h"
#include "file_system/leanfs.h"
#include "device/random.h"
#include "file_system/flock.h"
#include "file_system/open_file.h"
#include "file_system/virtual_file_system.h"
#include "inter_process_communication/clipboard.h"
#include "inter_process_communication/pipe.h"
#include "inter_process_communication/shared_memory.h"
#include "inter_process_communication/unix_socket.h"
#include "inter_process_communication/eventfd.h"
#include "inter_process_communication/timerfd.h"
#include "inter_process_communication/epoll.h"
#include "inter_process_communication/memfd.h"
#include "os_poll.h"
#include "drivers/block_device.h"
#include "memory_management/file_mapping.h"
#include "memory_management/heap.h"
#include "memory_management/physical_memory.h"
#include "memory_management/virtual_memory.h"
#include "power/power.h"
#include "os_file_system.h"
#include "process.h"
#include "process/process.h"
#include "process/package_capabilities.h"
#include "process/elf.h"
#include "profile/sampler.h"
#include "profile/syscall_counters.h"
#include "scheduler/scheduler.h"
#include "signal.h"
#include "spawn_error.h"
#include "syscall.h"
#include "display.h"
#include "os_time.h"
#include "os_network.h"
#include "device/tty.h"
#include "mman.h"
#include "capabilities.h"
#include "network/network.h"
#include "network/socket.h"
#include "network/tcp.h"
#include "window_manager.h"

typedef long (*syscall_function_t)(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6);

static int user_range_ok(uint64_t address, uint64_t length, int need_write) {
    if (address == 0) {
        return 0;
    }
    if (scheduler_current()->pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return 1;
    }
    if (length == 0) {
        return 1;
    }
    if (address < USER_REGION_BASE || address >= USER_REGION_LIMIT) {
        return 0;
    }
    uint64_t end = address + length;
    if (end < address || end > USER_REGION_LIMIT) {
        return 0;
    }
    scheduler_prefault_range(address, length, need_write);
    return virtual_memory_user_range_ok(scheduler_current()->pml4_phys, address, length, need_write);
}

static int copy_to_user(uint64_t destination, const void *source, uint64_t length) {
    if (!user_range_ok(destination, length, 1)) {
        return -1;
    }
    const uint8_t *s = (const uint8_t *)source;
    uint8_t *d = (uint8_t *)destination;
    for (uint64_t i = 0; i < length; i++) {
        d[i] = s[i];
    }
    return 0;
}

static int copy_from_user(void *destination, uint64_t source, uint64_t length) {
    if (!user_range_ok(source, length, 0)) {
        return -1;
    }
    const uint8_t *s = (const uint8_t *)source;
    uint8_t *d = (uint8_t *)destination;
    for (uint64_t i = 0; i < length; i++) {
        d[i] = s[i];
    }
    return 0;
}

static int copy_string_from_user(char *destination, uint64_t source, uint64_t max) {
    uint64_t checked_to = 0;
    for (uint64_t i = 0; i < max; i++) {
        uint64_t at = source + i;
        if (at >= checked_to) {
            uint64_t page = at & ~(PAGE_SIZE - 1);
            if (!user_range_ok(page, PAGE_SIZE, 0)) {
                return -1;
            }
            checked_to = page + PAGE_SIZE;
        }
        destination[i] = *(const char *)at;
        if (destination[i] == '\0') {
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
        int c_length = i - c_start;

        if (c_length == 1 && in[c_start] == '.') {
            continue;
        }
        if (c_length == 2 && in[c_start] == '.' && in[c_start + 1] == '.') {
            if (depth > 0) {
                n = starts[--depth];
                if (n > 1) {
                    n--;
                }
            }
            continue;
        }
        if (c_length > LEANFS_MAX_NAME) {
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
        if (n + c_length >= LEANFS_MAX_PATH) {
            return -1;
        }
        for (int k = 0; k < c_length; k++) {
            out[n++] = in[c_start + k];
        }
    }
    out[n] = '\0';
    return 0;
}

static int copy_path_from_user(char *out, uint64_t source) {
    char raw[LEANFS_MAX_PATH];
    if (copy_string_from_user(raw, source, sizeof(raw)) != 0) {
        return -1;
    }
    if (raw[0] == '/') {
        return path_normalize(out, raw);
    }
    char joined[LEANFS_MAX_PATH];
    task_t *self = scheduler_vm_owner(scheduler_current());
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
    /* The set belongs to the PROCESS, not to the thread asking. M146 drew
       this line for descriptors and M165 drew it for mappings; capabilities
       were the third thing still answering per task, and a process that
       reduced its authority on one thread kept all of it on every other -
       which is a claim rather than a boundary. A renderer dropping what it
       may do is exactly the caller that would have found that out. */
    task_t *self = scheduler_vm_owner(scheduler_current());
    if (self->caps & cap) {
        return 1;
    }
    int slot = PID_SLOT(self->id);
    if (slot >= 0 && slot < MAX_TASKS && !(cap_denials_logged[slot] & cap)) {
        cap_denials_logged[slot] |= cap;
        kernel_log_puts("[caps] ");
        kernel_log_puts(self->name);
        kernel_log_puts(" was refused '");
        for (int i = 0; i < CAP_NAME_COUNT; i++) {
            if (CAP_NAMES[i].bit == cap) {
                kernel_log_puts(CAP_NAMES[i].name);
                break;
            }
        }
        kernel_log_puts("' - see system_api/include/capabilities.h\n");
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

static int copy_write_path_from_user(char *out, uint64_t source) {
    if (copy_path_from_user(out, source) != 0) {
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

static long sys_write(uint64_t fd, uint64_t buffer, uint64_t length, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FILE_DESCRIPTORS || !user_range_ok(buffer, length, 0)) {
        return -1;
    }
    file_descriptor_slot_t *slot = &scheduler_current()->descriptor_table->slots[fd];
    const char *s = (const char *)buffer;
    if (slot->type == FILE_DESCRIPTOR_STDOUT) {
        for (uint64_t i = 0; i < length; i++) {
            kernel_log_putc(s[i]);
        }
        return (long)length;
    }
    if (slot->type == FILE_DESCRIPTOR_PIPE_WRITE) {
        return pipe_write(slot->pipe, s, (size_t)length, slot->nonblock);
    }
    if (slot->type == FILE_DESCRIPTOR_EVENT) {
        if (length < sizeof(uint64_t)) {
            return -1;
        }
        uint64_t v = 0;
        if (copy_from_user(&v, buffer, sizeof(v)) != 0) {
            return -1;
        }
        for (;;) {
            uint64_t seq = scheduler_event_sequence();
            int rc = eventfd_write(slot->event, v);
            if (rc == 0) {
                return (long)sizeof(v);
            }
            if (rc == -2) {
                return -1;
            }
            if (slot->nonblock) {
                return -OS_ERROR_AGAIN;
            }
            if (scheduler_signal_pending()) {
                return -OS_ERROR_INTR;
            }
            scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, 0, seq);
        }
    }
    if (slot->type == FILE_DESCRIPTOR_UNIX) {
        uint64_t sent = 0;
        while (sent < length) {
            uint32_t chunk = (length - sent) > UNIX_BUFFER_SIZE ? UNIX_BUFFER_SIZE : (uint32_t)(length - sent);
            uint8_t staging[UNIX_BUFFER_SIZE];
            if (copy_from_user(staging, buffer + sent, chunk) != 0) {
                return sent ? (long)sent : -1;
            }
            uint64_t seq = scheduler_event_sequence();
            long m = unix_socket_send(slot->un, staging, chunk, (const file_descriptor_slot_t *)0, 0);
            if (m < 0) {
                return sent ? (long)sent : -1;
            }
            if (m == 0) {
                if (slot->nonblock) {
                    return sent ? (long)sent : -OS_ERROR_AGAIN;
                }
                if (scheduler_signal_pending()) {
                    return sent ? (long)sent : -OS_ERROR_INTR;
                }
                scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, 0, seq);
                continue;
            }
            sent += (uint64_t)m;
        }
        return (long)sent;
    }
    if (slot->type == FILE_DESCRIPTOR_SOCKET) {
        struct tcpcb *tcb = socket_tcb(slot->sock);
        if (!tcb) {
            return -1;
        }
        uint64_t sent = 0;
        while (sent < length) {
            uint16_t chunk = (length - sent) > TCP_MAX_MSS ? TCP_MAX_MSS : (uint16_t)(length - sent);
            uint8_t staging[TCP_MAX_MSS];
            if (copy_from_user(staging, buffer + sent, chunk) != 0) {
                return sent ? (long)sent : -1;
            }
            uint64_t seq = scheduler_event_sequence();
            net_lock_acquire();
            int m = tcp_send(tcb, staging, chunk);
            net_lock_release();
            if (m < 0) {
                return sent ? (long)sent : -1;
            }
            if (m == 0) {
                if (slot->nonblock) {
                    return sent ? (long)sent : -OS_ERROR_AGAIN;
                }
                if (scheduler_signal_pending()) {
                    return sent ? (long)sent : -OS_ERROR_INTR;
                }
                scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, pit_get_ticks() * (1000 / PIT_HZ) + 10, seq);
                continue;
            }
            sent += (uint64_t)m;
        }
        return (long)sent;
    }
    if (slot->type == FILE_DESCRIPTOR_FILE) {
        if (!slot->file->writable) {
            return -1;
        }
        int64_t n = virtual_file_system_handle_write(slot->file->handle, s, (size_t)length, slot->file->offset);
        if (n < 0) {
            return -1;
        }
        slot->file->offset += (uint32_t)n;
        return (long)n;
    }
    return -1;
}

static long sys_read(uint64_t fd, uint64_t buffer, uint64_t length, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FILE_DESCRIPTORS || !user_range_ok(buffer, length, 1)) {
        return -1;
    }
    file_descriptor_slot_t *slot = &scheduler_current()->descriptor_table->slots[fd];
    char *destination = (char *)buffer;
    if (slot->type == FILE_DESCRIPTOR_STDIN) {
        uint64_t n = 0;
        while (n < length) {
            uint64_t seq = scheduler_event_sequence();
            int c = keyboard_read();
            if (c == -1) {
                if (n > 0) {
                    break;
                }
                scheduler_block_on_sequence(SCHEDULER_KEYBOARD_CHAN, 0, seq);
                if (scheduler_signal_pending()) {
                    return n ? (long)n : -OS_ERROR_INTR;
                }
                continue;
            }
            destination[n++] = (char)c;
        }
        return (long)n;
    }
    if (slot->type == FILE_DESCRIPTOR_PIPE_READ) {
        return pipe_read(slot->pipe, destination, (size_t)length, slot->nonblock);
    }
    if (slot->type == FILE_DESCRIPTOR_EVENT || slot->type == FILE_DESCRIPTOR_TIMER) {
        if (length < sizeof(uint64_t)) {
            return -1;
        }
        for (;;) {
            uint64_t seq = scheduler_event_sequence();
            uint64_t value = 0;
            int got = (slot->type == FILE_DESCRIPTOR_EVENT)
                          ? eventfd_read(slot->event, &value)
                          : timerfd_read(slot->timer, clock_now_ns(), &value);
            if (got == 0) {
                return copy_to_user(buffer, &value, sizeof(value)) == 0
                           ? (long)sizeof(value)
                           : -1;
            }
            if (slot->nonblock) {
                return -OS_ERROR_AGAIN;
            }
            if (scheduler_signal_pending()) {
                return -OS_ERROR_INTR;
            }
            uint64_t deadline = 0;
            if (slot->type == FILE_DESCRIPTOR_TIMER) {
                long ms = timerfd_next_ms(slot->timer, clock_now_ns());
                if (ms < 0) {
                    deadline = 0;
                } else {
                    deadline = pit_get_ticks() * (1000 / PIT_HZ) + (uint64_t)ms;
                }
            }
            scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, deadline, seq);
        }
    }
    if (slot->type == FILE_DESCRIPTOR_UNIX) {
        uint32_t want = length > UNIX_BUFFER_SIZE ? UNIX_BUFFER_SIZE : (uint32_t)length;
        uint8_t staging[UNIX_BUFFER_SIZE];
        for (;;) {
            uint64_t seq = scheduler_event_sequence();
            long n = unix_socket_receive(slot->un, staging, want, (file_descriptor_slot_t *)0, 0,
                                   (int *)0, (int *)0);
            if (n > 0) {
                return copy_to_user(buffer, staging, (size_t)n) == 0 ? n : -1;
            }
            if (n < 0) {
                return 0;
            }
            if (slot->nonblock) {
                return -OS_ERROR_AGAIN;
            }
            if (scheduler_signal_pending()) {
                return -OS_ERROR_INTR;
            }
            scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, 0, seq);
        }
    }
    if (slot->type == FILE_DESCRIPTOR_SOCKET) {
        struct tcpcb *tcb = socket_tcb(slot->sock);
        if (!tcb) {
            return -1;
        }
        uint16_t want = length > TCP_MAX_MSS ? TCP_MAX_MSS : (uint16_t)length;
        uint8_t staging[TCP_MAX_MSS];
        for (;;) {
            uint64_t seq = scheduler_event_sequence();
            net_lock_acquire();
            int n = tcp_receive(tcb, staging, want);
            net_lock_release();
            if (n > 0) {
                return copy_to_user(buffer, staging, (size_t)n) == 0 ? n : -1;
            }
            if (n < 0) {
                return 0;
            }
            if (slot->nonblock) {
                return -OS_ERROR_AGAIN;
            }
            if (scheduler_signal_pending()) {
                return -OS_ERROR_INTR;
            }
            scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, 0, seq);
        }
    }
    if (slot->type == FILE_DESCRIPTOR_FILE) {
        if (slot->file->is_directory) {
            return -1;
        }
        while (!virtual_file_system_handle_readable(slot->file->handle)) {
            uint64_t seq = scheduler_event_sequence();
            if (virtual_file_system_handle_readable(slot->file->handle)) {
                break;
            }
            scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, 0, seq);
            if (scheduler_signal_pending()) {
                return -OS_ERROR_INTR;
            }
        }
        int64_t n = virtual_file_system_handle_read(slot->file->handle, destination, (size_t)length, slot->file->offset);
        if (n < 0) {
            return -1;
        }
        slot->file->offset += (uint32_t)n;
        return (long)n;
    }
    return -1;
}

static long pfile_slot(uint64_t fd, uint64_t buffer, uint64_t length,
                       int64_t offset, int write, file_descriptor_slot_t **out_slot) {
    if (fd >= MAX_FILE_DESCRIPTORS || !user_range_ok(buffer, length, write ? 0 : 1)) {
        return -1;
    }
    if (offset < 0) {
        return -1;
    }
    file_descriptor_slot_t *slot = &scheduler_current()->descriptor_table->slots[fd];
    if (slot->type != FILE_DESCRIPTOR_FILE) {
        return slot->type == FILE_DESCRIPTOR_NONE ? -1 : -OS_ERROR_SPIPE;
    }
    if (slot->file->is_directory) {
        return -1;
    }
    *out_slot = slot;
    return 0;
}

static long sys_pread(uint64_t fd, uint64_t buffer, uint64_t length, uint64_t offset,
                      uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    file_descriptor_slot_t *slot = NULL;
    long error = pfile_slot(fd, buffer, length, (int64_t)offset, 0, &slot);
    if (error != 0) {
        return error;
    }
    int64_t n = virtual_file_system_handle_read(slot->file->handle, (char *)buffer, (size_t)length,
                                (uint32_t)offset);
    return n < 0 ? -1 : (long)n;
}

static long sys_pwrite(uint64_t fd, uint64_t buffer, uint64_t length, uint64_t offset,
                       uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    file_descriptor_slot_t *slot = NULL;
    long error = pfile_slot(fd, buffer, length, (int64_t)offset, 1, &slot);
    if (error != 0) {
        return error;
    }
    if (!slot->file->writable) {
        return -1;
    }
    int64_t n = virtual_file_system_handle_write(slot->file->handle, (const char *)buffer,
                                 (size_t)length, (uint32_t)offset);
    return n < 0 ? -1 : (long)n;
}

static long sys_exit(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    scheduler_kill_thread_group(scheduler_current());
    task_exit_with_code((int)code);
}

static long sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return scheduler_current()->tgid;
}

static long sys_gettid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return scheduler_current()->id;
}

/* A thread's own name. pthread_setname_np(3) is the spelling every C++
   runtime uses - ANGLE's system_utils_linux.cpp is the caller that made this
   necessary - and the interesting question is where the name should live. A
   name the C library remembers and hands back is a round trip that proves
   nothing: the point of naming a thread is that something ELSE can say which
   thread it is looking at. The scheduler has kept a name per task since the
   beginning, it is what the serial log and /bin/task_manager print, and a
   thread here IS a task, so that is where this puts it.

   The length rule is Linux's rather than this kernel's: a name that does not
   fit is refused rather than truncated, because a program that asks for
   "CompositorTileWorker" and silently gets "CompositorTile" has been told
   something false about its own machine. TASK_NAME_MAX is 24 against Linux's
   16, so every name Linux accepts fits here. */
static long sys_thread_setname(uint64_t name_pointer, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    char name[TASK_NAME_MAX];
    if (copy_string_from_user(name, name_pointer, sizeof(name)) != 0) {
        return -1;
    }
    scheduler_set_task_name(scheduler_current(), name);
    return 0;
}

static long sys_thread_getname(uint64_t out_pointer, uint64_t length,
                               uint64_t a3, uint64_t a4, uint64_t a5,
                               uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (length == 0) {
        return -1;
    }
    char name[TASK_NAME_MAX];
    k_memset(name, 0, sizeof(name));
    const char *current = scheduler_current()->name;
    uint64_t i = 0;
    for (; current[i] && i < TASK_NAME_MAX - 1; i++) {
        name[i] = current[i];
    }
    if (i + 1 > length) {
        return -1;
    }
    uint64_t wanted = length < TASK_NAME_MAX ? length : TASK_NAME_MAX;
    return copy_to_user(out_pointer, name, wanted) == 0 ? 0 : -1;
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
    if (!scheduler_has_free_task_slot()) {
        return -1;
    }
    task_t *self = scheduler_current();
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
    const char *argv[SPAWN_MAX_ARGUMENTS + 1];
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

static int copy_vectors_from_user(const char *path, uint64_t argument_pointer,
                                  uint64_t envp_pointer, int path_is_argv0,
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
        size_t length = k_strlen(path) + 1;
        k_memcpy(v->argbuf, path, length);
        v->argv[v->argc++] = v->argbuf;
        used = length;
    }
    if (argument_pointer) {
        for (int i = 0; v->argc < SPAWN_MAX_ARGUMENTS; i++) {
            if (!user_range_ok(argument_pointer + (uint64_t)i * sizeof(uint64_t), sizeof(uint64_t), 0)) {
                break;
            }
            uint64_t slot = ((const uint64_t *)argument_pointer)[i];
            if (slot == 0) {
                break;
            }
            char *destination = v->argbuf + used;
            size_t room = PAGE_SIZE - used;
            if (room < 2 || copy_string_from_user(destination, slot, room) != 0) {
                break;
            }
            v->argv[v->argc++] = destination;
            used += k_strlen(destination) + 1;
        }
    }
    v->argv[v->argc] = (const char *)0;

    if (envp_pointer) {
        v->envbuf = (char *)kmalloc(USER_ENV_MAX_BYTES);
        if (!v->envbuf) {
            free_vectors(v);
            return -1;
        }
        int envc = 0;
        size_t eused = 0;
        for (int i = 0; envc < USER_ENV_MAX_VARS; i++) {
            if (!user_range_ok(envp_pointer + (uint64_t)i * sizeof(uint64_t), sizeof(uint64_t), 0)) {
                break;
            }
            uint64_t slot = ((const uint64_t *)envp_pointer)[i];
            if (slot == 0) {
                break;
            }
            char *destination = v->envbuf + eused;
            size_t room = USER_ENV_MAX_BYTES - eused;
            if (room < 2 || copy_string_from_user(destination, slot, room) != 0) {
                break;
            }
            v->envv[envc++] = destination;
            eused += k_strlen(destination) + 1;
        }
        v->envv[envc] = (const char *)0;
        v->envp = v->envv;
    }
    return 0;
}

static long sys_spawn(uint64_t path_pointer, uint64_t argument_pointer, uint64_t envp_pointer, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_pointer) != 0) {
        return SPAWN_ERROR_NOT_FOUND;
    }
    char *arg = (char *)kmalloc(USER_ARGUMENT_BYTES);
    if (!arg) {
        return SPAWN_ERROR_NO_MEMORY;
    }
    const char *argv[SPAWN_MAX_ARGUMENTS + 1];
    int argc = 0;
    size_t used = 0;
    {
        size_t length = k_strlen(path) + 1;
        k_memcpy(arg, path, length);
        argv[argc++] = arg;
        used = length;
    }
    if (argument_pointer) {
        for (int i = 0; argc < SPAWN_MAX_ARGUMENTS; i++) {
            uint64_t slot;
            if (!user_range_ok(argument_pointer + (uint64_t)i * sizeof(uint64_t), sizeof(uint64_t), 0)) {
                break;
            }
            slot = ((const uint64_t *)argument_pointer)[i];
            if (slot == 0) {
                break;
            }
            char *destination = arg + used;
            size_t room = USER_ARGUMENT_BYTES - used;
            if (room < 2 || copy_string_from_user(destination, slot, room) != 0) {
                break;
            }
            argv[argc++] = destination;
            used += k_strlen(destination) + 1;
        }
    }
    argv[argc] = (const char *)0;

    char *envbuf = (char *)0;
    const char *envv[USER_ENV_MAX_VARS + 1];
    const char *const *envp = (const char *const *)0;
    if (envp_pointer) {
        envbuf = (char *)kmalloc(USER_ENV_MAX_BYTES);
        if (!envbuf) {
            kfree(arg);
            return SPAWN_ERROR_NO_MEMORY;
        }
        int envc = 0;
        size_t eused = 0;
        for (int i = 0; envc < USER_ENV_MAX_VARS; i++) {
            if (!user_range_ok(envp_pointer + (uint64_t)i * sizeof(uint64_t), sizeof(uint64_t), 0)) {
                break;
            }
            uint64_t slot = ((const uint64_t *)envp_pointer)[i];
            if (slot == 0) {
                break;
            }
            char *destination = envbuf + eused;
            size_t room = USER_ENV_MAX_BYTES - eused;
            if (room < 2 || copy_string_from_user(destination, slot, room) != 0) {
                break;
            }
            envv[envc++] = destination;
            eused += k_strlen(destination) + 1;
        }
        envv[envc] = (const char *)0;
        envp = envv;
    }

    leanfs_stat_t st;
    if (virtual_file_system_stat(path, &st) != 0 || st.is_directory) {
        kfree(arg);
        kfree(envbuf);
        return SPAWN_ERROR_NOT_FOUND;
    }
    uint8_t *image = (uint8_t *)kmalloc(st.size ? st.size : 1);
    if (!image) {
        kfree(arg);
        kfree(envbuf);
        return SPAWN_ERROR_NO_MEMORY;
    }
    int64_t size = virtual_file_system_read(path, image, st.size);
    if (size < 0) {
        kfree(image);
        kfree(arg);
        kfree(envbuf);
        return SPAWN_ERROR_NOT_FOUND;
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
            return SPAWN_ERROR_BAD_IMAGE;
        }

        const char *shifted[SPAWN_MAX_ARGUMENTS + 2];
        int sc = 0;
        shifted[sc++] = interp;
        shifted[sc++] = path;
        for (int a = 1; a < argc && sc < SPAWN_MAX_ARGUMENTS; a++) {
            shifted[sc++] = argv[a];
        }
        shifted[sc] = (const char *)0;

        leanfs_stat_t ist;
        if (virtual_file_system_stat(interp, &ist) != 0 || ist.is_directory) {
            kfree(arg);
            kfree(envbuf);
            return SPAWN_ERROR_NOT_FOUND;
        }
        uint8_t *iimage = (uint8_t *)kmalloc(ist.size ? ist.size : 1);
        if (!iimage) {
            kfree(arg);
            kfree(envbuf);
            return SPAWN_ERROR_NO_MEMORY;
        }
        int64_t isize = virtual_file_system_read(interp, iimage, ist.size);
        if (isize < 2 || (iimage[0] == '#' && iimage[1] == '!') ||
            !elf_validate(iimage, (size_t)isize)) {
            kfree(iimage);
            kfree(arg);
            kfree(envbuf);
            return SPAWN_ERROR_BAD_IMAGE;
        }
        if (!scheduler_has_free_task_slot()) {
            kfree(iimage);
            kfree(arg);
            kfree(envbuf);
            return SPAWN_ERROR_NO_TASK_SLOT;
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
        return it ? (long)it->id : SPAWN_ERROR_NO_MEMORY;
    }

    if (!elf_validate(image, (size_t)size)) {
        kfree(image);
        kfree(arg);
        kfree(envbuf);
        return SPAWN_ERROR_BAD_IMAGE;
    }
    if (!scheduler_has_free_task_slot()) {
        kfree(image);
        kfree(arg);
        kfree(envbuf);
        return SPAWN_ERROR_NO_TASK_SLOT;
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
        return SPAWN_ERROR_NO_MEMORY;
    }
    return t->id;
}

static long sys_wait(uint64_t pid_argument, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = scheduler_current();

    if ((int64_t)pid_argument == -1) {
        for (;;) {
            int any_children = 0;
            uint64_t seq = scheduler_event_sequence();
            int total = scheduler_task_count();
            for (int i = 0; i < total; i++) {
                task_t *t = scheduler_task_by_slot(i);
                if (!t || t->parent_id != self->id || t->reaped) {
                    continue;
                }
                any_children = 1;
                if (t->state == TASK_TERMINATED) {
                    t->reaped = 1;
                    int pid = t->id;
                    scheduler_reap_slot(t);
                    return pid;
                }
            }
            if (!any_children) {
                return -1;
            }
            scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, pit_get_ticks() * (1000 / PIT_HZ) + 50, seq);
        }
    }

    task_t *t = scheduler_task_by_id((int)pid_argument);
    if (!t) {
        return -1;
    }
    while (t->state != TASK_TERMINATED) {
        uint64_t seq = scheduler_event_sequence();
        if (t->state == TASK_TERMINATED) {
            break;
        }
        scheduler_block_on_sequence((const void *)t, pit_get_ticks() * (1000 / PIT_HZ) + 200, seq);
    }
    t->reaped = 1;
    int code = t->exit_code;
    scheduler_reap_slot(t);
    return code;
}

static long sys_readfile(uint64_t name_pointer, uint64_t buffer, uint64_t maxlen, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, name_pointer) != 0 ||
        !user_range_ok(buffer, maxlen, 1)) {
        return -1;
    }
    return (long)virtual_file_system_read(path, (void *)buffer, (size_t)maxlen);
}

static long sys_writefile(uint64_t name_pointer, uint64_t buffer, uint64_t length, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(path, name_pointer) != 0 ||
        !user_range_ok(buffer, length, 0)) {
        return -1;
    }
    long r = virtual_file_system_write(path, (const void *)buffer, (size_t)length);
    pkg_note_write(path, r);
    return r;
}

static long sys_unlink(uint64_t path_pointer, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(path, path_pointer) != 0) {
        return -1;
    }
    long r = virtual_file_system_unlink(path);
    pkg_note_write(path, r);
    return r;
}

static long sys_rename(uint64_t old_pointer, uint64_t new_pointer, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char old_path[LEANFS_MAX_PATH];
    char new_path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(old_path, old_pointer) != 0 ||
        copy_write_path_from_user(new_path, new_pointer) != 0) {
        return -1;
    }
    long r = virtual_file_system_rename(old_path, new_path);
    pkg_note_write(old_path, r);
    pkg_note_write(new_path, r);
    return r;
}

static long sys_listdir(uint64_t path_pointer, uint64_t buffer, uint64_t maxlen, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_pointer) != 0 ||
        !user_range_ok(buffer, maxlen, 1)) {
        return -1;
    }
    if (!virtual_file_system_is_directory(path)) {
        return -1;
    }
    return (long)virtual_file_system_list(path, (char *)buffer, (size_t)maxlen);
}

static long sys_getdents(uint64_t path_pointer, uint64_t cookie_pointer, uint64_t buffer,
                         uint64_t buflen, uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_pointer) != 0 ||
        !user_range_ok(cookie_pointer, sizeof(uint32_t), 1) ||
        !user_range_ok(buffer, buflen, 1)) {
        return -1;
    }
    int directory = virtual_file_system_directory_open(path);

    uint32_t cookie;
    if (copy_from_user(&cookie, cookie_pointer, sizeof(cookie)) != 0) {
        return -1;
    }

    size_t written = 0;
    for (;;) {
        leanfs_directory_entry_t e;
        uint32_t next = cookie;
        int rc = (directory >= 0) ? virtual_file_system_readdir_at(directory, &next, &e)
                            : virtual_file_system_readdir(path, &next, &e);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            break;
        }

        size_t name_length = k_strlen(e.name);
        size_t need = (sizeof(os_dirent_t) + name_length + 1 + 7) & ~(size_t)7;
        if (written + need > buflen) {
            break;
        }

        os_dirent_t rec;
        rec.ino = e.inode;
        rec.reclen = (unsigned short)need;
        rec.type = e.is_link ? OS_DT_LNK : (e.is_directory ? OS_DT_DIRECTORY : OS_DT_REG);
        rec.name_length = (unsigned char)name_length;
        if (copy_to_user(buffer + written, &rec, sizeof(rec)) != 0 ||
            copy_to_user(buffer + written + sizeof(rec), e.name, name_length + 1) != 0) {
            return -1;
        }
        written += need;
        cookie = next;
    }

    if (copy_to_user(cookie_pointer, &cookie, sizeof(cookie)) != 0) {
        return -1;
    }
    return (long)written;
}

static long sys_mkdir(uint64_t path_pointer, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(path, path_pointer) != 0) {
        return -1;
    }
    long r = virtual_file_system_mkdir(path);
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
        up = up->parent_id >= 0 ? scheduler_task_by_id(up->parent_id) : (task_t *)0;
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
        task_t *self_g = scheduler_current();
        int target_pgid = ((long)pid == 0) ? self_g->pgid : (int)(-(long)pid);
        int delivered = 0;
        int total = scheduler_task_count();
        for (int i = 0; i < total; i++) {
            task_t *m = scheduler_task_by_slot(i);
            if (!m || m->pgid != target_pgid || m->state == TASK_TERMINATED ||
                m->state == TASK_FREE) {
                continue;
            }
            if (!may_signal(self_g, m)) {
                continue;
            }
            delivered++;
            if (sig != 0) {
                scheduler_raise_signal(m, (int)sig);
            }
        }
        return delivered > 0 ? 0 : -1;
    }

    task_t *t = scheduler_task_by_id((int)pid);
    if (!t || t->state == TASK_TERMINATED) {
        return -1;
    }
    task_t *self = scheduler_current();
    if (!may_signal(self, t)) {
        return -1;
    }
    if (sig == 0) {
        return 0;
    }
    scheduler_raise_signal(t, (int)sig);
    return 0;
}

static long sys_pipe(uint64_t file_descriptors_out_pointer, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    int out[2];
    if (!user_range_ok(file_descriptors_out_pointer, sizeof(out), 1)) {
        return -1;
    }
    task_t *self = scheduler_current();
    int read_file_descriptor = -1, write_file_descriptor = -1;
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
        if (self->descriptor_table->slots[i].type == FILE_DESCRIPTOR_NONE) {
            if (read_file_descriptor < 0) {
                read_file_descriptor = i;
            } else {
                write_file_descriptor = i;
                break;
            }
        }
    }
    if (read_file_descriptor < 0 || write_file_descriptor < 0) {
        return -1;
    }

    pipe_t *p = pipe_create();
    if (!p) {
        return -1;
    }
    self->descriptor_table->slots[read_file_descriptor].type = FILE_DESCRIPTOR_PIPE_READ;
    self->descriptor_table->slots[read_file_descriptor].cloexec = 0;
    self->descriptor_table->slots[read_file_descriptor].pipe = p;
    self->descriptor_table->slots[write_file_descriptor].type = FILE_DESCRIPTOR_PIPE_WRITE;
    self->descriptor_table->slots[write_file_descriptor].cloexec = 0;
    self->descriptor_table->slots[write_file_descriptor].pipe = p;

    out[0] = read_file_descriptor;
    out[1] = write_file_descriptor;
    return copy_to_user(file_descriptors_out_pointer, out, sizeof(out));
}

static long sys_symlink(uint64_t target_pointer, uint64_t path_pointer, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(path, path_pointer) != 0) {
        return -1;
    }
    char target[LEANFS_MAX_PATH];
    if (copy_string_from_user(target, target_pointer, sizeof(target)) != 0) {
        return -1;
    }
    long r = virtual_file_system_symlink(path, target);
    pkg_note_write(path, r);
    return r;
}

static long sys_link(uint64_t old_pointer, uint64_t new_pointer, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char old_path[LEANFS_MAX_PATH];
    char new_path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(old_path, old_pointer) != 0 ||
        copy_write_path_from_user(new_path, new_pointer) != 0) {
        return -1;
    }
    long r = virtual_file_system_link(old_path, new_path);
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
    if (fd >= MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    if (scheduler_current()->descriptor_table->slots[fd].type != FILE_DESCRIPTOR_FILE) {
        return -1;
    }
    virtual_file_system_sync();
    block_device_flush();
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
    return (long)scheduler_set_alarm(scheduler_current(), (unsigned int)seconds);
}

static long sys_meminfo(uint64_t out_pointer, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    os_meminfo_t info;
    info.total_frames = physical_memory_total_frame_count();
    info.free_frames = physical_memory_free_frame_count();
    info.page_size = PAGE_SIZE;
    if (copy_to_user(out_pointer, &info, sizeof(info)) != 0) {
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
    virtual_file_system_sync();
    return 0;
}

static long sys_arch_prctl(uint64_t code, uint64_t address, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = scheduler_current();
    switch (code) {
    case ARCH_SET_FS:
        if (address != 0 && !user_range_ok(address, 1, 0)) {
            return -1;
        }
        self->fs_base = address;
        cpu_write_msr(MSR_FS_BASE, address);
        return 0;
    case ARCH_GET_FS:
        if (!user_range_ok(address, sizeof(uint64_t), 1)) {
            return -1;
        }
        return copy_to_user(address, &self->fs_base, sizeof(self->fs_base));
    default:
        return -1;
    }
}

static spinlock_t futex_lock;

static long sys_futex(uint64_t address, uint64_t op, uint64_t val,
                      uint64_t timeout_ms, uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    if ((address & 3u) != 0) {
        return -1;
    }
    if (!user_range_ok(address, sizeof(uint32_t), 0)) {
        return -1;
    }
    if (op == FUTEX_WAKE) {
        int max = (val > (uint64_t)(unsigned)MAX_TASKS) ? MAX_TASKS : (int)val;
        return scheduler_wake_n((const void *)address, max);
    }
    if (op != FUTEX_WAIT) {
        return -1;
    }

    uint64_t flags = spin_lock_irqsave(&futex_lock);
    uint32_t seen = *(const volatile uint32_t *)address;
    if (seen != (uint32_t)val) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return -1;
    }
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = pit_get_ticks() * (1000 / PIT_HZ) + timeout_ms;
    }
    scheduler_block_on((const void *)address, deadline, &futex_lock, &flags);
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
    return scheduler_current()->parent_id;
}

static long sys_fdpath(uint64_t fd, uint64_t out_pointer, uint64_t out_length,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    task_t *self = scheduler_current();
    if (self->descriptor_table->slots[fd].type != FILE_DESCRIPTOR_FILE || !self->descriptor_table->slots[fd].file) {
        return -1;
    }
    const char *p = self->descriptor_table->slots[fd].file->path;
    if (p[0] != '/') {
        return -1;
    }
    uint64_t n = 0;
    while (p[n]) {
        n++;
    }
    if (out_length < n + 1) {
        return -1;
    }
    if (copy_to_user(out_pointer, p, n + 1) != 0) {
        return -1;
    }
    return (long)n;
}

static long sys_rusage(uint64_t who, uint64_t out_pointer, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!user_range_ok(out_pointer, sizeof(os_rusage_t), 1)) {
        return -1;
    }
    task_t *self = scheduler_current();
    os_rusage_t r;
    if (who == OS_RUSAGE_SELF) {
        /* POSIX says RUSAGE_SELF is the process, and a thread here is a task
           that accrues its own ticks - so this has to be the thread group's
           sum rather than the caller's own. OS_RUSAGE_THREAD is the question
           this used to answer. */
        scheduler_thread_group_ticks(self, &r.user_ticks, &r.sys_ticks);
        uint64_t peak = virtual_memory_rss_peak_pages(self->pml4_phys);
        r.max_rss_pages = (peak > self->max_rss_pages) ? peak : self->max_rss_pages;
    } else if (who == OS_RUSAGE_THREAD) {
        r.user_ticks = self->user_ticks;
        r.sys_ticks = self->sys_ticks;
        uint64_t peak = virtual_memory_rss_peak_pages(self->pml4_phys);
        r.max_rss_pages = (peak > self->max_rss_pages) ? peak : self->max_rss_pages;
    } else if (who == OS_RUSAGE_CHILDREN) {
        r.user_ticks = self->child_user_ticks;
        r.sys_ticks = self->child_sys_ticks;
        r.max_rss_pages = self->child_max_rss_pages;
    } else {
        return -1;
    }
    *(os_rusage_t *)out_pointer = r;
    return 0;
}

static long sys_statvfs(uint64_t path_pointer, uint64_t out_pointer, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_pointer) != 0 ||
        !user_range_ok(out_pointer, sizeof(os_statvfs_t), 1)) {
        return -1;
    }
    virtual_file_system_statvfs_t st;
    if (virtual_file_system_statvfs(path, &st) != 0) {
        return -1;
    }
    os_statvfs_t *out = (os_statvfs_t *)out_pointer;
    out->block_size = st.block_size;
    out->total_blocks = st.total_blocks;
    out->free_blocks = st.free_blocks;
    out->total_inodes = st.total_inodes;
    out->free_inodes = st.free_inodes;
    out->name_max = st.name_max;
    return 0;
}

static long sys_utime(uint64_t path_pointer, uint64_t mtime, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(path, path_pointer) != 0) {
        return -1;
    }
    return virtual_file_system_utime(path, (uint32_t)mtime);
}

static long sys_readlink(uint64_t path_pointer, uint64_t buffer, uint64_t length,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_pointer) != 0 || !user_range_ok(buffer, length, 1)) {
        return -1;
    }
    return (long)virtual_file_system_readlink(path, (char *)buffer, (size_t)length);
}

static long sys_lstat(uint64_t path_pointer, uint64_t out_pointer, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_pointer) != 0 ||
        !user_range_ok(out_pointer, sizeof(os_stat_t), 1)) {
        return -OS_ERROR_FAULT;
    }
    leanfs_stat_t st;
    if (virtual_file_system_lstat(path, &st) != 0) {
        return -OS_ERROR_NOENT;
    }
    os_stat_t out;
    k_memset(&out, 0, sizeof(out));
    out.size = st.size;
    out.mtime = st.mtime;
    out.is_directory = st.is_directory;
    out.is_link = st.is_link;
    out.kind = st.is_directory ? OS_STAT_DIRECTORY : OS_STAT_FILE;
    out.inode = st.inode;
    return copy_to_user(out_pointer, &out, sizeof(out));
}

static long sys_ftruncate(uint64_t fd, uint64_t length, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = scheduler_current();
    if (fd < MAX_FILE_DESCRIPTORS && self->descriptor_table->slots[fd].type == FILE_DESCRIPTOR_MEMFD) {
        if (!self->descriptor_table->slots[fd].writable) {
            return -1;
        }
        return memfd_truncate(self->descriptor_table->slots[fd].memfd, length);
    }
    if (!has_cap(CAP_FS_WRITE)) {
        return -1;
    }
    if (fd >= MAX_FILE_DESCRIPTORS || self->descriptor_table->slots[fd].type != FILE_DESCRIPTOR_FILE) {
        return -1;
    }
    open_file_t *of = self->descriptor_table->slots[fd].file;
    if (!of || !of->writable) {
        return -1;
    }
    if (length > (uint64_t)LEANFS_MAX_FILE_SIZE) {
        return -1;
    }
    return virtual_file_system_handle_truncate_to(of->handle, (uint32_t)length);
}

static tty_t *tty_for_file_descriptor(task_t *self, uint64_t fd, int *pty_number) {
    *pty_number = -1;
    if (fd >= MAX_FILE_DESCRIPTORS) {
        return NULL;
    }
    file_descriptor_slot_t *slot = &self->descriptor_table->slots[fd];
    if (slot->type == FILE_DESCRIPTOR_STDIN || slot->type == FILE_DESCRIPTOR_STDOUT) {
        return tty_console();
    }
    if (slot->type == FILE_DESCRIPTOR_FILE) {
        return (tty_t *)virtual_file_system_handle_tty(slot->file->handle, pty_number);
    }
    return NULL;
}

static long sys_ioctl(uint64_t fd, uint64_t command, uint64_t arg, uint64_t a4,
                      uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = scheduler_current();

    /* FIONREAD is not a terminal question - it is asked of pipes and sockets
       far more often - so it is answered before the tty lookup rather than
       inside it. The number is the one poll already has to know. */
    if (command == FIONREAD) {
        if (fd >= MAX_FILE_DESCRIPTORS) {
            return -1;
        }
        file_descriptor_slot_t *slot = &self->descriptor_table->slots[fd];
        int bytes = -1;
        switch (slot->type) {
        case FILE_DESCRIPTOR_PIPE_READ:
            bytes = pipe_buffered(slot->pipe);
            break;
        case FILE_DESCRIPTOR_SOCKET:
            bytes = socket_pending(slot->sock);
            break;
        case FILE_DESCRIPTOR_UNIX:
            bytes = unix_socket_readable_bytes(slot->un);
            break;
        case FILE_DESCRIPTOR_FILE: {
            leanfs_stat_t st;
            if (virtual_file_system_handle_stat(slot->file->handle, &st) != 0) {
                return -1;
            }
            uint64_t at = slot->file->offset;
            bytes = st.size > at ? (int)(st.size - at) : 0;
            break;
        }
        default:
            break;
        }
        if (bytes < 0) {
            return -1;
        }
        if (copy_to_user(arg, &bytes, sizeof(bytes)) != 0) {
            return -1;
        }
        return 0;
    }

    int pty_number = -1;
    tty_t *t = tty_for_file_descriptor(self, fd, &pty_number);
    if (!t) {
        return -1;
    }

    switch (command) {
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

static long sys_setpgid(uint64_t pid_argument, uint64_t pgid_argument, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = scheduler_current();
    int pid = (int)pid_argument;
    int pgid = (int)pgid_argument;

    task_t *t = (pid == 0) ? self : scheduler_task_by_id(pid);
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
        task_t *leader = scheduler_task_by_id(pgid);
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
    task_t *self = scheduler_current();
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
    task_t *t = (pid == 0) ? scheduler_current() : scheduler_task_by_id((int)pid);
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
    task_t *t = scheduler_task_by_id((int)pid);
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
    task_t *current = scheduler_vm_owner(scheduler_current());
    uint64_t old_brk = current->heap_brk;
    uint64_t new_brk = old_brk + (uint64_t)increment;
    if (new_brk > USER_HEAP_LIMIT || new_brk < old_brk  ) {
        return -1;
    }
    while (current->heap_mapped_end < new_brk) {
        uint64_t phys = physical_memory_try_alloc_frame();
        if (phys == 0) {
            return -1;
        }
        if (virtual_memory_try_map_page_in(current->pml4_phys, current->heap_mapped_end, phys,
                                VIRTUAL_MEMORY_FLAG_WRITABLE | VIRTUAL_MEMORY_FLAG_USER) != 0) {
            physical_memory_free_frame(phys);
            return -1;
        }
        current->heap_mapped_end += PAGE_SIZE;
    }
    current->heap_brk = new_brk;
    return (long)old_brk;
}

static long sys_shared_memory_create(uint64_t size, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return shared_memory_create((size_t)size, scheduler_current()->id);
}

static long sys_shared_memory_unmap(uint64_t vaddr, uint64_t bytes, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if ((vaddr & (PAGE_SIZE - 1)) != 0 || bytes == 0) {
        return -1;
    }
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t end = vaddr + pages * PAGE_SIZE;
    if (end < vaddr || vaddr < USER_SHARED_MEMORY_BASE || end > USER_FRAMEBUFFER_BASE) {
        return -1;
    }
    uint64_t pml4 = scheduler_current()->pml4_phys;
    for (uint64_t i = 0; i < pages; i++) {
        (void)virtual_memory_unmap_page_in(pml4, vaddr + i * PAGE_SIZE);
    }
    return 0;
}

static long sys_shared_memory_map(uint64_t id, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    int64_t size = shared_memory_get_size((int)id);
    if (size < 0) {
        return -1;
    }
    task_t *current = scheduler_vm_owner(scheduler_current());
    uint64_t vaddr = current->shared_memory_next_vaddr;
    if (shared_memory_map_into((int)id, current->pml4_phys, vaddr, VIRTUAL_MEMORY_FLAG_WRITABLE | VIRTUAL_MEMORY_FLAG_USER) != 0) {
        return -1;
    }
    uint64_t pages = ((uint64_t)size + PAGE_SIZE - 1) / PAGE_SIZE;
    current->shared_memory_next_vaddr += pages * PAGE_SIZE;
    return (long)vaddr;
}

static long sys_shared_memory_free(uint64_t id, uint64_t vaddr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    int64_t pages = shared_memory_page_count((int)id);
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
        uint64_t pml4 = scheduler_current()->pml4_phys;
        for (int64_t i = 0; i < pages; i++) {
            (void)virtual_memory_unmap_page_in(pml4, vaddr + (uint64_t)i * PAGE_SIZE);
        }
    }
    return shared_memory_free((int)id, scheduler_current()->id);
}

static long sys_framebuffer_info(uint64_t out_pointer, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    window_manager_framebuffer_info_t out;
    out.width = framebuffer_width();
    out.height = framebuffer_height();
    out.pitch = framebuffer_pitch_bytes();
    out.bpp = 32;
    return copy_to_user(out_pointer, &out, sizeof(out));
}

static long sys_framebuffer_map(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_FRAMEBUFFER)) {
        return (long)(uint64_t)-1;
    }
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *current = scheduler_current();
    uint64_t phys_base = framebuffer_phys_address();
    uint64_t size = framebuffer_mapped_bytes();
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        if (virtual_memory_try_map_page_in(current->pml4_phys, USER_FRAMEBUFFER_BASE + i * PAGE_SIZE,
                                phys_base + i * PAGE_SIZE,
                                VIRTUAL_MEMORY_FLAG_WRITABLE | VIRTUAL_MEMORY_FLAG_USER) != 0) {
            return (long)(uint64_t)-1;
        }
    }
    kernel_log_release_console();
    return (long)USER_FRAMEBUFFER_BASE;
}

static long sys_mouse_read(uint64_t out_pointer, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    mouse_event_t ev;
    if (!user_range_ok(out_pointer, sizeof(ev), 1)) {
        return -1;
    }
    if (!mouse_read(&ev)) {
        return 0;
    }
    return copy_to_user(out_pointer, &ev, sizeof(ev)) == 0 ? 1 : -1;
}

static long sys_pipe_open(uint64_t name_pointer, uint64_t file_descriptors_out_pointer, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char name[NAMED_PIPE_NAME_LENGTH];
    int out[2];
    if (copy_string_from_user(name, name_pointer, sizeof(name)) != 0 ||
        !user_range_ok(file_descriptors_out_pointer, sizeof(out), 1)) {
        return -1;
    }
    task_t *self = scheduler_current();
    int read_file_descriptor = -1, write_file_descriptor = -1;
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
        if (self->descriptor_table->slots[i].type == FILE_DESCRIPTOR_NONE) {
            if (read_file_descriptor < 0) {
                read_file_descriptor = i;
            } else {
                write_file_descriptor = i;
                break;
            }
        }
    }
    if (read_file_descriptor < 0 || write_file_descriptor < 0) {
        return -1;
    }

    pipe_t *p = pipe_named(name);
    if (!p) {
        return -1;
    }
    self->descriptor_table->slots[read_file_descriptor].type = FILE_DESCRIPTOR_PIPE_READ;
    self->descriptor_table->slots[read_file_descriptor].cloexec = 0;
    self->descriptor_table->slots[read_file_descriptor].pipe = p;
    self->descriptor_table->slots[write_file_descriptor].type = FILE_DESCRIPTOR_PIPE_WRITE;
    self->descriptor_table->slots[write_file_descriptor].cloexec = 0;
    self->descriptor_table->slots[write_file_descriptor].pipe = p;

    out[0] = read_file_descriptor;
    out[1] = write_file_descriptor;
    return copy_to_user(file_descriptors_out_pointer, out, sizeof(out));
}

static long sys_keyboard_read(uint64_t out_pointer, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!user_range_ok(out_pointer, sizeof(char), 1)) {
        return -1;
    }
    int c = keyboard_read();
    if (c == -1) {
        return 0;
    }
    char ch = (char)c;
    return copy_to_user(out_pointer, &ch, sizeof(ch)) == 0 ? 1 : -1;
}

static long sys_pipe_poll(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    file_descriptor_slot_t *slot = &scheduler_current()->descriptor_table->slots[fd];
    if (slot->type != FILE_DESCRIPTOR_PIPE_READ) {
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

static uint32_t slot_inode(const file_descriptor_slot_t *slot) {
    leanfs_stat_t st;
    if (slot->type != FILE_DESCRIPTOR_FILE || virtual_file_system_handle_stat(slot->file->handle, &st) != 0) {
        return 0;
    }
    return st.inode;
}

static void drop_record_locks(task_t *self, const file_descriptor_slot_t *slot) {
    if (slot->type != FILE_DESCRIPTOR_FILE || flock_count() == 0) {
        return;
    }
    uint32_t ino = slot_inode(slot);
    if (ino && flock_release_file(ino, self->id) > 0) {
        scheduler_wake_all(FLOCK_CHAN);
    }
}

static long sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (oldfd >= MAX_FILE_DESCRIPTORS || newfd >= MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    task_t *self = scheduler_current();
    if (self->descriptor_table->slots[oldfd].type == FILE_DESCRIPTOR_NONE) {
        return -1;
    }
    if (newfd == oldfd) {
        return (long)newfd;
    }
    drop_record_locks(self, &self->descriptor_table->slots[newfd]);
    file_descriptor_release(&self->descriptor_table->slots[newfd]);
    self->descriptor_table->slots[newfd] = self->descriptor_table->slots[oldfd];
    file_descriptor_retain(&self->descriptor_table->slots[newfd]);
    return (long)newfd;
}

static long sys_close(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    task_t *self = scheduler_current();
    if (self->descriptor_table->slots[fd].type == FILE_DESCRIPTOR_NONE) {
        return -1;
    }
    drop_record_locks(self, &self->descriptor_table->slots[fd]);
    file_descriptor_release(&self->descriptor_table->slots[fd]);
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
    case PROFILE_OP_STATISTICS: {
        prof_statistics_t st;
        profile_get_statistics(&st);
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
            !user_range_ok(arg1, arg2 * sizeof(prof_syscall_counters_t), 1)) {
            return -1;
        }
        prof_syscall_counters_t *out = (prof_syscall_counters_t *)arg1;
        for (uint64_t i = 0; i < arg2; i++) {
            syscall_counters_entry_t e;
            syscall_counters_get((int)i, &e);
            out[i].calls = e.calls;
            out[i].cycles = e.cycles;
        }
        return (long)arg2;
    }
    case PROFILE_OP_SYSRESET:
        syscall_counters_reset();
        return 0;
    case PROFILE_OP_TIMING:
        return syscall_counters_set_timing(arg1 ? 1 : 0);
    default:
        return -1;
    }
}

static long sys_wait_nb(uint64_t pid_argument, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *t = scheduler_task_by_id((int)pid_argument);
    if (!t) {
        return -1;
    }
    if (t->state != TASK_TERMINATED) {
        return -2;
    }
    t->reaped = 1;
    int code = t->exit_code;
    scheduler_reap_slot(t);
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
    task_t *t = scheduler_task_by_id((int)pid);
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
    if (fd >= MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    file_descriptor_slot_t *slot = &scheduler_current()->descriptor_table->slots[fd];
    if (slot->type != FILE_DESCRIPTOR_PIPE_READ && slot->type != FILE_DESCRIPTOR_PIPE_WRITE) {
        return -1;
    }
    pipe_reset(slot->pipe);
    return 0;
}

static long sys_keyboard_modifiers(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return keyboard_modifiers();
}

static long sys_taskinfo(uint64_t buffer, uint64_t max_entries, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_PROCESS_LIST)) {
        return -1;
    }
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (max_entries == 0 || max_entries > TASK_INFO_MAX ||
        !user_range_ok(buffer, max_entries * sizeof(task_info_t), 1)) {
        return -1;
    }
    task_info_t *out = (task_info_t *)buffer;
    int total = scheduler_task_count();
    uint64_t written = 0;
    for (int i = 0; i < total && written < max_entries; i++) {
        task_t *t = scheduler_task_by_slot(i);
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
        int file_descriptors = 0;
        for (int f = 0; f < MAX_FILE_DESCRIPTORS; f++) {
            if (t->descriptor_table->slots[f].type != FILE_DESCRIPTOR_NONE) {
                file_descriptors++;
            }
        }
        e->open_file_descriptors = file_descriptors;
        e->shared_memory_segments = shared_memory_count_by_owner(t->id);
        int n = 0;
        for (; t->name[n] && n < TASK_INFO_NAME_MAX - 1; n++) {
            e->name[n] = t->name[n];
        }
        e->name[n] = '\0';
        written++;
    }
    return (long)written;
}

static long sys_clipboard_set(uint64_t buffer, uint64_t length, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_CLIPBOARD)) {
        return -1;
    }
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!user_range_ok(buffer, length, 0)) {
        return -1;
    }
    clipboard_set((const void *)buffer, (size_t)length);
    return 0;
}

static long sys_clipboard_get(uint64_t buffer, uint64_t maxlen, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!has_cap(CAP_CLIPBOARD)) {
        return -1;
    }
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (!user_range_ok(buffer, maxlen, 1)) {
        return -1;
    }
    return (long)clipboard_get((void *)buffer, (size_t)maxlen);
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

static int alloc_file_descriptor(task_t *t) {
    for (int i = 2; i < MAX_FILE_DESCRIPTORS; i++) {
        if (t->descriptor_table->slots[i].type == FILE_DESCRIPTOR_NONE) {
            return i;
        }
    }
    return -1;
}

/* /proc/<pid>/fd/<n>, which is how Unix spells "another descriptor for the
   object this one already names". Chromium's shared memory is the caller
   that made this necessary: it creates a memfd it can write to and then
   wants a second, read-only descriptor for the same pages to hand to a
   process it does not trust, and the only name a memfd has is this one.

   The rights can only shrink. A descriptor opened read-only cannot be
   reopened for writing through its own name, which is the same rule the
   capability set follows - and without it a read-only region would be a
   claim rather than a boundary, which M65 has a word for.

   Only this process's own descriptors: another task's table is not
   something a path lookup should reach into. */
static int process_file_descriptor_path(const char *path, int *out_fd) {
    const char *p = path;
    const char *prefix = "/proc/";
    for (int i = 0; prefix[i]; i++) {
        if (p[i] != prefix[i]) {
            return 0;
        }
    }
    p += 6;
    if (p[0] == 's' && p[1] == 'e' && p[2] == 'l' && p[3] == 'f' && p[4] == '/') {
        p += 5;
    } else {
        int pid = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            pid = pid * 10 + (*p - '0');
            p++;
            digits++;
        }
        if (digits == 0 || *p != '/' || pid != scheduler_current()->id) {
            return 0;
        }
        p++;
    }
    if (p[0] != 'f' || p[1] != 'd' || p[2] != '/') {
        return 0;
    }
    p += 3;
    int fd = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        fd = fd * 10 + (*p - '0');
        p++;
        digits++;
    }
    if (digits == 0 || *p != '\0' || fd < 0 || fd >= MAX_FILE_DESCRIPTORS) {
        return 0;
    }
    *out_fd = fd;
    return 1;
}

static long sys_open(uint64_t path_pointer, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_pointer) != 0) {
        return -1;
    }
    int writable = (flags & OPEN_WRITE) != 0;

    int source_fd = -1;
    if (process_file_descriptor_path(path, &source_fd)) {
        task_t *opener = scheduler_current();
        file_descriptor_slot_t *source =
            &opener->descriptor_table->slots[source_fd];
        if (source->type == FILE_DESCRIPTOR_NONE ||
            (flags & (OPEN_CREATE | OPEN_EXCL | OPEN_TRUNCATE))) {
            return -1;
        }
        if (source->type == FILE_DESCRIPTOR_FILE) {
            /* A file on the disk has a name of its own, and reopening it
               through procfs gives a new file description with its own
               offset rather than a second reference to this one - which is
               what Linux does and what a program reading the same file twice
               expects. So this becomes an ordinary open of that path. */
            if (!source->file || (writable && !source->file->writable)) {
                return -1;
            }
            k_memcpy(path, source->file->path, OPEN_FILE_PATH_MAX);
            path[OPEN_FILE_PATH_MAX - 1] = '\0';
        } else {
            /* Everything else - a memfd, a pipe end, a socket - has no name
               but this one, so the new descriptor refers to the same object.
               Its access is what was asked for, bounded by what the
               descriptor being reopened holds. */
            if (writable && source->type == FILE_DESCRIPTOR_MEMFD &&
                !source->writable) {
                return -1;
            }
            int fd = alloc_file_descriptor(opener);
            if (fd < 0) {
                return -1;
            }
            opener->descriptor_table->slots[fd] = *source;
            file_descriptor_retain(&opener->descriptor_table->slots[fd]);
            opener->descriptor_table->slots[fd].cloexec =
                (flags & OPEN_CLOEXEC) ? 1 : 0;
            if (source->type == FILE_DESCRIPTOR_MEMFD) {
                opener->descriptor_table->slots[fd].writable = writable ? 1 : 0;
            }
            return fd;
        }
    }

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
        if (virtual_file_system_lstat(path, &st) == 0 && st.is_link) {
            return -1;
        }
    }
    int handle = virtual_file_system_open(path, create_flags);
    if (handle < 0) {
        return -1;
    }
    if (flags & (OPEN_CREATE | OPEN_TRUNCATE | OPEN_WRITE)) {
        pkg_note_write(path, 0);
    }
    int opening_directory = virtual_file_system_is_directory(path);
    if (opening_directory &&
        (flags & (OPEN_WRITE | OPEN_TRUNCATE | OPEN_APPEND | OPEN_CREATE))) {
        return -1;
    }
    if ((flags & OPEN_TRUNCATE) && writable && virtual_file_system_handle_truncate(handle) != 0) {
        return -1;
    }
    task_t *self = scheduler_current();
    int fd = alloc_file_descriptor(self);
    if (fd < 0) {
        return -1;
    }
    open_file_t *of = open_file_alloc(handle, writable, path, opening_directory);
    if (!of) {
        return -1;
    }
    if (flags & OPEN_APPEND) {
        of->offset = virtual_file_system_handle_size(handle);
    }
    self->descriptor_table->slots[fd].type = FILE_DESCRIPTOR_FILE;
    self->descriptor_table->slots[fd].cloexec = (flags & OPEN_CLOEXEC) ? 1 : 0;
    self->descriptor_table->slots[fd].file = of;
    return fd;
}

static long sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    file_descriptor_slot_t *slot = &scheduler_current()->descriptor_table->slots[fd];
    if (slot->type != FILE_DESCRIPTOR_FILE) {
        return -1;
    }
    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)slot->file->offset; break;
    case SEEK_END: base = (int64_t)virtual_file_system_handle_size(slot->file->handle); break;
    default: return -1;
    }
    int64_t target = base + (int64_t)(int32_t)offset;
    if (target < 0 || target > (int64_t)LEANFS_MAX_FILE_SIZE) {
        return -1;
    }
    slot->file->offset = (uint32_t)target;
    return (long)target;
}

static long sys_stat(uint64_t path_pointer, uint64_t out_pointer, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_pointer) != 0) {
        return -OS_ERROR_FAULT;
    }
    leanfs_stat_t st;
    if (virtual_file_system_stat(path, &st) != 0) {
        return -OS_ERROR_NOENT;
    }
    os_stat_t out;
    k_memset(&out, 0, sizeof(out));
    out.size = st.size;
    out.mtime = st.mtime;
    out.is_directory = st.is_directory;
    out.kind = st.is_directory ? OS_STAT_DIRECTORY : OS_STAT_FILE;
    out.is_link = 0;
    out.inode = st.inode;
    return copy_to_user(out_pointer, &out, sizeof(out));
}

static void mmap_slot_remove(task_t *t, int index);

static int mmap_mergeable(const mmap_region_t *r, uint32_t prot, int handle,
                          int shared) {
    return r->pages != 0 && r->handle == -1 && handle == -1 &&
           !r->shared && !shared && r->prot == prot;
}

static void region_tag_reference(uint8_t memfd_id, uint16_t memfd_gen) {
    if (!memfd_id) {
        return;
    }
    memfd_region_reference(memfd_by_tag((uint8_t)(memfd_id - 1), memfd_gen));
}

static int mmap_slot_cmp_insert(task_t *t, uint64_t base, uint32_t pages, uint32_t prot,
                                int handle, uint32_t file_page, int shared, int merge,
                                uint8_t memfd_id, uint16_t memfd_gen) {
    uint64_t end = base + (uint64_t)pages * PAGE_SIZE;
    for (uint32_t i = 0; merge && i < t->mmap_capacity; i++) {
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
            if (i + 1 < t->mmap_capacity &&
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
    /* Room for one more, which may mean a bigger table. Growing here rather
       than at the call sites keeps the rule in one place: after this returns
       0 there is a zero-`pages` entry to write into. */
    if (scheduler_regions_reserve(t) != 0) {
        return -1;
    }
    int free_slot = -1;
    for (uint32_t i = 0; i < t->mmap_capacity; i++) {
        if (t->mmaps[i].pages == 0) {
            free_slot = (int)i;
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
    if (t->mmap_capacity == 0) {
        return;
    }
    uint32_t last = t->mmap_capacity - 1;
    scheduler_region_forget_memfd(&t->mmaps[index]);
    for (uint32_t i = (uint32_t)index; i < last; i++) {
        t->mmaps[i] = t->mmaps[i + 1];
        /* The table is compacted, so everything past the first zero-`pages`
           entry is already zero. Once that entry has been moved down there
           is nothing left to shift, which is what keeps an unmap from
           copying a megabyte when the table has grown to one. */
        if (t->mmaps[i].pages == 0) {
            return;
        }
    }
    t->mmaps[last].base = 0;
    t->mmaps[last].pages = 0;
    t->mmaps[last].prot = 0;
    t->mmaps[last].handle = -1;
    t->mmaps[last].file_page = 0;
    t->mmaps[last].memfd_id = 0;
    t->mmaps[last].memfd_gen = 0;
    t->mmaps[last].shared = 0;
}

static uint64_t mmap_find_gap(task_t *t, uint32_t pages) {
    uint64_t need = (uint64_t)pages * PAGE_SIZE;
    uint64_t candidate = USER_MMAP_BASE;
    for (uint32_t i = 0; i < t->mmap_capacity; i++) {
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
    for (uint32_t i = 0; i < t->mmap_capacity; i++) {
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

static long sys_munmap(uint64_t address, uint64_t length, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6);

static long sys_mmap(uint64_t address, uint64_t length, uint64_t prot, uint64_t flags,
                      uint64_t fd, uint64_t offset) {
    if (length == 0) {
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
        if ((long)fd < 0 || (uint64_t)fd >= MAX_FILE_DESCRIPTORS) {
            return -1;
        }
        if ((offset & (PAGE_SIZE - 1)) != 0) {
            return -1;
        }
        task_t *current = scheduler_current();
        if (current->descriptor_table->slots[fd].type == FILE_DESCRIPTOR_MEMFD) {
            struct memfd *m = current->descriptor_table->slots[fd].memfd;
            if (!shared) {
                return -1;
            }
            uint64_t size = memfd_size(m);
            uint64_t want_end = offset + (uint64_t)length;
            if (size == 0 || want_end < offset || want_end > size) {
                return -1;
            }
            if ((prot & PROT_WRITE) &&
                (!memfd_may_write(m) ||
                 !current->descriptor_table->slots[fd].writable)) {
                return -1;
            }
            memfd_region_reference(m);
            memfd_id = (uint8_t)(memfd_slot(m) + 1);
            memfd_gen = memfd_generation(m);
            file_page = (uint32_t)(offset / PAGE_SIZE);
            goto have_backing;
        }
        if (current->descriptor_table->slots[fd].type != FILE_DESCRIPTOR_FILE || !current->descriptor_table->slots[fd].file) {
            return -1;
        }
        if (shared && (prot & PROT_WRITE) && !current->descriptor_table->slots[fd].file->writable) {
            return -1;
        }
        if (shared && (prot & PROT_WRITE) && !has_cap(CAP_FS_WRITE)) {
            return -1;
        }
        handle = current->descriptor_table->slots[fd].file->handle;
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
    uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0 || pages > (USER_MMAP_LIMIT - USER_MMAP_BASE) / PAGE_SIZE) {
        return -1;
    }

    task_t *self = scheduler_vm_owner(scheduler_current());
    if (self->pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return -1;
    }

    uint64_t base = 0;
    if (flags & MAP_FIXED) {
        if ((address & (PAGE_SIZE - 1)) != 0) {
            return -1;
        }
        if (address < USER_MMAP_BASE || address + pages * PAGE_SIZE > USER_MMAP_LIMIT) {
            return -1;
        }
        if (!mmap_range_is_free(self, address, pages) &&
            sys_munmap(address, pages * PAGE_SIZE, 0, 0, 0, 0) != 0) {
            return -1;
        }
        base = address;
    } else if (address != 0) {
        uint64_t want = address & ~(PAGE_SIZE - 1);
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

static long sys_munmap(uint64_t address, uint64_t length, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if ((address & (PAGE_SIZE - 1)) != 0 || length == 0) {
        return -1;
    }
    uint64_t end = address + ((length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (end <= address) {
        return -1;
    }
    if (address < USER_MMAP_BASE || end > USER_MMAP_LIMIT) {
        return -1;
    }
    task_t *self = scheduler_vm_owner(scheduler_current());

    for (uint32_t i = 0; i < self->mmap_capacity; i++) {
        if (self->mmaps[i].pages == 0) {
            break;
        }
        uint64_t rstart = self->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)self->mmaps[i].pages * PAGE_SIZE;
        uint64_t cut_start = address > rstart ? address : rstart;
        uint64_t cut_end = end < rend ? end : rend;
        if (cut_start >= cut_end) {
            continue;
        }
        if (self->mmaps[i].shared) {
            scheduler_release_shared_range(self, cut_start, cut_end);
        } else {
            virtual_memory_unmap_range_free(self->pml4_phys, cut_start, cut_end);
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
            region_tag_reference(self->mmaps[i].memfd_id, self->mmaps[i].memfd_gen);
            if (mmap_slot_cmp_insert(self, cut_end,
                                      (uint32_t)((rend - cut_end) / PAGE_SIZE),
                                      self->mmaps[i].prot, self->mmaps[i].handle,
                                      self->mmaps[i].file_page +
                                          (uint32_t)((cut_end - rstart) / PAGE_SIZE),
                                      self->mmaps[i].shared, 0,
                                      self->mmaps[i].memfd_id,
                                      self->mmaps[i].memfd_gen) != 0) {
                scheduler_region_forget_memfd(&self->mmaps[i]);
                return -1;
            }
            i = -1;
        }
    }
    return 0;
}

static long sys_msync(uint64_t address, uint64_t length, uint64_t flags, uint64_t a4,
                      uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if ((address & (PAGE_SIZE - 1)) != 0 || length == 0) {
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
    uint64_t end = address + ((length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (end <= address || address < USER_MMAP_BASE || end > USER_MMAP_LIMIT) {
        return -1;
    }
    task_t *self = scheduler_vm_owner(scheduler_current());
    for (uint32_t i = 0; i < self->mmap_capacity; i++) {
        if (self->mmaps[i].pages == 0) {
            break;
        }
        uint64_t rstart = self->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)self->mmaps[i].pages * PAGE_SIZE;
        if (end <= rstart || address >= rend) {
            continue;
        }
        if (self->mmaps[i].shared && self->mmaps[i].handle >= 0) {
            file_mapping_sync(self->mmaps[i].handle);
        }
    }
    return 0;
}

static int mmap_split_for(task_t *t, uint64_t address, uint64_t end) {
    for (uint32_t i = 0; i < t->mmap_capacity; i++) {
        if (t->mmaps[i].pages == 0) {
            break;
        }
        uint64_t rstart = t->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)t->mmaps[i].pages * PAGE_SIZE;
        uint64_t cut = 0;
        if (address > rstart && address < rend) {
            cut = address;
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
        region_tag_reference(mid, mgen);
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

static long sys_mprotect(uint64_t address, uint64_t length, uint64_t prot, uint64_t a4,
                          uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if ((address & (PAGE_SIZE - 1)) != 0 || length == 0) {
        return -1;
    }
    if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC)) {
        return -1;
    }
    uint64_t end = address + ((length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (end <= address) {
        return -1;
    }
    /* mprotect applies to any MAPPED page, not only to one mmap made. A
       program's own image is mapped, and changing what may be done to part of
       it is an ordinary thing to want: M155 found this because V8 makes its
       read-only heap read-only, which is a page of its own data segment, and
       got a refusal it reported as a fatal error.

       The two regions are handled differently because only one of them has
       bookkeeping to keep. An mmap region is described by an entry in
       self->mmaps and the entry has to be split and updated so a later
       munmap or mremap agrees with the page tables. The image has no such
       description - the loader mapped it and nothing tracks it afterwards -
       so the page table IS the record, and the only thing to check is that
       every page in the range is really there.

       The stack is deliberately not here. It grows when it is touched, which
       means a page in it may not be mapped yet and refusing that would be
       right for the wrong reason; the condition for adding it is a program
       that asks. */
    int in_mmap = address >= USER_MMAP_BASE && end <= USER_MMAP_LIMIT;
    int in_image = address >= USER_IMAGE_BASE && end <= USER_IMAGE_LIMIT;
    if (!in_mmap && !in_image) {
        return -1;
    }
    task_t *self = scheduler_vm_owner(scheduler_current());
    if (self->pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return -1;
    }
    if (in_image) {
        if (!virtual_memory_user_range_ok(self->pml4_phys, address,
                                          (end - address) / PAGE_SIZE, 0)) {
            return -1;
        }
    } else {
        uint64_t covered = 0;
        for (uint32_t i = 0; i < self->mmap_capacity; i++) {
            if (self->mmaps[i].pages == 0) {
                break;
            }
            uint64_t rstart = self->mmaps[i].base;
            uint64_t rend = rstart + (uint64_t)self->mmaps[i].pages * PAGE_SIZE;
            uint64_t lo = rstart > address ? rstart : address;
            uint64_t hi = rend < end ? rend : end;
            if (lo < hi) {
                covered += hi - lo;
            }
        }
        if (covered != end - address) {
            return -1;
        }
        if (mmap_split_for(self, address, end) != 0) {
            return -1;
        }
    }
    if (in_mmap) {
        for (uint32_t i = 0; i < self->mmap_capacity; i++) {
            if (self->mmaps[i].pages == 0) {
                break;
            }
            uint64_t rstart = self->mmaps[i].base;
            uint64_t rend = rstart + (uint64_t)self->mmaps[i].pages * PAGE_SIZE;
            if (rstart >= address && rend <= end) {
                self->mmaps[i].prot = (uint32_t)prot;
            }
        }
    }
    uint64_t flags = VIRTUAL_MEMORY_FLAG_USER;
    if (prot & PROT_WRITE) {
        flags |= VIRTUAL_MEMORY_FLAG_WRITABLE;
    }
    if (prot & PROT_EXEC) {
        flags |= VIRTUAL_MEMORY_FLAG_EXEC;
    }
    virtual_memory_protect_range_in(self->pml4_phys, address, end, flags);
    return 0;
}

/* Which pages of a range are resident. This kernel has no swap, so "in this
   address space's page tables" IS resident, and the answer is exact rather
   than an estimate - which is the whole reason a memory report asks. */
static long sys_mincore(uint64_t address, uint64_t length, uint64_t vector,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if ((address & (PAGE_SIZE - 1)) != 0 || length == 0) {
        return -1;
    }
    uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t end = address + pages * PAGE_SIZE;
    if (end <= address) {
        return -1;
    }
    task_t *self = scheduler_vm_owner(scheduler_current());
    if (self->pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return -1;
    }

    /* M160. "Not resident" and "not mapped" are different answers and this
       call used to give the first one for both: it read the page table, and
       a page inside a mapping that has never been touched is absent from the
       page table for exactly the same reason a page nobody mapped is.
       mincore(2)'s contract is that a range containing unmapped pages is an
       error - ENOMEM - and that is the half a caller needs, because it is the
       only way to ask "may I touch this" without finding out by faulting.
       mlock is what asked: it has to bring pages in, and bringing in a page
       that is not there kills the process.

       So the mapping is looked up in the region table, which is what knows,
       and the page table is asked only about residency. */
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t page = address + i * PAGE_SIZE;
        int mapped = 0;
        for (uint32_t r = 0; r < self->mmap_capacity; r++) {
            if (self->mmaps[r].pages == 0) {
                continue;
            }
            uint64_t rstart = self->mmaps[r].base;
            uint64_t rend = rstart + (uint64_t)self->mmaps[r].pages * PAGE_SIZE;
            if (page >= rstart && page < rend) {
                mapped = 1;
                break;
            }
        }
        if (!mapped) {
            /* The program's own image, its stack and its heap are mappings
               this table does not hold, and they are as mapped as anything
               else. The page table is the right authority for those: a page
               that is present is mapped whatever the table says. */
            if (!virtual_memory_user_range_ok(self->pml4_phys, page, 1, 0)) {
                return -1;
            }
        }
        uint8_t resident = virtual_memory_user_range_ok(
            self->pml4_phys, page, 1, 0) ? 1 : 0;
        if (copy_to_user(vector + i, &resident, 1) != 0) {
            return -1;
        }
    }
    return 0;
}

static long sys_madvise(uint64_t address, uint64_t length, uint64_t advice, uint64_t a4,
                         uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if ((address & (PAGE_SIZE - 1)) != 0 || length == 0) {
        return -1;
    }
    uint64_t end = address + ((length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (end <= address) {
        return -1;
    }
    if (address < USER_MMAP_BASE || end > USER_MMAP_LIMIT) {
        return -1;
    }
    /* MADV_REMOVE punches a hole in the object BEHIND a shared mapping, so
       every process mapping it sees zeros afterwards. This kernel's memfds
       are reference counted and do not know who has them mapped, so the
       pages cannot be dropped from the other mappers' page tables - and
       dropping them from this one only would free memory somebody else is
       still pointing at. Refused, not quietly accepted: M65's rule. The
       condition is a memfd that knows its mappers.

       Linux itself returns EINVAL here for every mapping that is not on a
       shared-memory filesystem, so this is also the answer most callers
       already handle. */
    if (advice == MADV_REMOVE) {
        return -1;
    }
    if (advice != MADV_DONTNEED) {
        return 0;
    }
    task_t *self = scheduler_vm_owner(scheduler_current());
    if (self->pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return -1;
    }
    virtual_memory_unmap_range_free(self->pml4_phys, address, end);
    return 0;
}

static long sys_fstat(uint64_t fd, uint64_t out_pointer, uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    file_descriptor_slot_t *slot = &scheduler_current()->descriptor_table->slots[fd];
    os_stat_t out;
    k_memset(&out, 0, sizeof(out));

    switch (slot->type) {
    case FILE_DESCRIPTOR_FILE: {
        leanfs_stat_t st;
        if (virtual_file_system_handle_stat(slot->file->handle, &st) != 0) {
            return -1;
        }
        out.size = st.size;
        out.mtime = st.mtime;
        out.is_directory = st.is_directory;
        out.kind = st.is_directory ? OS_STAT_DIRECTORY : OS_STAT_FILE;
        out.is_link = 0;
        out.inode = st.inode;
        break;
    }
    case FILE_DESCRIPTOR_STDIN:
    case FILE_DESCRIPTOR_STDOUT:
        out.kind = OS_STAT_CHR;
        break;
    case FILE_DESCRIPTOR_PIPE_READ:
    case FILE_DESCRIPTOR_PIPE_WRITE:
        out.kind = OS_STAT_FIFO;
        break;
    case FILE_DESCRIPTOR_SOCKET:
    case FILE_DESCRIPTOR_UNIX:
        out.kind = OS_STAT_SOCKET;
        break;
    case FILE_DESCRIPTOR_MEMFD:
        out.kind = OS_STAT_FILE;
        out.size = (uint32_t)memfd_size(slot->memfd);
        break;
    case FILE_DESCRIPTOR_EVENT:
    case FILE_DESCRIPTOR_TIMER:
    case FILE_DESCRIPTOR_EPOLL:
        out.kind = OS_STAT_CHR;
        break;
    default:
        return -1;
    }
    return copy_to_user(out_pointer, &out, sizeof(out)) == 0 ? 0 : -1;
}

static long sys_rmdir(uint64_t path_pointer, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(path, path_pointer) != 0) {
        return -1;
    }
    long r = virtual_file_system_rmdir(path);
    pkg_note_write(path, r);
    return r;
}

static long sys_time(uint64_t out_pointer, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    os_datetime_t now;
    rtc_read(&now);
    if (out_pointer != 0 && copy_to_user(out_pointer, &now, sizeof(now)) != 0) {
        return -1;
    }
    return now.valid ? (long)os_unix_time(&now) : 0;
}

static long sys_getcaps(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return (long)scheduler_vm_owner(scheduler_current())->caps;
}

static long sys_dropcaps(uint64_t keep, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* Narrows the process, so a thread that gives something up gives it up
       for the thread that spawned it too. Without that, "this process may no
       longer open a socket" would be false the moment it had two threads. */
    task_t *self = scheduler_vm_owner(scheduler_current());
    self->caps &= (uint32_t)keep;
    return (long)self->caps;
}

static struct socket *socket_for_file_descriptor(uint64_t fd) {
    task_t *self = scheduler_current();
    if (fd >= MAX_FILE_DESCRIPTORS || self->descriptor_table->slots[fd].type != FILE_DESCRIPTOR_SOCKET) {
        return (struct socket *)0;
    }
    return self->descriptor_table->slots[fd].sock;
}

static long install_socket_file_descriptor(struct socket *s) {
    task_t *self = scheduler_current();
    int fd = alloc_file_descriptor(self);
    if (fd < 0) {
        socket_unref(s);
        return -1;
    }
    self->descriptor_table->slots[fd].type = FILE_DESCRIPTOR_SOCKET;
    self->descriptor_table->slots[fd].cloexec = 0;
    self->descriptor_table->slots[fd].sock = s;
    return fd;
}

static long install_unix_file_descriptor(struct unix_socket *u) {
    task_t *self = scheduler_current();
    int fd = alloc_file_descriptor(self);
    if (fd < 0) {
        unix_socket_unref(u);
        return -1;
    }
    self->descriptor_table->slots[fd].type = FILE_DESCRIPTOR_UNIX;
    self->descriptor_table->slots[fd].cloexec = 0;
    self->descriptor_table->slots[fd].nonblock = 0;
    self->descriptor_table->slots[fd].un = u;
    return fd;
}

static struct unix_socket *unix_for_file_descriptor(uint64_t fd) {
    task_t *self = scheduler_current();
    if (fd >= MAX_FILE_DESCRIPTORS || self->descriptor_table->slots[fd].type != FILE_DESCRIPTOR_UNIX) {
        return (struct unix_socket *)0;
    }
    return self->descriptor_table->slots[fd].un;
}

static long sys_socket(uint64_t type, uint64_t domain, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (domain == OS_AF_UNIX) {
        if (type != UNIX_SOCKET_STREAM && type != UNIX_SOCKET_SEQPACKET) {
            return -1;
        }
        struct unix_socket *u = unix_socket_alloc((int)type);
        return u ? install_unix_file_descriptor(u) : -1;
    }
    if (domain != OS_AF_INET) {
        return -1;
    }
    if (!has_cap(CAP_NETWORK)) {
        /* Named, rather than the bare -1 every other failure here returns.
           A program refused a socket and a program out of descriptors are
           different problems, and until M151 both arrived at the caller as
           EMFILE - which sent somebody looking at descriptor limits. */
        return -OS_ERROR_ACCESS;
    }
    if (type != SOCK_DGRAM && type != SOCK_STREAM) {
        return -1;
    }
    struct socket *s = socket_alloc((int)type);
    if (!s) {
        return -1;
    }
    return install_socket_file_descriptor(s);
}

/* SO_PEERCRED, and nothing else - there is no general getsockopt behind the
   kernel wall, so this answers the one question that needs one rather than
   inventing the whole interface for it. It needs no capability, for M118's
   reason: the process on the other end of a socket you already hold is not a
   fact this machine is keeping from you. */
static long sys_unix_peer_credentials(uint64_t file_descriptor, uint64_t out_pointer,
                                      uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!user_range_ok(out_pointer, sizeof(os_ucred_t), 1)) {
        return -1;
    }
    task_t *self = scheduler_current();
    int fd = (int)file_descriptor;
    if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS ||
        self->descriptor_table->slots[fd].type != FILE_DESCRIPTOR_UNIX) {
        return -1;
    }
    int pid = unix_socket_peer_pid(self->descriptor_table->slots[fd].un);
    if (pid < 0) {
        return -1;
    }
    os_ucred_t out;
    out.pid = (int32_t)pid;
    out.uid = 0;
    out.gid = 0;
    if (copy_to_user(out_pointer, &out, sizeof(out)) != 0) {
        return -1;
    }
    return 0;
}

static long sys_socketpair(uint64_t type, uint64_t file_descriptors_pointer, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!user_range_ok(file_descriptors_pointer, 2 * sizeof(int), 1)) {
        return -1;
    }
    struct unix_socket *a = (struct unix_socket *)0;
    struct unix_socket *b = (struct unix_socket *)0;
    if (unix_socket_pair((int)type, &a, &b) != 0) {
        return -1;
    }
    task_t *self = scheduler_current();
    int fa = alloc_file_descriptor(self);
    if (fa >= 0) {
        self->descriptor_table->slots[fa].type = FILE_DESCRIPTOR_UNIX;
        self->descriptor_table->slots[fa].cloexec = 0;
        self->descriptor_table->slots[fa].nonblock = 0;
        self->descriptor_table->slots[fa].un = a;
    }
    int framebuffer = fa >= 0 ? alloc_file_descriptor(self) : -1;
    if (framebuffer < 0) {
        if (fa >= 0) {
            file_descriptor_release(&self->descriptor_table->slots[fa]);
        } else {
            unix_socket_unref(a);
        }
        unix_socket_unref(b);
        return -1;
    }
    self->descriptor_table->slots[framebuffer].type = FILE_DESCRIPTOR_UNIX;
    self->descriptor_table->slots[framebuffer].cloexec = 0;
    self->descriptor_table->slots[framebuffer].nonblock = 0;
    self->descriptor_table->slots[framebuffer].un = b;
    int out[2] = {fa, framebuffer};
    if (copy_to_user(file_descriptors_pointer, out, sizeof(out)) != 0) {
        file_descriptor_release(&self->descriptor_table->slots[fa]);
        file_descriptor_release(&self->descriptor_table->slots[framebuffer]);
        return -1;
    }
    return 0;
}

static int copy_un_name(char *out, uint64_t source, uint64_t length) {
    if (length == 0 || length > UNIX_PATH_MAX) {
        return -1;
    }
    return copy_from_user(out, source, (size_t)length) == 0 ? 0 : -1;
}

static long sys_bindun(uint64_t fd, uint64_t name_pointer, uint64_t length, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    char name[UNIX_PATH_MAX];
    if (copy_un_name(name, name_pointer, length) != 0) {
        return -1;
    }
    struct unix_socket *u = unix_for_file_descriptor(fd);
    return u ? unix_socket_bind(u, name, (int)length) : -1;
}

static long sys_connectun(uint64_t fd, uint64_t name_pointer, uint64_t length, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    char name[UNIX_PATH_MAX];
    if (copy_un_name(name, name_pointer, length) != 0) {
        return -1;
    }
    struct unix_socket *u = unix_for_file_descriptor(fd);
    return u ? unix_socket_connect(u, name, (int)length) : -1;
}

#define UNIX_MESSAGE_STAGING UNIX_BUFFER_SIZE

static long sys_sendmsg(uint64_t fd, uint64_t message_pointer, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)flags; (void)a4; (void)a5; (void)a6;
    task_t *self = scheduler_current();
    os_message_t message;
    if (copy_from_user(&message, message_pointer, sizeof(message)) != 0) {
        return -1;
    }
    struct unix_socket *u = unix_for_file_descriptor(fd);
    if (!u) {
        return -1;
    }
    if (message.nfds > UNIX_MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    uint32_t length = message.length;
    if (length > UNIX_MESSAGE_STAGING) {
        if (unix_socket_type(u) == UNIX_SOCKET_SEQPACKET) {
            return -1;
        }
        length = UNIX_MESSAGE_STAGING;
    }
    file_descriptor_slot_t slots[UNIX_MAX_FILE_DESCRIPTORS];
    int nfds = (int)message.nfds;
    if (nfds > 0) {
        int nums[UNIX_MAX_FILE_DESCRIPTORS];
        if (copy_from_user(nums, message.file_descriptors, (size_t)nfds * sizeof(int)) != 0) {
            return -1;
        }
        for (int i = 0; i < nfds; i++) {
            if (nums[i] < 0 || nums[i] >= MAX_FILE_DESCRIPTORS ||
                self->descriptor_table->slots[nums[i]].type == FILE_DESCRIPTOR_NONE) {
                return -1;
            }
            slots[i] = self->descriptor_table->slots[nums[i]];
        }
    }
    uint8_t staging[UNIX_MESSAGE_STAGING];
    if (length && copy_from_user(staging, message.data, (size_t)length) != 0) {
        return -1;
    }
    for (;;) {
        uint64_t seq = scheduler_event_sequence();
        long n = unix_socket_send(u, staging, length, slots, nfds);
        if (n != 0 || (length == 0 && nfds == 0)) {
            return n;
        }
        if (self->descriptor_table->slots[fd].nonblock) {
            return -OS_ERROR_AGAIN;
        }
        if (scheduler_signal_pending()) {
            return -OS_ERROR_INTR;
        }
        scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, 0, seq);
    }
}

static long sys_recvmsg(uint64_t fd, uint64_t message_pointer, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)flags; (void)a4; (void)a5; (void)a6;
    task_t *self = scheduler_current();
    os_message_t message;
    if (copy_from_user(&message, message_pointer, sizeof(message)) != 0) {
        return -1;
    }
    struct unix_socket *u = unix_for_file_descriptor(fd);
    if (!u) {
        return -1;
    }
    if (message.nfds > UNIX_MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    if (message.length && !user_range_ok(message.data, message.length, 1)) {
        return -1;
    }
    uint32_t want = message.length > UNIX_MESSAGE_STAGING ? UNIX_MESSAGE_STAGING : message.length;
    uint8_t staging[UNIX_MESSAGE_STAGING];
    file_descriptor_slot_t slots[UNIX_MAX_FILE_DESCRIPTORS];
    for (;;) {
        uint64_t seq = scheduler_event_sequence();
        int nfds = 0;
        int rflags = 0;
        long n = unix_socket_receive(u, staging, want, slots, (int)message.nfds, &nfds, &rflags);
        if (n < 0) {
            message.nfds = 0;
            message.flags = 0;
            copy_to_user(message_pointer, &message, sizeof(message));
            return 0;
        }
        if (n > 0 || nfds > 0) {
            int nums[UNIX_MAX_FILE_DESCRIPTORS];
            int installed = 0;
            for (int i = 0; i < nfds; i++) {
                int nfd = alloc_file_descriptor(self);
                if (nfd < 0) {
                    file_descriptor_release(&slots[i]);
                    rflags |= OS_MESSAGE_CTRUNC;
                    continue;
                }
                self->descriptor_table->slots[nfd] = slots[i];
                self->descriptor_table->slots[nfd].cloexec = 0;
                self->descriptor_table->slots[nfd].nonblock = 0;
                nums[installed++] = nfd;
            }
            message.nfds = (uint32_t)installed;
            message.flags = (uint32_t)rflags;
            int copied =
                (installed == 0 ||
                 copy_to_user(message.file_descriptors, nums, (size_t)installed * sizeof(int)) == 0) &&
                (n == 0 || copy_to_user(message.data, staging, (size_t)n) == 0) &&
                copy_to_user(message_pointer, &message, sizeof(message)) == 0;
            if (!copied) {
                for (int i = 0; i < installed; i++) {
                    file_descriptor_release(&self->descriptor_table->slots[nums[i]]);
                }
                return -1;
            }
            return n;
        }
        if (self->descriptor_table->slots[fd].nonblock) {
            return -OS_ERROR_AGAIN;
        }
        if (scheduler_signal_pending()) {
            return -OS_ERROR_INTR;
        }
        scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, 0, seq);
    }
}

static long sys_memfd_create(uint64_t name_pointer, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (flags & ~(uint64_t)(OS_MFD_CLOEXEC | OS_MFD_ALLOW_SEALING)) {
        return -1;
    }
    char name[MEMFD_NAME_MAX];
    name[0] = '\0';
    if (name_pointer) {
        if (copy_from_user(name, name_pointer, sizeof(name)) != 0) {
            return -1;
        }
        name[sizeof(name) - 1] = '\0';
    }
    struct memfd *m = memfd_create_object(name_pointer ? name : (const char *)0);
    if (!m) {
        return -1;
    }
    if (!(flags & OS_MFD_ALLOW_SEALING)) {
        memfd_add_seals(m, MEMFD_SEAL_SEAL);
    }
    task_t *self = scheduler_current();
    int fd = alloc_file_descriptor(self);
    if (fd < 0) {
        memfd_unref(m);
        return -1;
    }
    self->descriptor_table->slots[fd].type = FILE_DESCRIPTOR_MEMFD;
    self->descriptor_table->slots[fd].memfd = m;
    self->descriptor_table->slots[fd].cloexec = (flags & OS_MFD_CLOEXEC) ? 1 : 0;
    self->descriptor_table->slots[fd].nonblock = 0;
    self->descriptor_table->slots[fd].writable = 1;
    return fd;
}

static long sys_memfd_seal(uint64_t fd, uint64_t add, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *self = scheduler_current();
    if (fd >= MAX_FILE_DESCRIPTORS || self->descriptor_table->slots[fd].type != FILE_DESCRIPTOR_MEMFD) {
        return -1;
    }
    struct memfd *m = self->descriptor_table->slots[fd].memfd;
    if (add != 0 && memfd_add_seals(m, (uint32_t)add) != 0) {
        return -1;
    }
    return (long)memfd_get_seals(m);
}

static long sys_sockshut(uint64_t fd, uint64_t how, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct unix_socket *u = unix_for_file_descriptor(fd);
    if (!u) {
        return -1;
    }
    return unix_socket_shutdown(u, (int)how);
}

static long sys_listen(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct unix_socket *u = unix_for_file_descriptor(fd);
    if (u) {
        return unix_socket_listen(u);
    }
    struct socket *s = socket_for_file_descriptor(fd);
    return s ? socket_listen(s) : -1;
}

static long sys_connect(uint64_t fd, uint64_t ip, uint64_t port, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_file_descriptor(fd);
    if (!s || port == 0 || port > 0xFFFF) {
        return -1;
    }
    return socket_connect(s, (uint32_t)ip, (uint16_t)port);
}

static long sys_connstat(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_file_descriptor(fd);
    struct tcpcb *tcb = s ? socket_tcb(s) : (struct tcpcb *)0;
    if (!tcb) {
        return -1;
    }
    if (!tcp_connect_settled(tcb)) {
        return 0;
    }
    return tcp_state(tcb) == TCP_ESTABLISHED ? 1 : -1;
}

/* Which address a socket is on. getsockname(2) used to answer this out of
   the machine's own network configuration with a port of zero, which is
   right about the address and a lie about the port - and a program that
   binds to port zero and then asks which port it got, as every server that
   does not want a fixed one does, was told nothing. */
static long sys_sockname(uint64_t fd, uint64_t out_pointer, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    os_sockaddr_t out;
    k_memset(&out, 0, sizeof(out));
    struct unix_socket *u = unix_for_file_descriptor(fd);
    if (u) {
        /* A Unix-domain socket has a path rather than an address, and this
           call has nowhere to put one. Zeros, which is what binding one to
           nothing reports on every other system. */
        return copy_to_user(out_pointer, &out, sizeof(out)) == 0 ? 0 : -1;
    }
    struct socket *s = socket_for_file_descriptor(fd);
    if (!s) {
        return -1;
    }
    out.ip = socket_local_ip(s);
    out.port = socket_local_port(s);
    return copy_to_user(out_pointer, &out, sizeof(out)) == 0 ? 0 : -1;
}

static long sys_accept(uint64_t fd, uint64_t from_pointer, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct unix_socket *ulistener = unix_for_file_descriptor(fd);
    if (ulistener) {
        struct unix_socket *uconn = unix_socket_accept(ulistener);
        if (!uconn) {
            return -1;
        }
        if (from_pointer) {
            os_sockaddr_t none = {0, 0, 0};
            if (copy_to_user(from_pointer, &none, sizeof(none)) != 0) {
                unix_socket_unref(uconn);
                return -1;
            }
        }
        return install_unix_file_descriptor(uconn);
    }
    struct socket *listener = socket_for_file_descriptor(fd);
    if (!listener) {
        return -1;
    }
    struct socket *conn = socket_accept(listener);
    if (!conn) {
        return -1;
    }
    if (from_pointer) {
        struct tcpcb *tcb = socket_tcb(conn);
        os_sockaddr_t from = {tcp_remote_ip(tcb), tcp_remote_port(tcb), 0};
        if (copy_to_user(from_pointer, &from, sizeof(from)) != 0) {
            socket_unref(conn);
            return -1;
        }
    }
    return install_socket_file_descriptor(conn);
}

static long sys_send(uint64_t fd, uint64_t buffer, uint64_t length, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_file_descriptor(fd);
    struct tcpcb *tcb = s ? socket_tcb(s) : (struct tcpcb *)0;
    if (!tcb || length > TCP_MAX_MSS) {
        if (!tcb) {
            return -1;
        }
        length = TCP_MAX_MSS;
    }
    static uint8_t staging[TCP_MAX_MSS];
    if (length && copy_from_user(staging, buffer, (size_t)length) != 0) {
        return -1;
    }
    return tcp_send(tcb, staging, (uint16_t)length);
}

static long sys_receive(uint64_t fd, uint64_t buffer, uint64_t max, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_file_descriptor(fd);
    struct tcpcb *tcb = s ? socket_tcb(s) : (struct tcpcb *)0;
    if (!tcb) {
        return -1;
    }
    if (max > TCP_MAX_MSS) {
        max = TCP_MAX_MSS;
    }
    static uint8_t staging[TCP_MAX_MSS];
    int n = tcp_receive(tcb, staging, (uint16_t)max);
    if (n <= 0) {
        return n;
    }
    return copy_to_user(buffer, staging, (size_t)n) == 0 ? n : -1;
}

static long sys_bind(uint64_t fd, uint64_t port, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_file_descriptor(fd);
    if (!s || port > 0xFFFF) {
        return -1;
    }
    return socket_bind(s, (uint16_t)port);
}

static long sys_sendto(uint64_t fd, uint64_t ip, uint64_t port, uint64_t buffer, uint64_t length, uint64_t a6) {
    (void)a6;
    struct socket *s = socket_for_file_descriptor(fd);
    if (!s || port > 0xFFFF || length > UDP_MAX_PAYLOAD) {
        return -1;
    }
    static uint8_t staging[UDP_MAX_PAYLOAD];
    if (length && copy_from_user(staging, buffer, (size_t)length) != 0) {
        return -1;
    }
    return socket_sendto(s, (uint32_t)ip, (uint16_t)port, staging, (uint16_t)length);
}

static long sys_recvfrom(uint64_t fd, uint64_t buffer, uint64_t max, uint64_t from_pointer, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    struct socket *s = socket_for_file_descriptor(fd);
    if (!s || max > SOCKET_MAX_DATAGRAM) {
        return -1;
    }
    static uint8_t staging[SOCKET_MAX_DATAGRAM];
    os_sockaddr_t from = {0, 0, 0};
    int n = socket_recvfrom(s, staging, (uint16_t)max, &from.ip, &from.port);
    if (n < 0) {
        return -1;
    }
    if (n && copy_to_user(buffer, staging, (size_t)n) != 0) {
        return -1;
    }
    if (from_pointer && copy_to_user(from_pointer, &from, sizeof(from)) != 0) {
        return -1;
    }
    return n;
}

static long sys_sockpoll(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct socket *s = socket_for_file_descriptor(fd);
    return s ? socket_pending(s) : -1;
}

static long sys_netconf(uint64_t out_pointer, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
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
    return copy_to_user(out_pointer, &conf, sizeof(conf)) == 0 ? 0 : -1;
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
    task_t *self = scheduler_current();
    if (audio_owner < 0) {
        return 0;
    }
    if (audio_owner == self->id) {
        return 1;
    }
    task_t *owner = scheduler_task_by_id(audio_owner);
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
    task_t *self = scheduler_current();
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
    pc_speaker_off();
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
    pc_speaker_tone((uint32_t)freq_hz, (uint32_t)ms);
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
    pc_speaker_set_muted(percent == 0);
    return 0;
}

static long sys_audio_play(uint64_t buffer, uint64_t frames, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
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
    if (!user_range_ok(buffer, frames * 4, 0)) {
        return -1;
    }
    return ac97_play((const int16_t *)buffer, (uint32_t)frames);
}

static long sys_display_modes(uint64_t out_pointer, uint64_t max_entries, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
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
    if (n > 0 && copy_to_user(out_pointer, modes, (size_t)n * sizeof(modes[0])) != 0) {
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
    framebuffer_remap(pitch, (uint32_t)width, (uint32_t)height);
    return 0;
}

static int file_descriptor_is_ready(task_t *self, int fd) {
    if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS) {
        return 0;
    }
    file_descriptor_slot_t *slot = &self->descriptor_table->slots[fd];
    switch (slot->type) {
    case FILE_DESCRIPTOR_STDIN:
        return keyboard_peek() ? 1 : 0;
    case FILE_DESCRIPTOR_PIPE_READ:
        return (pipe_buffered(slot->pipe) > 0 || pipe_write_closed(slot->pipe)) ? 1 : 0;
    case FILE_DESCRIPTOR_SOCKET:
        return socket_pending(slot->sock) > 0 ? 1 : 0;
    case FILE_DESCRIPTOR_UNIX:
        return unix_socket_pending(slot->un);
    case FILE_DESCRIPTOR_EVENT:
        return eventfd_readable(slot->event) ? 1 : 0;
    case FILE_DESCRIPTOR_TIMER:
        return timerfd_readable(slot->timer, clock_now_ns()) ? 1 : 0;
    case FILE_DESCRIPTOR_EPOLL:
        return 0;
    case FILE_DESCRIPTOR_FILE:
        return virtual_file_system_handle_readable(slot->file->handle);
    default:
        return 0;
    }
}

static uint32_t file_descriptor_epoll_mask_for(task_t *self, int fd, const void *object) {
    if (fd < 0 || fd >= MAX_FILE_DESCRIPTORS) {
        return EPOLL_STALE;
    }
    file_descriptor_slot_t *slot = &self->descriptor_table->slots[fd];
    if (slot->type == FILE_DESCRIPTOR_NONE) {
        return EPOLL_STALE;
    }
    if (object && slot->pipe != (struct pipe *)object) {
        return EPOLL_STALE;
    }
    uint32_t m = 0;
    if (file_descriptor_is_ready(self, fd)) {
        m |= EPOLLIN;
    }
    switch (slot->type) {
    case FILE_DESCRIPTOR_STDOUT:
    case FILE_DESCRIPTOR_FILE:
        m |= EPOLLOUT;
        break;
    case FILE_DESCRIPTOR_STDIN:
        break;
    case FILE_DESCRIPTOR_PIPE_WRITE:
        if (pipe_writable(slot->pipe)) {
            m |= EPOLLOUT;
        }
        if (pipe_read_closed(slot->pipe)) {
            m |= EPOLLERR;
        }
        break;
    case FILE_DESCRIPTOR_PIPE_READ:
        if (pipe_write_closed(slot->pipe) && pipe_buffered(slot->pipe) <= 0) {
            m |= EPOLLHUP;
        }
        break;
    case FILE_DESCRIPTOR_SOCKET: {
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
    case FILE_DESCRIPTOR_UNIX:
        if (unix_socket_writable(slot->un)) {
            m |= EPOLLOUT;
        }
        if (unix_socket_hup(slot->un)) {
            m |= EPOLLHUP;
        } else if (unix_socket_rdhup(slot->un)) {
            m |= EPOLLRDHUP;
        }
        break;
    case FILE_DESCRIPTOR_EVENT:
        if (eventfd_writable(slot->event)) {
            m |= EPOLLOUT;
        }
        break;
    case FILE_DESCRIPTOR_TIMER:
    case FILE_DESCRIPTOR_EPOLL:
        break;
    default:
        break;
    }
    return m;
}

static uint32_t epoll_mask_callback(void *context, int fd, const void *object) {
    return file_descriptor_epoll_mask_for((task_t *)context, fd, object);
}

static long install_file_descriptor_of(file_descriptor_type_t type, void *object, uint64_t flags) {
    task_t *self = scheduler_current();
    int fd = alloc_file_descriptor(self);
    if (fd < 0) {
        switch (type) {
        case FILE_DESCRIPTOR_EVENT: eventfd_unref((struct eventfd *)object); break;
        case FILE_DESCRIPTOR_TIMER: timerfd_unref((struct timerfd *)object); break;
        case FILE_DESCRIPTOR_EPOLL: epoll_unref((struct epoll *)object); break;
        default: break;
        }
        return -1;
    }
    self->descriptor_table->slots[fd].type = type;
    self->descriptor_table->slots[fd].event = (struct eventfd *)object;
    self->descriptor_table->slots[fd].cloexec = (flags & OS_FILE_DESCRIPTOR_CLOEXEC) ? 1 : 0;
    self->descriptor_table->slots[fd].nonblock = (flags & OS_FILE_DESCRIPTOR_NONBLOCK) ? 1 : 0;
    return fd;
}

static long sys_eventfd(uint64_t initval, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (flags & ~(uint64_t)(OS_EFD_SEMAPHORE | OS_FILE_DESCRIPTOR_NONBLOCK | OS_FILE_DESCRIPTOR_CLOEXEC)) {
        return -1;
    }
    struct eventfd *e = eventfd_create(initval, (flags & OS_EFD_SEMAPHORE) != 0);
    return e ? install_file_descriptor_of(FILE_DESCRIPTOR_EVENT, e, flags) : -1;
}

static long sys_timerfd_create(uint64_t clockid, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (flags & ~(uint64_t)(OS_FILE_DESCRIPTOR_NONBLOCK | OS_FILE_DESCRIPTOR_CLOEXEC)) {
        return -1;
    }
    struct timerfd *t = timerfd_create((int)clockid);
    return t ? install_file_descriptor_of(FILE_DESCRIPTOR_TIMER, t, flags) : -1;
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

static long sys_timerfd_settime(uint64_t fd, uint64_t flags, uint64_t new_pointer, uint64_t old_pointer, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    os_itimer_t want;
    if (copy_from_user(&want, new_pointer, sizeof(want)) != 0) {
        return -1;
    }
    task_t *self = scheduler_current();
    if (fd >= MAX_FILE_DESCRIPTORS || self->descriptor_table->slots[fd].type != FILE_DESCRIPTOR_TIMER) {
        return -1;
    }
    if (flags & ~(uint64_t)OS_TFD_ABSTIME) {
        return -1;
    }
    struct timerfd *t = self->descriptor_table->slots[fd].timer;
    int absolute = (flags & OS_TFD_ABSTIME) != 0;
    os_itimer_t had = {0, 0};
    if (timerfd_settime(t, timer_clock_ns(t, absolute), absolute, want.value_ns,
                        want.interval_ns, &had.value_ns, &had.interval_ns) != 0) {
        return -1;
    }
    if (old_pointer && copy_to_user(old_pointer, &had, sizeof(had)) != 0) {
        return -1;
    }
    return 0;
}

static long sys_timerfd_gettime(uint64_t fd, uint64_t out_pointer, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!user_range_ok(out_pointer, sizeof(os_itimer_t), 1)) {
        return -1;
    }
    task_t *self = scheduler_current();
    if (fd >= MAX_FILE_DESCRIPTORS || self->descriptor_table->slots[fd].type != FILE_DESCRIPTOR_TIMER) {
        return -1;
    }
    os_itimer_t out = {0, 0};
    timerfd_gettime(self->descriptor_table->slots[fd].timer, clock_now_ns(), &out.value_ns, &out.interval_ns);
    return copy_to_user(out_pointer, &out, sizeof(out)) == 0 ? 0 : -1;
}

static long sys_epoll_create(uint64_t flags, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (flags & ~(uint64_t)OS_FILE_DESCRIPTOR_CLOEXEC) {
        return -1;
    }
    struct epoll *ep = epoll_create_set();
    return ep ? install_file_descriptor_of(FILE_DESCRIPTOR_EPOLL, ep, flags) : -1;
}

static long sys_epoll_control(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t ev_pointer, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    task_t *self = scheduler_current();
    os_epoll_event_t ev = {0, 0, 0};
    if (op != EPOLL_CTL_DEL && copy_from_user(&ev, ev_pointer, sizeof(ev)) != 0) {
        return -1;
    }
    if (epfd >= MAX_FILE_DESCRIPTORS || self->descriptor_table->slots[epfd].type != FILE_DESCRIPTOR_EPOLL) {
        return -1;
    }
    if (fd >= MAX_FILE_DESCRIPTORS || self->descriptor_table->slots[fd].type == FILE_DESCRIPTOR_NONE) {
        return -1;
    }
    if (self->descriptor_table->slots[fd].type == FILE_DESCRIPTOR_EPOLL) {
        return -1;
    }
    const void *object = (const void *)self->descriptor_table->slots[fd].pipe;
    return epoll_control_set(self->descriptor_table->slots[epfd].epoll, (int)op, (int)fd, object,
                         ev.events, ev.data);
}

static long sys_epoll_wait(uint64_t epfd, uint64_t out_pointer, uint64_t maxevents, uint64_t timeout_ms, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    task_t *self = scheduler_current();
    if (maxevents == 0 || maxevents > EPOLL_MAX_WATCH) {
        return -1;
    }
    if (!user_range_ok(out_pointer, maxevents * sizeof(os_epoll_event_t), 1)) {
        return -1;
    }
    if (epfd >= MAX_FILE_DESCRIPTORS || self->descriptor_table->slots[epfd].type != FILE_DESCRIPTOR_EPOLL) {
        return -1;
    }
    struct epoll *ep = self->descriptor_table->slots[epfd].epoll;
    long timeout = (long)timeout_ms;
    uint64_t now = pit_get_ticks() * (1000 / PIT_HZ);
    uint64_t deadline = (timeout < 0) ? 0 : now + (uint64_t)timeout;
    epoll_ev_t evs[EPOLL_MAX_WATCH];
    for (;;) {
        uint64_t seq = scheduler_event_sequence();
        int n = epoll_scan(ep, epoll_mask_callback, self, evs, (int)maxevents);
        if (n > 0) {
            if (copy_to_user(out_pointer, evs, (size_t)n * sizeof(epoll_ev_t)) != 0) {
                return -1;
            }
            return n;
        }
        if (timeout == 0) {
            return 0;
        }
        if (scheduler_signal_pending()) {
            return -OS_ERROR_INTR;
        }
        now = pit_get_ticks() * (1000 / PIT_HZ);
        if (timeout > 0 && now >= deadline) {
            return 0;
        }
        uint64_t park_until = deadline;
        for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
            if (self->descriptor_table->slots[i].type != FILE_DESCRIPTOR_TIMER) {
                continue;
            }
            long ms = timerfd_next_ms(self->descriptor_table->slots[i].timer, clock_now_ns());
            if (ms < 0) {
                continue;
            }
            uint64_t when = now + (uint64_t)ms;
            if (park_until == 0 || when < park_until) {
                park_until = when;
            }
        }
        scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, park_until, seq);
    }
}

static long sys_waitfds(uint64_t file_descriptors_pointer, uint64_t count, uint64_t timeout_ms,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (count > MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    if (count > 0 && !user_range_ok(file_descriptors_pointer, count * sizeof(int), 0)) {
        return -1;
    }
    const int *file_descriptors = (const int *)file_descriptors_pointer;
    task_t *self = scheduler_current();

    long timeout = (long)timeout_ms;
    uint64_t now = pit_get_ticks() * (1000 / PIT_HZ);
    uint64_t deadline = (timeout < 0) ? 0 : now + (uint64_t)timeout;

    for (;;) {
        uint64_t seq = scheduler_event_sequence();
        for (uint64_t i = 0; i < count; i++) {
            if (file_descriptor_is_ready(self, file_descriptors[i])) {
                return (long)i;
            }
        }
        if (timeout == 0) {
            return -2;
        }
        if (deadline != 0 && pit_get_ticks() * (1000 / PIT_HZ) >= deadline) {
            return -2;
        }

        scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, deadline, seq);
        if (scheduler_signal_pending()) {
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
    return (long)scheduler_idle_ticks((int)cpu);
}

static long sys_kernel_log(uint64_t from, uint64_t buffer, uint64_t max, uint64_t next_out,
                      uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    if (!has_cap(CAP_SYSLOG)) {
        return -1;
    }
    if (max == 0 || !user_range_ok(buffer, max, 1)) {
        return -1;
    }
    if (next_out && !user_range_ok(next_out, sizeof(uint64_t), 1)) {
        return -1;
    }
    uint64_t next = 0;
    size_t n = kernel_log_read(from, (char *)buffer, (size_t)max, &next);
    if (next_out) {
        *(uint64_t *)next_out = next;
    }
    return (long)n;
}

static long sys_kernel_log_total(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return (long)kernel_log_written_total();
}

static long sys_sigaction(uint64_t signo, uint64_t handler, uint64_t restorer,
                           uint64_t flags, uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    if (!SIG_IS_CATCHABLE((int)signo)) {
        return -1;
    }
    task_t *self = scheduler_current();
    if (handler != SIG_DFL_ADDR && handler != SIG_IGN_ADDR) {
        if (handler < USER_REGION_BASE || handler >= USER_REGION_LIMIT) {
            return -1;
        }
        if (restorer < USER_REGION_BASE || restorer >= USER_REGION_LIMIT) {
            return -1;
        }
        self->sig_restorer = restorer;
    }
    long previous = (long)self->sig_handler[signo];
    self->sig_handler[signo] = handler;
    if (flags & SA_SIGINFO) {
        self->sig_siginfo |= (1u << signo);
    } else {
        self->sig_siginfo &= ~(1u << signo);
    }
    if (flags & SA_ONSTACK) {
        self->sig_onstack |= (1u << signo);
    } else {
        self->sig_onstack &= ~(1u << signo);
    }
    if (handler == SIG_IGN_ADDR || handler == SIG_DFL_ADDR) {
        self->sig_pending &= ~(1u << signo);
    }
    return previous;
}

/* An alternate stack for signal delivery. The point of it is the one case
   the ordinary path cannot serve: a SIGSEGV raised BY the stack, where there
   is no room below rsp to build a frame. */
static long sys_sigaltstack(uint64_t new_pointer, uint64_t old_pointer,
                            uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    task_t *self = scheduler_current();
    os_stack_t current;
    current.base = self->sig_alt_stack_base;
    current.size = self->sig_alt_stack_size;
    current.flags = 0;
    if (self->sig_alt_stack_base == 0) {
        current.flags |= OS_SS_DISABLE;
    } else if (self->sig_on_alt_stack) {
        current.flags |= OS_SS_ONSTACK;
    }
    current.reserved = 0;

    if (new_pointer) {
        if (!user_range_ok(new_pointer, sizeof(os_stack_t), 0)) {
            return -1;
        }
        /* Changing it while a handler is running on it would move the ground
           out from under that handler. */
        if (self->sig_on_alt_stack) {
            return -1;
        }
        os_stack_t want;
        if (copy_from_user(&want, new_pointer, sizeof(want)) != 0) {
            return -1;
        }
        if (want.flags & OS_SS_DISABLE) {
            self->sig_alt_stack_base = 0;
            self->sig_alt_stack_size = 0;
        } else {
            if (want.flags != 0) {
                return -1;
            }
            if (want.size < OS_MINSIGSTKSZ) {
                return -1;
            }
            if (!user_range_ok(want.base, want.size, 1)) {
                return -1;
            }
            self->sig_alt_stack_base = want.base;
            self->sig_alt_stack_size = want.size;
        }
    }

    if (old_pointer) {
        if (!user_range_ok(old_pointer, sizeof(os_stack_t), 1) ||
            copy_to_user(old_pointer, &current, sizeof(current)) != 0) {
            return -1;
        }
    }
    return 0;
}

#define USER_RFLAGS_MASK 0x0000000000000CD5ULL
#define RFLAGS_IF        0x0000000000000200ULL

static long sys_sigreturn(uint64_t frame_pointer, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6);

static long sys_sigprocmask(uint64_t how, uint64_t mask, uint64_t old_out,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = scheduler_current();
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

static long sys_chdir(uint64_t path_pointer, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char path[LEANFS_MAX_PATH];
    if (copy_path_from_user(path, path_pointer) != 0) {
        return -1;
    }
    if (!virtual_file_system_is_directory(path)) {
        return -1;
    }
    task_t *self = scheduler_vm_owner(scheduler_current());
    int i = 0;
    for (; path[i] && i < PATH_MAX_LENGTH - 1; i++) {
        self->cwd[i] = path[i];
    }
    self->cwd[i] = '\0';
    return 0;
}

static long sys_getcwd(uint64_t buffer, uint64_t maxlen, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = scheduler_vm_owner(scheduler_current());
    const char *cwd = (self->cwd[0] == '/') ? self->cwd : "/";
    uint64_t length = 0;
    while (cwd[length]) {
        length++;
    }
    if (maxlen < length + 1) {
        return -1;
    }
    if (copy_to_user(buffer, cwd, length + 1) != 0) {
        return -1;
    }
    return (long)length;
}

static long sys_rename_replace(uint64_t old_pointer, uint64_t new_pointer, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    char old_path[LEANFS_MAX_PATH];
    char new_path[LEANFS_MAX_PATH];
    if (copy_write_path_from_user(old_path, old_pointer) != 0 ||
        copy_write_path_from_user(new_path, new_pointer) != 0) {
        return -1;
    }
    long r = virtual_file_system_rename_replace(old_path, new_path);
    pkg_note_write(old_path, r);
    pkg_note_write(new_path, r);
    return r;
}

static long sys_fork(isr_regs_t *regs) {
    task_t *parent = scheduler_current();
    if (!parent || parent->pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return -1;
    }

    /* M83 refused this from a process with more than one thread and named the
       condition: clearing a writable bit invalidates only this core's TLB, so
       a sibling on another core would keep writing to a page the child was
       promised is private. M165 is what asked - //content's launcher forks
       from a browser process that has already started its thread pool, which
       is what every multi-process program does - and the answer is the
       shootdown M83 said it needed rather than a weaker fork.

       The mappings belong to the address space rather than to the thread that
       happens to be calling, so the shared range released here is the owner's.
       A thread's own mmap table is empty and releasing it would free nothing
       while fork went on to duplicate the framebuffer. */
    task_t *vm_owner = scheduler_vm_owner(parent);
    scheduler_release_shared_range(vm_owner, USER_MMAP_BASE, USER_MMAP_LIMIT);
    smp_tlb_shootdown();

    uint64_t child_pml4 = process_fork_address_space(parent->pml4_phys);
    if (child_pml4 == 0) {
        return -1;
    }

    /* Every writable page the parent still has is copy-on-write now. Until
       each sibling has discarded what it cached, one of them can still write
       through the old translation - so the child is not handed out until they
       have. */
    smp_tlb_shootdown();

    task_t *child = task_fork(child_pml4, regs);
    if (!child) {
        process_destroy_address_space(child_pml4);
        return -1;
    }

    if (vm_owner->env_block && vm_owner->env_length && vm_owner->env_count) {
        if (scheduler_set_env(child, vm_owner->env_block, vm_owner->env_length,
                              vm_owner->env_count) != 0) {
            scheduler_raise_signal(child, SIGKILL);
            return -1;
        }
    }

    return (long)child->id;
}

static long sys_fcntl(uint64_t fd, uint64_t command, uint64_t arg, uint64_t a4,
                      uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = scheduler_current();
    if (fd >= MAX_FILE_DESCRIPTORS || self->descriptor_table->slots[fd].type == FILE_DESCRIPTOR_NONE) {
        return -1;
    }
    switch (command) {
    case F_GETFD_COMMAND:
        return self->descriptor_table->slots[fd].cloexec ? FILE_DESCRIPTOR_CLOEXEC_BIT : 0;
    case F_SETFD_COMMAND:
        self->descriptor_table->slots[fd].cloexec = (arg & FILE_DESCRIPTOR_CLOEXEC_BIT) ? 1 : 0;
        return 0;
    case F_GETFL_COMMAND: {
        long access;
        switch (self->descriptor_table->slots[fd].type) {
        case FILE_DESCRIPTOR_STDIN:      access = OPEN_READ; break;
        case FILE_DESCRIPTOR_STDOUT:     access = OPEN_WRITE; break;
        case FILE_DESCRIPTOR_PIPE_READ:  access = OPEN_READ; break;
        case FILE_DESCRIPTOR_PIPE_WRITE: access = OPEN_WRITE; break;
        case FILE_DESCRIPTOR_FILE:
            access = OPEN_READ | (self->descriptor_table->slots[fd].file->writable ? OPEN_WRITE : 0);
            break;
        case FILE_DESCRIPTOR_SOCKET:     access = OPEN_READ | OPEN_WRITE; break;
        case FILE_DESCRIPTOR_UNIX:       access = OPEN_READ | OPEN_WRITE; break;
        case FILE_DESCRIPTOR_EVENT:      access = OPEN_READ | OPEN_WRITE; break;
        case FILE_DESCRIPTOR_TIMER:      access = OPEN_READ; break;
        case FILE_DESCRIPTOR_EPOLL:      access = OPEN_READ; break;
        case FILE_DESCRIPTOR_MEMFD:
            access = OPEN_READ |
                     (self->descriptor_table->slots[fd].writable ? OPEN_WRITE : 0);
            break;
        default:            return -1;
        }
        return access | (self->descriptor_table->slots[fd].nonblock ? OS_NONBLOCK_BIT : 0);
    }
    case F_SETFL_COMMAND:
        self->descriptor_table->slots[fd].nonblock = (arg & OS_NONBLOCK_BIT) ? 1 : 0;
        return 0;
    case F_GETLK_COMMAND:
    case F_SETLK_COMMAND:
    case F_SETLKW_COMMAND: {
        os_flock_t request;
        if (self->descriptor_table->slots[fd].type != FILE_DESCRIPTOR_FILE ||
            copy_from_user(&request, arg, sizeof(request)) != 0) {
            return -1;
        }
        uint32_t ino = slot_inode(&self->descriptor_table->slots[fd]);
        if (ino == 0) {
            return -1;
        }
        int64_t start = request.start;
        int64_t length = request.length;
        if (request.whence == 1) {
            start += (int64_t)self->descriptor_table->slots[fd].file->offset;
        } else if (request.whence == 2) {
            start += (int64_t)virtual_file_system_handle_size(self->descriptor_table->slots[fd].file->handle);
        } else if (request.whence != 0) {
            return -1;
        }
        if (length < 0) {
            start += length;
            length = -length;
        }
        if (start < 0 || (request.type != OS_FLOCK_RD && request.type != OS_FLOCK_WR &&
                          request.type != OS_FLOCK_UNLCK)) {
            return -1;
        }
        if (command == F_GETLK_COMMAND) {
            os_flock_t ans;
            flock_test(ino, self->id, request.type, (uint64_t)start, (uint64_t)length, &ans);
            return copy_to_user(arg, &ans, sizeof(ans)) == 0 ? 0 : -1;
        }
        for (;;) {
            uint64_t seq = scheduler_event_sequence();
            int r = flock_set(ino, self->id, request.type, (uint64_t)start, (uint64_t)length);
            if (r == 0) {
                if (request.type == OS_FLOCK_UNLCK) {
                    scheduler_wake_all(FLOCK_CHAN);
                }
                return 0;
            }
            if (r != FLOCK_CONFLICT || command == F_SETLK_COMMAND) {
                return r;
            }
            scheduler_block_on_sequence(FLOCK_CHAN, 0, seq);
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

static long sys_waitpid(uint64_t pid_argument, uint64_t status_pointer, uint64_t options,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *self = scheduler_current();
    int64_t want = (int64_t)pid_argument;
    int want_pgid = 0;
    if (want < -1) {
        want_pgid = (int)(-want);
    } else if (want == 0) {
        want_pgid = self->pgid;
    }
    if (status_pointer && !user_range_ok(status_pointer, sizeof(int), 1)) {
        return -1;
    }

    for (;;) {
        int any_children = 0;
        uint64_t seq = scheduler_event_sequence();
        task_t *only = (task_t *)0;

        if (want > 0) {
            task_t *t = scheduler_task_by_id((int)want);
            if (!t || t->reaped || t->parent_id != self->id) {
                return -1;
            }
            any_children = 1;
            only = t;
            if (t->state == TASK_TERMINATED) {
                int pid = t->id;
                int status = wait_status_of(t);
                t->reaped = 1;
                scheduler_reap_slot(t);
                if (status_pointer) {
                    (void)copy_to_user(status_pointer, &status, sizeof(status));
                }
                return pid;
            }
            if (stop_to_report(t, options)) {
                int status = stop_status_of(t);
                t->stop_reported = 1;
                if (status_pointer) {
                    (void)copy_to_user(status_pointer, &status, sizeof(status));
                }
                return t->id;
            }
        } else {
            int total = scheduler_task_count();
            for (int i = 0; i < total; i++) {
                task_t *t = scheduler_task_by_slot(i);
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
                    scheduler_reap_slot(t);
                    if (status_pointer) {
                        (void)copy_to_user(status_pointer, &status, sizeof(status));
                    }
                    return pid;
                }
                if (stop_to_report(t, options)) {
                    int status = stop_status_of(t);
                    t->stop_reported = 1;
                    if (status_pointer) {
                        (void)copy_to_user(status_pointer, &status, sizeof(status));
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
            scheduler_block_on_sequence((const void *)only,
                               pit_get_ticks() * (1000 / PIT_HZ) + 200, seq);
        } else {
            scheduler_block_on_sequence(SCHEDULER_POLL_CHAN,
                               pit_get_ticks() * (1000 / PIT_HZ) + 50, seq);
        }
    }
}

static long sys_execve(isr_regs_t *regs) {
    task_t *self = scheduler_current();
    if (!self || self->pml4_phys == virtual_memory_kernel_pml4_phys()) {
        return -1;
    }
    if (scheduler_count_sharing_address_space(self->pml4_phys) > 1) {
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
        size_t length = k_strlen(path) + 1;
        k_memcpy(v.argbuf, path, length);
        v.argv[0] = v.argbuf;
        v.argv[1] = (const char *)0;
        v.argc = 1;
    }

    leanfs_stat_t st;
    if (virtual_file_system_stat(path, &st) != 0 || st.is_directory) {
        free_vectors(&v);
        return -1;
    }
    uint8_t *image = (uint8_t *)kmalloc(st.size ? st.size : 1);
    if (!image) {
        free_vectors(&v);
        return -1;
    }
    int64_t size = virtual_file_system_read(path, image, st.size);
    if (size < 2 || (image[0] == '#' && image[1] == '!') ||
        !elf_validate(image, (size_t)size)) {
        kfree(image);
        free_vectors(&v);
        return -1;
    }

    const char *inherited[USER_ENV_MAX_VARS + 1];
    const char *const *effective = v.envp;
    task_t *owner = scheduler_vm_owner(self);
    if (!effective && owner && owner->env_block && owner->env_count) {
        uint32_t n = 0;
        uint32_t off = 0;
        while (off < owner->env_length && n < USER_ENV_MAX_VARS) {
            inherited[n++] = owner->env_block + off;
            while (off < owner->env_length && owner->env_block[off]) {
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
    uint64_t peak = virtual_memory_rss_peak_pages(old_pml4);
    if (peak > self->max_rss_pages) {
        self->max_rss_pages = peak;
    }
    self->pml4_phys = new_pml4;
    virtual_memory_switch_address_space(new_pml4);
    process_destroy_address_space(old_pml4);

    self->heap_brk = USER_HEAP_START;
    self->heap_mapped_end = USER_HEAP_START;
    self->shared_memory_next_vaddr = USER_SHARED_MEMORY_BASE;
    scheduler_regions_forget_memfds(self);
    /* An exec keeps nothing of the old address space, so the table goes back
       to the heap rather than being cleared in place: the program replacing
       this one may map nothing at all, and the one being replaced may have
       grown the table to megabytes. */
    scheduler_regions_release(self);
    self->fs_base = 0;

    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
        if (self->descriptor_table->slots[i].cloexec) {
            drop_record_locks(self, &self->descriptor_table->slots[i]);
            file_descriptor_release(&self->descriptor_table->slots[i]);
            self->descriptor_table->slots[i].type = FILE_DESCRIPTOR_NONE;
        }
    }

    for (int i = 0; i <= SIG_MAX; i++) {
        if (self->sig_handler[i] != SIG_IGN_ADDR) {
            self->sig_handler[i] = SIG_DFL_ADDR;
        }
    }
    self->sig_restorer = 0;
    self->sig_siginfo = 0;
    self->sig_onstack = 0;
    /* The new program's address space is not the old one's, so an alternate
       stack registered by what came before is an address it does not own. */
    self->sig_alt_stack_base = 0;
    self->sig_alt_stack_size = 0;
    self->sig_on_alt_stack = 0;
    self->si_pid = 0;
    self->si_status = 0;
    self->si_address = 0;

    const char *base = path;
    for (const char *c = path; *c; c++) {
        if (*c == '/') {
            base = c + 1;
        }
    }
    scheduler_set_task_name(self, base);

    self->caps &= caps_for_spawn_path(path);

    if (v.envp) {
        char *packed = (char *)kmalloc(USER_ENV_MAX_BYTES);
        if (packed) {
            uint32_t length = 0;
            uint32_t count = 0;
            for (int i = 0; v.envv[i] && count < USER_ENV_MAX_VARS; i++) {
                uint32_t n = (uint32_t)k_strlen(v.envv[i]) + 1;
                if (length + n > USER_ENV_MAX_BYTES) {
                    break;
                }
                k_memcpy(packed + length, v.envv[i], n);
                length += n;
                count++;
            }
            scheduler_set_env(self, packed, length, count);
            kfree(packed);
        }
    }
    free_vectors(&v);

    uint64_t cs = regs->cs;
    uint64_t ss = regs->ss;
    k_memset(regs, 0, sizeof(*regs));
    regs->rip = entry;
    regs->rsp = USER_STACK_TOP;
    regs->rdi = USER_ARGUMENT_ADDRESS;
    regs->cs = cs;
    regs->ss = ss;
    regs->rflags = 0x202;
    regs->vector = 0x80;
    return 0;
}

static long sys_getrandom(uint64_t buffer, uint64_t length, uint64_t flags,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (flags & ~(uint64_t)(GRND_NONBLOCK_BIT | GRND_RANDOM_BIT)) {
        return -1;
    }
    if (!user_range_ok(buffer, length, 1)) {
        return -1;
    }
    uint8_t chunk[256];
    uint64_t done = 0;
    while (done < length) {
        size_t n = length - done > sizeof(chunk) ? sizeof(chunk) : (size_t)(length - done);
        random_bytes(chunk, n);
        if (copy_to_user(buffer + done, chunk, n) != 0) {
            return -1;
        }
        done += n;
    }
    k_memset(chunk, 0, sizeof(chunk));
    return (long)done;
}

static const syscall_function_t syscall_table[SYSCALL_COUNT] = {
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
    [SYS_shared_memory_create] = sys_shared_memory_create,
    [SYS_shared_memory_map] = sys_shared_memory_map,
    [SYS_framebuffer_info] = sys_framebuffer_info,
    [SYS_framebuffer_map] = sys_framebuffer_map,
    [SYS_mouse_read] = sys_mouse_read,
    [SYS_pipe_open] = sys_pipe_open,
    [SYS_keyboard_read] = sys_keyboard_read,
    [SYS_pipe_poll] = sys_pipe_poll,
    [SYS_uptime_ms] = sys_uptime_ms,
    [SYS_dup2] = sys_dup2,
    [SYS_wait_nb] = sys_wait_nb,
    [SYS_yield] = sys_yield,
    [SYS_task_alive] = sys_task_alive,
    [SYS_pipe_reset] = sys_pipe_reset,
    [SYS_keyboard_modifiers] = sys_keyboard_modifiers,
    [SYS_clipboard_set] = sys_clipboard_set,
    [SYS_clipboard_get] = sys_clipboard_get,
    [SYS_writefile] = sys_writefile,
    [SYS_taskinfo] = sys_taskinfo,
    [SYS_shutdown] = sys_shutdown,
    [SYS_close] = sys_close,
    [SYS_shared_memory_free] = sys_shared_memory_free,
    [SYS_mkdir] = sys_mkdir,
    [SYS_shared_memory_unmap] = sys_shared_memory_unmap,
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
    [SYS_receive] = sys_receive,
    [SYS_kernel_log] = sys_kernel_log,
    [SYS_kernel_log_total] = sys_kernel_log_total,
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
    [SYS_mincore] = sys_mincore,
    [SYS_sockname] = sys_sockname,
    [SYS_munmap] = sys_munmap,
    [SYS_thread_create] = sys_thread_create,
    [SYS_thread_setname] = sys_thread_setname,
    [SYS_thread_getname] = sys_thread_getname,
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
    [SYS_unix_peer_credentials] = sys_unix_peer_credentials,
    [SYS_sigaltstack] = sys_sigaltstack,
    [SYS_bindun] = sys_bindun,
    [SYS_connectun] = sys_connectun,
    [SYS_sendmsg] = sys_sendmsg,
    [SYS_recvmsg] = sys_recvmsg,
    [SYS_sockshut] = sys_sockshut,
    [SYS_epoll_create] = sys_epoll_create,
    [SYS_epoll_control] = sys_epoll_control,
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
    case SYS_receive:
        return 1;
    default:
        return 0;
    }
}

static long sys_sigreturn(uint64_t frame_pointer, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)frame_pointer;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return -1;
}

/* Whether a stack pointer is inside the alternate stack. Recomputed rather
   than carried in sig_frame_t, because that frame is ABI this libc's
   sigreturn trampoline already lays out - and the address answers the
   question exactly, including for a handler that nested. */
static int on_alt_stack(const task_t *t, uint64_t sp) {
    return t->sig_alt_stack_base != 0 && sp >= t->sig_alt_stack_base &&
           sp < t->sig_alt_stack_base + t->sig_alt_stack_size;
}

static int signal_deliver(isr_regs_t *regs) {
    task_t *self = scheduler_current();
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
            info.si_addr = (void *)self->si_address;
            /* What the hardware said, where the fault path worked it out;
               SI_KERNEL only where it did not. */
            info.si_code = self->si_fault_code ? self->si_fault_code : SI_KERNEL;
        } else if (signo == SIGFPE || signo == SIGILL) {
            info.si_addr = (void *)self->si_address;
            info.si_code = self->si_fault_code ? self->si_fault_code : SI_KERNEL;
        } else {
            info.si_code = SI_USER;
        }
    }

    /* SA_ONSTACK, and only when there is a stack to go to and we are not
       already on it - a nested signal on the alternate stack keeps building
       frames where it is, exactly as it would on the ordinary one. */
    int use_alt = (self->sig_onstack & (1u << signo)) != 0 &&
                  self->sig_alt_stack_base != 0 && !self->sig_on_alt_stack;
    uint64_t sp = use_alt ? self->sig_alt_stack_base + self->sig_alt_stack_size
                          : regs->rsp;
    sp &= ~15ULL;
    /* The red zone is the caller's, below the interrupted rsp. A fresh
       alternate stack has no caller and so has none to skip. */
    if (!use_alt) {
        sp -= 128;
    }
    if (want_info) {
        sp -= sizeof(siginfo_t);
        sp &= ~15ULL;
    }
    uint64_t info_address = sp;
    sp -= sizeof(os_ucontext_t);
    sp &= ~15ULL;
    uint64_t ucontext_address = sp;
    sp -= sizeof(sig_frame_t);
    sp &= ~15ULL;
    uint64_t frame_address = sp;
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
    frame.ucontext_address = ucontext_address;

    /* A real ucontext, not a null third argument. The registers below are
       the ones the handler interrupted, and sys_sigreturn reads them back
       out of this structure - so a handler that changes uc_mcontext changes
       where it returns to, which is what makes this a context rather than a
       description of one. A sampling profiler is the caller that needs it:
       it reads RIP, RSP and RBP here and walks the stack from them. */
    os_ucontext_t context;
    k_memset(&context, 0, sizeof(context));
    context.uc_flags = 0;
    context.uc_link = 0;
    context.uc_stack.ss_sp = self->sig_alt_stack_base;
    context.uc_stack.ss_size = self->sig_alt_stack_size;
    context.uc_stack.ss_flags = self->sig_alt_stack_base == 0
                                    ? OS_SS_DISABLE
                                    : (self->sig_on_alt_stack ? OS_SS_ONSTACK : 0);
    context.uc_sigmask = self->sig_blocked;
    context.uc_mcontext.gregs[OS_REG_R8] = (int64_t)regs->r8;
    context.uc_mcontext.gregs[OS_REG_R9] = (int64_t)regs->r9;
    context.uc_mcontext.gregs[OS_REG_R10] = (int64_t)regs->r10;
    context.uc_mcontext.gregs[OS_REG_R11] = (int64_t)regs->r11;
    context.uc_mcontext.gregs[OS_REG_R12] = (int64_t)regs->r12;
    context.uc_mcontext.gregs[OS_REG_R13] = (int64_t)regs->r13;
    context.uc_mcontext.gregs[OS_REG_R14] = (int64_t)regs->r14;
    context.uc_mcontext.gregs[OS_REG_R15] = (int64_t)regs->r15;
    context.uc_mcontext.gregs[OS_REG_RDI] = (int64_t)regs->rdi;
    context.uc_mcontext.gregs[OS_REG_RSI] = (int64_t)regs->rsi;
    context.uc_mcontext.gregs[OS_REG_RBP] = (int64_t)regs->rbp;
    context.uc_mcontext.gregs[OS_REG_RBX] = (int64_t)regs->rbx;
    context.uc_mcontext.gregs[OS_REG_RDX] = (int64_t)regs->rdx;
    context.uc_mcontext.gregs[OS_REG_RAX] = (int64_t)regs->rax;
    context.uc_mcontext.gregs[OS_REG_RCX] = (int64_t)regs->rcx;
    context.uc_mcontext.gregs[OS_REG_RSP] = (int64_t)regs->rsp;
    context.uc_mcontext.gregs[OS_REG_RIP] = (int64_t)regs->rip;
    context.uc_mcontext.gregs[OS_REG_EFL] = (int64_t)regs->rflags;
    context.uc_mcontext.gregs[OS_REG_CSGSFS] = (int64_t)(regs->cs & 0xFFFF);
    context.uc_mcontext.gregs[OS_REG_CR2] = (int64_t)self->si_address;
    context.uc_mcontext.fpregs = 0;

    if (copy_to_user(frame_address, &frame, sizeof(frame)) != 0) {
        self->sig_pending &= ~(1u << signo);
        return 0;
    }
    uint64_t ret_address = self->sig_restorer;
    if (copy_to_user(new_rsp, &ret_address, sizeof(ret_address)) != 0) {
        self->sig_pending &= ~(1u << signo);
        return 0;
    }

    if (want_info && copy_to_user(info_address, &info, sizeof(info)) != 0) {
        self->sig_pending &= ~(1u << signo);
        return 0;
    }
    if (copy_to_user(ucontext_address, &context, sizeof(context)) != 0) {
        self->sig_pending &= ~(1u << signo);
        return 0;
    }

    self->sig_pending &= ~(1u << signo);
    self->sig_blocked |= (1u << signo);

    self->sig_on_alt_stack = (uint8_t)on_alt_stack(self, new_rsp);

    regs->rsp = new_rsp;
    regs->rip = handler;
    regs->rdi = (uint64_t)signo;
    regs->rsi = want_info ? info_address : 0;
    regs->rdx = ucontext_address;
    regs->rax = 0;
    return 1;
}

int signal_deliver_fault_with_code(isr_regs_t *regs, int signo,
                                   uint64_t fault_address, int fault_code) {
    task_t *self = scheduler_current();
    if (self) {
        self->si_fault_code = fault_code;
    }
    return signal_deliver_fault(regs, signo, fault_address);
}

int signal_deliver_fault(isr_regs_t *regs, int signo, uint64_t fault_address) {
    task_t *self = scheduler_current();
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
    self->si_address = fault_address;
    self->sig_pending |= (1u << signo);
    return signal_deliver(regs);
}

static int signal_return(isr_regs_t *regs) {
    task_t *self = scheduler_current();
    uint64_t frame_address = regs->rdi;
    sig_frame_t frame;
    if ((regs->cs & 3) != 3) {
        return -1;
    }
    if (copy_from_user(&frame, frame_address, sizeof(frame)) != 0) {
        return -1;
    }
    /* The ucontext is what the handler was given and what it may have
       edited, so it is the authority here; the frame's own copy is the
       fallback for a frame this kernel did not write a ucontext for. */
    os_ucontext_t context;
    int have_context = frame.ucontext_address != 0 &&
                       copy_from_user(&context, frame.ucontext_address,
                                      sizeof(context)) == 0;
    uint64_t rip = have_context ? (uint64_t)context.uc_mcontext.gregs[OS_REG_RIP]
                                : frame.rip;
    uint64_t rsp = have_context ? (uint64_t)context.uc_mcontext.gregs[OS_REG_RSP]
                                : frame.rsp;
    if (rip < USER_REGION_BASE || rip >= USER_REGION_LIMIT) {
        return -1;
    }
    if (rsp < USER_REGION_BASE || rsp >= USER_REGION_LIMIT) {
        return -1;
    }
    if (have_context) {
        regs->rax = (uint64_t)context.uc_mcontext.gregs[OS_REG_RAX];
        regs->rbx = (uint64_t)context.uc_mcontext.gregs[OS_REG_RBX];
        regs->rcx = (uint64_t)context.uc_mcontext.gregs[OS_REG_RCX];
        regs->rdx = (uint64_t)context.uc_mcontext.gregs[OS_REG_RDX];
        regs->rsi = (uint64_t)context.uc_mcontext.gregs[OS_REG_RSI];
        regs->rdi = (uint64_t)context.uc_mcontext.gregs[OS_REG_RDI];
        regs->rbp = (uint64_t)context.uc_mcontext.gregs[OS_REG_RBP];
        regs->r8 = (uint64_t)context.uc_mcontext.gregs[OS_REG_R8];
        regs->r9 = (uint64_t)context.uc_mcontext.gregs[OS_REG_R9];
        regs->r10 = (uint64_t)context.uc_mcontext.gregs[OS_REG_R10];
        regs->r11 = (uint64_t)context.uc_mcontext.gregs[OS_REG_R11];
        regs->r12 = (uint64_t)context.uc_mcontext.gregs[OS_REG_R12];
        regs->r13 = (uint64_t)context.uc_mcontext.gregs[OS_REG_R13];
        regs->r14 = (uint64_t)context.uc_mcontext.gregs[OS_REG_R14];
        regs->r15 = (uint64_t)context.uc_mcontext.gregs[OS_REG_R15];
        regs->rflags = ((uint64_t)context.uc_mcontext.gregs[OS_REG_EFL] &
                        USER_RFLAGS_MASK) | RFLAGS_IF;
        self->sig_blocked = context.uc_sigmask;
    } else {
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
        regs->rflags = (frame.rflags & USER_RFLAGS_MASK) | RFLAGS_IF;
        self->sig_blocked = frame.saved_blocked;
    }
    regs->rip = rip;
    regs->rsp = rsp;
    self->sig_blocked &= ~(1u << SIGKILL) & ~(1u << SIGSEGV);
    self->sig_on_alt_stack = (uint8_t)on_alt_stack(self, rsp);
    return 0;
}

static void syscall_dispatch(isr_regs_t *regs);

void syscall_handler(isr_regs_t *regs) {
    uint64_t num = regs->rax;
    int timing = syscall_counters_timing_enabled();
    uint64_t started = timing ? tsc_read() : 0;

    syscall_dispatch(regs);

    syscall_counters_record((int)num, timing ? (tsc_read() - started) : 0);
}

static void syscall_dispatch(isr_regs_t *regs) {
    task_t *self = scheduler_current();
    if (self->pending_signal != 0) {
        int sig = self->pending_signal;
        self->pending_signal = 0;
        task_exit_with_code(128 + sig);
    }
    scheduler_take_pending_stop_if_any(self);

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
