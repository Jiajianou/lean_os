/* kernel/fs/vfs.h
 *
 * Generic file-level API the rest of the kernel calls instead of talking
 * to leanfs.h directly - the seam a second filesystem type would plug
 * into if this project ever needed one. Right now there's exactly one
 * implementation (leanfs.c), so this is a thin, honest pass-through
 * rather than a driver table or vtable dispatch mechanism that would be
 * pure speculative generality for a single-filesystem kernel.
 *
 * M53: every call takes an absolute path ("/bin/ls") rather than a bare
 * name. The shape is unchanged - this is a resolver in front of the same
 * whole-file read/write, not a new API - which is why vfs_read and
 * vfs_write still take and return exactly what they did.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "leanfs.h" /* M59: leanfs_stat_t - the one type this pass-through has to name */

void vfs_init(void);
int64_t vfs_read(const char *path, void *buf, size_t maxlen);
int vfs_write(const char *path, const void *buf, size_t len);
int vfs_exists(const char *path);
int vfs_is_dir(const char *path);
int vfs_mkdir(const char *path);
uint32_t vfs_free_blocks(void);
int vfs_unlink(const char *path);
int vfs_rename(const char *old_path, const char *new_path);
size_t vfs_list(const char *path, char *buf, size_t maxlen);

/* M81: one directory entry at a time - see leanfs_readdir for the cookie
 * contract. The pass-through this file has always been, plus the lock
 * every other call here takes: a walk that holds the lock only for the
 * entry it is fetching is what lets a caller take as long as it likes
 * between entries without stopping every other filesystem user. */
int vfs_readdir(const char *path, uint32_t *cookie, leanfs_dir_entry_t *out);

/* M81: the same walk with the path resolved once - see leanfs_dir_open. */
int vfs_dir_open(const char *path);
int vfs_readdir_at(int handle, uint32_t *cookie, leanfs_dir_entry_t *out);

/* M59: see the leanfs.h declarations of the same names for what each one
 * promises. This layer stays the thin, honest pass-through it has always
 * been - a second filesystem type would implement these, not reshape
 * them. */
int vfs_rmdir(const char *path);
int vfs_stat(const char *path, leanfs_stat_t *out);

/* M87: symbolic links. readlink and lstat are the two that do NOT follow
 * a final link - see leanfs.h for why that distinction is the point. */
int vfs_symlink(const char *path, const char *target);
/* M93: a second name for a file, and how many names it has. Refused for
 * anything under a synthetic mount - see the implementation. */
int vfs_link(const char *old_path, const char *new_path);
/* M88: what statvfs() reports, in the filesystem's own units.
 *
 * Deliberately not system_api's os_statvfs_t, for the same reason
 * leanfs_stat_t is not os_stat_t: one is what a filesystem knows and the
 * other is an ABI, and the day there is a second filesystem here they
 * stop being the same shape. syscall.c converts field by field. */
typedef struct {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t name_max;
} vfs_statvfs_t;

int vfs_statvfs(const char *path, vfs_statvfs_t *out);

/* M89: the mount table, read out one row at a time, for /proc/mounts.
 *
 * The table has existed since M87 and nothing outside this file could
 * see it - which is why `mount` and `df` had nothing to read. `i` is
 * 0-based; returns 0 past the end. `*prefix` is "/" for the root and the
 * mount point otherwise, `*type` the filesystem's name. */
int vfs_mount_info(int i, const char **prefix, const char **type);

/* M88: set a file's mtime. See leanfs_utime for why a filesystem that
 * stamps rtc_now() on every write needs one call that does not. */
int vfs_utime(const char *path, uint32_t mtime);

uint32_t vfs_nlink(const char *path);
int64_t vfs_readlink(const char *path, char *buf, size_t maxlen);
int vfs_lstat(const char *path, leanfs_stat_t *out);
int vfs_open(const char *path, int create);
int64_t vfs_handle_read(int handle, void *buf, size_t len, uint32_t off);
int64_t vfs_handle_write(int handle, const void *buf, size_t len, uint32_t off);
uint32_t vfs_handle_size(int handle);
int vfs_handle_stat(int handle, leanfs_stat_t *out); /* M77 - see leanfs.h */
int vfs_handle_truncate(int handle);

/* M101: the last fd naming this handle has gone - see vfs_ops_t.close.
 * A no-op for leanfs handles, which hold no per-open state; the reason
 * it exists is procfs, whose open() claims a slot out of a fixed table.
 * Called from openfile_unref rather than from SYS_close, because a
 * descriptor is not the last one until the refcount says so. */
void vfs_handle_close(int handle);

/* M87: truncate to any length - see leanfs_handle_truncate_to. */
int vfs_handle_truncate_to(int handle, uint32_t len);

/* M47: the flush the shutdown path (kernel/power/power.c) calls before
 * cutting power.
 *
 * leanfs is write-through today - every vfs_write reaches
 * ata_write_sectors before it returns, so there is genuinely nothing
 * buffered for this to push out, and this is honest about that rather
 * than pretending to do work. It exists because the alternative is worse:
 * the day leanfs grows a write cache, the fix belongs here, not in a
 * shutdown path that would otherwise have to learn what a filesystem is.
 * Says what it did in the log, so "did the disk get flushed" is a
 * question the boot log answers rather than one you reason about. */
void vfs_sync(void);

/* M71: rename that may replace an existing destination - the half
 * vfs_rename deliberately refuses. See leanfs.h. */
int vfs_rename_replace(const char *old_path, const char *new_path);

/* M71: rebuild the free-block bitmap from the inodes. Runs automatically
 * on an unclean mount; exposed for the self-test. */
int vfs_check(void);
