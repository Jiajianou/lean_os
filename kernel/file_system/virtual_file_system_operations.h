#pragma once

#include <stddef.h>
#include <stdint.h>

#include "leanfs.h"

struct tty;

typedef struct virtual_file_system_ops {
    int (*stat)(const char *rel, leanfs_stat_t *out);
    int (*is_directory)(const char *rel);
    int (*exists)(const char *rel);

    int (*open)(const char *rel, int create);
    int64_t (*read)(int handle, void *buffer, size_t length, uint32_t off);
    int64_t (*write)(int handle, const void *buffer, size_t length, uint32_t off);
    uint32_t (*size)(int handle);
    int (*handle_stat)(int handle, leanfs_stat_t *out);

    int (*readdir)(const char *rel, uint32_t *cookie, leanfs_directory_entry_t *out);

    void (*close)(int handle);

    int (*readable)(int handle);

    struct tty *(*tty_of)(int handle, int *pty_number);
} virtual_file_system_ops_t;

#define VIRTUAL_FILE_SYSTEM_HANDLE_MOUNT(h)   (((uint32_t)(h) >> 24) & 0xFFu)
#define VIRTUAL_FILE_SYSTEM_HANDLE_LOCAL(h)   ((int)((uint32_t)(h) & 0x00FFFFFFu))
#define VIRTUAL_FILE_SYSTEM_HANDLE_MAKE(m, l) ((int)((((uint32_t)(m)) << 24) | ((uint32_t)(l) & 0x00FFFFFFu)))

const virtual_file_system_ops_t *devfs_ops(void);
void devfs_init(void);

const virtual_file_system_ops_t *procfs_ops(void);
void procfs_init(void);
