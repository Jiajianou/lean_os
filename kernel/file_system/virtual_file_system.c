#include "virtual_file_system.h"

#include "drivers/kernel_log.h"
#include "leanfs.h"
#include "library/kernel_library.h"
#include "paths.h"
#include "virtual_file_system_operations.h"
#include "library/spinlock.h"

static spinlock_t fs_lock;

#define VIRTUAL_FILE_SYSTEM_MAX_MOUNTS 3

typedef struct {
    const char *prefix;
    uint32_t prefix_length;
    const virtual_file_system_ops_t *ops;
} virtual_file_system_mount_t;

static virtual_file_system_mount_t mounts[VIRTUAL_FILE_SYSTEM_MAX_MOUNTS];
static int mount_count;

static void virtual_file_system_mount(const char *prefix, const virtual_file_system_ops_t *ops) {
    if (mount_count >= VIRTUAL_FILE_SYSTEM_MAX_MOUNTS) {
        return;
    }
    mounts[mount_count].prefix = prefix;
    mounts[mount_count].prefix_length = (uint32_t)k_strlen(prefix);
    mounts[mount_count].ops = ops;
    mount_count++;
}

int virtual_file_system_mount_info(int i, const char **prefix, const char **type) {
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

static int virtual_file_system_resolve_mount(const char *path, const char **rel) {
    if (!path) {
        return 0;
    }
    for (int i = 0; i < mount_count; i++) {
        uint32_t n = mounts[i].prefix_length;
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

void virtual_file_system_init(void) {
    leanfs_init();
    devfs_init();
    procfs_init();
    virtual_file_system_mount(PATH_DEV, devfs_ops());
    virtual_file_system_mount(PATH_PROCESS, procfs_ops());
}

int64_t virtual_file_system_read(const char *path, void *buffer, size_t maxlen) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
    if (m >= 0) {
        int h = mounts[m].ops->open(rel, 0);
        if (h < 0) {
            return -1;
        }
        int64_t n = mounts[m].ops->read(h, buffer, maxlen, 0);
        return n;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int64_t r = leanfs_read(path, buffer, maxlen);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_write(const char *path, const void *buffer, size_t length) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_write(path, buffer, length);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_exists(const char *path) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
    if (m >= 0) {
        return mounts[m].ops->exists(rel);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_exists(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_is_directory(const char *path) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
    if (m >= 0) {
        return mounts[m].ops->is_directory(rel);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_is_directory(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_mkdir(const char *path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_mkdir(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

uint32_t virtual_file_system_free_blocks(void) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    uint32_t r = leanfs_free_blocks();
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_unlink(const char *path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_unlink(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_rename(const char *old_path, const char *new_path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(old_path, &rel) >= 0 ||
        virtual_file_system_resolve_mount(new_path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_rename(old_path, new_path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

#define VIRTUAL_FILE_SYSTEM_SYNTH_COOKIE 0x40000000u

static int root_is(const char *path) {
    return path && path[0] == '/' && path[1] == '\0';
}

int virtual_file_system_readdir(const char *path, uint32_t *cookie, leanfs_directory_entry_t *out) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
    if (m >= 0) {
        return mounts[m].ops->readdir(rel, cookie, out);
    }

    if (!root_is(path) || *cookie < VIRTUAL_FILE_SYSTEM_SYNTH_COOKIE) {
        uint64_t f = spin_lock_irqsave(&fs_lock);
        int r = leanfs_readdir(path, cookie, out);
        spin_unlock_irqrestore(&fs_lock, f);
        if (r != 0 || !root_is(path)) {
            return r;
        }
        *cookie = VIRTUAL_FILE_SYSTEM_SYNTH_COOKIE;
    }

    uint32_t i = *cookie - VIRTUAL_FILE_SYSTEM_SYNTH_COOKIE;
    if ((int)i >= mount_count) {
        return 0;
    }
    out->inode = 0;
    out->is_directory = 1;
    out->is_link = 0;
    k_strlcpy(out->name, mounts[i].prefix + 1, sizeof(out->name));
    *cookie = VIRTUAL_FILE_SYSTEM_SYNTH_COOKIE + i + 1;
    return 1;
}

int virtual_file_system_directory_open(const char *path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_directory_open(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_readdir_at(int handle, uint32_t *cookie, leanfs_directory_entry_t *out) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_readdir_at(handle, cookie, out);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

size_t virtual_file_system_list(const char *path, char *buffer, size_t maxlen) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
    if (m >= 0) {
        size_t written = 0;
        uint32_t cookie = 0;
        leanfs_directory_entry_t e;
        while (mounts[m].ops->readdir(rel, &cookie, &e) == 1) {
            size_t nlen = k_strlen(e.name);
            size_t need = nlen + (e.is_directory ? 1u : 0u) + 1u;
            if (written + need > maxlen) {
                break;
            }
            k_memcpy(buffer + written, e.name, nlen);
            written += nlen;
            if (e.is_directory) {
                buffer[written++] = '/';
            }
            buffer[written++] = '\n';
        }
        return written;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    size_t r = leanfs_list(path, buffer, maxlen);
    spin_unlock_irqrestore(&fs_lock, f);
    if (root_is(path)) {
        for (int i = 0; i < mount_count; i++) {
            const char *name = mounts[i].prefix + 1;
            size_t nlen = k_strlen(name);
            if (r + nlen + 2 > maxlen) {
                break;
            }
            k_memcpy(buffer + r, name, nlen);
            r += nlen;
            buffer[r++] = '/';
            buffer[r++] = '\n';
        }
    }
    return r;
}

void virtual_file_system_sync(void) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    leanfs_sync();
    spin_unlock_irqrestore(&fs_lock, f);
    kernel_log_puts("[vfs] sync: leanfs is write-through; superblock marked cleanly unmounted.\n");
}

int virtual_file_system_rename_replace(const char *old_path, const char *new_path) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_rename_replace(old_path, new_path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_check(void) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    uint32_t problems = leanfs_check();
    spin_unlock_irqrestore(&fs_lock, f);
    return (int)problems;
}

int virtual_file_system_rmdir(const char *path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_rmdir(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_stat(const char *path, leanfs_stat_t *out) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
    if (m >= 0) {
        return mounts[m].ops->stat(rel, out);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_stat(path, out);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_symlink(const char *path, const char *target) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_symlink(path, target);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_link(const char *old_path, const char *new_path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(old_path, &rel) >= 0 || virtual_file_system_resolve_mount(new_path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_link(old_path, new_path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_utime(const char *path, uint32_t mtime) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_utime(path, mtime);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_statvfs(const char *path, virtual_file_system_statvfs_t *out) {
    const char *rel;
    if (!out || virtual_file_system_resolve_mount(path, &rel) >= 0) {
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

uint32_t virtual_file_system_nlink(const char *path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    uint32_t r = leanfs_nlink(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int64_t virtual_file_system_readlink(const char *path, char *buffer, size_t maxlen) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
    if (m >= 0) {
        if (!mounts[m].ops->readlink) {
            return -1;
        }
        return mounts[m].ops->readlink(rel, buffer, maxlen);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int64_t r = leanfs_readlink(path, buffer, maxlen);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_lstat(const char *path, leanfs_stat_t *out) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
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

int virtual_file_system_open(const char *path, int create) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
    if (m >= 0) {
        int local = mounts[m].ops->open(rel, create);
        return local < 0 ? -1 : VIRTUAL_FILE_SYSTEM_HANDLE_MAKE(m + 1, local);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_open(path, create);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int64_t virtual_file_system_handle_read(int handle, void *buffer, size_t length, uint32_t off) {
    uint32_t m = VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle);
    if (m > 0) {
        return mounts[m - 1].ops->read(VIRTUAL_FILE_SYSTEM_HANDLE_LOCAL(handle), buffer, length, off);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int64_t r = leanfs_handle_read(handle, buffer, length, off);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int64_t virtual_file_system_handle_write(int handle, const void *buffer, size_t length, uint32_t off) {
    uint32_t m = VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle);
    if (m > 0) {
        return mounts[m - 1].ops->write(VIRTUAL_FILE_SYSTEM_HANDLE_LOCAL(handle), buffer, length, off);
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int64_t r = leanfs_handle_write(handle, buffer, length, off);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

uint32_t virtual_file_system_handle_size(int handle) {
    uint32_t m = VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle);
    if (m > 0) {
        return mounts[m - 1].ops->size(VIRTUAL_FILE_SYSTEM_HANDLE_LOCAL(handle));
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    uint32_t r = leanfs_handle_size(handle);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_handle_stat(int handle, leanfs_stat_t *out) {
    uint32_t m = VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle);
    if (m > 0) {
        return mounts[m - 1].ops->handle_stat(VIRTUAL_FILE_SYSTEM_HANDLE_LOCAL(handle), out);
    }
    return leanfs_handle_stat(handle, out);
}

int virtual_file_system_handle_truncate_to(int handle, uint32_t length) {
    if (VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle) > 0) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_handle_truncate_to(handle, length);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_handle_truncate(int handle) {
    if (VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle) > 0) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_handle_truncate(handle);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int virtual_file_system_handle_readable(int handle) {
    uint32_t m = VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle);
    if (m > 0 && m <= (uint32_t)mount_count && mounts[m - 1].ops->readable) {
        return mounts[m - 1].ops->readable(VIRTUAL_FILE_SYSTEM_HANDLE_LOCAL(handle));
    }
    return 1;
}

struct tty *virtual_file_system_handle_tty(int handle, int *pty_number) {
    uint32_t m = VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle);
    if (m > 0 && m <= (uint32_t)mount_count && mounts[m - 1].ops->tty_of) {
        return mounts[m - 1].ops->tty_of(VIRTUAL_FILE_SYSTEM_HANDLE_LOCAL(handle), pty_number);
    }
    return (struct tty *)0;
}

/* An open file on this machine's own filesystem holds its inode, so that an
   unlink while it is open leaves an orphan rather than a hole (leanfs.c).
   Other mounts have their own open and close. */
void virtual_file_system_handle_hold(int handle) {
    if (handle < 0 || VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle) != 0) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    leanfs_handle_hold(handle);
    spin_unlock_irqrestore(&fs_lock, f);
}

void virtual_file_system_handle_release(int handle) {
    if (handle < 0 || VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle) != 0) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&fs_lock);
    leanfs_handle_release(handle);
    spin_unlock_irqrestore(&fs_lock, f);
}

void virtual_file_system_handle_close(int handle) {
    uint32_t m = VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle);
    if (m > 0 && m <= (uint32_t)mount_count && mounts[m - 1].ops->close) {
        mounts[m - 1].ops->close(VIRTUAL_FILE_SYSTEM_HANDLE_LOCAL(handle));
    }
}
