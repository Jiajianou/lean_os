/* kernel/fs/leanfs.h
 *
 * "leanfs": a minimal, custom, flat (no subdirectories) filesystem for
 * lean_os's single ATA disk. Not FAT, not ext2 - a small bespoke format
 * sized for what this project actually needs right now (a handful of
 * small files), matching milestones.md's "minimal custom filesystem
 * format (or a simple FAT-like layout)" allowance for M12.
 *
 * On-disk layout, starting at LEANFS_START_LBA:
 *   1 sector    superblock
 *   7 sectors   inode table (LEANFS_MAX_INODES entries)
 *   16 sectors  free-block bitmap (LEANFS_DATA_BLOCKS bits)
 *   N sectors   data blocks (1 block == 1 sector == 512 bytes)
 *
 * Each inode has a small fixed number of direct block pointers plus one
 * singly-indirect block (a data block full of 32-bit block pointers) -
 * M15's addition, needed once GUI app binaries/toolkit code started
 * bumping up against the direct-only 8 KiB cap. Deliberately stops at
 * one level of indirection (no doubly-indirect) - LEANFS_MAX_FILE_SIZE
 * is generous for anything this project actually ships, and a second
 * level of indirection is complexity this "minimal" format doesn't need
 * yet. leanfs_init() formats a fresh filesystem automatically if the
 * superblock magic doesn't match (nothing to migrate - this is the only
 * version this format has ever had).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define LEANFS_MAX_NAME             27
#define LEANFS_DIRECT_BLOCKS        16
#define LEANFS_BLOCK_SIZE           512
#define LEANFS_INDIRECT_POINTERS    (LEANFS_BLOCK_SIZE / (int)sizeof(uint32_t)) /* 128 */
#define LEANFS_MAX_FILE_SIZE        ((LEANFS_DIRECT_BLOCKS + LEANFS_INDIRECT_POINTERS) * LEANFS_BLOCK_SIZE) /* 72 KiB */
#define LEANFS_MAX_INODES           32
/* Total data region capacity. 65536 blocks = 32 MiB - sized for several
 * GUI app binaries plus a toolkit/font assets, not just the handful of
 * ~7 KiB coreutils M12-M14 shipped. Must be a multiple of
 * (LEANFS_BLOCK_SIZE * 8) so the bitmap lands on a whole number of
 * sectors. */
#define LEANFS_DATA_BLOCKS          65536u

void leanfs_init(void);

/* Reads the whole file into buf (up to maxlen bytes). On success returns
 * the file's real size (may be > maxlen, in which case only maxlen bytes
 * were copied - caller's responsibility to size its buffer). Returns -1
 * if no file by that name exists. */
int64_t leanfs_read(const char *name, void *buf, size_t maxlen);

/* Creates or overwrites a file. Returns 0 on success, -1 if there's no
 * free inode, no free space, or len exceeds LEANFS_MAX_FILE_SIZE. */
int leanfs_write(const char *name, const void *buf, size_t len);

int leanfs_exists(const char *name);

/* Writes every filename followed by '\n' into buf, up to maxlen bytes
 * (stops early, silently, if a name wouldn't fit - "ls" on a filesystem
 * this small isn't expected to overflow a page-sized buffer). Returns
 * the number of bytes written. leanfs is flat, so this is the entire
 * namespace - no directory argument. */
size_t leanfs_list(char *buf, size_t maxlen);
