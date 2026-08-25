/* kernel/fs/leanfs.h
 *
 * "leanfs": a minimal, custom filesystem for lean_os's single ATA disk.
 * Not FAT, not ext2 - a small bespoke format sized for what this project
 * actually needs, matching milestones.md's "minimal custom filesystem
 * format (or a simple FAT-like layout)" allowance for M12.
 *
 * On-disk layout, starting at LEANFS_START_LBA:
 *   1 sector    superblock
 *   N sectors   inode table (LEANFS_MAX_INODES entries)
 *   16 sectors  free-block bitmap (LEANFS_DATA_BLOCKS bits)
 *   M sectors   data blocks (1 block == 1 sector == 512 bytes)
 *
 * Each inode has a small fixed number of direct block pointers plus one
 * singly-indirect block (a data block full of 32-bit block pointers) -
 * M15's addition, needed once GUI app binaries/toolkit code started
 * bumping up against the direct-only 8 KiB cap. Deliberately stops at
 * one level of indirection.
 *
 * M53 makes it a *tree*. It was flat until then, and the flatness showed
 * up in three apps at once: the file manager listed this OS's own
 * executables next to your text files, the launcher offered to run
 * `settings.conf`, and SYS_listfiles was documented as "the entire
 * namespace" because it had no choice.
 *
 * A directory is a file whose contents are name/inode records
 * (leanfs_dirent_t below) - the smallest change that is a real directory
 * rather than a prefix convention, and one that reuses every block-
 * allocation path a file already had. The consequence worth stating: a
 * name now lives in exactly one place, its parent directory's records.
 * Inodes carry no name at all any more, which is what makes "two sources
 * of truth for what this file is called" structurally impossible rather
 * than merely avoided.
 *
 * Inode 0 is the root directory, created by format(). Every path is
 * absolute and starts at it; there is no working directory in this
 * project, so there is nothing for a relative path to be relative to.
 *
 * leanfs_init() formats a fresh filesystem automatically if the
 * superblock magic or geometry doesn't match - nothing to migrate, and
 * M53's own layout change is exactly the case that relies on it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define LEANFS_MAX_NAME             27

/* M53: the longest absolute path this filesystem will resolve, NUL
 * included. Deep enough for anything the layout below actually needs
 * ("/home/notes.txt" is 15) with room for nesting a person might create,
 * and short enough that the syscall layer can put one on a kernel stack -
 * which is 8 KiB, so this being a number rather than "however long the
 * caller's string is" is load-bearing rather than tidy. A longer path is
 * refused, never truncated: a truncated path names a different file. */
#define LEANFS_MAX_PATH             128
#define LEANFS_DIRECT_BLOCKS        16
#define LEANFS_BLOCK_SIZE           512
#define LEANFS_INDIRECT_POINTERS    (LEANFS_BLOCK_SIZE / (int)sizeof(uint32_t)) /* 128 */
#define LEANFS_MAX_FILE_SIZE        ((LEANFS_DIRECT_BLOCKS + LEANFS_INDIRECT_POINTERS) * LEANFS_BLOCK_SIZE) /* 72 KiB */

/* M53: 32 -> 96. The old number was an exact fit the moment directories
 * arrived and it took a count to see it: this project ships 24 programs,
 * which with four directories, the boot self-tests' own fixtures and
 * settings.conf came to exactly 32 - no headroom at all, and "the next
 * program you add silently cannot be seeded" is the same shape of bug
 * this project has shipped three times behind an exactly-sized cap (M40,
 * M48, M50). 96 is that plus room for sixty more files, and costs 15
 * sectors of inode table instead of 7. */
#define LEANFS_MAX_INODES           96

/* Total data region capacity. 65536 blocks = 32 MiB. Must be a multiple
 * of (LEANFS_BLOCK_SIZE * 8) so the bitmap lands on a whole number of
 * sectors. */
#define LEANFS_DATA_BLOCKS          65536u

/* An entry in a directory's own contents. 32 bytes exactly, so 16 fit in
 * a block and no record ever straddles one - which is what lets every
 * directory operation below be a whole-block read/modify/write rather
 * than needing byte-level addressing this driver does not have. A free
 * slot is one whose name is empty. */
typedef struct __attribute__((packed)) {
    char name[LEANFS_MAX_NAME + 1];
    uint32_t inode;
} leanfs_dirent_t;

/* The most entries any one directory can hold. There are no hard links
 * here, so no directory can name more inodes than exist. */
#define LEANFS_MAX_DIRENTS LEANFS_MAX_INODES

void leanfs_init(void);

/* Every call below takes an absolute path ("/bin/ls"). A path that does
 * not begin with '/', names a component longer than LEANFS_MAX_NAME,
 * contains an empty component, or tries to walk through something that
 * is not a directory is refused - as is anything using "." or ".." to
 * climb, which this format deliberately does not store (see resolve() in
 * leanfs.c). */

/* Reads the whole file into buf (up to maxlen bytes). On success returns
 * the file's real size (may be > maxlen, in which case only maxlen bytes
 * were copied - caller's responsibility to size its buffer). Returns -1
 * if the path doesn't resolve to a regular file. */
int64_t leanfs_read(const char *path, void *buf, size_t maxlen);

/* Creates or overwrites a regular file. The parent directory must
 * already exist. Returns 0 on success, -1 if the path is malformed, the
 * parent is missing or full, there's no free inode, no free space, or
 * len exceeds LEANFS_MAX_FILE_SIZE. */
int leanfs_write(const char *path, const void *buf, size_t len);

/* 1 if the path resolves to anything at all (file or directory). */
int leanfs_exists(const char *path);

/* 1 if the path resolves to a directory specifically - what a path bar
 * needs in order to know whether entering it means anything. */
int leanfs_is_dir(const char *path);

/* Creates one directory. Its parent must exist; an existing path of
 * either kind is an error rather than a no-op, so "I created this" and
 * "this was already here" can't be confused. Returns 0 or -1. */
int leanfs_mkdir(const char *path);

/* M56: how many data blocks are currently free.
 *
 * Exists so a test can state the thing that actually matters about
 * removing a file - that its blocks came back - as one comparison
 * instead of as "do it sixty times and see if the disk fills up". That
 * loop was the honest version when there was nothing to count, and it
 * cost four thousand ATA sector writes: every metadata update in this
 * filesystem rewrites the whole inode table and bitmap (31 sectors), and
 * PIO writes are the most expensive thing this OS does. */
uint32_t leanfs_free_blocks(void);

/* M56: removes one regular file. Frees its blocks and its inode and
 * drops its record from the parent directory, in that order, so nothing
 * can be reached through a name after its blocks are gone.
 *
 * Refuses a directory outright rather than recursing or checking for
 * emptiness. This filesystem has never had to remove anything - which is
 * how `m48trunc` ended up living on it forever - and "rmdir" is a
 * different operation with a different failure mode; adding it
 * speculatively alongside the one the file manager actually needs would
 * be the kind of guess this project has avoided elsewhere. Returns 0, or
 * -1 for a path that doesn't resolve, isn't a regular file, or is
 * malformed. */
int leanfs_unlink(const char *path);

/* M56: moves one entry from one name to another, which may be in a
 * different directory. No data moves - a rename is a change to *records*,
 * which is only true because M53 stopped storing a name in the inode; it
 * would have been a copy before that.
 *
 * The new name must not already exist: silently replacing a file is a
 * way to lose one, and the file manager it exists for can ask. Returns
 * 0, or -1 if either path is malformed, the source is missing, or the
 * destination is taken. */
int leanfs_rename(const char *old_path, const char *new_path);

/* Writes the name of every entry in the directory at `path`, each
 * followed by '\n', into buf up to maxlen bytes (stopping early and
 * silently if a name wouldn't fit). A directory's own name is suffixed
 * with '/' so a caller can tell the two apart without a second call -
 * which is what the file manager needs to know what a double-click
 * should do. Returns the number of bytes written, or 0 if the path
 * isn't a directory. */
size_t leanfs_list(const char *path, char *buf, size_t maxlen);
