#pragma once

#include <stddef.h>
#include <stdint.h>

#include "leanfs.h"

void virtual_file_system_init(void);
int64_t virtual_file_system_read(const char *path, void *buf, size_t maxlen);
int virtual_file_system_write(const char *path, const void *buf, size_t len);
int virtual_file_system_exists(const char *path);
int virtual_file_system_is_directory(const char *path);
int virtual_file_system_mkdir(const char *path);
uint32_t virtual_file_system_free_blocks(void);
int virtual_file_system_unlink(const char *path);
int virtual_file_system_rename(const char *old_path, const char *new_path);
size_t virtual_file_system_list(const char *path, char *buf, size_t maxlen);

int virtual_file_system_readdir(const char *path, uint32_t *cookie, leanfs_directory_entry_t *out);

int virtual_file_system_directory_open(const char *path);
int virtual_file_system_readdir_at(int handle, uint32_t *cookie, leanfs_directory_entry_t *out);

int virtual_file_system_rmdir(const char *path);
int virtual_file_system_stat(const char *path, leanfs_stat_t *out);

int virtual_file_system_symlink(const char *path, const char *target);
int virtual_file_system_link(const char *old_path, const char *new_path);
typedef struct {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t name_max;
} virtual_file_system_statvfs_t;

int virtual_file_system_statvfs(const char *path, virtual_file_system_statvfs_t *out);

int virtual_file_system_mount_info(int i, const char **prefix, const char **type);

int virtual_file_system_utime(const char *path, uint32_t mtime);

uint32_t virtual_file_system_nlink(const char *path);
int64_t virtual_file_system_readlink(const char *path, char *buf, size_t maxlen);
int virtual_file_system_lstat(const char *path, leanfs_stat_t *out);
int virtual_file_system_open(const char *path, int create);
int64_t virtual_file_system_handle_read(int handle, void *buf, size_t len, uint32_t off);
int64_t virtual_file_system_handle_write(int handle, const void *buf, size_t len, uint32_t off);
uint32_t virtual_file_system_handle_size(int handle);
int virtual_file_system_handle_stat(int handle, leanfs_stat_t *out);
int virtual_file_system_handle_truncate(int handle);

void virtual_file_system_handle_close(int handle);

int virtual_file_system_handle_readable(int handle);

struct tty;
struct tty *virtual_file_system_handle_tty(int handle, int *pty_number);

int virtual_file_system_handle_truncate_to(int handle, uint32_t len);

void virtual_file_system_sync(void);

int virtual_file_system_rename_replace(const char *old_path, const char *new_path);

int virtual_file_system_check(void);
