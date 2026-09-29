#pragma once

/* Must equal the Makefile's FS_START_LBA. The two are separate because one is
   read by make and the other by the compiler, and the image's recipe checks
   that the boot image has not grown into it - which is how M165 found this
   needed moving, at 8336 sectors of kernel. */
#define LEANFS_START_LBA 16384u

#define LEANFS_START_BLOCK (LEANFS_START_LBA / LEANFS_SECTORS_PER_BLOCK)

#include <stddef.h>
#include <stdint.h>

#define LEANFS_MAX_NAME             255

#define LEANFS_MAX_PATH             4096
#define LEANFS_DIRECT_BLOCKS        16

#define LEANFS_BLOCK_SIZE           4096
#define LEANFS_SECTOR_SIZE          512
#define LEANFS_SECTORS_PER_BLOCK    (LEANFS_BLOCK_SIZE / LEANFS_SECTOR_SIZE)
#define LEANFS_INDIRECT_POINTERS    (LEANFS_BLOCK_SIZE / (int)sizeof(uint32_t))
#define LEANFS_DINDIRECT_BLOCKS     ((uint64_t)LEANFS_INDIRECT_POINTERS * LEANFS_INDIRECT_POINTERS)
#define LEANFS_MAX_FILE_BLOCKS      ((uint64_t)LEANFS_DIRECT_BLOCKS + LEANFS_INDIRECT_POINTERS + LEANFS_DINDIRECT_BLOCKS)
#define LEANFS_MAX_FILE_SIZE        0xFFFFF000u

#define LEANFS_MAX_INODES           131072u

#define LEANFS_DATA_BLOCKS          524288u

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_length;
    uint8_t  name_length;
    uint8_t  type;
} leanfs_dirent_t;

#define LEANFS_DIRENT_HEADER   8u
#define LEANFS_DIRENT_ALIGN 4u

#define LEANFS_DIRENT_NEED(name_length) \
    ((LEANFS_DIRENT_HEADER + (uint32_t)(name_length) + LEANFS_DIRENT_ALIGN - 1u) & ~(LEANFS_DIRENT_ALIGN - 1u))

typedef struct {
    uint32_t inode;
    uint8_t  is_directory;
    uint8_t  is_link;
    char     name[LEANFS_MAX_NAME + 1];
} leanfs_directory_entry_t;

int leanfs_readdir(const char *path, uint32_t *cookie, leanfs_directory_entry_t *out);

int leanfs_directory_open(const char *path);
int leanfs_readdir_at(int handle, uint32_t *cookie, leanfs_directory_entry_t *out);

void leanfs_init(void);

int64_t leanfs_read(const char *path, void *buffer, size_t maxlen);

int leanfs_write(const char *path, const void *buffer, size_t length);

int leanfs_exists(const char *path);

uint32_t leanfs_free_scratch_lba(uint32_t blocks);

int leanfs_is_directory(const char *path);

int leanfs_mkdir(const char *path);

uint32_t leanfs_free_blocks(void);

uint32_t leanfs_total_blocks(void);
uint32_t leanfs_total_inodes(void);
uint32_t leanfs_free_inodes(void);

int leanfs_utime(const char *path, uint32_t mtime);

int leanfs_unlink(const char *path);

void leanfs_handle_hold(int handle);
void leanfs_handle_release(int handle);
int leanfs_handle_orphaned(int handle);

int leanfs_rename(const char *old_path, const char *new_path);

size_t leanfs_list(const char *path, char *buffer, size_t maxlen);

int leanfs_rmdir(const char *path);

typedef struct {
    uint32_t size;
    uint32_t mtime;
    uint8_t is_directory;
    uint8_t is_link;
    uint32_t inode;
} leanfs_stat_t;

int leanfs_stat(const char *path, leanfs_stat_t *out);

int leanfs_symlink(const char *path, const char *target);

int leanfs_link(const char *old_path, const char *new_path);

uint32_t leanfs_nlink(const char *path);
int64_t leanfs_readlink(const char *path, char *buffer, size_t maxlen);
int leanfs_lstat(const char *path, leanfs_stat_t *out);

int leanfs_handle_stat(int handle, leanfs_stat_t *out);

#define LEANFS_OPEN_CREATE 1
#define LEANFS_OPEN_EXCL   2
int leanfs_open(const char *path, int create);

int64_t leanfs_handle_read(int handle, void *buffer, size_t length, uint32_t off);
int64_t leanfs_handle_write(int handle, const void *buffer, size_t length, uint32_t off);

uint32_t leanfs_handle_size(int handle);

int leanfs_handle_truncate(int handle);

int leanfs_handle_truncate_to(int handle, uint32_t length);

uint32_t leanfs_meta_writes(void);

uint32_t leanfs_check(void);

int leanfs_sync(void);

void leanfs_unmount_clean(void);

void leanfs_transaction_boundary(int from_journal_task);

int leanfs_journal_active(void);

int leanfs_rename_replace(const char *old_path, const char *new_path);

void leanfs_debug_orphan(const char *path);
