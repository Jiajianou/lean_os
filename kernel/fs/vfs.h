/* kernel/fs/vfs.h
 *
 * Generic file-level API the rest of the kernel calls instead of talking
 * to leanfs.h directly - the seam a second filesystem type would plug
 * into if this project ever needed one. Right now there's exactly one
 * implementation (leanfs.c), so this is a thin, honest pass-through
 * rather than a driver table or vtable dispatch mechanism that would be
 * pure speculative generality for a single-filesystem kernel.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

void vfs_init(void);
int64_t vfs_read(const char *name, void *buf, size_t maxlen);
int vfs_write(const char *name, const void *buf, size_t len);
int vfs_exists(const char *name);
size_t vfs_list(char *buf, size_t maxlen);
