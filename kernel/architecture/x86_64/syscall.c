#include "syscall_entry.h"

#include <stdint.h>

#include "architecture/x86_64/cpu.h"
#include "architecture/x86_64/symmetric_multiprocessing.h"
#include "architecture/x86_64/timestamp_counter.h"
#include "drivers/ac97.h"
#include "drivers/boot_config.h"
#include "drivers/dispi.h"
#include "drivers/display_scale.h"
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
#include "architecture/x86_64/memfd_region_tag.h"
#include "os_poll.h"
#include "drivers/block_device.h"
#include "memory_management/file_mapping.h"
#include "memory_management/heap.h"
#include "memory_management/physical_memory.h"
#include "memory_management/virtual_memory.h"
#include "power/power.h"
#include "os_file_system.h"
#include "os_resource.h"
#include "process/resource_limits.h"
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
#include "network/wireless_manager.h"
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
    return clock_monotonic_ns();
}

/* What one syscall stages on the KERNEL STACK between a user buffer and a
   socket, in either direction. It is not the socket's buffer size: M173 grew
   the ring from 4 KB to 64 KB for a browser whose IPC had been living on
   partial sends, and three arrays that had been sized by the ring burst a
   32 KB kernel stack into the next task's - the scheduler noticed it was
   standing 33 KB below the stack it thought it was on. Every user of this
   loops or reports a partial count, so a chunk is all it ever needed. */
#define UNIX_STAGING_CHUNK 4096

/* M225 (fd-use-holds): fdget and fdput. Every call that USES a descriptor
   takes it here first - a copy of the slot, with a reference of the call's
   own when another thread shares the table - and gives it back with
   done_with_descriptor on every way out, so a sibling thread's close() in
   the middle of the call cannot free the object under it. What a call does
   with the object goes through the copy it returns, never through the slot
   again: a slot re-read after a wait could name nothing, or something else.
   See file_descriptor_get for what is counted and what a closed descriptor
   means to a call already using it. Null when fd names nothing open. */
static file_descriptor_slot_t *use_descriptor(uint64_t fd, file_descriptor_use_t *use) {
    if (fd >= MAX_FILE_DESCRIPTORS ||
        file_descriptor_get(scheduler_current(), (int)fd, use) != 0) {
        return (file_descriptor_slot_t *)0;
    }
    return &use->slot;
}

/* An uncounted use - a table nobody else names - has nothing to give back,
   and does not pay for finding the current task a second time (an
   interrupts-off section of its own) to learn that. */
static void done_with_descriptor(file_descriptor_use_t *use) {
    if (use->counted) {
        file_descriptor_put(scheduler_current(), use);
    }
}

static int copy_log_piece_from_user(char *to, const char *from, size_t length) {
    return copy_from_user(to, (uint64_t)from, length);
}

static long write_to_descriptor(file_descriptor_slot_t *slot, uint64_t buffer, uint64_t length) {
    const char *s = (const char *)buffer;
    if (slot->type == FILE_DESCRIPTOR_STDOUT) {
        /* Staged, then a piece at a time under one hold of the log's lock.
           It was kernel_log_putc per byte - a lock taken and dropped per
           character - so on eight cores two programs' lines were spliced
           into each other a character at a time, and a battery marker that
           had been printed was never found. */
        return kernel_log_write_from(s, (size_t)length, copy_log_piece_from_user);
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
            scheduler_watch_begin();
            scheduler_watch_add((const void *)slot->event);
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
            scheduler_watch_block(0);
        }
    }
    if (slot->type == FILE_DESCRIPTOR_UNIX) {
        uint64_t sent = 0;
        while (sent < length) {
            uint32_t chunk = (length - sent) > UNIX_STAGING_CHUNK ? UNIX_STAGING_CHUNK : (uint32_t)(length - sent);
            uint8_t staging[UNIX_STAGING_CHUNK];
            if (copy_from_user(staging, buffer + sent, chunk) != 0) {
                return sent ? (long)sent : -1;
            }
            scheduler_watch_begin();
            scheduler_watch_add((const void *)slot->un);
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
                scheduler_watch_block(0);
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
                scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, clock_monotonic_ms() + 10, seq);
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
        if (slot->file->synchronous && virtual_file_system_sync() != 0) {
            return -1;
        }
        return (long)n;
    }
    return -1;
}

static long sys_write(uint64_t fd, uint64_t buffer, uint64_t length, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FILE_DESCRIPTORS || !user_range_ok(buffer, length, 0)) {
        return -1;
    }
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = write_to_descriptor(slot, buffer, length);
    done_with_descriptor(&use);
    return result;
}

static long read_from_descriptor(file_descriptor_slot_t *slot, uint64_t buffer, uint64_t length) {
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
            scheduler_watch_begin();
            scheduler_watch_add((const void *)slot->pipe);
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
                    deadline = clock_deadline_ms((uint64_t)ms);
                }
            }
            scheduler_watch_block(deadline);
        }
    }
    if (slot->type == FILE_DESCRIPTOR_UNIX) {
        uint32_t want = length > UNIX_STAGING_CHUNK ? UNIX_STAGING_CHUNK : (uint32_t)length;
        uint8_t staging[UNIX_STAGING_CHUNK];
        for (;;) {
            scheduler_watch_begin();
            scheduler_watch_add((const void *)slot->un);
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
            scheduler_watch_block(0);
        }
    }
    if (slot->type == FILE_DESCRIPTOR_SOCKET) {
        struct tcpcb *tcb = socket_tcb(slot->sock);
        if (!tcb) {
            return -1;
        }
        if (socket_read_shut(slot->sock)) {
            return 0;
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

static long sys_read(uint64_t fd, uint64_t buffer, uint64_t length, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (fd >= MAX_FILE_DESCRIPTORS || !user_range_ok(buffer, length, 1)) {
        return -1;
    }
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = read_from_descriptor(slot, buffer, length);
    done_with_descriptor(&use);
    return result;
}

/* On success the caller owns `use` and gives it back. */
static long pfile_slot(uint64_t fd, uint64_t buffer, uint64_t length,
                       int64_t offset, int write, file_descriptor_use_t *use) {
    if (fd >= MAX_FILE_DESCRIPTORS || !user_range_ok(buffer, length, write ? 0 : 1)) {
        return -1;
    }
    if (offset < 0) {
        return -1;
    }
    file_descriptor_slot_t *slot = use_descriptor(fd, use);
    if (!slot) {
        return -1;
    }
    long error = 0;
    if (slot->type != FILE_DESCRIPTOR_FILE) {
        error = -OS_ERROR_SPIPE;
    } else if (slot->file->is_directory) {
        error = -1;
    }
    if (error != 0) {
        done_with_descriptor(use);
    }
    return error;
}

static long sys_pread(uint64_t fd, uint64_t buffer, uint64_t length, uint64_t offset,
                      uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    file_descriptor_use_t use;
    long error = pfile_slot(fd, buffer, length, (int64_t)offset, 0, &use);
    if (error != 0) {
        return error;
    }
    int64_t n = virtual_file_system_handle_read(use.slot.file->handle, (char *)buffer,
                                                (size_t)length, (uint32_t)offset);
    done_with_descriptor(&use);
    return n < 0 ? -1 : (long)n;
}

static long sys_pwrite(uint64_t fd, uint64_t buffer, uint64_t length, uint64_t offset,
                       uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    file_descriptor_use_t use;
    long error = pfile_slot(fd, buffer, length, (int64_t)offset, 1, &use);
    if (error != 0) {
        return error;
    }
    const open_file_t *of = use.slot.file;
    long result = -1;
    if (of->writable) {
        int64_t n = virtual_file_system_handle_write(of->handle, (const char *)buffer,
                                                     (size_t)length, (uint32_t)offset);
        if (n >= 0 && !(of->synchronous && virtual_file_system_sync() != 0)) {
            result = (long)n;
        }
    }
    done_with_descriptor(&use);
    return result;
}

static long sys_exit(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    scheduler_begin_exit(scheduler_current());
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
    scheduler_set_thread_name(scheduler_current(), name);
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
    /* M225: the id from the spawn's own record - by now the thread may have
       run, ended and been joined by a sibling, and its slot be another's. */
    return t ? (long)scheduler_last_spawn((uint32_t *)0) : -1;
}

static long sys_thread_detach(uint64_t thread_id, uint64_t a2, uint64_t a3, uint64_t a4,
                              uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return scheduler_detach_thread(scheduler_current(), (int)thread_id);
}

/* M207: the wireless network. Its state is anybody's to read - a clock in
   the taskbar may show it - but the list of networks around the machine says
   where the machine is, and joining one decides where its traffic goes, so
   everything else needs CAP_NETWORK. A password crosses once, into a copy
   that is wiped when the radio's task has turned it into a key. */
static long sys_wireless(uint64_t operation, uint64_t argument, uint64_t count, uint64_t a4,
                         uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (operation == WIRELESS_OPERATION_STATUS) {
        os_wireless_status_t status;
        wireless_status(&status);
        return copy_to_user(argument, &status, sizeof(status)) == 0 ? 0 : -14;
    }
    if (!has_cap(CAP_NETWORK)) {
        return -1;
    }
    switch (operation) {
        case WIRELESS_OPERATION_SCAN:
            return wireless_request_scan();
        case WIRELESS_OPERATION_NETWORKS: {
            os_wireless_network_t rows[WIRELESS_NETWORKS_MAX];
            uint32_t capacity = count > WIRELESS_NETWORKS_MAX ? WIRELESS_NETWORKS_MAX : (uint32_t)count;
            long n = wireless_networks(rows, capacity);
            if (n > 0 && copy_to_user(argument, rows, (uint64_t)n * sizeof(rows[0])) != 0) {
                return -14;
            }
            return n;
        }
        case WIRELESS_OPERATION_CONNECT: {
            os_wireless_connect_t request;
            if (copy_from_user(&request, argument, sizeof(request)) != 0) {
                return -14;
            }
            request.ssid[WIRELESS_SSID_MAX] = 0;
            request.password[WIRELESS_PASSWORD_MAX] = 0;
            long result = wireless_request_connect(&request);
            volatile uint8_t *bytes = (volatile uint8_t *)&request;
            for (uint32_t i = 0; i < sizeof(request); i++) {
                bytes[i] = 0;
            }
            return result;
        }
        case WIRELESS_OPERATION_DISCONNECT:
            return wireless_request_disconnect();
        case WIRELESS_OPERATION_FORGET: {
            os_wireless_network_t network;
            if (copy_from_user(&network, argument, sizeof(network)) != 0) {
                return -14;
            }
            return wireless_request_forget(network.ssid, network.ssid_length);
        }
        default:
            return -22;
    }
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
        long it_id = it ? (long)scheduler_last_spawn((uint32_t *)0) : 0;
        kfree(iimage);
        kfree(arg);
        kfree(envbuf);
        return it ? it_id : SPAWN_ERROR_NO_MEMORY;
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
    /* M225: from the spawn's record, not the task - a sibling thread's
       wait(-1) may already have reaped a child that ended at once. */
    long id = t ? (long)scheduler_last_spawn((uint32_t *)0) : 0;
    kfree(image);
    kfree(arg);
    kfree(envbuf);
    if (!t) {
        return SPAWN_ERROR_NO_MEMORY;
    }
    return id;
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
                if (!t || t->parent_id != self->tgid || t->reaped) {
                    continue;
                }
                any_children = 1;
                if (scheduler_process_has_ended(t)) {
                    t->reaped = 1;
                    int pid = t->id;
                    scheduler_reap_slot(t);
                    return pid;
                }
            }
            if (!any_children) {
                return -1;
            }
            scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, clock_monotonic_ms() + 50, seq);
        }
    }

    task_t *t = scheduler_task_by_id((int)pid_argument);
    /* M225: not the kernel's tasks unless the kernel asks, and not itself. */
    if (!scheduler_may_wait_for(self, t)) {
        return -1;
    }
    /* M206: the slot can be reaped from under this wait - by the sweep a
       spawn runs - and given to a new task, whose id is not this one. Waiting
       on the slot would then wait on the newcomer and reap it. */
    /* M225: until the PROCESS has ended - its leader and every thread it
       had (scheduler_process_has_ended) - not just the task named. */
    while (!scheduler_process_has_ended(t) && t->id == (int)pid_argument) {
        uint64_t seq = scheduler_event_sequence();
        if (scheduler_process_has_ended(t) || t->id != (int)pid_argument) {
            break;
        }
        scheduler_block_on_sequence((const void *)t, clock_monotonic_ms() + 200, seq);
    }
    if (t->id != (int)pid_argument) {
        return -1;
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
        if (up->lineage_id == self->tgid) {
            return 1;
        }
        up = up->lineage_id >= 0 ? scheduler_task_by_id(up->lineage_id) : (task_t *)0;
    }
    return has_cap(CAP_KILL_ANY);
}

static long sys_kill(uint64_t pid, uint64_t sig, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    /* M224: three refusals, told apart. They were all -1, and kill(2)'s
       caller asks which: ESRCH is how anything - Node's process.kill, a
       shell's `kill %1` - learns a process is gone rather than protected.
       M225: who a pid names is scheduler_kill's to decide - a kernel task
       is nobody's target but the kernel's, and process group 0 is nobody. */
    if ((long)sig < 0 || sig > SIG_MAX) {
        return -OS_ERROR_INVALID;
    }
    return scheduler_kill(scheduler_current(), (long)pid, (int)sig, may_signal);
}

/* Numbering a descriptor is claiming it. The lowest free slot used to be
   FOUND here and FILLED by the caller afterwards, with nothing in between to
   say it was taken - so two threads of one process opening at the same
   moment on two processors were handed the same number, and one of the two
   objects was silently lost. One processor never does two things at the same
   moment, which is why a hundred and ninety milestones ran on it without
   meeting this. Chromium on four did at once: base's descriptor-ownership
   tracker stopped the browser with "Crashing due to FD ownership violation"
   before it drew a pixel. The claim is a compare-and-swap from NONE to
   RESERVED, so exactly one claimant wins each slot, and the caller either
   fills it (install_file_descriptor) or gives it back. M225: the claim and
   the rest of the slot protocol live with the table in scheduler.c, where a
   host test can run them on several threads at once. */
static int claim_file_descriptor(task_t *t, int from) {
    return file_descriptor_claim(t->descriptor_table, from);
}

static void unclaim_file_descriptor(task_t *t, int fd) {
    file_descriptor_unclaim(t->descriptor_table, fd);
}

/* M225: a claimed slot is filled in one step under the table's lock, so a
   sibling copying or closing it sees either nothing or the whole of it -
   never a type whose object is not written yet. The reference the caller
   holds on `object` passes to the slot. */
static void install_file_descriptor(task_t *t, int fd, file_descriptor_type_t type,
                                    void *object, int cloexec, int nonblock,
                                    int writable) {
    file_descriptor_slot_t slot;
    k_memset(&slot, 0, sizeof(slot));
    slot.type = type;
    slot.pipe = (struct pipe *)object;
    slot.cloexec = cloexec ? 1 : 0;
    slot.nonblock = nonblock ? 1 : 0;
    slot.writable = writable ? 1 : 0;
    file_descriptor_install(t->descriptor_table, fd, &slot);
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
    int read_file_descriptor = claim_file_descriptor(self, 0);
    int write_file_descriptor = read_file_descriptor < 0 ? -1 : claim_file_descriptor(self, 0);
    if (read_file_descriptor < 0 || write_file_descriptor < 0) {
        unclaim_file_descriptor(self, read_file_descriptor);
        return -1;
    }

    pipe_t *p = pipe_create();
    if (!p) {
        unclaim_file_descriptor(self, read_file_descriptor);
        unclaim_file_descriptor(self, write_file_descriptor);
        return -1;
    }
    install_file_descriptor(self, read_file_descriptor, FILE_DESCRIPTOR_PIPE_READ, p, 0, 0, 0);
    install_file_descriptor(self, write_file_descriptor, FILE_DESCRIPTOR_PIPE_WRITE, p, 0, 0, 0);

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
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    int is_file = slot->type == FILE_DESCRIPTOR_FILE;
    done_with_descriptor(&use);
    if (!is_file) {
        return -1;
    }
    /* M194: with a journal a write lands in memory and the disk's answer
       comes at the commit, so this is where a failed disk has to be reported
       - which is what fsync is for, and what it did not do: it returned 0
       whatever happened. */
    return virtual_file_system_sync();
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
    return virtual_file_system_sync();
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
    /* M193: the wake takes the same lock the wait checks the word under. It
       did not, and on two processors a waker could change the word and call
       wake between a waiter's check and its sleep: the wake found nobody
       asleep and the waiter then slept on a word that had already changed -
       for ever, or until its timeout. One processor cannot interleave the
       two, so this was invisible until the machine had more than one; then
       it hung the forksmp stage for its whole half hour, one thread asleep on
       a pthread word, and turned every timed wait it hit into a stall. */
    if (op == FUTEX_WAKE) {
        int max = (val > (uint64_t)(unsigned)MAX_TASKS) ? MAX_TASKS : (int)val;
        uint64_t wake_flags = spin_lock_irqsave(&futex_lock);
        int woken = scheduler_wake_n((const void *)address, scheduler_current()->pml4_phys, max);
        spin_unlock_irqrestore(&futex_lock, wake_flags);
        return woken;
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
        deadline = clock_deadline_ms(timeout_ms);
    }
    scheduler_block_on_in_space((const void *)address, scheduler_current()->pml4_phys, deadline,
                                &futex_lock, &flags);
    spin_unlock_irqrestore(&futex_lock, flags);
    if (deadline != 0 && clock_monotonic_ms() >= deadline) {
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
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = -1;
    const char *p = (slot->type == FILE_DESCRIPTOR_FILE && slot->file) ? slot->file->path
                                                                       : (const char *)0;
    if (p && p[0] == '/') {
        uint64_t n = 0;
        while (p[n]) {
            n++;
        }
        if (out_length >= n + 1 && copy_to_user(out_pointer, p, n + 1) == 0) {
            result = (long)n;
        }
    }
    done_with_descriptor(&use);
    return result;
}

/* Resource limits. The whole reason these are system calls rather than a
   table in the C library is the pointer: getrlimit(2) must answer EFAULT
   for a destination it cannot write, and user code that writes through the
   pointer itself takes the signal instead of reporting it. Chromium's
   base::ProtectedMemory uses exactly that as a measurement - it makes a page
   read-only and then calls getrlimit on it, requiring -1/EFAULT - and a
   renderer died on every page of its protected section here.

   The numbers are in kernel/process/resource_limits.c, which is a unit a
   host test compiles: what they are is a fact about this machine and belongs
   somewhere that can be graded without booting it. */
static long sys_getrlimit(uint64_t resource, uint64_t out_pointer, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    os_rlimit_t limit;
    if (resource_limit_for(resource, &limit) != 0) {
        return -OS_ERROR_INVALID;
    }
    /* The order matters and it is the order Linux uses: a resource this
       kernel does not know is EINVAL whatever the pointer is, and only then
       is the destination looked at. A caller probing a page with a resource
       number nobody implements would otherwise get EFAULT for the wrong
       reason. */
    if (copy_to_user(out_pointer, &limit, sizeof(limit)) != 0) {
        return -OS_ERROR_FAULT;
    }
    return 0;
}

/* Nothing here can be changed, and that is M65's rule rather than an
   omission: a descriptor table is a fixed array in the task, the task table
   is a fixed array in the scheduler, and a setrlimit that returned 0 would
   be claiming a ceiling had moved when nothing about the machine had. So
   the value asked for is compared with the one that is true, and anything
   else is EPERM - which is what a process without privilege gets on Linux
   for raising a hard limit, and is a refusal a caller can act on. */
static long sys_setrlimit(uint64_t resource, uint64_t in_pointer, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    os_rlimit_t current;
    if (resource_limit_for(resource, &current) != 0) {
        return -OS_ERROR_INVALID;
    }
    os_rlimit_t wanted;
    if (copy_from_user(&wanted, in_pointer, sizeof(wanted)) != 0) {
        return -OS_ERROR_FAULT;
    }
    if (wanted.current == current.current && wanted.maximum == current.maximum) {
        return 0;
    }
    return -OS_ERROR_PERMISSION;
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
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = -1;
    if (slot->type == FILE_DESCRIPTOR_MEMFD) {
        if (slot->writable) {
            result = memfd_truncate(slot->memfd, length);
        }
    } else if (slot->type == FILE_DESCRIPTOR_FILE && has_cap(CAP_FS_WRITE)) {
        open_file_t *of = slot->file;
        if (of && of->writable && length <= (uint64_t)LEANFS_MAX_FILE_SIZE) {
            result = virtual_file_system_handle_truncate_to(of->handle, (uint32_t)length);
        }
    }
    done_with_descriptor(&use);
    return result;
}

/* The terminal a held descriptor reaches, if any: the console, or the pty
   behind an open file - which the caller holds for the length of the call,
   so the handle that names the pty stays open while it is used. */
static tty_t *tty_for_file_descriptor(const file_descriptor_slot_t *slot, int *pty_number) {
    *pty_number = -1;
    if (slot->type == FILE_DESCRIPTOR_STDIN || slot->type == FILE_DESCRIPTOR_STDOUT) {
        return tty_console();
    }
    if (slot->type == FILE_DESCRIPTOR_FILE) {
        return (tty_t *)virtual_file_system_handle_tty(slot->file->handle, pty_number);
    }
    return NULL;
}

static long ioctl_on(task_t *self, const file_descriptor_slot_t *slot, uint64_t command,
                     uint64_t arg) {
    /* FIONREAD is not a terminal question - it is asked of pipes and sockets
       far more often - so it is answered before the tty lookup rather than
       inside it. The number is the one poll already has to know. */
    if (command == FIONREAD) {
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
    tty_t *t = tty_for_file_descriptor(slot, &pty_number);
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

static long sys_ioctl(uint64_t fd, uint64_t command, uint64_t arg, uint64_t a4,
                      uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = ioctl_on(scheduler_current(), slot, command, arg);
    done_with_descriptor(&use);
    return result;
}

static long sys_setpgid(uint64_t pid_argument, uint64_t pgid_argument, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return scheduler_set_process_group(scheduler_current(), (int)pid_argument,
                                       (int)pgid_argument);
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
    task_t *t = scheduler_task_for_pid_argument((int)pid);
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
    /* M225: getpgid(0) is the caller's group, as getsid(0) already was. It
       was the KERNEL task's - pid 0 is a real id here - and getpgrp() is
       getpgid(0), so every program on the machine was told it was in group
       0; that was true only while programs the kernel started shared the
       kernel's group. */
    task_t *t = scheduler_task_for_pid_argument((int)pid);
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
    shared_memory_unmap_range(scheduler_current()->pml4_phys, vaddr, pages);
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
        if (vaddr < USER_SHARED_MEMORY_BASE || last > USER_SHARED_MEMORY_LIMIT || last < vaddr) {
            return -1;
        }
        shared_memory_unmap_range(scheduler_current()->pml4_phys, vaddr, (uint64_t)pages);
    }
    return shared_memory_free((int)id, scheduler_current()->id);
}

/* The size every program is told is the DESKTOP's: on a panel the kernel
   scales by two, a 3840x2400 framebuffer is a 1920x1200 screen, and that is
   what a client laying out a window - Chromium's ozone platform among them -
   has to plan for. Only the compositor, which owns the real pixels and does
   the scaling, asks for the physical mode (a2 = WINDOW_MANAGER_FRAMEBUFFER_PHYSICAL).
   The struct stays the size it has always been, because a program built
   against it is handing the kernel a buffer exactly that big. */
static long sys_framebuffer_info(uint64_t out_pointer, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    window_manager_framebuffer_info_t out;
    if (a2 == WINDOW_MANAGER_FRAMEBUFFER_PHYSICAL) {
        out.width = framebuffer_width();
        out.height = framebuffer_height();
    } else {
        framebuffer_desktop_size(&out.width, &out.height);
    }
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
                                VIRTUAL_MEMORY_FLAG_WRITABLE | VIRTUAL_MEMORY_FLAG_USER |
                                VIRTUAL_MEMORY_FLAG_WRITE_COMBINING) != 0) {
            return (long)(uint64_t)-1;
        }
    }
    kernel_log_release_console();
    return (long)USER_FRAMEBUFFER_BASE;
}

/* M204: the address space that drains the pointer. Every program holding
   CAP_FRAMEBUFFER used to have its waitfds ended by pending movement, and
   only the compositor ever reads it - so while the compositor spent 45 ms on
   a frame, init and desktop_icons returned from waitfds 1.6 million times a
   second on the laptop, on the cores the compositor needed. */
static uint64_t pointer_reader_pml4;

static int pointer_wakes(const task_t *self) {
    if (!(self->caps & CAP_FRAMEBUFFER)) {
        return 0;
    }
    uint64_t reader = __atomic_load_n(&pointer_reader_pml4, __ATOMIC_RELAXED);
    return reader == 0 || reader == self->pml4_phys;
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
    task_t *self = scheduler_current();
    if (self->caps & CAP_FRAMEBUFFER) {
        __atomic_store_n(&pointer_reader_pml4, self->pml4_phys, __ATOMIC_RELAXED);
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
    int read_file_descriptor = claim_file_descriptor(self, 0);
    int write_file_descriptor = read_file_descriptor < 0 ? -1 : claim_file_descriptor(self, 0);
    if (read_file_descriptor < 0 || write_file_descriptor < 0) {
        unclaim_file_descriptor(self, read_file_descriptor);
        return -1;
    }

    pipe_t *p = pipe_named(name);
    if (!p) {
        unclaim_file_descriptor(self, read_file_descriptor);
        unclaim_file_descriptor(self, write_file_descriptor);
        return -1;
    }
    install_file_descriptor(self, read_file_descriptor, FILE_DESCRIPTOR_PIPE_READ, p, 0, 0, 0);
    install_file_descriptor(self, write_file_descriptor, FILE_DESCRIPTOR_PIPE_WRITE, p, 0, 0, 0);

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
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = slot->type == FILE_DESCRIPTOR_PIPE_READ ? (long)pipe_buffered(slot->pipe) : -1;
    done_with_descriptor(&use);
    return result;
}

static long sys_uptime_ms(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return (long)(clock_monotonic_ms());
}

static long sys_clock_ns(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return (long)clock_monotonic_ns();
}

static uint32_t slot_inode(const file_descriptor_slot_t *slot) {
    leanfs_stat_t st;
    if (slot->type != FILE_DESCRIPTOR_FILE || virtual_file_system_handle_stat(slot->file->handle, &st) != 0) {
        return 0;
    }
    return st.inode;
}

/* POSIX's close rule, kept on purpose: closing ANY descriptor for a file
   releases every record lock the PROCESS holds on it - whichever thread
   took them and whichever descriptor they were taken through. */
static void drop_record_locks(task_t *self, const file_descriptor_slot_t *slot) {
    if (slot->type != FILE_DESCRIPTOR_FILE || flock_count() == 0) {
        return;
    }
    uint32_t ino = slot_inode(slot);
    if (ino && flock_release_file(ino, scheduler_record_lock_owner(self)) > 0) {
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
    /* "The FD_CLOEXEC flag associated with the new file descriptor shall be
       cleared" - POSIX, dup2(). Copying the whole slot carried it across
       instead, and the only way to see that is to dup2 a close-on-exec
       descriptor and then exec: the copy vanishes, and what the child reports
       is not "this descriptor was closed" but whatever it was about to do
       with it. Chromium's launcher does exactly that for every child process
       it starts - the shared memory region it hands a renderer is
       close-on-exec in the browser and dup2'd to a fixed number in the fork -
       so every child died in the same place, three rungs away from the cause.
       F_DUPFD is built on this call and inherits the fix; F_DUPFD_CLOEXEC
       sets the flag again afterwards, which is why it was never wrong.

       M225: the copy, its reference and the replacement of what newfd held
       are one step under the table's lock (descriptor_table_duplicate), and
       what newfd held is let go of after it - a sibling closing oldfd at the
       same moment can no longer drop the last reference between the copy
       and the reference taken for it. */
    file_descriptor_slot_t displaced;
    int got = descriptor_table_duplicate(self->descriptor_table, (int)oldfd, (int)newfd, 0,
                                         &displaced);
    if (got == DESCRIPTOR_TABLE_BUSY) {
        /* M225 (fd-use-holds): newfd is a number another thread has
           claimed and not yet filled - between open()'s numbering and its
           install. Linux answers EBUSY for exactly that window (dup2(2)),
           and a bare -1 here left the C library to guess, which it did by
           asking fstat and saying EBADF about a descriptor that was a moment
           from being open. */
        return -OS_ERROR_BUSY;
    }
    if (got < 0) {
        return -1;
    }
    drop_record_locks(self, &displaced);
    file_descriptor_release(&displaced);
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
    /* M225: taken out of the slot under the table's lock, then let go of.
       Two threads closing one descriptor at once both found it open and
       both released it - one reference dropped twice, which freed a socket,
       an eventfd or a memfd while another descriptor still counted on it.
       Now one of them detaches it and the other is told it was not open;
       and a slot another thread has claimed and not yet filled is not open
       either (it used to be "closed", and its owner then filled a slot that
       somebody else had been handed the same number for). */
    file_descriptor_slot_t closing;
    if (file_descriptor_detach(self->descriptor_table, (int)fd, &closing) != 0) {
        return -1;
    }
    drop_record_locks(self, &closing);
    file_descriptor_release(&closing);
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
    /* M225: SYS_wait's rule - see scheduler_may_wait_for. */
    if (!scheduler_may_wait_for(scheduler_current(), t)) {
        return -1;
    }
    if (!scheduler_process_has_ended(t)) {
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

/* How a process ended, as a wait(2) status, for a caller that is not its
   parent and so cannot wait for it: the compositor deciding whether a window
   whose program is gone crashed. Answered while the zombie is there and for
   a while after it has been reaped. -OS_ERROR_AGAIN while it runs. */
static long sys_task_end_status(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    task_t *t = scheduler_task_by_id((int)pid);
    if (t && t->state != TASK_TERMINATED) {
        return -OS_ERROR_AGAIN;
    }
    if (t) {
        return scheduler_wait_status(t);
    }
    int status = 0;
    if (scheduler_recent_exit_status((int)pid, &status)) {
        return status;
    }
    return -OS_ERROR_NOENT;
}

static long sys_pipe_reset(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = -1;
    if (slot->type == FILE_DESCRIPTOR_PIPE_READ || slot->type == FILE_DESCRIPTOR_PIPE_WRITE) {
        pipe_reset(slot->pipe);
        result = 0;
    }
    done_with_descriptor(&use);
    return result;
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
        /* M225: under the scheduler lock, and a task that has already let
           its table go (every zombie) has none. This read a null table's
           slots for a zombie, and the table of a task whose exit was freeing
           it on another processor. */
        e->open_file_descriptors = scheduler_open_descriptor_count(t);
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

static int descriptor_exhaustions;

/* How many descriptors this process has no room for, which is a fact about
   THE PROCESS where open_file_exhaustion_count() is one about the machine.
   Both arrive at a caller as "it failed", and telling them apart by reading
   the source is what M183 did not want to do a second time. */
static int alloc_file_descriptor(task_t *t) {
    int claimed = claim_file_descriptor(t, 2);
    if (claimed >= 0) {
        return claimed;
    }
    descriptor_exhaustions++;
    if (descriptor_exhaustions == 1 || descriptor_exhaustions % 64 == 0) {
        kernel_log_puts("[fd] ");
        kernel_log_puts(t->name);
        kernel_log_puts(" is at its ceiling of ");
        kernel_log_put_dec(MAX_FILE_DESCRIPTORS);
        kernel_log_puts(" descriptors (");
        kernel_log_put_dec((uint32_t)descriptor_exhaustions);
        kernel_log_puts(" refusal(s) since boot)\n");
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
        if (flags & (OPEN_CREATE | OPEN_EXCL | OPEN_TRUNCATE)) {
            return -1;
        }
        /* M225: a reference of this call's own on what the descriptor names,
           taken under the table's lock. It was read straight out of the slot
           and retained afterwards, so a sibling thread closing that
           descriptor in between left the new one holding an object whose
           last reference was already gone (or, for a file, a path read out
           of an open file that had just been recycled). The held reference
           becomes the new descriptor's, or is given back. */
        file_descriptor_slot_t source;
        if (file_descriptor_hold(opener->descriptor_table, source_fd, &source) != 0) {
            return -1;
        }
        if (source.type == FILE_DESCRIPTOR_FILE) {
            /* A file on the disk has a name of its own, and reopening it
               through procfs gives a new file description with its own
               offset rather than a second reference to this one - which is
               what Linux does and what a program reading the same file twice
               expects. So this becomes an ordinary open of that path. */
            if (!source.file || (writable && !source.file->writable)) {
                file_descriptor_release(&source);
                return -1;
            }
            k_memcpy(path, source.file->path, OPEN_FILE_PATH_MAX);
            path[OPEN_FILE_PATH_MAX - 1] = '\0';
            file_descriptor_release(&source);
        } else {
            /* Everything else - a memfd, a pipe end, a socket - has no name
               but this one, so the new descriptor refers to the same object.
               Its access is what was asked for, bounded by what the
               descriptor being reopened holds. */
            if (writable && source.type == FILE_DESCRIPTOR_MEMFD && !source.writable) {
                file_descriptor_release(&source);
                return -1;
            }
            int fd = alloc_file_descriptor(opener);
            if (fd < 0) {
                file_descriptor_release(&source);
                return -1;
            }
            source.cloexec = (flags & OPEN_CLOEXEC) ? 1 : 0;
            if (source.type == FILE_DESCRIPTOR_MEMFD) {
                source.writable = writable ? 1 : 0;
            }
            file_descriptor_install(opener->descriptor_table, fd, &source);
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
        unclaim_file_descriptor(self, fd);
        return -1;
    }
    if (flags & OPEN_APPEND) {
        of->offset = virtual_file_system_handle_size(handle);
    }
    /* O_SYNC belongs to the open file rather than to the descriptor, so a dup
       of it writes through too. M223: Node maps WASI's sync flags to it, and a
       constant the kernel ignored would promise durability nothing provided. */
    of->synchronous = (flags & OPEN_SYNC) ? 1 : 0;
    install_file_descriptor(self, fd, FILE_DESCRIPTOR_FILE, of,
                            (flags & OPEN_CLOEXEC) ? 1 : 0, 0, 0);
    return fd;
}

static long sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = -1;
    if (slot->type == FILE_DESCRIPTOR_FILE && whence <= SEEK_END) {
        int64_t base = 0;
        if (whence == SEEK_CUR) {
            base = (int64_t)slot->file->offset;
        } else if (whence == SEEK_END) {
            base = (int64_t)virtual_file_system_handle_size(slot->file->handle);
        }
        int64_t target = base + (int64_t)(int32_t)offset;
        if (target >= 0 && target <= (int64_t)LEANFS_MAX_FILE_SIZE) {
            slot->file->offset = (uint32_t)target;
            result = (long)target;
        }
    }
    done_with_descriptor(&use);
    return result;
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

static void region_tag_reference(uint32_t memfd_id, uint16_t memfd_gen) {
    memfd_region_tag_reference(memfd_id, memfd_gen);
}

static int mmap_slot_cmp_insert(task_t *t, uint64_t base, uint32_t pages, uint32_t prot,
                                int handle, uint32_t file_page, int shared, int merge,
                                uint32_t memfd_id, uint16_t memfd_gen) {
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

static long munmap_locked(task_t *self, uint64_t address, uint64_t end);

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
    uint32_t memfd_id = 0;
    uint16_t memfd_gen = 0;
    if (!anon) {
        if ((long)fd < 0 || (uint64_t)fd >= MAX_FILE_DESCRIPTORS) {
            return -1;
        }
        if ((offset & (PAGE_SIZE - 1)) != 0) {
            return -1;
        }
        /* M225: held while it is looked at and its region reference is taken
           - a sibling thread closing the descriptor at this moment could
           otherwise drop the memfd's last reference first, and the region
           would be counted on a memfd that was gone. (fd-use-holds: and the
           open file's handle is read off a held one too.) */
        file_descriptor_use_t use;
        file_descriptor_slot_t *held = use_descriptor((uint64_t)fd, &use);
        if (!held) {
            return -1;
        }
        if (held->type == FILE_DESCRIPTOR_MEMFD) {
            struct memfd *m = held->memfd;
            uint64_t size = memfd_size(m);
            uint64_t want_end = offset + (uint64_t)length;
            if (!shared || size == 0 || want_end < offset || want_end > size ||
                ((prot & PROT_WRITE) && (!memfd_may_write(m) || !held->writable))) {
                done_with_descriptor(&use);
                return -1;
            }
            memfd_region_reference(m);
            memfd_id = memfd_slot(m) + 1;
            memfd_gen = memfd_generation(m);
            done_with_descriptor(&use);
            file_page = (uint32_t)(offset / PAGE_SIZE);
            goto have_backing;
        }
        int refused = held->type != FILE_DESCRIPTOR_FILE || !held->file ||
                      (shared && (prot & PROT_WRITE) &&
                       (!held->file->writable || !has_cap(CAP_FS_WRITE)));
        if (!refused) {
            handle = held->file->handle;
        }
        done_with_descriptor(&use);
        if (refused) {
            return -1;
        }
        file_page = (uint32_t)(offset / PAGE_SIZE);
    } else {
        /* The descriptor is not looked at for an anonymous mapping - Linux's
           rule, and what its manual says portable callers may rely on only
           the other way round ("some implementations require fd to be -1").
           Chromium's GWP-ASan passes 0 when it maps a PROT_NONE page over a
           freed allocation, and refusing that stopped the browser on a PCHECK
           (M187). */
        if (offset != 0) {
            return -1;
        }
        if (shared) {
            return -1;
        }
    }
have_backing:;
    /* M225: from here a memfd mapping holds a region reference (memfd_id is
       its tag), and every refusal gives it back - these three returned
       without, and the memfd outlived its last descriptor. */
    uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    task_t *self = scheduler_vm_owner(scheduler_current());
    if ((prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC)) ||
        pages == 0 || pages > (USER_MMAP_LIMIT - USER_MMAP_BASE) / PAGE_SIZE ||
        self->pml4_phys == virtual_memory_kernel_pml4_phys()) {
        memfd_region_tag_unref(memfd_id, memfd_gen);
        return -1;
    }

    /* M181: one locked stretch over everything that reads or writes the table,
       and one way out of it, because a return that skips the unlock is a
       machine that stops. The memfd reference is dropped after the lock is
       given back - it is not table work, and the rule for this lock is that
       as little as possible happens underneath it. */
    uint64_t region_flags = scheduler_regions_begin_change(self);
    uint64_t base = 0;
    int failed = 0;

    if (flags & MAP_FIXED) {
        if ((address & (PAGE_SIZE - 1)) != 0) {
            failed = 1;
        } else if (address < USER_MMAP_BASE ||
                   address + pages * PAGE_SIZE > USER_MMAP_LIMIT) {
            failed = 1;
        } else if (!mmap_range_is_free(self, address, pages) &&
                   munmap_locked(self, address, address + pages * PAGE_SIZE) != 0) {
            failed = 1;
        } else {
            base = address;
        }
    } else if (address != 0) {
        uint64_t want = address & ~(PAGE_SIZE - 1);
        if (want >= USER_MMAP_BASE && want + pages * PAGE_SIZE <= USER_MMAP_LIMIT &&
            mmap_range_is_free(self, want, pages)) {
            base = want;
        }
    }
    if (!failed && base == 0) {
        base = mmap_find_gap(self, (uint32_t)pages);
        if (base == 0) {
            failed = 1;
        }
    }
    if (!failed &&
        mmap_slot_cmp_insert(self, base, (uint32_t)pages, (uint32_t)prot,
                             handle, file_page, shared, 1, memfd_id, memfd_gen) != 0) {
        failed = 1;
    }
    scheduler_regions_end_change(self, region_flags);

    if (failed) {
        /* M225: the region reference taken above goes back on EVERY way
           out, by the whole tag. Only a failed insert gave it back, and
           with the slot narrowed to 8 bits - past 256 memfds that was
           another memfd's reference or nobody's - while a MAP_FIXED that was
           refused, or an address space with no gap left, kept it for good:
           the memfd and every page of it outlived its last descriptor. */
        memfd_region_tag_unref(memfd_id, memfd_gen);
        return -1;
    }

    return (long)base;
}

/* M204: the pages of a region that holds no reference on its frames - a
   memfd's, or a shared file's page cache - leave this address space without
   being freed; only private memory is freed. madvise(MADV_DONTNEED) asked
   nothing and freed a shared memfd mapping's frames, and the memfd went on
   handing them out after the allocator had given them to somebody else -
   one process's memory written by another. munmap asked "is it shared",
   which is the same question only while a memfd cannot be mapped privately,
   so it asks this one too. */
static uint32_t madvise_borrowed_reports;

static int region_borrows_frames(const mmap_region_t *region) {
    return region->memfd_id != 0 || (region->shared && region->handle >= 0);
}

static void release_region_pages(task_t *self, const mmap_region_t *region,
                                 uint64_t start, uint64_t end) {
    if (region_borrows_frames(region)) {
        scheduler_release_shared_range(self, start, end);
    } else {
        virtual_memory_unmap_range_free(self->pml4_phys, start, end);
    }
}

/* M181: the table work, with the region lock already held. sys_mmap needs this
   for MAP_FIXED and cannot call sys_munmap to get it - a spinlock is not
   recursive and the wrapper below would wait for the caller. */
static long munmap_locked(task_t *self, uint64_t address, uint64_t end) {
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
        release_region_pages(self, &self->mmaps[i], cut_start, cut_end);
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
    uint64_t region_flags = scheduler_regions_begin_change(self);
    long result = munmap_locked(self, address, end);
    scheduler_regions_end_change(self, region_flags);
    return result;
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
    /* M181: file_mapping_sync writes dirty pages back to the DISK, which is
       the last thing to hold a lock across - so the table is asked for one
       handle at a time and let go of before each write. The table can change
       between two of those, which msync has never promised it would not; what
       it cannot do any more is be walked while somebody shifts its entries. */
    uint32_t scan = 0;
    for (;;) {
        int handle = -1;
        uint64_t region_flags = scheduler_regions_lock(self);
        while (scan < self->mmap_capacity) {
            if (self->mmaps[scan].pages == 0) {
                scan = self->mmap_capacity;
                break;
            }
            uint64_t rstart = self->mmaps[scan].base;
            uint64_t rend = rstart + (uint64_t)self->mmaps[scan].pages * PAGE_SIZE;
            int overlaps = !(end <= rstart || address >= rend);
            int syncable = overlaps && self->mmaps[scan].shared &&
                           self->mmaps[scan].handle >= 0;
            if (syncable) {
                handle = self->mmaps[scan].handle;
                scan++;
                break;
            }
            scan++;
        }
        scheduler_regions_unlock(self, region_flags);
        if (handle < 0) {
            break;
        }
        file_mapping_sync(handle);
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
        uint32_t mid = t->mmaps[i].memfd_id;
        uint16_t mgen = t->mmaps[i].memfd_gen;
        t->mmaps[i].pages = (uint32_t)((cut - rstart) / PAGE_SIZE);
        region_tag_reference(mid, mgen);
        if (mmap_slot_cmp_insert(t, cut, (uint32_t)((rend - cut) / PAGE_SIZE), prot,
                                 handle, fp, shared, 0, mid, mgen) != 0) {
            t->mmaps[i].pages = (uint32_t)((rend - rstart) / PAGE_SIZE);
            /* M225: the whole tag (memfd_region_tag.h): this took the
               slot to 8 bits, which past 256 memfds was another memfd's
               reference given back, or nobody's. */
            memfd_region_tag_unref(mid, mgen);
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
        /* Bytes, not pages. This asked about (end - address) / PAGE_SIZE
           BYTES, which for any range is a number small enough to fall inside
           the first page - so "is every page in this range really mapped"
           only ever looked at the first one, and a range with a hole in it
           was accepted. M169 found it while reading the call beside it. */
        if (!virtual_memory_user_range_ok(self->pml4_phys, address,
                                          end - address, 0)) {
            return -1;
        }
    } else {
        /* M181: the scan, the split and the permission update are one locked
           stretch - they were three separate walks of a table nothing was
           holding still, and the split in the middle MOVES the entries the
           third one then looks for. in_mmap and in_image cannot both be true,
           so the update that used to sit outside this branch belongs in it. */
        uint64_t region_flags = scheduler_regions_begin_change(self);
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
        int refused = (covered != end - address);
        if (!refused && mmap_split_for(self, address, end) != 0) {
            refused = 1;
        }
        if (!refused) {
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
        scheduler_regions_end_change(self, region_flags);
        if (refused) {
            return -1;
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
        /* M181: the THIRD copy of this walk in the tree - M180 unified the two
           in the fault path and this one was in a syscall. It asks the
           scheduler now, which holds the lock for exactly as long as the
           lookup and no longer: the copy_to_user below can fault, the fault
           handler takes that same lock, and a syscall holding it while
           faulting would wait for itself. */
        int mapped = scheduler_region_covers(self, page);
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
    /* DONTNEED drops this process's pages and nothing else: a private page
       comes back zeroed, a shared one comes back from the object behind it
       with what it held. Every region the range touches is asked which it
       is - this freed whatever was mapped, and a memfd's frames are not the
       mapping's to free. */
    uint64_t region_flags = scheduler_regions_lock(self);
    for (uint32_t i = 0; i < self->mmap_capacity; i++) {
        if (self->mmaps[i].pages == 0) {
            break;
        }
        uint64_t rstart = self->mmaps[i].base;
        uint64_t rend = rstart + (uint64_t)self->mmaps[i].pages * PAGE_SIZE;
        uint64_t cut_start = address > rstart ? address : rstart;
        uint64_t cut_end = end < rend ? end : rend;
        if (cut_start < cut_end) {
            if (region_borrows_frames(&self->mmaps[i]) &&
                __atomic_fetch_add(&madvise_borrowed_reports, 1, __ATOMIC_RELAXED) < 3) {
                kernel_log_puts("[mm] madvise(DONTNEED) over a ");
                kernel_log_puts(self->mmaps[i].memfd_id ? "memfd" : "shared file");
                kernel_log_puts(" mapping in '");
                kernel_log_puts(self->name);
                kernel_log_puts("' - its frames stay with the object\n");
            }
            release_region_pages(self, &self->mmaps[i], cut_start, cut_end);
        }
    }
    scheduler_regions_unlock(self, region_flags);
    return 0;
}

static long sys_fstat(uint64_t fd, uint64_t out_pointer, uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6) {
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    os_stat_t out;
    k_memset(&out, 0, sizeof(out));
    int known = 1;

    switch (slot->type) {
    case FILE_DESCRIPTOR_FILE: {
        leanfs_stat_t st;
        if (virtual_file_system_handle_stat(slot->file->handle, &st) != 0) {
            known = 0;
            break;
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
        known = 0;
        break;
    }
    done_with_descriptor(&use);
    if (!known) {
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

/* M225 (fd-use-holds): the socket `fd` names, held in `use` for the caller
   to give back - or null, holding nothing, when it names anything else. */
static struct socket *socket_for_file_descriptor(uint64_t fd, file_descriptor_use_t *use) {
    file_descriptor_slot_t *slot = use_descriptor(fd, use);
    if (!slot) {
        return (struct socket *)0;
    }
    if (slot->type != FILE_DESCRIPTOR_SOCKET) {
        done_with_descriptor(use);
        return (struct socket *)0;
    }
    return slot->sock;
}

static long install_socket_file_descriptor(struct socket *s) {
    task_t *self = scheduler_current();
    int fd = alloc_file_descriptor(self);
    if (fd < 0) {
        socket_unref(s);
        return -OS_ERROR_MFILE;
    }
    install_file_descriptor(self, fd, FILE_DESCRIPTOR_SOCKET, s, 0, 0, 0);
    return fd;
}

static long install_unix_file_descriptor(struct unix_socket *u) {
    task_t *self = scheduler_current();
    int fd = alloc_file_descriptor(self);
    if (fd < 0) {
        unix_socket_unref(u);
        return -OS_ERROR_MFILE;
    }
    install_file_descriptor(self, fd, FILE_DESCRIPTOR_UNIX, u, 0, 0, 0);
    return fd;
}

/* The same for a unix socket. */
static struct unix_socket *unix_for_file_descriptor(uint64_t fd, file_descriptor_use_t *use) {
    file_descriptor_slot_t *slot = use_descriptor(fd, use);
    if (!slot) {
        return (struct unix_socket *)0;
    }
    if (slot->type != FILE_DESCRIPTOR_UNIX) {
        done_with_descriptor(use);
        return (struct unix_socket *)0;
    }
    return slot->un;
}

static long sys_socket(uint64_t type, uint64_t domain, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (domain == OS_AF_UNIX) {
        if (type != UNIX_SOCKET_STREAM && type != UNIX_SOCKET_SEQPACKET) {
            return -1;
        }
        struct unix_socket *u = unix_socket_alloc((int)type);
        return u ? install_unix_file_descriptor(u) : -OS_ERROR_NFILE;
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
        return -OS_ERROR_NFILE;
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
    file_descriptor_use_t use;
    struct unix_socket *u = unix_for_file_descriptor(file_descriptor, &use);
    if (!u) {
        return -1;
    }
    int pid = unix_socket_peer_pid(u);
    done_with_descriptor(&use);
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
    /* M225: both numbers are claimed and handed to the caller before either
       slot is filled, so a failure gives back two claims - which nobody else
       can touch - rather than closing two live descriptors by number, which
       a sibling thread could already have closed or replaced. */
    int fa = alloc_file_descriptor(self);
    int fb = fa >= 0 ? alloc_file_descriptor(self) : -1;
    int out[2] = {fa, fb};
    if (fb < 0 || copy_to_user(file_descriptors_pointer, out, sizeof(out)) != 0) {
        unclaim_file_descriptor(self, fa);
        unclaim_file_descriptor(self, fb);
        unix_socket_unref(a);
        unix_socket_unref(b);
        return -1;
    }
    install_file_descriptor(self, fa, FILE_DESCRIPTOR_UNIX, a, 0, 0, 0);
    install_file_descriptor(self, fb, FILE_DESCRIPTOR_UNIX, b, 0, 0, 0);
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
    file_descriptor_use_t use;
    struct unix_socket *u = unix_for_file_descriptor(fd, &use);
    if (!u) {
        return -1;
    }
    long result = unix_socket_bind(u, name, (int)length);
    done_with_descriptor(&use);
    return result;
}

static long sys_connectun(uint64_t fd, uint64_t name_pointer, uint64_t length, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    char name[UNIX_PATH_MAX];
    if (copy_un_name(name, name_pointer, length) != 0) {
        return -1;
    }
    file_descriptor_use_t use;
    struct unix_socket *u = unix_for_file_descriptor(fd, &use);
    if (!u) {
        return -1;
    }
    long result = unix_socket_connect(u, name, (int)length);
    done_with_descriptor(&use);
    return result;
}

#define UNIX_MESSAGE_STAGING UNIX_STAGING_CHUNK

static void done_with_descriptors(file_descriptor_use_t *uses, int count) {
    for (int i = count - 1; i >= 0; i--) {
        done_with_descriptor(&uses[i]);
    }
}

/* M225: every descriptor a message carries is HELD - a reference of this
   call's own, taken under the table's lock - from the moment it is read out
   of the table until the call returns. It was a plain copy of the slot that
   unix_socket_send then retained, so a sibling thread closing one of them in
   between dropped the last reference first and the message carried, and
   later installed in the receiver, an object that was already gone. The
   channel itself is held the same way, for the same reason. A reserved slot
   (being filled in by another thread) is not something that can be sent: it
   went out as one, and arrived as a slot nobody would ever fill.
   (fd-use-holds: through use_descriptor, so an exit in the middle of a
   blocked send gives the references back, and O_NONBLOCK is read off the
   held channel rather than off a slot a sibling may have closed.) */
static long sys_sendmsg(uint64_t fd, uint64_t message_pointer, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)flags; (void)a4; (void)a5; (void)a6;
    os_message_t message;
    if (copy_from_user(&message, message_pointer, sizeof(message)) != 0) {
        return -1;
    }
    if (message.nfds > UNIX_MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    file_descriptor_use_t channel_use;
    struct unix_socket *u = unix_for_file_descriptor(fd, &channel_use);
    if (!u) {
        return -1;
    }
    int nonblock = channel_use.slot.nonblock;
    uint32_t length = message.length;
    if (length > UNIX_MESSAGE_STAGING) {
        if (unix_socket_type(u) == UNIX_SOCKET_SEQPACKET) {
            done_with_descriptor(&channel_use);
            return -1;
        }
        length = UNIX_MESSAGE_STAGING;
    }
    file_descriptor_use_t uses[UNIX_MAX_FILE_DESCRIPTORS];
    file_descriptor_slot_t slots[UNIX_MAX_FILE_DESCRIPTORS];
    int nfds = (int)message.nfds;
    int held = 0;
    long result = -1;
    if (nfds > 0) {
        int nums[UNIX_MAX_FILE_DESCRIPTORS];
        if (copy_from_user(nums, message.file_descriptors, (size_t)nfds * sizeof(int)) != 0) {
            goto out;
        }
        for (; held < nfds; held++) {
            if (nums[held] < 0 || !use_descriptor((uint64_t)nums[held], &uses[held])) {
                goto out;
            }
            slots[held] = uses[held].slot;
        }
    }
    uint8_t staging[UNIX_MESSAGE_STAGING];
    if (length && copy_from_user(staging, message.data, (size_t)length) != 0) {
        goto out;
    }
    for (;;) {
        scheduler_watch_begin();
        scheduler_watch_add((const void *)u);
        long n = unix_socket_send(u, staging, length, slots, nfds);
        if (n != 0 || (length == 0 && nfds == 0)) {
            result = n;
            goto out;
        }
        if (nonblock) {
            result = -OS_ERROR_AGAIN;
            goto out;
        }
        if (scheduler_signal_pending()) {
            result = -OS_ERROR_INTR;
            goto out;
        }
        scheduler_watch_block(0);
    }
out:
    done_with_descriptors(uses, held);
    done_with_descriptor(&channel_use);
    return result;
}

static long sys_recvmsg(uint64_t fd, uint64_t message_pointer, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)flags; (void)a4; (void)a5; (void)a6;
    task_t *self = scheduler_current();
    os_message_t message;
    if (copy_from_user(&message, message_pointer, sizeof(message)) != 0) {
        return -1;
    }
    if (message.nfds > UNIX_MAX_FILE_DESCRIPTORS) {
        return -1;
    }
    if (message.length && !user_range_ok(message.data, message.length, 1)) {
        return -1;
    }
    file_descriptor_use_t channel_use;
    struct unix_socket *u = unix_for_file_descriptor(fd, &channel_use);
    if (!u) {
        return -1;
    }
    int nonblock = channel_use.slot.nonblock;
    uint32_t want = message.length > UNIX_MESSAGE_STAGING ? UNIX_MESSAGE_STAGING : message.length;
    uint8_t staging[UNIX_MESSAGE_STAGING];
    file_descriptor_slot_t slots[UNIX_MAX_FILE_DESCRIPTORS];
    long result;
    for (;;) {
        scheduler_watch_begin();
        scheduler_watch_add((const void *)u);
        int nfds = 0;
        int rflags = 0;
        long n = unix_socket_receive(u, staging, want, slots, (int)message.nfds, &nfds, &rflags);
        if (n < 0) {
            message.nfds = 0;
            message.flags = 0;
            copy_to_user(message_pointer, &message, sizeof(message));
            result = 0;
            break;
        }
        if (n > 0 || nfds > 0) {
            /* M225: numbered (claimed) first, copied out, and only then
               installed - so a failed copy gives back claims nobody else can
               touch, rather than closing by number descriptors a sibling
               thread could already have closed or replaced. */
            int nums[UNIX_MAX_FILE_DESCRIPTORS];
            file_descriptor_slot_t *kept[UNIX_MAX_FILE_DESCRIPTORS];
            int installed = 0;
            for (int i = 0; i < nfds; i++) {
                int nfd = alloc_file_descriptor(self);
                if (nfd < 0) {
                    file_descriptor_release(&slots[i]);
                    rflags |= OS_MESSAGE_CTRUNC;
                    continue;
                }
                kept[installed] = &slots[i];
                nums[installed++] = nfd;
            }
            message.nfds = (uint32_t)installed;
            message.flags = (uint32_t)rflags;
            int copied =
                (installed == 0 ||
                 copy_to_user(message.file_descriptors, nums, (size_t)installed * sizeof(int)) == 0) &&
                (n == 0 || copy_to_user(message.data, staging, (size_t)n) == 0) &&
                copy_to_user(message_pointer, &message, sizeof(message)) == 0;
            for (int i = 0; i < installed; i++) {
                if (copied) {
                    kept[i]->cloexec = 0;
                    kept[i]->nonblock = 0;
                    file_descriptor_install(self->descriptor_table, nums[i], kept[i]);
                } else {
                    unclaim_file_descriptor(self, nums[i]);
                    file_descriptor_release(kept[i]);
                }
            }
            result = copied ? n : -1;
            break;
        }
        if (nonblock) {
            result = -OS_ERROR_AGAIN;
            break;
        }
        if (scheduler_signal_pending()) {
            result = -OS_ERROR_INTR;
            break;
        }
        scheduler_watch_block(0);
    }
    done_with_descriptor(&channel_use);
    return result;
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
    install_file_descriptor(self, fd, FILE_DESCRIPTOR_MEMFD, m,
                            (flags & OS_MFD_CLOEXEC) ? 1 : 0, 0, 1);
    return fd;
}

static long sys_memfd_seal(uint64_t fd, uint64_t add, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = -1;
    if (slot->type == FILE_DESCRIPTOR_MEMFD &&
        (add == 0 || memfd_add_seals(slot->memfd, (uint32_t)add) == 0)) {
        result = (long)memfd_get_seals(slot->memfd);
    }
    done_with_descriptor(&use);
    return result;
}

static long sys_sockshut(uint64_t fd, uint64_t how, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = -1;
    if (slot->type == FILE_DESCRIPTOR_UNIX) {
        result = unix_socket_shutdown(slot->un, (int)how);
    } else if (slot->type == FILE_DESCRIPTOR_SOCKET && how <= 2) {
        /* M223: and a TCP connection, which until now had no shutdown at
           all - so a half-close sent no FIN and the peer waited for an
           end-of-file that never came. Node's socket.end() is exactly that
           call. */
        result = socket_shutdown(slot->sock, (int)how);
    }
    done_with_descriptor(&use);
    return result;
}

static long sys_listen(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = -1;
    if (slot->type == FILE_DESCRIPTOR_UNIX) {
        result = unix_socket_listen(slot->un);
    } else if (slot->type == FILE_DESCRIPTOR_SOCKET) {
        result = socket_listen(slot->sock);
    }
    done_with_descriptor(&use);
    return result;
}

static long sys_connect(uint64_t fd, uint64_t ip, uint64_t port, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (port == 0 || port > 0xFFFF) {
        return -1;
    }
    file_descriptor_use_t use;
    struct socket *s = socket_for_file_descriptor(fd, &use);
    if (!s) {
        return -1;
    }
    long result = socket_connect(s, (uint32_t)ip, (uint16_t)port);
    done_with_descriptor(&use);
    return result;
}

static long sys_connstat(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    file_descriptor_use_t use;
    struct socket *s = socket_for_file_descriptor(fd, &use);
    if (!s) {
        return -1;
    }
    struct tcpcb *tcb = socket_tcb(s);
    long result = -1;
    if (tcb) {
        result = !tcp_connect_settled(tcb) ? 0 : tcp_state(tcb) == TCP_ESTABLISHED ? 1 : -1;
    }
    done_with_descriptor(&use);
    return result;
}

/* Which address a socket is on. getsockname(2) used to answer this out of
   the machine's own network configuration with a port of zero, which is
   right about the address and a lie about the port - and a program that
   binds to port zero and then asks which port it got, as every server that
   does not want a fixed one does, was told nothing. */
static long sys_sockname(uint64_t fd, uint64_t out_pointer, uint64_t peer,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    os_sockaddr_t out;
    k_memset(&out, 0, sizeof(out));
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = 0;
    if (slot->type == FILE_DESCRIPTOR_UNIX) {
        /* A Unix-domain socket has a path rather than an address, and this
           call has nowhere to put one. Zeros, which is what binding one to
           nothing reports on every other system. */
    } else if (slot->type != FILE_DESCRIPTOR_SOCKET) {
        result = -1;
    } else if (peer) {
        /* M223: getpeername(2), which answered ENOTCONN for every socket.
           The peer is the TCP connection's remote end, and a socket without
           one - unconnected, listening, or a datagram socket - is the
           ENOTCONN case it always claimed. */
        struct tcpcb *tcb = socket_tcb(slot->sock);
        if (!tcb || tcp_remote_port(tcb) == 0 || tcp_state(tcb) == TCP_LISTEN) {
            result = -2;
        } else {
            out.ip = tcp_remote_ip(tcb);
            out.port = tcp_remote_port(tcb);
        }
    } else {
        out.ip = socket_local_ip(slot->sock);
        out.port = socket_local_port(slot->sock);
    }
    done_with_descriptor(&use);
    if (result != 0) {
        return result;
    }
    return copy_to_user(out_pointer, &out, sizeof(out)) == 0 ? 0 : -1;
}

/* M226: what a socket is - see os_socket_identity_t. -2 is ENOTCONN, for a
   peer asked of a socket without one. */
static long sys_sockident(uint64_t fd, uint64_t out_pointer, uint64_t peer,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    os_socket_identity_t out;
    k_memset(&out, 0, sizeof(out));
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = 0;
    if (slot->type == FILE_DESCRIPTOR_UNIX) {
        out.family = OS_AF_UNIX;
        out.type = (uint32_t)unix_socket_type(slot->un);
        int length = unix_socket_name(slot->un, peer ? 1 : 0, out.name, (int)sizeof(out.name));
        if (length < 0) {
            result = -2;
        } else {
            out.name_length = (uint32_t)length;
        }
    } else if (slot->type == FILE_DESCRIPTOR_SOCKET) {
        out.family = OS_AF_INET;
        out.type = socket_tcb(slot->sock) ? OS_SOCKET_STREAM : OS_SOCKET_DGRAM;
    } else {
        result = -1;
    }
    done_with_descriptor(&use);
    if (result != 0) {
        return result;
    }
    return copy_to_user(out_pointer, &out, sizeof(out)) == 0 ? 0 : -1;
}

/* The listener is held for the whole wait, as Linux holds it: a sibling's
   close() does not wake an accept() blocked on the socket, and the
   connection it then takes is installed as ever. */
static long accept_on(const file_descriptor_slot_t *slot, uint64_t from_pointer) {
    if (slot->type == FILE_DESCRIPTOR_UNIX) {
        struct unix_socket *uconn = unix_socket_accept(slot->un);
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
    if (slot->type != FILE_DESCRIPTOR_SOCKET) {
        return -1;
    }
    struct socket *conn = socket_accept(slot->sock);
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

static long sys_accept(uint64_t fd, uint64_t from_pointer, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = accept_on(slot, from_pointer);
    done_with_descriptor(&use);
    return result;
}

/* The staging buffer is on the stack and the call is under the network lock,
   for the same two reasons everywhere else in this file: a static one is
   shared by every task that is in this call at once, and tcp_send walks the
   same state the NIC's interrupt handler does. sys_write's socket arm had
   both right; these two had neither. */
static long sys_send(uint64_t fd, uint64_t buffer, uint64_t length, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    file_descriptor_use_t use;
    struct socket *s = socket_for_file_descriptor(fd, &use);
    if (!s) {
        return -1;
    }
    struct tcpcb *tcb = socket_tcb(s);
    long result = -1;
    if (tcb) {
        if (length > TCP_MAX_MSS) {
            length = TCP_MAX_MSS;
        }
        uint8_t staging[TCP_MAX_MSS];
        if (!length || copy_from_user(staging, buffer, (size_t)length) == 0) {
            net_lock_acquire();
            result = tcp_send(tcb, staging, (uint16_t)length);
            net_lock_release();
        }
    }
    done_with_descriptor(&use);
    return result;
}

static long sys_receive(uint64_t fd, uint64_t buffer, uint64_t max, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    file_descriptor_use_t use;
    struct socket *s = socket_for_file_descriptor(fd, &use);
    if (!s) {
        return -1;
    }
    struct tcpcb *tcb = socket_tcb(s);
    long result = -1;
    if (tcb) {
        if (max > TCP_MAX_MSS) {
            max = TCP_MAX_MSS;
        }
        uint8_t staging[TCP_MAX_MSS];
        net_lock_acquire();
        int n = tcp_receive(tcb, staging, (uint16_t)max);
        net_lock_release();
        if (n <= 0) {
            result = n;
        } else {
            result = copy_to_user(buffer, staging, (size_t)n) == 0 ? n : -1;
        }
    }
    done_with_descriptor(&use);
    return result;
}

/* recv(2) with MSG_PEEK, which is not a convenience and is not something a
   C library can do for itself.

   net::SocketPosix asks "is this connection still there, and has anything
   arrived on it" by peeking ONE byte; Chromium does that before it reuses a
   socket. A recv that ignores MSG_PEEK and reads instead takes that byte out
   of the stream for good, and the next reader starts one byte late. On a TLS
   connection that is a record header read at the wrong offset - a version
   field that is really the tail of a length - so BoringSSL sends a fatal
   protocol_version alert and every https page on this machine failed with
   ERR_SSL_PROTOCOL_ERROR. It only bit when data had ALREADY arrived, which
   for TLS 1.3 is the normal case: the server sends its session tickets
   unprompted the moment the handshake ends.

   `dontwait` is MSG_DONTWAIT rather than the descriptor's own O_NONBLOCK,
   because recv(2) takes both and a caller may pass either. */
static long peek_at(const file_descriptor_slot_t *slot, uint64_t buffer, uint64_t max,
                    int nonblock) {
    /* recv(2) with a length of zero answers zero rather than waiting for a
       byte it has nowhere to put. */
    if (max == 0) {
        return (slot->type == FILE_DESCRIPTOR_UNIX ||
                slot->type == FILE_DESCRIPTOR_SOCKET) ? 0 : -1;
    }

    if (slot->type == FILE_DESCRIPTOR_UNIX) {
        uint32_t want = max > UNIX_STAGING_CHUNK ? UNIX_STAGING_CHUNK : (uint32_t)max;
        uint8_t staging[UNIX_STAGING_CHUNK];
        for (;;) {
            scheduler_watch_begin();
            scheduler_watch_add((const void *)slot->un);
            long n = unix_socket_peek(slot->un, staging, want);
            if (n > 0) {
                return copy_to_user(buffer, staging, (size_t)n) == 0 ? n : -1;
            }
            if (n < 0) {
                return 0;
            }
            if (nonblock) {
                return -OS_ERROR_AGAIN;
            }
            if (scheduler_signal_pending()) {
                return -OS_ERROR_INTR;
            }
            scheduler_watch_block(0);
        }
    }

    if (slot->type == FILE_DESCRIPTOR_SOCKET) {
        struct tcpcb *tcb = socket_tcb(slot->sock);
        if (!tcb) {
            /* A datagram socket. Its queue is a queue of MESSAGES, and
               showing one without taking it is a different piece of
               bookkeeping from this one; the condition for building it is a
               caller that peeks at a datagram. Refusing is the truthful
               answer meanwhile - the alternative, forwarding to recvfrom,
               is exactly the silent consumption this call exists to end. */
            return -OS_ERROR_INVALID;
        }
        uint16_t want = max > TCP_MAX_MSS ? TCP_MAX_MSS : (uint16_t)max;
        uint8_t staging[TCP_MAX_MSS];
        for (;;) {
            uint64_t seq = scheduler_event_sequence();
            net_lock_acquire();
            int n = tcp_peek(tcb, staging, want);
            net_lock_release();
            if (n > 0) {
                return copy_to_user(buffer, staging, (size_t)n) == 0 ? n : -1;
            }
            if (n < 0) {
                return 0;
            }
            if (nonblock) {
                return -OS_ERROR_AGAIN;
            }
            if (scheduler_signal_pending()) {
                return -OS_ERROR_INTR;
            }
            scheduler_block_on_sequence(SCHEDULER_POLL_CHAN, 0, seq);
        }
    }

    return -1;
}

static long sys_peek(uint64_t fd, uint64_t buffer, uint64_t max, uint64_t dontwait,
                     uint64_t a5, uint64_t a6) {
    (void)a5;
    (void)a6;
    if (max && !user_range_ok(buffer, max, 1)) {
        return -1;
    }
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = peek_at(slot, buffer, max, slot->nonblock || dontwait);
    done_with_descriptor(&use);
    return result;
}

static long sys_bind(uint64_t fd, uint64_t port, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (port > 0xFFFF) {
        return -1;
    }
    file_descriptor_use_t use;
    struct socket *s = socket_for_file_descriptor(fd, &use);
    if (!s) {
        return -1;
    }
    long result = socket_bind(s, (uint16_t)port);
    done_with_descriptor(&use);
    return result;
}

/* M226: SO_REUSEADDR, kept on the socket - see SYS_sockopt. A negative
   value asks for what is set. Only an IP socket keeps it. */
static long sys_sockopt(uint64_t fd, uint64_t option, uint64_t value, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (option != OS_SOCKOPT_REUSEADDR) {
        return -1;
    }
    file_descriptor_use_t use;
    struct socket *s = socket_for_file_descriptor(fd, &use);
    if (!s) {
        return -1;
    }
    long result = (long)value < 0 ? socket_reuse_address(s)
                                  : socket_set_reuse_address(s, value != 0);
    done_with_descriptor(&use);
    return result;
}

static long sys_sendto(uint64_t fd, uint64_t ip, uint64_t port, uint64_t buffer, uint64_t length, uint64_t a6) {
    (void)a6;
    if (port > 0xFFFF || length > UDP_MAX_PAYLOAD) {
        return -1;
    }
    file_descriptor_use_t use;
    struct socket *s = socket_for_file_descriptor(fd, &use);
    if (!s) {
        return -1;
    }
    static uint8_t staging[UDP_MAX_PAYLOAD];
    long result = -1;
    if (!length || copy_from_user(staging, buffer, (size_t)length) == 0) {
        result = socket_sendto(s, (uint32_t)ip, (uint16_t)port, staging, (uint16_t)length);
    }
    done_with_descriptor(&use);
    return result;
}

static long sys_recvfrom(uint64_t fd, uint64_t buffer, uint64_t max, uint64_t from_pointer, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    if (max > SOCKET_MAX_DATAGRAM) {
        return -1;
    }
    file_descriptor_use_t use;
    struct socket *s = socket_for_file_descriptor(fd, &use);
    if (!s) {
        return -1;
    }
    static uint8_t staging[SOCKET_MAX_DATAGRAM];
    os_sockaddr_t from = {0, 0, 0};
    int n = socket_recvfrom(s, staging, (uint16_t)max, &from.ip, &from.port);
    done_with_descriptor(&use);
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
    file_descriptor_use_t use;
    struct socket *s = socket_for_file_descriptor(fd, &use);
    if (!s) {
        return -1;
    }
    long result = socket_pending(s);
    done_with_descriptor(&use);
    return result;
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

/* M213: what the screen is, what it could be, and the two choices a person
   makes about it - see system_api/include/display.h. */
static long sys_display(uint64_t operation, uint64_t a, uint64_t b, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    if (operation == DISPLAY_OPERATION_SET_SCALE) {
        if (!has_cap(CAP_DISPLAY_MODE)) {
            return -1;
        }
        return framebuffer_set_scale_percent((uint32_t)a);
    }
    if (operation == DISPLAY_OPERATION_SET_STARTUP_MODE) {
        if (!has_cap(CAP_DISPLAY_MODE)) {
            return -1;
        }
        return boot_config_set_startup_mode((uint32_t)a, (uint32_t)b);
    }
    if (operation != DISPLAY_OPERATION_STATUS || b != sizeof(display_status_t)) {
        return -1;
    }
    display_status_t *status = (display_status_t *)kmalloc(sizeof(display_status_t));
    if (!status) {
        return -1;
    }
    k_memset(status, 0, sizeof(*status));
    status->physical_width = framebuffer_width();
    status->physical_height = framebuffer_height();
    framebuffer_desktop_size(&status->desktop_width, &status->desktop_height);
    status->scale_percent = framebuffer_scale_percent();
    status->scale_requested = framebuffer_scale_requested();
    status->scale_automatic = display_scale_automatic(status->physical_width, status->physical_height,
                                                      boot_options_active()->display_scale);
    status->scale_count = (uint32_t)display_scale_choices(status->physical_width, status->physical_height,
                                                          status->scales, DISPLAY_SCALES_MAX);
    status->live_modes = dispi_available() ? 1u : 0u;
    boot_options_t startup;
    if (boot_config_startup(&startup) == 0) {
        status->startup_writable = 1;
        switch (startup.video_selection) {
        case BOOT_VIDEO_LARGEST:
            status->startup_selection = DISPLAY_STARTUP_LARGEST;
            break;
        case BOOT_VIDEO_EXACT:
            status->startup_selection = DISPLAY_STARTUP_EXACT;
            status->startup_width = startup.video_width;
            status->startup_height = startup.video_height;
            break;
        case BOOT_VIDEO_FIRMWARE:
            status->startup_selection = DISPLAY_STARTUP_FIRMWARE;
            break;
        default:
            status->startup_selection = DISPLAY_STARTUP_BUILT_IN;
            break;
        }
    }
    const boot_options_t *options = boot_options_active();
    uint32_t offered = options->offered_count < DISPLAY_FIRMWARE_MODES_MAX ? options->offered_count
                                                                          : DISPLAY_FIRMWARE_MODES_MAX;
    for (uint32_t i = 0; i < offered; i++) {
        status->firmware_modes[i].width = options->offered[i][0];
        status->firmware_modes[i].height = options->offered[i][1];
    }
    status->firmware_mode_count = offered;
    long result = copy_to_user(a, status, sizeof(*status));
    kfree(status);
    return result;
}

static int epoll_set_ready(struct epoll *ep, int depth);

/* Readiness of a HELD descriptor (M225, fd-use-holds): it read the slot and
   asked its object, which a sibling's close could free between the two.
   DEPTH is how many epoll sets the question has already gone through - an
   epoll descriptor is ready when the set it names has something to report
   (M226), and that is asked of the descriptors inside it one level down. */
static int file_descriptor_is_ready_at(const file_descriptor_slot_t *slot, int depth) {
    switch (slot->type) {
    case FILE_DESCRIPTOR_STDIN:
        return keyboard_peek() ? 1 : 0;
    case FILE_DESCRIPTOR_PIPE_READ:
        return (pipe_buffered(slot->pipe) > 0 || pipe_write_closed(slot->pipe)) ? 1 : 0;
    case FILE_DESCRIPTOR_SOCKET:
        return (socket_pending(slot->sock) > 0 || socket_read_ended(slot->sock)) ? 1 : 0;
    case FILE_DESCRIPTOR_UNIX:
        return unix_socket_pending(slot->un);
    case FILE_DESCRIPTOR_EVENT:
        return eventfd_readable(slot->event) ? 1 : 0;
    case FILE_DESCRIPTOR_TIMER:
        return timerfd_readable(slot->timer, clock_now_ns()) ? 1 : 0;
    case FILE_DESCRIPTOR_EPOLL:
        return epoll_set_ready(slot->epoll, depth);
    case FILE_DESCRIPTOR_FILE:
        return virtual_file_system_handle_readable(slot->file->handle);
    default:
        return 0;
    }
}

static int file_descriptor_is_ready(const file_descriptor_slot_t *slot) {
    return file_descriptor_is_ready_at(slot, 1);
}

static uint32_t epoll_mask_of(const file_descriptor_slot_t *slot, int depth);

/* An epoll watch's events, asked of the descriptor it names - held for the
   question, so the object compared against the watch's is the one asked. */
static uint32_t file_descriptor_epoll_mask_for(int fd, const void *object, int depth) {
    if (fd < 0) {
        return EPOLL_STALE;
    }
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor((uint64_t)fd, &use);
    if (!slot) {
        return EPOLL_STALE;
    }
    uint32_t m = EPOLL_STALE;
    if (!object || slot->pipe == (struct pipe *)object) {
        m = epoll_mask_of(slot, depth);
    }
    done_with_descriptor(&use);
    return m;
}

static uint32_t epoll_mask_of(const file_descriptor_slot_t *slot, int depth) {
    uint32_t m = 0;
    if (file_descriptor_is_ready_at(slot, depth)) {
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

/* CONTEXT is the depth the watched descriptors are at, or null for a set
   asked directly by a wait. */
static uint32_t epoll_mask_callback(void *context, int fd, const void *object) {
    int depth = context ? *(const int *)context : 1;
    return file_descriptor_epoll_mask_for(fd, object, depth);
}

/* M226: a set inside a set. Electron joins libuv's loop to Chromium's by
   putting libuv's epoll descriptor into an epoll set of its own and waiting
   on that, so the inner set's descriptor has to say "readable" exactly when
   a wait on the inner set would return something - asked without consuming
   an edge or a one-shot (epoll_peek), because the wait that consumes them is
   libuv's own. Past EPOLL_MAX_NESTING the answer is no: registration refuses
   to build anything that deep from the set downwards, and this bounds the
   recursion even for a shape it could not see. */
static int epoll_set_ready(struct epoll *ep, int depth) {
    if (!ep || depth > EPOLL_MAX_NESTING) {
        return 0;
    }
    int next = depth + 1;
    return epoll_peek(ep, epoll_mask_callback, &next);
}

typedef struct {
    int fds[EPOLL_MAX_WATCH];
    const void *objects[EPOLL_MAX_WATCH];
} epoll_registrations_t;

/* Whether FROM is TARGET or holds it at any depth, through this task's own
   descriptors - each one held while it is asked, as everywhere else here.
   Too deep counts as reaching it: ELOOP is Linux's answer to both. The
   registrations come from the heap, because each level is 1.5 KB and a wait
   already keeps a set's worth of events on a 32 KB kernel stack. */
static int epoll_reaches(struct epoll *from, struct epoll *target, int depth) {
    if (from == target || depth > EPOLL_MAX_NESTING) {
        return 1;
    }
    epoll_registrations_t *r = (epoll_registrations_t *)kmalloc(sizeof(*r));
    if (!r) {
        return 1;
    }
    int n = epoll_registered(from, r->fds, r->objects, EPOLL_MAX_WATCH);
    int reached = 0;
    for (int i = 0; i < n && !reached; i++) {
        if (r->fds[i] < 0) {
            continue;
        }
        file_descriptor_use_t use;
        file_descriptor_slot_t *slot = use_descriptor((uint64_t)r->fds[i], &use);
        if (!slot) {
            continue;
        }
        if (slot->type == FILE_DESCRIPTOR_EPOLL && (const void *)slot->epoll == r->objects[i]) {
            reached = epoll_reaches(slot->epoll, target, depth + 1);
        }
        done_with_descriptor(&use);
    }
    kfree(r);
    return reached;
}

/* Everything a wait on a set inside a set has to be woken by: the inner set
   itself, what it watches, and - one level further, to the same bound - the
   sets inside it; a timerfd anywhere in there brings PARK_UNTIL forward,
   because a timer's expiry wakes nobody and is found by parking until it.
   A machine with more objects than a task can watch sets watch_everything,
   which costs spurious wakes and loses none. */
static uint64_t epoll_watch_nested(struct epoll *ep, int depth, uint64_t park_until) {
    scheduler_watch_add(ep);
    if (depth > EPOLL_MAX_NESTING) {
        return park_until;
    }
    epoll_registrations_t *r = (epoll_registrations_t *)kmalloc(sizeof(*r));
    if (!r) {
        return park_until;
    }
    int n = epoll_objects(ep, r->fds, r->objects, EPOLL_MAX_WATCH);
    for (int i = 0; i < n; i++) {
        if (r->fds[i] < 0) {
            continue;
        }
        file_descriptor_use_t use;
        file_descriptor_slot_t *slot = use_descriptor((uint64_t)r->fds[i], &use);
        if (!slot) {
            continue;
        }
        if ((const void *)slot->pipe == r->objects[i]) {
            if (slot->type == FILE_DESCRIPTOR_STDIN) {
                scheduler_watch_add(SCHEDULER_INPUT_OBJECT);
            } else {
                scheduler_watch_add(r->objects[i]);
            }
            if (slot->type == FILE_DESCRIPTOR_TIMER) {
                long ms = timerfd_next_ms(slot->timer, clock_now_ns());
                if (ms >= 0) {
                    uint64_t when = clock_deadline_ms((uint64_t)ms);
                    if (park_until == 0 || when < park_until) {
                        park_until = when;
                    }
                }
            } else if (slot->type == FILE_DESCRIPTOR_EPOLL) {
                park_until = epoll_watch_nested(slot->epoll, depth + 1, park_until);
            }
        }
        done_with_descriptor(&use);
    }
    kfree(r);
    return park_until;
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
    install_file_descriptor(self, fd, type, object,
                            (flags & OS_FILE_DESCRIPTOR_CLOEXEC) ? 1 : 0,
                            (flags & OS_FILE_DESCRIPTOR_NONBLOCK) ? 1 : 0, 0);
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
    if (flags & ~(uint64_t)OS_TFD_ABSTIME) {
        return -1;
    }
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    int set = -1;
    int absolute = (flags & OS_TFD_ABSTIME) != 0;
    os_itimer_t had = {0, 0};
    if (slot->type == FILE_DESCRIPTOR_TIMER) {
        struct timerfd *t = slot->timer;
        set = timerfd_settime(t, timer_clock_ns(t, absolute), absolute, want.value_ns,
                              want.interval_ns, &had.value_ns, &had.interval_ns);
    }
    done_with_descriptor(&use);
    if (set != 0) {
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
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    int is_timer = slot->type == FILE_DESCRIPTOR_TIMER;
    os_itimer_t out = {0, 0};
    if (is_timer) {
        timerfd_gettime(slot->timer, clock_now_ns(), &out.value_ns, &out.interval_ns);
    }
    done_with_descriptor(&use);
    if (!is_timer) {
        return -1;
    }
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
    os_epoll_event_t ev = {0, 0, 0};
    if (op != EPOLL_CTL_DEL && copy_from_user(&ev, ev_pointer, sizeof(ev)) != 0) {
        return -1;
    }
    /* Both held: the set, and the descriptor whose object is written into
       it - a watch registered for an object a sibling had just closed and
       freed would compare equal to whatever the allocator put there next. */
    file_descriptor_use_t set_use;
    file_descriptor_slot_t *set = use_descriptor(epfd, &set_use);
    if (!set) {
        return -1;
    }
    file_descriptor_use_t watched_use;
    file_descriptor_slot_t *watched = use_descriptor(fd, &watched_use);
    long result = -1;
    /* M226: a set may hold another set, as on Linux - but not itself, not a
       set that already reaches it, and not deeper than EPOLL_MAX_NESTING.
       Only an ADD makes a new edge; MOD and DEL change what an existing one
       asks for. */
    if (set->type == FILE_DESCRIPTOR_EPOLL && watched && watched->type == FILE_DESCRIPTOR_EPOLL &&
        op == EPOLL_CTL_ADD && epoll_reaches(watched->epoll, set->epoll, 1)) {
        result = -OS_ERROR_LOOP;
    } else if (set->type == FILE_DESCRIPTOR_EPOLL && watched) {
        struct epoll *ep = set->epoll;
        result = epoll_control_set(ep, (int)op, (int)fd, (const void *)watched->pipe, ev.events,
                                   ev.data);
        if (result == 0) {
            scheduler_wake_object(ep);
        }
    }
    if (watched) {
        done_with_descriptor(&watched_use);
    }
    done_with_descriptor(&set_use);
    return result;
}

/* The set is held for the whole wait, as Linux holds it: a sibling's close
   of the epoll descriptor does not end a wait already on it. Each watched
   descriptor is held only while it is asked (epoll_mask_callback, and the
   timer deadlines below) - what is registered with the scheduler is an
   address to be woken by, never dereferenced. */
static long epoll_wait_on(struct epoll *ep, uint64_t out_pointer, uint64_t maxevents,
                          uint64_t timeout_ms) {
    long timeout = (long)timeout_ms;
    uint64_t now = clock_monotonic_ms();
    uint64_t deadline = (timeout < 0) ? 0 : clock_deadline_ms((uint64_t)timeout);
    epoll_ev_t evs[EPOLL_MAX_WATCH];
    int watched_fds[EPOLL_MAX_WATCH];
    const void *watched_objects[EPOLL_MAX_WATCH];
    for (;;) {
        scheduler_watch_begin();
        scheduler_watch_add(ep);
        int watched = epoll_objects(ep, watched_fds, watched_objects, EPOLL_MAX_WATCH);
        int holds_a_set = 0;
        for (int i = 0; i < watched; i++) {
            file_descriptor_type_t type =
                file_descriptor_peek_type(scheduler_current(), watched_fds[i]);
            if (type == FILE_DESCRIPTOR_STDIN) {
                scheduler_watch_add(SCHEDULER_INPUT_OBJECT);
            } else {
                scheduler_watch_add(watched_objects[i]);
            }
            if (type == FILE_DESCRIPTOR_EPOLL) {
                holds_a_set = 1;
            }
        }
        int n = epoll_scan(ep, epoll_mask_callback, (void *)0, evs, (int)maxevents);
        if (n > 0) {
            scheduler_watch_end();
            if (copy_to_user(out_pointer, evs, (size_t)n * sizeof(epoll_ev_t)) != 0) {
                return -1;
            }
            return n;
        }
        if (timeout == 0) {
            scheduler_watch_end();
            return 0;
        }
        if (scheduler_signal_pending()) {
            scheduler_watch_end();
            return -OS_ERROR_INTR;
        }
        now = clock_monotonic_ms();
        if (timeout > 0 && now >= deadline) {
            scheduler_watch_end();
            return 0;
        }
        uint64_t park_until = deadline;
        if (holds_a_set) {
            for (int i = 0; i < watched; i++) {
                int fd = watched_fds[i];
                if (fd < 0) {
                    continue;
                }
                file_descriptor_use_t use;
                file_descriptor_slot_t *slot = use_descriptor((uint64_t)fd, &use);
                if (!slot) {
                    continue;
                }
                if (slot->type == FILE_DESCRIPTOR_EPOLL &&
                    (const void *)slot->epoll == watched_objects[i]) {
                    park_until = epoll_watch_nested(slot->epoll, 2, park_until);
                }
                done_with_descriptor(&use);
            }
        }
        for (int i = 0; i < watched; i++) {
            int fd = watched_fds[i];
            if (fd < 0 || file_descriptor_peek_type(scheduler_current(), fd) != FILE_DESCRIPTOR_TIMER) {
                continue;
            }
            file_descriptor_use_t use;
            file_descriptor_slot_t *slot = use_descriptor((uint64_t)fd, &use);
            if (!slot) {
                continue;
            }
            long ms = -1;
            if (slot->type == FILE_DESCRIPTOR_TIMER &&
                (const void *)slot->timer == watched_objects[i]) {
                ms = timerfd_next_ms(slot->timer, clock_now_ns());
            }
            done_with_descriptor(&use);
            if (ms < 0) {
                continue;
            }
            uint64_t when = clock_deadline_ms((uint64_t)ms);
            if (park_until == 0 || when < park_until) {
                park_until = when;
            }
        }
        scheduler_watch_block(park_until);
    }
}

static long sys_epoll_wait(uint64_t epfd, uint64_t out_pointer, uint64_t maxevents, uint64_t timeout_ms, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    if (maxevents == 0 || maxevents > 0x7FFFFFFF / sizeof(os_epoll_event_t)) {
        return -1;
    }
    /* M226: maxevents is the size of the caller's buffer, not a limit on the
       set - libuv passes 1024 - and a set never has more to report than it
       has watches, so a bigger buffer is a buffer partly unused. It was a
       refusal, which libuv's loop reads as an impossible errno and aborts. */
    if (maxevents > EPOLL_MAX_WATCH) {
        maxevents = EPOLL_MAX_WATCH;
    }
    if (!user_range_ok(out_pointer, maxevents * sizeof(os_epoll_event_t), 1)) {
        return -1;
    }
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(epfd, &use);
    if (!slot) {
        return -1;
    }
    long result = slot->type == FILE_DESCRIPTOR_EPOLL
                      ? epoll_wait_on(slot->epoll, out_pointer, maxevents, timeout_ms)
                      : -1;
    done_with_descriptor(&use);
    return result;
}

/* M203: why each waitfds returned, for the stick log - see
   syscall_waitfds_returns. */
static uint64_t waitfds_returns[WAITFDS_RETURN_REASONS];

uint64_t syscall_waitfds_returns(int reason) {
    return (reason >= 0 && reason < WAITFDS_RETURN_REASONS)
               ? __atomic_load_n(&waitfds_returns[reason], __ATOMIC_RELAXED)
               : 0;
}

#define WAITFDS_RETURN(reason, value)                                              \
    do {                                                                           \
        __atomic_fetch_add(&waitfds_returns[reason], 1, __ATOMIC_RELAXED);         \
        return (value);                                                            \
    } while (0)

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
    uint64_t deadline = (timeout < 0) ? 0 : clock_deadline_ms((uint64_t)timeout);

    for (;;) {
        scheduler_watch_begin();
        /* The pointer is not a descriptor, and its one reader ends this wait
           on movement below - so it watches input whatever it passed. */
        int reads_pointer = pointer_wakes(self);
        if (reads_pointer) {
            scheduler_watch_add(SCHEDULER_INPUT_OBJECT);
        }
        uint64_t park_until = deadline;
        /* M225 (fd-use-holds): each descriptor held while it is watched and
           asked - its object is asked whether it is ready, and a sibling's
           close could free it between reading the slot and asking. Watched
           before asked, so a wake between the two is not lost. */
        for (uint64_t i = 0; i < count; i++) {
            int fd = file_descriptors[i];
            file_descriptor_use_t use;
            file_descriptor_slot_t *slot = fd >= 0 ? use_descriptor((uint64_t)fd, &use)
                                                   : (file_descriptor_slot_t *)0;
            if (!slot) {
                continue;
            }
            scheduler_watch_add(slot->type == FILE_DESCRIPTOR_STDIN ? SCHEDULER_INPUT_OBJECT
                                                                    : (const void *)slot->pipe);
            if (slot->type == FILE_DESCRIPTOR_EPOLL) {
                park_until = epoll_watch_nested(slot->epoll, 1, park_until);
            }
            int ready = file_descriptor_is_ready(slot);
            done_with_descriptor(&use);
            if (ready) {
                scheduler_watch_end();
                WAITFDS_RETURN(WAITFDS_RETURN_READY, (long)i);
            }
        }
        /* M197: the pointer is not a descriptor. The one program that may
           read it used to find its movement by waking a hundred times a
           second; it waits properly now, so movement has to end the wait. */
        if (reads_pointer && mouse_pending()) {
            scheduler_watch_end();
            WAITFDS_RETURN(WAITFDS_RETURN_POINTER, -2);
        }
        /* M198: and so does a program ending. Its windows are the
           compositor's to take down, and its death is no descriptor's event:
           a compositor that was busy when the exit woke everybody found
           nothing ready afterwards and slept until its backstop - and the
           kill storm counted a window that was still up. */
        uint64_t exits = scheduler_exit_sequence();
        if ((self->caps & CAP_FRAMEBUFFER) && self->pml4_phys != virtual_memory_kernel_pml4_phys() &&
            exits != self->seen_exit_sequence) {
            self->seen_exit_sequence = exits;
            scheduler_watch_end();
            WAITFDS_RETURN(WAITFDS_RETURN_EXITS, -2);
        }
        if (timeout == 0) {
            scheduler_watch_end();
            WAITFDS_RETURN(WAITFDS_RETURN_ZERO, -2);
        }
        if (deadline != 0 && clock_monotonic_ms() >= deadline) {
            scheduler_watch_end();
            WAITFDS_RETURN(WAITFDS_RETURN_DEADLINE, -2);
        }

        scheduler_watch_block(park_until);
        if (scheduler_signal_pending()) {
            WAITFDS_RETURN(WAITFDS_RETURN_SIGNAL, -2);
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
    if (flags & SA_RESETHAND) {
        self->sig_resethand |= (1u << signo);
    } else {
        self->sig_resethand &= ~(1u << signo);
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

    /* Releasing the shared pages first was the whole answer while a process
       forking was one thread. It is not one for a browser: between the release
       and the copy below, a sibling on another core touches a histogram page,
       faults it back in writable, and the copy then makes it copy-on-write -
       so the parent's next write lands on a private copy and the memory it
       shares with its children stops being shared. Chromium's persistent
       histogram allocator noticed first, as "corrupt" (M187). So the copy is
       told which ranges are shared and leaves them alone however they got
       there. */
    virtual_memory_range_t *shared_ranges = (virtual_memory_range_t *)0;
    int shared_count = scheduler_shared_ranges(vm_owner, &shared_ranges);
    if (shared_count < 0) {
        return -1;
    }
    uint64_t child_pml4 = process_fork_address_space(parent->pml4_phys, shared_ranges,
                                                     shared_count);
    if (shared_ranges) {
        kfree(shared_ranges);
    }
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

    /* M225: the child is runnable from the moment task_fork let go of the
       lock, so nothing is read out of it or written into it from here: it
       may already have run, ended, been reaped by a sibling's waitpid(-1)
       and had its slot handed to somebody else. Its environment record was
       given to it inside task_fork, and its id is the one task_fork wrote
       down on this thread while the slot could not change. */
    return (long)scheduler_last_spawn((uint32_t *)0);
}

/* fcntl's record locks on a HELD open file. F_SETLKW can wait a long time,
   and while it waits a sibling thread may close the descriptor - which, by
   POSIX's close rule, gives back every lock the process has on the file. A
   lock this call is granted after that would belong to a file the process
   no longer has open, and nothing would ever give it back; Linux checks for
   exactly this race once the lock is granted and undoes it
   (fcntl_setlk's "close/fcntl race"), and so does this. */
static long record_lock_on(task_t *self, uint64_t fd, const file_descriptor_slot_t *slot,
                           uint64_t command, uint64_t arg) {
    os_flock_t request;
    if (slot->type != FILE_DESCRIPTOR_FILE ||
        copy_from_user(&request, arg, sizeof(request)) != 0) {
        return -1;
    }
    uint32_t ino = slot_inode(slot);
    if (ino == 0) {
        return -1;
    }
    int64_t start = request.start;
    int64_t length = request.length;
    if (request.whence == 1) {
        start += (int64_t)slot->file->offset;
    } else if (request.whence == 2) {
        start += (int64_t)virtual_file_system_handle_size(slot->file->handle);
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
    int owner = scheduler_record_lock_owner(self);
    if (command == F_GETLK_COMMAND) {
        os_flock_t ans;
        flock_test(ino, owner, request.type, (uint64_t)start, (uint64_t)length, &ans);
        return copy_to_user(arg, &ans, sizeof(ans)) == 0 ? 0 : -1;
    }
    for (;;) {
        uint64_t seq = scheduler_event_sequence();
        int r = flock_set(ino, owner, request.type, (uint64_t)start, (uint64_t)length);
        if (r == 0) {
            if (request.type == OS_FLOCK_UNLCK) {
                scheduler_wake_all(FLOCK_CHAN);
                return 0;
            }
            file_descriptor_use_t again;
            file_descriptor_slot_t *now = use_descriptor(fd, &again);
            int still_open = now && now->type == FILE_DESCRIPTOR_FILE && now->file == slot->file;
            if (now) {
                done_with_descriptor(&again);
            }
            if (!still_open) {
                flock_set(ino, owner, OS_FLOCK_UNLCK, (uint64_t)start, (uint64_t)length);
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

static long fcntl_on(task_t *self, uint64_t fd, const file_descriptor_slot_t *slot,
                     uint64_t command, uint64_t arg) {
    switch (command) {
    case F_GETFD_COMMAND:
        return slot->cloexec ? FILE_DESCRIPTOR_CLOEXEC_BIT : 0;
    case F_SETFD_COMMAND:
        return file_descriptor_set_flags(self->descriptor_table, (int)fd,
                                         (arg & FILE_DESCRIPTOR_CLOEXEC_BIT) ? 1 : 0, -1) == 0
                   ? 0
                   : -1;
    case F_GETFL_COMMAND: {
        long access;
        switch (slot->type) {
        case FILE_DESCRIPTOR_STDIN:      access = OPEN_READ; break;
        case FILE_DESCRIPTOR_STDOUT:     access = OPEN_WRITE; break;
        case FILE_DESCRIPTOR_PIPE_READ:  access = OPEN_READ; break;
        case FILE_DESCRIPTOR_PIPE_WRITE: access = OPEN_WRITE; break;
        case FILE_DESCRIPTOR_FILE:
            access = OPEN_READ | (slot->file->writable ? OPEN_WRITE : 0) |
                     (slot->file->synchronous ? OPEN_SYNC : 0);
            break;
        case FILE_DESCRIPTOR_SOCKET:     access = OPEN_READ | OPEN_WRITE; break;
        case FILE_DESCRIPTOR_UNIX:       access = OPEN_READ | OPEN_WRITE; break;
        case FILE_DESCRIPTOR_EVENT:      access = OPEN_READ | OPEN_WRITE; break;
        case FILE_DESCRIPTOR_TIMER:      access = OPEN_READ; break;
        case FILE_DESCRIPTOR_EPOLL:      access = OPEN_READ; break;
        case FILE_DESCRIPTOR_MEMFD:
            access = OPEN_READ | (slot->writable ? OPEN_WRITE : 0);
            break;
        default:            return -1;
        }
        return access | (slot->nonblock ? OS_NONBLOCK_BIT : 0);
    }
    case F_SETFL_COMMAND:
        return file_descriptor_set_flags(self->descriptor_table, (int)fd, -1,
                                         (arg & OS_NONBLOCK_BIT) ? 1 : 0) == 0
                   ? 0
                   : -1;
    case F_DUPFD_COMMAND:
    case F_DUPFD_CLOEXEC_COMMAND: {
        if (arg >= MAX_FILE_DESCRIPTORS) {
            return -1;
        }
        /* M225: claimed, copied and referenced under the table's lock, so
           a sibling closing `fd` at this moment cannot drop the object's
           last reference between the copy and the new descriptor's. */
        int i = descriptor_table_duplicate_lowest(self->descriptor_table, (int)fd, (int)arg,
                                                  command == F_DUPFD_CLOEXEC_COMMAND);
        return i < 0 ? -1 : (long)i;
    }
    case F_GETLK_COMMAND:
    case F_SETLK_COMMAND:
    case F_SETLKW_COMMAND:
        return record_lock_on(self, fd, slot, command, arg);
    default:
        return -1;
    }
}

/* M225 (fd-use-holds): held for the whole call, as Linux's fcntl holds its
   file - and "not open" means not live: a slot another thread has numbered
   and not yet filled was open to F_GETFD (it answered 0) and to the record
   lock commands. F_SETFD and F_SETFL on such a slot were already refused by
   file_descriptor_set_flags; all three are EBADF on Linux, which is what
   the C library makes of this -1. */
static long sys_fcntl(uint64_t fd, uint64_t command, uint64_t arg, uint64_t a4,
                      uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a5;
    (void)a6;
    file_descriptor_use_t use;
    file_descriptor_slot_t *slot = use_descriptor(fd, &use);
    if (!slot) {
        return -1;
    }
    long result = fcntl_on(scheduler_current(), fd, slot, command, arg);
    done_with_descriptor(&use);
    return result;
}

static int wait_status_of(const task_t *t) {
    return scheduler_wait_status(t);
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
        return -OS_ERROR_FAULT;
    }

    for (;;) {
        int any_children = 0;
        uint64_t seq = scheduler_event_sequence();
        task_t *only = (task_t *)0;

        if (want > 0) {
            task_t *t = scheduler_task_by_id((int)want);
            if (!t || t->reaped || t->parent_id != self->tgid) {
                return -1;
            }
            any_children = 1;
            only = t;
            if (scheduler_process_has_ended(t)) {
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
                if (!t || t->parent_id != self->tgid || t->reaped) {
                    continue;
                }
                if (want_pgid && t->pgid != want_pgid) {
                    continue;
                }
                any_children = 1;
                if (scheduler_process_has_ended(t)) {
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
                               clock_monotonic_ms() + 200, seq);
        } else {
            scheduler_block_on_sequence(SCHEDULER_POLL_CHAN,
                               clock_monotonic_ms() + 50, seq);
        }
    }
}

/* Follow a symbolic link once, in place.

   execve(2) follows links, and until M167 nothing here did - there were no
   links to follow. /proc/self/exe is the one that matters and it is the one
   every multi-process Chromium program uses: content::ChildProcessHost asks
   for the path of the executable to re-run and that is what Linux gives it,
   so a renderer, a utility process and the network service are all launched
   by exec'ing it. Reading it instead of following it gets the TEXT of the
   target path - "/bin/chromiumshell" - and an ELF header that begins with a
   slash is not one, which is why the launcher's message was "failed to
   execvp" on a file that is plainly there.

   Once rather than in a loop: this kernel's only links are procfs's, they
   point at real files, and a chain would have to be built before it could
   be followed. Resolving BEFORE the manifest is consulted is not incidental
   either - the capability set comes from the spawn path, and /proc/self/exe
   is not a name anybody can write a manifest entry for. */
static void follow_link_in_place(char *path, size_t capacity) {
    char target[LEANFS_MAX_PATH];
    int64_t n = virtual_file_system_readlink(path, target, sizeof(target) - 1);
    if (n <= 0 || (size_t)n >= sizeof(target)) {
        return;
    }
    target[n] = '\0';
    if (target[0] != '/') {
        return;
    }
    size_t length = (size_t)n + 1;
    if (length > capacity) {
        return;
    }
    k_memcpy(path, target, length);
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
    follow_link_in_place(path, sizeof(path));

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

    /* M225 (fd-use-holds): the environment inherited is the PROCESS's
       record, COPIED out under the scheduler lock (scheduler_duplicate_env)
       as fork and spawn copy it. This pointed into the owner's block with no
       lock - which an exec on the leader replaces, and the last member's
       reap frees. A copy that cannot be made fails the exec rather than
       starting the new program with no environment; a process that has none
       allocates nothing. */
    const char *inherited[USER_ENV_MAX_VARS + 1];
    const char *const *effective = v.envp;
    char *own_env = (char *)0;
    if (!effective) {
        uint32_t own_length = 0;
        uint32_t own_count = 0;
        int no_memory = 0;
        own_env = scheduler_duplicate_env(scheduler_vm_owner(self), &own_length, &own_count,
                                          &no_memory);
        if (no_memory) {
            free_vectors(&v);
            return -1;
        }
        if (own_env && own_count) {
            uint32_t n = 0;
            uint32_t off = 0;
            while (off < own_length && n < USER_ENV_MAX_VARS) {
                inherited[n++] = own_env + off;
                while (off < own_length && own_env[off]) {
                    off++;
                }
                off++;
            }
            inherited[n] = (const char *)0;
            effective = inherited;
        }
    }

    uint64_t entry = 0;
    int not_cached = 0;
    uint64_t new_pml4 = process_build_address_space_from_path(path, v.argv, effective, &entry,
                                                              &not_cached);
    if (not_cached) {
        uint8_t *image = (uint8_t *)kmalloc(st.size ? st.size : 1);
        if (!image) {
            kfree(own_env);
            free_vectors(&v);
            return -1;
        }
        int64_t size = virtual_file_system_read(path, image, st.size);
        if (size < 2 || (image[0] == '#' && image[1] == '!') ||
            !elf_validate(image, (size_t)size)) {
            kfree(image);
            kfree(own_env);
            free_vectors(&v);
            return -1;
        }
        new_pml4 = process_build_address_space(image, (size_t)size, v.argv, effective, &entry);
        kfree(image);
    }
    /* The new program's stack has its own copy by now. */
    kfree(own_env);
    if (new_pml4 == 0) {
        free_vectors(&v);
        return -1;
    }

    uint64_t old_pml4 = self->pml4_phys;
    uint64_t peak = virtual_memory_rss_peak_pages(old_pml4);
    if (peak > self->max_rss_pages) {
        self->max_rss_pages = peak;
    }
    /* Release the OLD address space's shared and memfd-backed regions before
       tearing it down, exactly as task_exit does. process_destroy_address_space
       walks the page tables and frees every present frame; a memfd frame is
       owned by the memfd object, not by this address space, so freeing it in
       the walk leaves the memfd holding a dangling reference it frees a second
       time when the descriptor closes - a double free that only surfaced once
       a browser passed memfds to children that then exec'd (M173). This runs
       while self->pml4_phys is still old_pml4, which release_shared_range
       reads, and it unmaps those pages so the walk below skips them; the
       memfd region references are dropped by scheduler_regions_forget_memfds
       just after, as before. */
    scheduler_release_shared_range(self, USER_MMAP_BASE, USER_MMAP_LIMIT);
    scheduler_load_address_space(self, new_pml4);
    process_destroy_address_space(old_pml4);

    /* An exec replaces the program, so it replaces the command line: a
       /proc/<pid>/cmdline still naming what this process used to be is how
       every child of a browser looked identical. */
    scheduler_set_cmdline(self, v.argv);

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

    /* M225: each detached under the table's lock and let go of after it,
       as close() does - a sibling thread may be closing or duplicating the
       same descriptor right now. */
    {
        file_descriptor_slot_t closing;
        int at = 0;
        while ((at = descriptor_table_detach_cloexec(self->descriptor_table, at, &closing)) >= 0) {
            drop_record_locks(self, &closing);
            file_descriptor_release(&closing);
            at++;
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
    self->sig_resethand = 0;
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
    [SYS_display] = sys_display,
    [SYS_sockopt] = sys_sockopt,
    [SYS_sockident] = sys_sockident,
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
    [SYS_peek] = sys_peek,
    [SYS_clock_ns] = sys_clock_ns,
    [SYS_thread_detach] = sys_thread_detach,
    [SYS_task_end_status] = sys_task_end_status,
    [SYS_wireless] = sys_wireless,
    [SYS_getrlimit] = sys_getrlimit,
    [SYS_setrlimit] = sys_setrlimit,
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
    if (self->sig_resethand & (1u << signo)) {
        /* One delivery, then the default - which is what lets a handler that
           cleans up and re-raises actually end the process. */
        self->sig_handler[signo] = SIG_DFL_ADDR;
        self->sig_resethand &= ~(1u << signo);
        self->sig_siginfo &= ~(1u << signo);
    }
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
    task_t *self = scheduler_current();
    uint16_t outer = self->kernel_activity;
    self->kernel_activity = (uint16_t)(num < SYSCALL_COUNT ? num + 1 : KERNEL_ACTIVITY_NONE);
    self->syscalls++;

    syscall_dispatch(regs);

    /* M225 (fd-use-holds): every descriptor a call got, it put. The
       outermost call only - one made inside another (none is, today) returns
       with its caller's uses still rightly on the list. A load and a branch. */
    if (outer == KERNEL_ACTIVITY_NONE && self->descriptor_uses) {
        file_descriptor_uses_settled(self);
    }

    self->kernel_activity = outer;

    syscall_counters_record((int)num, timing ? (tsc_read() - started) : 0);
}

static void syscall_dispatch(isr_regs_t *regs) {
    task_t *self = scheduler_current();
    if (self->pending_signal != 0) {
        int sig = self->pending_signal;
        self->pending_signal = 0;
        task_exit_with_signal(sig);
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
