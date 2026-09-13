#include "vfsops.h"
#include "vfs.h"
#include "architecture/x86_64/ioapic.h"
#include "architecture/x86_64/smp.h"

#include "drivers/pit.h"
#include "library/libk.h"
#include "memory_management/heap.h"
#include "memory_management/pmm.h"
#include "profile/sampler.h"
#include "profile/syscount.h"
#include "scheduler/sched.h"

#define PROC_MAX_OPEN 16

#define PROC_BUF_SMALL 512

#define PROC_BUF_LARGE 8192

typedef struct {
    int used;
    uint32_t len;
    uint32_t cap;
    char *buf;
} proc_file_t;

static proc_file_t proc_files[PROC_MAX_OPEN];

void procfs_init(void) {
    k_memset(proc_files, 0, sizeof(proc_files));
}

static uint32_t put_str(char *dst, uint32_t at, uint32_t cap, const char *s) {
    while (*s && at < cap - 1) {
        dst[at++] = *s++;
    }
    return at;
}

static uint32_t put_dec(char *dst, uint32_t at, uint32_t cap, uint64_t v) {
    char tmp[24];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && at < cap - 1) {
        dst[at++] = tmp[--n];
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

static uint32_t buf_cap_for(int kind) {
    return (kind == P_PROFILE || kind == P_SYSCALLS || kind == P_INTERRUPTS)
               ? PROC_BUF_LARGE
               : PROC_BUF_SMALL;
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
        pid = sched_current()->id;
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
    if (!sched_task_by_id(pid)) {
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

static void generate(proc_file_t *f, int kind, int pid) {
    uint32_t at = 0;
    uint32_t cap = f->cap;
    switch (kind) {
    case P_UPTIME:
        at = put_dec(f->buf, at, cap, pit_get_ticks() * (1000 / PIT_HZ) / 1000);
        at = put_str(f->buf, at, cap, ".");
        at = put_dec(f->buf, at, cap,
                     (pit_get_ticks() * (1000 / PIT_HZ) / 100) % 10);
        at = put_str(f->buf, at, cap, "\n");
        break;
    case P_MEMINFO: {
        uint64_t free_kb = pmm_free_frame_count() * 4;
        at = put_str(f->buf, at, cap, "MemFree:        ");
        at = put_dec(f->buf, at, cap, free_kb);
        at = put_str(f->buf, at, cap, " kB\n");
        break;
    }
    case P_MOUNTS: {
        const char *prefix;
        const char *type;
        for (int i = 0; vfs_mount_info(i, &prefix, &type); i++) {
            at = put_str(f->buf, at, cap, type);
            at = put_str(f->buf, at, cap, " ");
            at = put_str(f->buf, at, cap, prefix);
            at = put_str(f->buf, at, cap, " ");
            at = put_str(f->buf, at, cap, type);
            at = put_str(f->buf, at, cap, " rw 0 0\n");
        }
        break;
    }
    case P_INTERRUPTS: {
        at = put_str(f->buf, at, cap, "     ");
        for (int c = 0; c < smp_cpu_count && c < MAX_CPUS; c++) {
            at = put_str(f->buf, at, cap, "     CPU");
            at = put_dec(f->buf, at, cap, (uint64_t)c);
        }
        at = put_str(f->buf, at, cap, "\n");
        for (int v = 0; v < 256; v++) {
            uint64_t total = 0;
            for (int c = 0; c < MAX_CPUS; c++) {
                total += ioapic_irq_count((uint8_t)v, c);
            }
            if (total == 0) {
                continue;
            }
            at = put_dec(f->buf, at, cap, (uint64_t)v);
            at = put_str(f->buf, at, cap, ":");
            for (int c = 0; c < smp_cpu_count && c < MAX_CPUS; c++) {
                at = put_str(f->buf, at, cap, " ");
                at = put_dec(f->buf, at, cap, ioapic_irq_count((uint8_t)v, c));
            }
            at = put_str(f->buf, at, cap,
                         ioapic_available() ? "  IO-APIC\n" : "  XT-PIC\n");
        }
        break;
    }
    case P_STATUS: {
        task_t *t = sched_task_by_id(pid);
        if (!t) {
            break;
        }
        at = put_str(f->buf, at, cap, "Name:\t");
        at = put_str(f->buf, at, cap, t->name[0] ? t->name : "(unnamed)");
        at = put_str(f->buf, at, cap, "\nState:\t");
        at = put_str(f->buf, at, cap, state_name(t->state));
        at = put_str(f->buf, at, cap, "\nPid:\t");
        at = put_dec(f->buf, at, cap, (uint64_t)t->id);
        at = put_str(f->buf, at, cap, "\nPPid:\t");
        at = put_dec(f->buf, at, cap, (uint64_t)(t->parent_id < 0 ? 0 : t->parent_id));
        at = put_str(f->buf, at, cap, "\nPGid:\t");
        at = put_dec(f->buf, at, cap, (uint64_t)t->pgid);
        at = put_str(f->buf, at, cap, "\nSid:\t");
        at = put_dec(f->buf, at, cap, (uint64_t)t->sid);
        at = put_str(f->buf, at, cap, "\n");
        break;
    }
    case P_CMDLINE: {
        task_t *t = sched_task_by_id(pid);
        if (t) {
            at = put_str(f->buf, at, cap, t->name);
        }
        break;
    }
    case P_EXE: {
        task_t *t = sched_task_by_id(pid);
        if (t) {
            at = put_str(f->buf, at, cap, "/bin/");
            at = put_str(f->buf, at, cap, t->name);
        }
        break;
    }
    case P_PROFILE: {
        prof_stats_t st;
        profile_get_stats(&st);
        at = put_str(f->buf, at, cap, "running: ");
        at = put_dec(f->buf, at, cap, (uint64_t)(st.running ? 1 : 0));
        at = put_str(f->buf, at, cap, "\nsamples: ");
        at = put_dec(f->buf, at, cap, st.samples);
        at = put_str(f->buf, at, cap, "\nkernel: ");
        at = put_dec(f->buf, at, cap, st.kernel);
        at = put_str(f->buf, at, cap, "\nuser: ");
        at = put_dec(f->buf, at, cap, st.user);
        at = put_str(f->buf, at, cap, "\nidle: ");
        at = put_dec(f->buf, at, cap, st.idle);
        at = put_str(f->buf, at, cap, "\ndistinct: ");
        at = put_dec(f->buf, at, cap, st.distinct);
        at = put_str(f->buf, at, cap, "\noverflow: ");
        at = put_dec(f->buf, at, cap, st.overflow);
        at = put_str(f->buf, at, cap, "\n");
        break;
    }
    case P_SYSCALLS: {
        at = put_str(f->buf, at, cap, "# num calls cycles\n");
        for (int i = 0; i < SYSCALL_COUNT; i++) {
            syscount_entry_t e;
            syscount_get(i, &e);
            if (e.calls == 0) {
                continue;
            }
            at = put_dec(f->buf, at, cap, (uint64_t)i);
            at = put_str(f->buf, at, cap, " ");
            at = put_dec(f->buf, at, cap, e.calls);
            at = put_str(f->buf, at, cap, " ");
            at = put_dec(f->buf, at, cap, e.cycles);
            at = put_str(f->buf, at, cap, "\n");
        }
        break;
    }
    default:
        break;
    }
    f->buf[at] = '\0';
    f->len = at;
}

static int proc_exists(const char *rel) {
    int pid = 0;
    return classify(rel, &pid) != P_NONE;
}

static int proc_is_dir(const char *rel) {
    int pid = 0;
    int k = classify(rel, &pid);
    return k == P_ROOT || k == P_PIDDIR;
}

static int proc_stat(const char *rel, leanfs_stat_t *out) {
    int pid = 0;
    int k = classify(rel, &pid);
    if (k == P_NONE) {
        return -1;
    }
    out->mtime = 0;
    out->is_dir = (k == P_ROOT || k == P_PIDDIR) ? 1 : 0;
    out->is_link = 0;
    out->inode = PROCFS_INO_BASE + ((uint32_t)pid << 8) + (uint32_t)k;
    if (out->is_dir) {
        out->size = 0;
        return 0;
    }
    static char probe_buf[PROC_BUF_LARGE];
    static proc_file_t probe;
    probe.buf = probe_buf;
    probe.cap = PROC_BUF_LARGE;
    generate(&probe, k, pid);
    out->size = probe.len;
    return 0;
}

static int proc_open(const char *rel, int create) {
    (void)create;
    int pid = 0;
    int k = classify(rel, &pid);
    if (k == P_NONE || k == P_ROOT || k == P_PIDDIR) {
        return -1;
    }
    for (int i = 0; i < PROC_MAX_OPEN; i++) {
        if (!proc_files[i].used) {
            uint32_t cap = buf_cap_for(k);
            char *buf = (char *)kmalloc(cap);
            if (!buf) {
                return -1;
            }
            proc_files[i].used = 1;
            proc_files[i].buf = buf;
            proc_files[i].cap = cap;
            generate(&proc_files[i], k, pid);
            return i;
        }
    }
    return -1;
}

static void proc_close(int handle) {
    if (handle < 0 || handle >= PROC_MAX_OPEN || !proc_files[handle].used) {
        return;
    }
    kfree(proc_files[handle].buf);
    proc_files[handle].buf = (char *)0;
    proc_files[handle].cap = 0;
    proc_files[handle].len = 0;
    proc_files[handle].used = 0;
}

static int64_t proc_read(int handle, void *buf, size_t len, uint32_t off) {
    if (handle < 0 || handle >= PROC_MAX_OPEN || !proc_files[handle].used) {
        return -1;
    }
    proc_file_t *f = &proc_files[handle];
    if (off >= f->len) {
        return 0;
    }
    uint32_t avail = f->len - off;
    uint32_t n = (len < avail) ? (uint32_t)len : avail;
    k_memcpy(buf, f->buf + off, n);
    return (int64_t)n;
}

static int64_t proc_write(int handle, const void *buf, size_t len, uint32_t off) {
    (void)handle;
    (void)buf;
    (void)len;
    (void)off;
    return -1;
}

static uint32_t proc_size(int handle) {
    if (handle < 0 || handle >= PROC_MAX_OPEN || !proc_files[handle].used) {
        return 0;
    }
    return proc_files[handle].len;
}

static int proc_handle_stat(int handle, leanfs_stat_t *out) {
    if (handle < 0 || handle >= PROC_MAX_OPEN || !proc_files[handle].used) {
        return -1;
    }
    out->size = proc_files[handle].len;
    out->mtime = 0;
    out->is_dir = 0;
    out->is_link = 0;
    out->inode = PROCFS_INO_BASE + 0x00800000u + (uint32_t)handle;
    return 0;
}

static const char *const PID_FILES[] = {"status", "cmdline", "exe"};
static const char *const ROOT_FILES[] = {"uptime", "meminfo", "mounts",
                                         "interrupts", "profile", "syscalls"};
#define ROOT_FILE_COUNT ((uint32_t)(sizeof(ROOT_FILES) / sizeof(ROOT_FILES[0])))

static int proc_readdir(const char *rel, uint32_t *cookie, leanfs_dir_entry_t *out) {
    int pid = 0;
    int k = classify(rel, &pid);
    uint32_t i = *cookie;

    if (k == P_PIDDIR) {
        if (i >= 3) {
            return 0;
        }
        out->inode = 0;
        out->is_dir = 0;
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
        out->is_dir = 0;
        out->is_link = 0;
        k_strlcpy(out->name, ROOT_FILES[i], sizeof(out->name));
        *cookie = i + 1;
        return 1;
    }
    uint32_t slot = i - ROOT_FILE_COUNT;
    int total = sched_task_count();
    while ((int)slot < total) {
        task_t *t = sched_task_by_slot((int)slot);
        if (t && t->state != TASK_TERMINATED) {
            out->inode = (uint32_t)t->id;
            out->is_dir = 1;
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

static const vfs_ops_t PROCFS_OPS = {
    .stat = proc_stat,
    .is_dir = proc_is_dir,
    .exists = proc_exists,
    .open = proc_open,
    .read = proc_read,
    .write = proc_write,
    .size = proc_size,
    .handle_stat = proc_handle_stat,
    .readdir = proc_readdir,
    .close = proc_close,
};

const vfs_ops_t *procfs_ops(void) {
    return &PROCFS_OPS;
}
