#include "vfs.h"

#include "drivers/klog.h"
#include "leanfs.h"
#include "lib/libk.h"
#include "paths.h" /* system_api/include/paths.h - PATH_DEV, PATH_PROC */
#include "vfsops.h"
#include "lib/spinlock.h"

/* ---- M67: fs_lock ----------------------------------------------------
 *
 * What it protects: every byte of mutable state in kernel/fs/leanfs.c -
 * the in-memory superblock, the inode table, the block bitmap, the
 * dirent scratch buffer and the per-sector dirty flags - plus the ATA
 * PIO driver underneath it, which is a sequence of port writes that
 * means nothing if a second caller interleaves its own.
 *
 * Against whom: for sixty-six milestones, nobody. `int 0x80` was an
 * *interrupt* gate, so IF was clear from the syscall instruction to the
 * iretq, and that was this filesystem's entire mutual exclusion - never
 * written down, because nothing had to write it down. M67 makes vector
 * 0x80 a trap gate, so a timer tick can now land between any two
 * instructions of a file write and hand the CPU to a task that calls
 * vfs_write itself. On a second core it never even needed the tick.
 *
 * One coarse lock, not a lock per inode. leanfs has exactly one writer's
 * worth of structure - a single shared bitmap and a single inode table
 * that any write may touch - so per-inode locking would buy nothing but
 * a lock-ordering problem. The cost is that two concurrent file
 * operations serialise, which is what they would do at the disk anyway:
 * there is one ATA channel and PIO is synchronous.
 *
 * Taken with interrupts off (spin_lock_irqsave), for the reason M56
 * wrote down for vmm_lock and which applies with more force here: a
 * fatal signal is delivered from the timer tick, so a task preempted
 * while holding this lock could be torn down by task_exit_with_code and
 * never release it - a permanent deadlock rather than a slow spin. Off
 * for the duration of one operation is the honest cost of that
 * guarantee; M68 converts this to a sleeping mutex once there is a
 * blocked state to sleep in, which is the real fix for the latency.
 *
 * vfs_init is deliberately NOT locked: it runs once, from kernel_main,
 * before any other task exists, and it formats and seeds the disk - tens
 * of thousands of PIO transfers. Holding IF off across that would stop
 * the timer for long enough to matter. There is nothing to race with.
 */
static spinlock_t fs_lock;

/* ---- M87: the mount table --------------------------------------------
 *
 * vfs.h has called itself "a thin, honest pass-through rather than a
 * driver table or vtable dispatch mechanism that would be pure
 * speculative generality for a single-filesystem kernel" since M53, and
 * that was true right up until `/dev/null` needed to exist. It is not a
 * single-filesystem kernel now: three filesystems, two of which have no
 * disk behind them at all.
 *
 * Longest-prefix match over a short array, searched on every path-taking
 * call. Not a tree, not a hash - three entries, and a linear scan of
 * three is faster than anything cleverer plus easier to be sure of. The
 * root is last and matches everything, which is what makes it the
 * fallback without needing a special case.
 *
 * A mount's prefix is matched only at a component boundary, so `/devices`
 * belongs to the root filesystem and not to `/dev`. Getting that wrong
 * would silently shadow every path that happens to start with the same
 * letters.
 */
#define VFS_MAX_MOUNTS 3

typedef struct {
    const char *prefix;   /* "" for the root, which matches everything */
    uint32_t prefix_len;
    const vfs_ops_t *ops; /* NULL for the root - leanfs is called directly */
} vfs_mount_t;

static vfs_mount_t mounts[VFS_MAX_MOUNTS];
static int mount_count;

static void vfs_mount(const char *prefix, const vfs_ops_t *ops) {
    if (mount_count >= VFS_MAX_MOUNTS) {
        return;
    }
    mounts[mount_count].prefix = prefix;
    mounts[mount_count].prefix_len = (uint32_t)k_strlen(prefix);
    mounts[mount_count].ops = ops;
    mount_count++;
}

/* Which mount owns `path`, and what the path looks like from inside it.
 *
 * `*rel` points into `path` for a mounted filesystem - so "/dev/null"
 * gives mount "dev" and rel "/null" - or at the whole path for the root.
 * The mount point itself ("/dev") gives rel "/", which is how a
 * filesystem is asked about its own root directory. */
static int vfs_resolve_mount(const char *path, const char **rel) {
    if (!path) {
        return 0;
    }
    for (int i = 0; i < mount_count; i++) {
        uint32_t n = mounts[i].prefix_len;
        if (n == 0) {
            continue; /* the root, handled by the fallthrough below */
        }
        if (k_memcmp(path, mounts[i].prefix, n) != 0) {
            continue;
        }
        /* Only at a component boundary: "/dev" and "/dev/..." belong to
         * the mount, "/devices" does not. */
        if (path[n] == '\0') {
            *rel = "/";
            return i;
        }
        if (path[n] == '/') {
            *rel = path + n;
            return i;
        }
    }
    *rel = path;
    return -1; /* the root filesystem */
}


void vfs_init(void) {
    leanfs_init();
    devfs_init();
    procfs_init();
    /* Order matters only in that the root must be reachable; the scan in
     * vfs_resolve_mount falls through to it rather than matching it, so
     * these two are simply the two that exist. */
    vfs_mount(PATH_DEV, devfs_ops());
    vfs_mount(PATH_PROC, procfs_ops());
}

int64_t vfs_read(const char *path, void *buf, size_t maxlen) {
    const char *rel;
    int m = vfs_resolve_mount(path, &rel);
    if (m >= 0) {
        /* Whole-file read over open and read, because that is what a
         * synthetic file has: no inode to read directly from. */
        int h = mounts[m].ops->open(rel, 0);
        if (h < 0) {
            return -1;
        }
        int64_t n = mounts[m].ops->read(h, buf, maxlen, 0);
        return n;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int64_t r = leanfs_read(path, buf, maxlen);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_write(const char *path, const void *buf, size_t len) {
    const char *rel;
    if (vfs_resolve_mount(path, &rel) >= 0) {
        return -1; /* see the note at vfs_mkdir */
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_write(path, buf, len);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_exists(const char *path) {
    const char *rel;
    int m = vfs_resolve_mount(path, &rel);
    if (m >= 0) {
        return mounts[m].ops->exists(rel);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_exists(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_is_dir(const char *path) {
    const char *rel;
    int m = vfs_resolve_mount(path, &rel);
    if (m >= 0) {
        return mounts[m].ops->is_dir(rel);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_is_dir(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

/* M87: a mounted synthetic filesystem owns this path, and none of them
 * can be written to, created in, removed from or renamed. Refused here
 * rather than by asking the filesystem, because "you cannot make a file
 * in /proc" is a property of /proc rather than an operation it declines -
 * and a vfs_ops table with four entries that all return -1 would be four
 * ways to get the same answer wrong. */
int vfs_mkdir(const char *path) {
    const char *rel;
    if (vfs_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_mkdir(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

uint32_t vfs_free_blocks(void) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    uint32_t r = leanfs_free_blocks();
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_unlink(const char *path) {
    const char *rel;
    if (vfs_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_unlink(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_rename(const char *old_path, const char *new_path) {
    const char *rel;
    if (vfs_resolve_mount(old_path, &rel) >= 0 ||
        vfs_resolve_mount(new_path, &rel) >= 0) {
        return -1; /* neither end of a rename may be synthetic */
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_rename(old_path, new_path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

/* M87: a mount point has to appear in its parent's listing, or `ls /`
 * says this machine has no /dev.
 *
 * Every mount here is a direct child of the root, so the parent is
 * always "/" and the extra entries are always all of them. That is worth
 * stating because it is the assumption that would break first: a mount
 * at /usr/share would need this to ask which mounts live under the
 * directory being listed, and the loop below would have to filter. Three
 * mounts, all at the top, is what makes the simple version correct.
 *
 * The cookies are numbered from VFS_SYNTH_COOKIE upward - far past any
 * byte offset a real directory could produce, since leanfs's are offsets
 * into a file capped at 8 MiB. So "have I finished the real entries" is
 * a comparison rather than a flag the caller would have to carry. */
#define VFS_SYNTH_COOKIE 0x40000000u

static int root_is(const char *path) {
    return path && path[0] == '/' && path[1] == '\0';
}

int vfs_readdir(const char *path, uint32_t *cookie, leanfs_dir_entry_t *out) {
    const char *rel;
    int m = vfs_resolve_mount(path, &rel);
    if (m >= 0) {
        return mounts[m].ops->readdir(rel, cookie, out);
    }

    if (!root_is(path) || *cookie < VFS_SYNTH_COOKIE) {
        uint64_t f = spin_lock_irqsave(&fs_lock);
        int r = leanfs_readdir(path, cookie, out);
        spin_unlock_irqrestore(&fs_lock, f);
        if (r != 0 || !root_is(path)) {
            return r;
        }
        /* The real entries are done and this is the root - carry on into
         * the mount points rather than reporting the end. */
        *cookie = VFS_SYNTH_COOKIE;
    }

    uint32_t i = *cookie - VFS_SYNTH_COOKIE;
    if ((int)i >= mount_count) {
        return 0;
    }
    out->inode = 0; /* a mount point is not an inode of the filesystem it sits in */
    out->is_dir = 1;
    k_strlcpy(out->name, mounts[i].prefix + 1, sizeof(out->name)); /* past the leading '/' */
    *cookie = VFS_SYNTH_COOKIE + i + 1;
    return 1;
}

int vfs_dir_open(const char *path) {
    const char *rel;
    if (vfs_resolve_mount(path, &rel) >= 0) {
        /* M87: no handle form for a synthetic directory, and that is a
         * real answer rather than a gap. A leanfs handle is an inode
         * index, which is what makes "open once, walk many times" cheap;
         * /dev and /proc have no inodes and their whole contents are
         * generated from the path each time, so a handle would be a
         * second name for the path with nothing gained. The caller falls
         * back to the path form - see sys_getdents. */
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_dir_open(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_readdir_at(int handle, uint32_t *cookie, leanfs_dir_entry_t *out) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_readdir_at(handle, cookie, out);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

size_t vfs_list(const char *path, char *buf, size_t maxlen) {
    const char *rel;
    int m = vfs_resolve_mount(path, &rel);
    if (m >= 0) {
        /* The same newline-separated shape leanfs_list produces, built
         * from the same walk SYS_getdents uses - one directory walk per
         * filesystem, not two. */
        size_t written = 0;
        uint32_t cookie = 0;
        leanfs_dir_entry_t e;
        while (mounts[m].ops->readdir(rel, &cookie, &e) == 1) {
            size_t nlen = k_strlen(e.name);
            size_t need = nlen + (e.is_dir ? 1u : 0u) + 1u;
            if (written + need > maxlen) {
                break;
            }
            k_memcpy(buf + written, e.name, nlen);
            written += nlen;
            if (e.is_dir) {
                buf[written++] = '/';
            }
            buf[written++] = '\n';
        }
        return written;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    size_t r = leanfs_list(path, buf, maxlen);
    spin_unlock_irqrestore(&fs_lock, f);
    if (root_is(path)) {
        /* The mount points, appended - see vfs_readdir for why they have
         * to be here and why "all of them" is the right set. */
        for (int i = 0; i < mount_count; i++) {
            const char *name = mounts[i].prefix + 1;
            size_t nlen = k_strlen(name);
            if (r + nlen + 2 > maxlen) {
                break;
            }
            k_memcpy(buf + r, name, nlen);
            r += nlen;
            buf[r++] = '/';
            buf[r++] = '\n';
        }
    }
    return r;
}

void vfs_sync(void) {
    /* Nothing to write back: leanfs_write_file's own ata_write_sectors
     * calls are synchronous, so a file is on the platter by the time
     * vfs_write returns to its caller.
     *
     * M71: what this DOES do now is mark the superblock cleanly
     * unmounted. That is not a flush and the distinction matters - it is
     * a record that the machine reached this point on purpose, which is
     * the only way a filesystem with no journal can tell an orderly
     * shutdown from a power cut on the next mount. A disk that never gets
     * here is checked before it is trusted. */
    uint64_t f = spin_lock_irqsave(&fs_lock);
    leanfs_sync();
    spin_unlock_irqrestore(&fs_lock, f);
    klog_puts("[vfs] sync: leanfs is write-through; superblock marked cleanly unmounted.\n");
}

/* M59 */
/* M71 */
int vfs_rename_replace(const char *old_path, const char *new_path) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_rename_replace(old_path, new_path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_check(void) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    leanfs_check();
    spin_unlock_irqrestore(&fs_lock, f);
    return 0;
}

int vfs_rmdir(const char *path) {
    const char *rel;
    if (vfs_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_rmdir(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_stat(const char *path, leanfs_stat_t *out) {
    const char *rel;
    int m = vfs_resolve_mount(path, &rel);
    if (m >= 0) {
        return mounts[m].ops->stat(rel, out);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_stat(path, out);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

/* M87: symbolic links. Refused on a synthetic filesystem for the same
 * reason every other change to one is - see the note at vfs_mkdir. */
int vfs_symlink(const char *path, const char *target) {
    const char *rel;
    if (vfs_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_symlink(path, target);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int64_t vfs_readlink(const char *path, char *buf, size_t maxlen) {
    const char *rel;
    if (vfs_resolve_mount(path, &rel) >= 0) {
        return -1; /* nothing synthetic is a link */
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int64_t r = leanfs_readlink(path, buf, maxlen);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_lstat(const char *path, leanfs_stat_t *out) {
    const char *rel;
    int m = vfs_resolve_mount(path, &rel);
    if (m >= 0) {
        /* A synthetic filesystem has no links, so lstat and stat are the
         * same question there - answered by the same function rather than
         * by a second one that would have to stay in step with it. */
        int r = mounts[m].ops->stat(rel, out);
        if (r == 0) {
            out->is_link = 0;
        }
        return r;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_lstat(path, out);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_open(const char *path, int create) {
    const char *rel;
    int m = vfs_resolve_mount(path, &rel);
    if (m >= 0) {
        int local = mounts[m].ops->open(rel, create);
        /* M87: tagged with the mount so a later read knows which
         * filesystem to ask. leanfs is mount index 0 below, which is what
         * keeps its handles numerically what they have always been. */
        return local < 0 ? -1 : VFS_HANDLE_MAKE(m + 1, local);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_open(path, create);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int64_t vfs_handle_read(int handle, void *buf, size_t len, uint32_t off) {
    uint32_t m = VFS_HANDLE_MOUNT(handle);
    if (m > 0) {
        return mounts[m - 1].ops->read(VFS_HANDLE_LOCAL(handle), buf, len, off);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int64_t r = leanfs_handle_read(handle, buf, len, off);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int64_t vfs_handle_write(int handle, const void *buf, size_t len, uint32_t off) {
    uint32_t m = VFS_HANDLE_MOUNT(handle);
    if (m > 0) {
        return mounts[m - 1].ops->write(VFS_HANDLE_LOCAL(handle), buf, len, off);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int64_t r = leanfs_handle_write(handle, buf, len, off);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

uint32_t vfs_handle_size(int handle) {
    uint32_t m = VFS_HANDLE_MOUNT(handle);
    if (m > 0) {
        return mounts[m - 1].ops->size(VFS_HANDLE_LOCAL(handle));
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    uint32_t r = leanfs_handle_size(handle);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_handle_stat(int handle, leanfs_stat_t *out) {
    uint32_t m = VFS_HANDLE_MOUNT(handle);
    if (m > 0) {
        return mounts[m - 1].ops->handle_stat(VFS_HANDLE_LOCAL(handle), out);
    }
    return leanfs_handle_stat(handle, out);
}

int vfs_handle_truncate_to(int handle, uint32_t len) {
    if (VFS_HANDLE_MOUNT(handle) > 0) {
        return -1; /* nothing synthetic has a length to change */
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_handle_truncate_to(handle, len);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_handle_truncate(int handle) {
    if (VFS_HANDLE_MOUNT(handle) > 0) {
        return -1; /* nothing synthetic has a length to drop */
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_handle_truncate(handle);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}
