#include "virtual_file_system_operations.h"
#include "virtual_file_system.h"
#include "architecture/x86_64/ioapic.h"
#include "architecture/x86_64/symmetric_multiprocessing.h"

#include "drivers/pit.h"
#include "library/kernel_library.h"
#include "memory_management/heap.h"
#include "memory_management/physical_memory.h"
#include "profile/sampler.h"
#include "profile/syscall_counters.h"
#include "scheduler/scheduler.h"

#define PROCESS_MAX_OPEN 16

#define PROCESS_BUFFER_SMALL 512

#define PROCESS_BUFFER_LARGE 8192

typedef struct {
    int used;
    uint32_t length;
    uint32_t cap;
    char *buffer;
} process_file_t;

static process_file_t process_files[PROCESS_MAX_OPEN];

void procfs_init(void) {
    k_memset(process_files, 0, sizeof(process_files));
}

static uint32_t put_string(char *destination, uint32_t at, uint32_t cap, const char *s) {
    while (*s && at < cap - 1) {
        destination[at++] = *s++;
    }
    return at;
}

static uint32_t put_dec(char *destination, uint32_t at, uint32_t cap, uint64_t v) {
    char temporary[24];
    int n = 0;
    if (v == 0) {
        temporary[n++] = '0';
    }
    while (v > 0) {
        temporary[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && at < cap - 1) {
        destination[at++] = temporary[--n];
    }
    return at;
}

#define PROCFS_INO_BASE 0x50000000u

enum {
    P_NONE = 0,
    P_ROOT,
    P_UPTIME,
    P_MEMINFO,
    P_MOUNTS,
    P_INTERRUPTS,
    P_PIDDIR,
    P_STATUS,
    P_CMDLINE,
    P_EXE,
    P_PROFILE,
    P_SYSCALLS,
};

static uint32_t buffer_cap_for(int kind) {
    return (kind == P_PROFILE || kind == P_SYSCALLS || kind == P_INTERRUPTS)
               ? PROCESS_BUFFER_LARGE
               : PROCESS_BUFFER_SMALL;
}

static int classify(const char *rel, int *out_pid) {
    if (!rel || rel[0] != '/') {
        return P_NONE;
    }
    if (rel[1] == '\0') {
        return P_ROOT;
    }
    const char *p = rel + 1;

    if (k_strcmp(p, "uptime") == 0) {
        return P_UPTIME;
    }
    if (k_strcmp(p, "meminfo") == 0) {
        return P_MEMINFO;
    }
    if (k_strcmp(p, "mounts") == 0) {
        return P_MOUNTS;
    }
    if (k_strcmp(p, "interrupts") == 0) {
        return P_INTERRUPTS;
    }
    if (k_strcmp(p, "profile") == 0) {
        return P_PROFILE;
    }
    if (k_strcmp(p, "syscalls") == 0) {
        return P_SYSCALLS;
    }

    int pid = -1;
    const char *rest = p;
    if (p[0] == 's' && p[1] == 'e' && p[2] == 'l' && p[3] == 'f' &&
        (p[4] == '\0' || p[4] == '/')) {
        pid = scheduler_current()->id;
        rest = p + 4;
    } else {
        int v = 0;
        int digits = 0;
        while (*rest >= '0' && *rest <= '9') {
            v = v * 10 + (*rest - '0');
            rest++;
            digits++;
        }
        if (digits == 0 || (*rest != '\0' && *rest != '/')) {
            return P_NONE;
        }
        pid = v;
    }
    if (!scheduler_task_by_id(pid)) {
        return P_NONE;
    }
    *out_pid = pid;

    if (*rest == '\0') {
        return P_PIDDIR;
    }
    rest++;
    if (k_strcmp(rest, "status") == 0) {
        return P_STATUS;
    }
    if (k_strcmp(rest, "cmdline") == 0) {
        return P_CMDLINE;
    }
    if (k_strcmp(rest, "exe") == 0) {
        return P_EXE;
    }
    return P_NONE;
}

static const char *state_name(int state) {
    switch (state) {
    case TASK_READY:      return "ready";
    case TASK_RUNNING:    return "running";
    case TASK_BLOCKED:    return "blocked";
    case TASK_STOPPED:    return "stopped";
    case TASK_TERMINATED: return "terminated";
    default:              return "free";
    }
}

static void generate(process_file_t *f, int kind, int pid) {
    uint32_t at = 0;
    uint32_t cap = f->cap;
    switch (kind) {
    case P_UPTIME:
        at = put_dec(f->buffer, at, cap, pit_get_ticks() * (1000 / PIT_HZ) / 1000);
        at = put_string(f->buffer, at, cap, ".");
        at = put_dec(f->buffer, at, cap,
                     (pit_get_ticks() * (1000 / PIT_HZ) / 100) % 10);
        at = put_string(f->buffer, at, cap, "\n");
        break;
    case P_MEMINFO: {
        uint64_t free_kb = physical_memory_free_frame_count() * 4;
        at = put_string(f->buffer, at, cap, "MemFree:        ");
        at = put_dec(f->buffer, at, cap, free_kb);
        at = put_string(f->buffer, at, cap, " kB\n");
        break;
    }
    case P_MOUNTS: {
        const char *prefix;
        const char *type;
        for (int i = 0; virtual_file_system_mount_info(i, &prefix, &type); i++) {
            at = put_string(f->buffer, at, cap, type);
            at = put_string(f->buffer, at, cap, " ");
            at = put_string(f->buffer, at, cap, prefix);
            at = put_string(f->buffer, at, cap, " ");
            at = put_string(f->buffer, at, cap, type);
            at = put_string(f->buffer, at, cap, " rw 0 0\n");
        }
        break;
    }
    case P_INTERRUPTS: {
        at = put_string(f->buffer, at, cap, "     ");
        for (int c = 0; c < smp_cpu_count && c < MAX_CPUS; c++) {
            at = put_string(f->buffer, at, cap, "     CPU");
            at = put_dec(f->buffer, at, cap, (uint64_t)c);
        }
        at = put_string(f->buffer, at, cap, "\n");
        for (int v = 0; v < 256; v++) {
            uint64_t total = 0;
            for (int c = 0; c < MAX_CPUS; c++) {
                total += ioapic_irq_count((uint8_t)v, c);
            }
            if (total == 0) {
                continue;
            }
            at = put_dec(f->buffer, at, cap, (uint64_t)v);
            at = put_string(f->buffer, at, cap, ":");
            for (int c = 0; c < smp_cpu_count && c < MAX_CPUS; c++) {
                at = put_string(f->buffer, at, cap, " ");
                at = put_dec(f->buffer, at, cap, ioapic_irq_count((uint8_t)v, c));
            }
            at = put_string(f->buffer, at, cap,
                         ioapic_available() ? "  IO-APIC\n" : "  XT-PIC\n");
        }
        break;
    }
    case P_STATUS: {
        task_t *t = scheduler_task_by_id(pid);
        if (!t) {
            break;
        }
        at = put_string(f->buffer, at, cap, "Name:\t");
        at = put_string(f->buffer, at, cap, t->name[0] ? t->name : "(unnamed)");
        at = put_string(f->buffer, at, cap, "\nState:\t");
        at = put_string(f->buffer, at, cap, state_name(t->state));
        at = put_string(f->buffer, at, cap, "\nPid:\t");
        at = put_dec(f->buffer, at, cap, (uint64_t)t->id);
        at = put_string(f->buffer, at, cap, "\nPPid:\t");
        at = put_dec(f->buffer, at, cap, (uint64_t)(t->parent_id < 0 ? 0 : t->parent_id));
        at = put_string(f->buffer, at, cap, "\nPGid:\t");
        at = put_dec(f->buffer, at, cap, (uint64_t)t->pgid);
        at = put_string(f->buffer, at, cap, "\nSid:\t");
        at = put_dec(f->buffer, at, cap, (uint64_t)t->sid);
        at = put_string(f->buffer, at, cap, "\n");
        break;
    }
    case P_CMDLINE: {
        task_t *t = scheduler_task_by_id(pid);
        if (t) {
            at = put_string(f->buffer, at, cap, t->name);
        }
        break;
    }
    case P_EXE: {
        task_t *t = scheduler_task_by_id(pid);
        if (t) {
            at = put_string(f->buffer, at, cap, "/bin/");
            at = put_string(f->buffer, at, cap, t->name);
        }
        break;
    }
    case P_PROFILE: {
        prof_statistics_t st;
        profile_get_statistics(&st);
        at = put_string(f->buffer, at, cap, "running: ");
        at = put_dec(f->buffer, at, cap, (uint64_t)(st.running ? 1 : 0));
        at = put_string(f->buffer, at, cap, "\nsamples: ");
        at = put_dec(f->buffer, at, cap, st.samples);
        at = put_string(f->buffer, at, cap, "\nkernel: ");
        at = put_dec(f->buffer, at, cap, st.kernel);
        at = put_string(f->buffer, at, cap, "\nuser: ");
        at = put_dec(f->buffer, at, cap, st.user);
        at = put_string(f->buffer, at, cap, "\nidle: ");
        at = put_dec(f->buffer, at, cap, st.idle);
        at = put_string(f->buffer, at, cap, "\ndistinct: ");
        at = put_dec(f->buffer, at, cap, st.distinct);
        at = put_string(f->buffer, at, cap, "\noverflow: ");
        at = put_dec(f->buffer, at, cap, st.overflow);
        at = put_string(f->buffer, at, cap, "\n");
        break;
    }
    case P_SYSCALLS: {
        at = put_string(f->buffer, at, cap, "# num calls cycles\n");
        for (int i = 0; i < SYSCALL_COUNT; i++) {
            syscall_counters_entry_t e;
            syscall_counters_get(i, &e);
            if (e.calls == 0) {
                continue;
            }
            at = put_dec(f->buffer, at, cap, (uint64_t)i);
            at = put_string(f->buffer, at, cap, " ");
            at = put_dec(f->buffer, at, cap, e.calls);
            at = put_string(f->buffer, at, cap, " ");
            at = put_dec(f->buffer, at, cap, e.cycles);
            at = put_string(f->buffer, at, cap, "\n");
        }
        break;
    }
    default:
        break;
    }
    f->buffer[at] = '\0';
    f->length = at;
}

static int process_exists(const char *rel) {
    int pid = 0;
    return classify(rel, &pid) != P_NONE;
}

static int process_is_directory(const char *rel) {
    int pid = 0;
    int k = classify(rel, &pid);
    return k == P_ROOT || k == P_PIDDIR;
}

static int process_stat(const char *rel, leanfs_stat_t *out) {
    int pid = 0;
    int k = classify(rel, &pid);
    if (k == P_NONE) {
        return -1;
    }
    out->mtime = 0;
    out->is_directory = (k == P_ROOT || k == P_PIDDIR) ? 1 : 0;
    out->is_link = 0;
    out->inode = PROCFS_INO_BASE + ((uint32_t)pid << 8) + (uint32_t)k;
    if (out->is_directory) {
        out->size = 0;
        return 0;
    }
    static char probe_buffer[PROCESS_BUFFER_LARGE];
    static process_file_t probe;
    probe.buffer = probe_buffer;
    probe.cap = PROCESS_BUFFER_LARGE;
    generate(&probe, k, pid);
    out->size = probe.length;
    return 0;
}

static int process_open(const char *rel, int create) {
    (void)create;
    int pid = 0;
    int k = classify(rel, &pid);
    if (k == P_NONE || k == P_ROOT || k == P_PIDDIR) {
        return -1;
    }
    for (int i = 0; i < PROCESS_MAX_OPEN; i++) {
        if (!process_files[i].used) {
            uint32_t cap = buffer_cap_for(k);
            char *buffer = (char *)kmalloc(cap);
            if (!buffer) {
                return -1;
            }
            process_files[i].used = 1;
            process_files[i].buffer = buffer;
            process_files[i].cap = cap;
            generate(&process_files[i], k, pid);
            return i;
        }
    }
    return -1;
}

static void process_close(int handle) {
    if (handle < 0 || handle >= PROCESS_MAX_OPEN || !process_files[handle].used) {
        return;
    }
    kfree(process_files[handle].buffer);
    process_files[handle].buffer = (char *)0;
    process_files[handle].cap = 0;
    process_files[handle].length = 0;
    process_files[handle].used = 0;
}

static int64_t process_read(int handle, void *buffer, size_t length, uint32_t off) {
    if (handle < 0 || handle >= PROCESS_MAX_OPEN || !process_files[handle].used) {
        return -1;
    }
    process_file_t *f = &process_files[handle];
    if (off >= f->length) {
        return 0;
    }
    uint32_t avail = f->length - off;
    uint32_t n = (length < avail) ? (uint32_t)length : avail;
    k_memcpy(buffer, f->buffer + off, n);
    return (int64_t)n;
}

static int64_t process_write(int handle, const void *buffer, size_t length, uint32_t off) {
    (void)handle;
    (void)buffer;
    (void)length;
    (void)off;
    return -1;
}

static uint32_t process_size(int handle) {
    if (handle < 0 || handle >= PROCESS_MAX_OPEN || !process_files[handle].used) {
        return 0;
    }
    return process_files[handle].length;
}

static int process_handle_stat(int handle, leanfs_stat_t *out) {
    if (handle < 0 || handle >= PROCESS_MAX_OPEN || !process_files[handle].used) {
        return -1;
    }
    out->size = process_files[handle].length;
    out->mtime = 0;
    out->is_directory = 0;
    out->is_link = 0;
    out->inode = PROCFS_INO_BASE + 0x00800000u + (uint32_t)handle;
    return 0;
}

static const char *const PID_FILES[] = {"status", "cmdline", "exe"};
static const char *const ROOT_FILES[] = {"uptime", "meminfo", "mounts",
                                         "interrupts", "profile", "syscalls"};
#define ROOT_FILE_COUNT ((uint32_t)(sizeof(ROOT_FILES) / sizeof(ROOT_FILES[0])))

static int process_readdir(const char *rel, uint32_t *cookie, leanfs_directory_entry_t *out) {
    int pid = 0;
    int k = classify(rel, &pid);
    uint32_t i = *cookie;

    if (k == P_PIDDIR) {
        if (i >= 3) {
            return 0;
        }
        out->inode = 0;
        out->is_directory = 0;
        out->is_link = 0;
        k_strlcpy(out->name, PID_FILES[i], sizeof(out->name));
        *cookie = i + 1;
        return 1;
    }
    if (k != P_ROOT) {
        return -1;
    }
    if (i < ROOT_FILE_COUNT) {
        out->inode = 0;
        out->is_directory = 0;
        out->is_link = 0;
        k_strlcpy(out->name, ROOT_FILES[i], sizeof(out->name));
        *cookie = i + 1;
        return 1;
    }
    uint32_t slot = i - ROOT_FILE_COUNT;
    int total = scheduler_task_count();
    while ((int)slot < total) {
        task_t *t = scheduler_task_by_slot((int)slot);
        if (t && t->state != TASK_TERMINATED) {
            out->inode = (uint32_t)t->id;
            out->is_directory = 1;
            out->is_link = 0;
            char name[16];
            uint32_t at = put_dec(name, 0, sizeof(name), (uint64_t)t->id);
            name[at] = '\0';
            k_strlcpy(out->name, name, sizeof(out->name));
            *cookie = i + 1;
            return 1;
        }
        slot++;
        i++;
    }
    return 0;
}

static const virtual_file_system_ops_t PROCFS_OPS = {
    .stat = process_stat,
    .is_directory = process_is_directory,
    .exists = process_exists,
    .open = process_open,
    .read = process_read,
    .write = process_write,
    .size = process_size,
    .handle_stat = process_handle_stat,
    .readdir = process_readdir,
    .close = process_close,
};

const virtual_file_system_ops_t *procfs_ops(void) {
    return &PROCFS_OPS;
}
