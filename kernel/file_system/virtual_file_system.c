#include "virtual_file_system.h"

#include "drivers/kernel_log.h"
#include "leanfs.h"
#include "library/kernel_library.h"
#include "paths.h"
#include "virtual_file_system_operations.h"
#include "library/spinlock.h"
#include "scheduler/scheduler.h"

/* M197. This was a spinlock taken with interrupts off, held across whole
   filesystem operations - device reads and writes included, which on a USB
   stick are a busy-wait of up to seventeen milliseconds a command. Every
   other core wanting any file for that time spun with its interrupts off:
   no tick, no IPI, no TLB shootdown answered except from the spin loop.
   Reading a 300 MB browser put the machine's filesystem users in a line for
   three seconds, each burning its core. It is a lock a task sleeps on now,
   and its holder runs with interrupts on. */
static sleep_lock_t fs_lock;

static uint64_t fs_lock_take(void) {
    sleep_lock_acquire(&fs_lock);
    return 0;
}

/* M194: releasing fs_lock is the end of an operation, so it is where the
   filesystem is consistent and the one place a journal commit may cut the
   stream of writes. M197: only a commit that cannot wait is made here - the
   journal's map filling up. A commit that is merely due is the journal
   task's, so no program's stat() pays for a USB write it did not ask for. */
static void fs_unlock(uint64_t flags) {
    (void)flags;
    leanfs_transaction_boundary(0);
    sleep_lock_release(&fs_lock);
}

#define VIRTUAL_FILE_SYSTEM_READ_CHUNK (4u * 1024 * 1024)

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

/* Nothing may be left uncommitted just because nobody touched a file after
   writing one: this asks the journal twice a second, under fs_lock so the
   answer is taken between operations, whether a commit or a checkpoint is
   due. */
static void journal_commit_task(void *argument) {
    (void)argument;
    for (;;) {
        scheduler_sleep_ms(500);
        fs_lock_take();
        leanfs_transaction_boundary(1);
        sleep_lock_release(&fs_lock);
    }
}

void virtual_file_system_init(void) {
    leanfs_init();
    if (leanfs_journal_active()) {
        task_t *committer = task_spawn("journal", journal_commit_task, (void *)0);
        if (committer) {
            committer->parent_id = -1;
        }
    }
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
    uint64_t f = fs_lock_take();
    if (maxlen <= VIRTUAL_FILE_SYSTEM_READ_CHUNK) {
        int64_t r = leanfs_read(path, buffer, maxlen);
        fs_unlock(f);
        return r;
    }

    /* fs_lock was held with interrupts off, and an exec reads the whole program
       under it: 306 MB of browser off a USB stick was twenty seconds in which
       the processor doing it took no timer tick, no keystroke and no other
       task. A big read gives the lock back between pieces. The inode is held
       across the gaps so an unlink in one cannot free the blocks the next
       piece reads. */
    leanfs_stat_t st;
    int handle = leanfs_open(path, 0);
    if (handle < 0 || leanfs_stat(path, &st) != 0 || st.is_directory) {
        fs_unlock(f);
        return -1;
    }
    leanfs_handle_hold(handle);
    size_t wanted = maxlen < st.size ? maxlen : st.size;
    size_t done = 0;
    int64_t result = st.size;
    while (done < wanted) {
        size_t piece = wanted - done;
        if (piece > VIRTUAL_FILE_SYSTEM_READ_CHUNK) {
            piece = VIRTUAL_FILE_SYSTEM_READ_CHUNK;
        }
        int64_t n = leanfs_handle_read(handle, (uint8_t *)buffer + done, piece, (uint32_t)done);
        if (n <= 0) {
            result = -1;
            break;
        }
        done += (size_t)n;
        fs_unlock(f);
        f = fs_lock_take();
    }
    leanfs_handle_release(handle);
    fs_unlock(f);
    return result;
}

int virtual_file_system_write(const char *path, const void *buffer, size_t length) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_write(path, buffer, length);
    fs_unlock(f);
    return r;
}

int virtual_file_system_exists(const char *path) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
    if (m >= 0) {
        return mounts[m].ops->exists(rel);
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_exists(path);
    fs_unlock(f);
    return r;
}

int virtual_file_system_is_directory(const char *path) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
    if (m >= 0) {
        return mounts[m].ops->is_directory(rel);
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_is_directory(path);
    fs_unlock(f);
    return r;
}

int virtual_file_system_mkdir(const char *path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_mkdir(path);
    fs_unlock(f);
    return r;
}

uint32_t virtual_file_system_free_blocks(void) {
    uint64_t f = fs_lock_take();
    uint32_t r = leanfs_free_blocks();
    fs_unlock(f);
    return r;
}

int virtual_file_system_unlink(const char *path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_unlink(path);
    fs_unlock(f);
    return r;
}

int virtual_file_system_rename(const char *old_path, const char *new_path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(old_path, &rel) >= 0 ||
        virtual_file_system_resolve_mount(new_path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_rename(old_path, new_path);
    fs_unlock(f);
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
        uint64_t f = fs_lock_take();
        int r = leanfs_readdir(path, cookie, out);
        fs_unlock(f);
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
    uint64_t f = fs_lock_take();
    int r = leanfs_directory_open(path);
    fs_unlock(f);
    return r;
}

int virtual_file_system_readdir_at(int handle, uint32_t *cookie, leanfs_directory_entry_t *out) {
    uint64_t f = fs_lock_take();
    int r = leanfs_readdir_at(handle, cookie, out);
    fs_unlock(f);
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
    uint64_t f = fs_lock_take();
    size_t r = leanfs_list(path, buffer, maxlen);
    fs_unlock(f);
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

int virtual_file_system_sync(void) {
    uint64_t f = fs_lock_take();
    int r = leanfs_sync();
    fs_unlock(f);
    return r;
}

void virtual_file_system_unmount_clean(void) {
    uint64_t f = fs_lock_take();
    leanfs_unmount_clean();
    fs_unlock(f);
    kernel_log_puts("[vfs] sync: leanfs is write-through; superblock marked cleanly unmounted.\n");
}

int virtual_file_system_rename_replace(const char *old_path, const char *new_path) {
    uint64_t f = fs_lock_take();
    int r = leanfs_rename_replace(old_path, new_path);
    fs_unlock(f);
    return r;
}

int virtual_file_system_check(void) {
    uint64_t f = fs_lock_take();
    uint32_t problems = leanfs_check();
    fs_unlock(f);
    return (int)problems;
}

int virtual_file_system_rmdir(const char *path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_rmdir(path);
    fs_unlock(f);
    return r;
}

int virtual_file_system_stat(const char *path, leanfs_stat_t *out) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
    if (m >= 0) {
        return mounts[m].ops->stat(rel, out);
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_stat(path, out);
    fs_unlock(f);
    return r;
}

int virtual_file_system_symlink(const char *path, const char *target) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_symlink(path, target);
    fs_unlock(f);
    return r;
}

int virtual_file_system_link(const char *old_path, const char *new_path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(old_path, &rel) >= 0 || virtual_file_system_resolve_mount(new_path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_link(old_path, new_path);
    fs_unlock(f);
    return r;
}

int virtual_file_system_utime(const char *path, uint32_t mtime) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_utime(path, mtime);
    fs_unlock(f);
    return r;
}

int virtual_file_system_statvfs(const char *path, virtual_file_system_statvfs_t *out) {
    const char *rel;
    if (!out || virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return -1;
    }
    uint64_t f = fs_lock_take();
    int ok = leanfs_exists(path);
    out->block_size = LEANFS_BLOCK_SIZE;
    out->total_blocks = leanfs_total_blocks();
    out->free_blocks = leanfs_free_blocks();
    out->total_inodes = leanfs_total_inodes();
    out->free_inodes = leanfs_free_inodes();
    out->name_max = LEANFS_MAX_NAME;
    fs_unlock(f);
    return ok ? 0 : -1;
}

uint32_t virtual_file_system_nlink(const char *path) {
    const char *rel;
    if (virtual_file_system_resolve_mount(path, &rel) >= 0) {
        return 0;
    }
    uint64_t f = fs_lock_take();
    uint32_t r = leanfs_nlink(path);
    fs_unlock(f);
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
    uint64_t f = fs_lock_take();
    int64_t r = leanfs_readlink(path, buffer, maxlen);
    fs_unlock(f);
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
    uint64_t f = fs_lock_take();
    int r = leanfs_lstat(path, out);
    fs_unlock(f);
    return r;
}

int virtual_file_system_open(const char *path, int create) {
    const char *rel;
    int m = virtual_file_system_resolve_mount(path, &rel);
    if (m >= 0) {
        int local = mounts[m].ops->open(rel, create);
        return local < 0 ? -1 : VIRTUAL_FILE_SYSTEM_HANDLE_MAKE(m + 1, local);
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_open(path, create);
    fs_unlock(f);
    return r;
}

int64_t virtual_file_system_handle_read(int handle, void *buffer, size_t length, uint32_t off) {
    uint32_t m = VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle);
    if (m > 0) {
        return mounts[m - 1].ops->read(VIRTUAL_FILE_SYSTEM_HANDLE_LOCAL(handle), buffer, length, off);
    }
    uint64_t f = fs_lock_take();
    int64_t r = leanfs_handle_read(handle, buffer, length, off);
    fs_unlock(f);
    return r;
}

int64_t virtual_file_system_handle_write(int handle, const void *buffer, size_t length, uint32_t off) {
    uint32_t m = VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle);
    if (m > 0) {
        return mounts[m - 1].ops->write(VIRTUAL_FILE_SYSTEM_HANDLE_LOCAL(handle), buffer, length, off);
    }
    uint64_t f = fs_lock_take();
    int64_t r = leanfs_handle_write(handle, buffer, length, off);
    fs_unlock(f);
    return r;
}

uint32_t virtual_file_system_handle_size(int handle) {
    uint32_t m = VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle);
    if (m > 0) {
        return mounts[m - 1].ops->size(VIRTUAL_FILE_SYSTEM_HANDLE_LOCAL(handle));
    }
    uint64_t f = fs_lock_take();
    uint32_t r = leanfs_handle_size(handle);
    fs_unlock(f);
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
    uint64_t f = fs_lock_take();
    int r = leanfs_handle_truncate_to(handle, length);
    fs_unlock(f);
    return r;
}

int virtual_file_system_handle_truncate(int handle) {
    if (VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle) > 0) {
        return 0;
    }
    uint64_t f = fs_lock_take();
    int r = leanfs_handle_truncate(handle);
    fs_unlock(f);
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
    uint64_t f = fs_lock_take();
    leanfs_handle_hold(handle);
    fs_unlock(f);
}

void virtual_file_system_handle_release(int handle) {
    if (handle < 0 || VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle) != 0) {
        return;
    }
    uint64_t f = fs_lock_take();
    leanfs_handle_release(handle);
    fs_unlock(f);
}

void virtual_file_system_handle_close(int handle) {
    uint32_t m = VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(handle);
    if (m > 0 && m <= (uint32_t)mount_count && mounts[m - 1].ops->close) {
        mounts[m - 1].ops->close(VIRTUAL_FILE_SYSTEM_HANDLE_LOCAL(handle));
    }
}
