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

void vfs_init(void);
int64_t vfs_read(const char *path, void *buf, size_t maxlen);
int vfs_write(const char *path, const void *buf, size_t len);
int vfs_exists(const char *path);
int vfs_is_dir(const char *path);
int vfs_mkdir(const char *path);
size_t vfs_list(const char *path, char *buf, size_t maxlen);

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
