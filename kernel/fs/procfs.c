/* kernel/fs/procfs.c - M87
 *
 * `/proc`, for programs that have never heard of this operating system.
 *
 * SYS_taskinfo (M45) already answers "what is running" better than this
 * does - one snapshot, taken under the scheduler's own lock, with a
 * typed record per task. It stays, and `task_manager` keeps using it,
 * because a lean_os program should use the lean_os interface. This is
 * for the other kind of caller: the one that was written against Linux
 * and reads `/proc/self/exe` to find out where it lives.
 *
 * The contents are generated when a file is opened, into a per-handle
 * buffer, and read out of that buffer afterwards. Not generated per
 * read, and the difference matters: a program that reads `/proc/uptime`
 * in two calls must not see two different uptimes and a byte offset
 * that means nothing between them. A snapshot at open is what makes the
 * file behave like a file.
 */
#include "vfsops.h"

#include "drivers/pit.h"
#include "lib/libk.h"
#include "mm/pmm.h"
#include "sched/sched.h"

/* How many /proc files may be open at once, and how big one may be.
 *
 * Small on purpose. Every file here is a few lines of text about one
 * process or about the machine; the largest is a status block. A cap
 * that is visibly enough for what the directory contains is better than
 * one sized for a filesystem this is not. */
#define PROC_MAX_OPEN 16
#define PROC_BUF_MAX  512

typedef struct {
    int used;
    uint32_t len;
    char buf[PROC_BUF_MAX];
} proc_file_t;

static proc_file_t proc_files[PROC_MAX_OPEN];

void procfs_init(void) {
    k_memset(proc_files, 0, sizeof(proc_files));
}

/* ---- a tiny formatter, because there is no snprintf in the kernel ---- */

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

/* ---- what each path is ------------------------------------------------
 *
 * Parsed rather than tabulated, because half of these paths contain a
 * process id and a table cannot hold those. */
enum {
    P_NONE = 0,
    P_ROOT,       /* /proc */
    P_UPTIME,     /* /proc/uptime */
    P_MEMINFO,    /* /proc/meminfo */
    P_PIDDIR,     /* /proc/N or /proc/self */
    P_STATUS,     /* /proc/N/status */
    P_CMDLINE,    /* /proc/N/cmdline */
    P_EXE,        /* /proc/N/exe */
};

/* Splits "/self/status" into kind=P_STATUS, pid=<caller>. Returns P_NONE
 * for anything this filesystem does not have. */
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

    /* A pid, or "self". */
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
        return P_NONE; /* a pid that is not running is not a directory */
    }
    *out_pid = pid;

    if (*rest == '\0') {
        return P_PIDDIR;
    }
    rest++; /* past the '/' */
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

/* Fills `f` with the contents of the file `kind` names. */
static void generate(proc_file_t *f, int kind, int pid) {
    uint32_t at = 0;
    switch (kind) {
    case P_UPTIME:
        at = put_dec(f->buf, at, PROC_BUF_MAX, pit_get_ticks() * (1000 / PIT_HZ) / 1000);
        at = put_str(f->buf, at, PROC_BUF_MAX, ".");
        at = put_dec(f->buf, at, PROC_BUF_MAX,
                     (pit_get_ticks() * (1000 / PIT_HZ) / 100) % 10);
        at = put_str(f->buf, at, PROC_BUF_MAX, "\n");
        break;
    case P_MEMINFO: {
        /* In kibibytes, and named the way every /proc/meminfo names them,
         * because a program that reads this is matching on the label. */
        uint64_t free_kb = pmm_free_frame_count() * 4;
        at = put_str(f->buf, at, PROC_BUF_MAX, "MemFree:        ");
        at = put_dec(f->buf, at, PROC_BUF_MAX, free_kb);
        at = put_str(f->buf, at, PROC_BUF_MAX, " kB\n");
        break;
    }
    case P_STATUS: {
        task_t *t = sched_task_by_id(pid);
        if (!t) {
            break;
        }
        at = put_str(f->buf, at, PROC_BUF_MAX, "Name:\t");
        at = put_str(f->buf, at, PROC_BUF_MAX, t->name[0] ? t->name : "(unnamed)");
        at = put_str(f->buf, at, PROC_BUF_MAX, "\nState:\t");
        at = put_str(f->buf, at, PROC_BUF_MAX, state_name(t->state));
        at = put_str(f->buf, at, PROC_BUF_MAX, "\nPid:\t");
        at = put_dec(f->buf, at, PROC_BUF_MAX, (uint64_t)t->id);
        at = put_str(f->buf, at, PROC_BUF_MAX, "\nPPid:\t");
        at = put_dec(f->buf, at, PROC_BUF_MAX, (uint64_t)(t->parent_id < 0 ? 0 : t->parent_id));
        at = put_str(f->buf, at, PROC_BUF_MAX, "\nPGid:\t");
        at = put_dec(f->buf, at, PROC_BUF_MAX, (uint64_t)t->pgid);
        at = put_str(f->buf, at, PROC_BUF_MAX, "\nSid:\t");
        at = put_dec(f->buf, at, PROC_BUF_MAX, (uint64_t)t->sid);
        at = put_str(f->buf, at, PROC_BUF_MAX, "\n");
        break;
    }
    case P_CMDLINE: {
        task_t *t = sched_task_by_id(pid);
        if (t) {
            /* The name, not the full vector. The argument region belongs
             * to the process's own address space and this kernel does not
             * keep a copy - so reporting the name is the whole of what is
             * actually known here, and inventing a plausible command line
             * out of it would be exactly the fiction M65 declined. */
            at = put_str(f->buf, at, PROC_BUF_MAX, t->name);
        }
        break;
    }
    case P_EXE: {
        task_t *t = sched_task_by_id(pid);
        if (t) {
            /* Every program on this machine lives in /bin, and the task's
             * name is its filename - so this is a real answer rather than
             * a guess, and it stops being one the day something runs from
             * somewhere else. Said here so that day is visible. */
            at = put_str(f->buf, at, PROC_BUF_MAX, "/bin/");
            at = put_str(f->buf, at, PROC_BUF_MAX, t->name);
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
    if (out->is_dir) {
        out->size = 0;
        return 0;
    }
    /* Generated to find out how long it is, which is the only way to
     * know - and thrown away, because the value a later read returns has
     * to be the one taken when that read's handle was opened. A stat and
     * a read are two different questions about a file that changes. */
    static proc_file_t probe;
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
            proc_files[i].used = 1;
            generate(&proc_files[i], k, pid);
            return i;
        }
    }
    return -1; /* the table is full - see PROC_MAX_OPEN */
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
    /* Nothing here is writable, and refusing is the honest answer rather
     * than accepting and discarding: a program that writes to
     * /proc/self/status has misunderstood something, and telling it so is
     * more useful than pretending. */
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
    return 0;
}

/* The root lists the machine-wide files and then one directory per live
 * task; a pid directory lists its three files. The cookie is an index
 * into whichever of those two lists applies. */
static const char *const PID_FILES[] = {"status", "cmdline", "exe"};
static const char *const ROOT_FILES[] = {"uptime", "meminfo"};

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
        k_strlcpy(out->name, PID_FILES[i], sizeof(out->name));
        *cookie = i + 1;
        return 1;
    }
    if (k != P_ROOT) {
        return -1;
    }
    if (i < 2) {
        out->inode = 0;
        out->is_dir = 0;
        k_strlcpy(out->name, ROOT_FILES[i], sizeof(out->name));
        *cookie = i + 1;
        return 1;
    }
    /* Then the live tasks, by slot, skipping the dead - the same walk
     * SYS_taskinfo does and for the same reason: a pid is not an index. */
    uint32_t slot = i - 2;
    int total = sched_task_count();
    while ((int)slot < total) {
        task_t *t = sched_task_by_slot((int)slot);
        if (t && t->state != TASK_TERMINATED) {
            out->inode = (uint32_t)t->id;
            out->is_dir = 1;
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
};

const vfs_ops_t *procfs_ops(void) {
    return &PROCFS_OPS;
}
