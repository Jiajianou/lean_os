#pragma once

#include <stddef.h>
#include <stdint.h>

#include "leanfs.h"

struct tty;

typedef struct vfs_ops {
    int (*stat)(const char *rel, leanfs_stat_t *out);
    int (*is_dir)(const char *rel);
    int (*exists)(const char *rel);

    int (*open)(const char *rel, int create);
    int64_t (*read)(int handle, void *buf, size_t len, uint32_t off);
    int64_t (*write)(int handle, const void *buf, size_t len, uint32_t off);
    uint32_t (*size)(int handle);
    int (*handle_stat)(int handle, leanfs_stat_t *out);

    int (*readdir)(const char *rel, uint32_t *cookie, leanfs_dir_entry_t *out);

    void (*close)(int handle);

    int (*readable)(int handle);

    struct tty *(*tty_of)(int handle, int *pty_number);
} vfs_ops_t;

#define VFS_HANDLE_MOUNT(h)   (((uint32_t)(h) >> 24) & 0xFFu)
#define VFS_HANDLE_LOCAL(h)   ((int)((uint32_t)(h) & 0x00FFFFFFu))
#define VFS_HANDLE_MAKE(m, l) ((int)((((uint32_t)(m)) << 24) | ((uint32_t)(l) & 0x00FFFFFFu)))

const vfs_ops_t *devfs_ops(void);
void devfs_init(void);

const vfs_ops_t *procfs_ops(void);
void procfs_init(void);
