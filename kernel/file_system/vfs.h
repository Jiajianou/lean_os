#pragma once

#include <stddef.h>
#include <stdint.h>

#include "leanfs.h"

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

int vfs_readdir(const char *path, uint32_t *cookie, leanfs_dir_entry_t *out);

int vfs_dir_open(const char *path);
int vfs_readdir_at(int handle, uint32_t *cookie, leanfs_dir_entry_t *out);

int vfs_rmdir(const char *path);
int vfs_stat(const char *path, leanfs_stat_t *out);

int vfs_symlink(const char *path, const char *target);
int vfs_link(const char *old_path, const char *new_path);
typedef struct {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t name_max;
} vfs_statvfs_t;

int vfs_statvfs(const char *path, vfs_statvfs_t *out);

int vfs_mount_info(int i, const char **prefix, const char **type);

int vfs_utime(const char *path, uint32_t mtime);

uint32_t vfs_nlink(const char *path);
int64_t vfs_readlink(const char *path, char *buf, size_t maxlen);
int vfs_lstat(const char *path, leanfs_stat_t *out);
int vfs_open(const char *path, int create);
int64_t vfs_handle_read(int handle, void *buf, size_t len, uint32_t off);
int64_t vfs_handle_write(int handle, const void *buf, size_t len, uint32_t off);
uint32_t vfs_handle_size(int handle);
int vfs_handle_stat(int handle, leanfs_stat_t *out);
int vfs_handle_truncate(int handle);

void vfs_handle_close(int handle);

int vfs_handle_readable(int handle);

struct tty;
struct tty *vfs_handle_tty(int handle, int *pty_number);

int vfs_handle_truncate_to(int handle, uint32_t len);

void vfs_sync(void);

int vfs_rename_replace(const char *old_path, const char *new_path);

int vfs_check(void);
