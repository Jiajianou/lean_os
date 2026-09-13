#include "vfs.h"

#include "drivers/klog.h"
#include "leanfs.h"
#include "lib/libk.h"
#include "paths.h"
#include "vfsops.h"
#include "lib/spinlock.h"

static spinlock_t fs_lock;

#define VFS_MAX_MOUNTS 3

typedef struct {
    const char *prefix;
    uint32_t prefix_len;
    const vfs_ops_t *ops;
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

int vfs_mount_info(int i, const char **prefix, const char **type) {
    if (i < 0 || i >= mount_count) {
        return 0;
    }
    const char *p = mounts[i].prefix;
    if (!p || !p[0]) {
        p = "/";
    }
    *prefix = p;
    *type = mounts[i].ops ? (p[1] == 'd' ? "devfs" : "procfs") : "leanfs";
    return 1;
}

static int vfs_resolve_mount(const char *path, const char **rel) {
    if (!path) {
        return 0;
    }
    for (int i = 0; i < mount_count; i++) {
        uint32_t n = mounts[i].prefix_len;
        if (n == 0) {
            continue;
        }
        if (k_memcmp(path, mounts[i].prefix, n) != 0) {
            continue;
        }
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
    return -1;
}

void vfs_init(void) {
    leanfs_init();
    devfs_init();
    procfs_init();
    vfs_mount(PATH_DEV, devfs_ops());
    vfs_mount(PATH_PROC, procfs_ops());
}

int64_t vfs_read(const char *path, void *buf, size_t maxlen) {
    const char *rel;
    int m = vfs_resolve_mount(path, &rel);
    if (m >= 0) {
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
        return -1;
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
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_rename(old_path, new_path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

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
        *cookie = VFS_SYNTH_COOKIE;
    }

    uint32_t i = *cookie - VFS_SYNTH_COOKIE;
    if ((int)i >= mount_count) {
        return 0;
    }
    out->inode = 0;
    out->is_dir = 1;
    out->is_link = 0;
    k_strlcpy(out->name, mounts[i].prefix + 1, sizeof(out->name));
    *cookie = VFS_SYNTH_COOKIE + i + 1;
    return 1;
}

int vfs_dir_open(const char *path) {
    const char *rel;
    if (vfs_resolve_mount(path, &rel) >= 0) {
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
    uint64_t f = spin_lock_irqsave(&fs_lock);
    leanfs_sync();
    spin_unlock_irqrestore(&fs_lock, f);
    klog_puts("[vfs] sync: leanfs is write-through; superblock marked cleanly unmounted.\n");
}

int vfs_rename_replace(const char *old_path, const char *new_path) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_rename_replace(old_path, new_path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_check(void) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    uint32_t problems = leanfs_check();
    spin_unlock_irqrestore(&fs_lock, f);
    return (int)problems;
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

int vfs_link(const char *old_path, const char *new_path) {
    const char *rel;
    if (vfs_resolve_mount(old_path, &rel) >= 0 || vfs_resolve_mount(new_path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_link(old_path, new_path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_utime(const char *path, uint32_t mtime) {
    const char *rel;
    if (vfs_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_utime(path, mtime);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_statvfs(const char *path, vfs_statvfs_t *out) {
    const char *rel;
    if (!out || vfs_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int ok = leanfs_exists(path);
    out->block_size = LEANFS_BLOCK_SIZE;
    out->total_blocks = leanfs_total_blocks();
    out->free_blocks = leanfs_free_blocks();
    out->total_inodes = leanfs_total_inodes();
    out->free_inodes = leanfs_free_inodes();
    out->name_max = LEANFS_MAX_NAME;
    spin_unlock_irqrestore(&fs_lock, f);
    return ok ? 0 : -1;
}

uint32_t vfs_nlink(const char *path) {
    const char *rel;
    if (vfs_resolve_mount(path, &rel) >= 0) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    uint32_t r = leanfs_nlink(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int64_t vfs_readlink(const char *path, char *buf, size_t maxlen) {
    const char *rel;
    if (vfs_resolve_mount(path, &rel) >= 0) {
        return -1;
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
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_handle_truncate_to(handle, len);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_handle_truncate(int handle) {
    if (VFS_HANDLE_MOUNT(handle) > 0) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_handle_truncate(handle);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_handle_readable(int handle) {
    uint32_t m = VFS_HANDLE_MOUNT(handle);
    if (m > 0 && m <= (uint32_t)mount_count && mounts[m - 1].ops->readable) {
        return mounts[m - 1].ops->readable(VFS_HANDLE_LOCAL(handle));
    }
    return 1;
}

struct tty *vfs_handle_tty(int handle, int *pty_number) {
    uint32_t m = VFS_HANDLE_MOUNT(handle);
    if (m > 0 && m <= (uint32_t)mount_count && mounts[m - 1].ops->tty_of) {
        return mounts[m - 1].ops->tty_of(VFS_HANDLE_LOCAL(handle), pty_number);
    }
    return (struct tty *)0;
}

void vfs_handle_close(int handle) {
    uint32_t m = VFS_HANDLE_MOUNT(handle);
    if (m > 0 && m <= (uint32_t)mount_count && mounts[m - 1].ops->close) {
        mounts[m - 1].ops->close(VFS_HANDLE_LOCAL(handle));
    }
}
