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
 * Each inode has a small fixed number of direct block pointers, one
 * singly-indirect block (a data block full of 32-bit block pointers) -
 * M15's addition, needed once GUI app binaries/toolkit code started
 * bumping up against the direct-only 8 KiB cap - and, since M59, one
 * doubly-indirect block, which is what takes the ceiling to 8 MiB.
 * Deliberately stops at two levels: a third would be another order of
 * magnitude past anything this OS can hold.
 *
 * (This paragraph said "deliberately stops at one level of indirection"
 * for twenty-two milestones after M59 added the second. Corrected in
 * M81, which had reason to read it.)
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
 * Inode 0 is the root directory, created by format(). Every path this
 * file resolves is absolute and starts at it. That is still true and no
 * longer means what it used to: M75 gave *tasks* a working directory,
 * and the syscall layer joins a relative path onto it before leanfs ever
 * sees one (see copy_path_from_user). A working directory is a property
 * of a caller, not of a filesystem, which is why it lives there and this
 * sentence stays true.
 *
 * M81 makes the directory record variable-length so a name can be 255
 * bytes without a directory paying 260 bytes for every short one - see
 * leanfs_dirent_t below for the rule that keeps records from straddling
 * a block, which is what all of this rests on.
 *
 * leanfs_init() formats a fresh filesystem automatically if the
 * superblock magic or geometry doesn't match - M53's and M81's layout
 * changes are both exactly the case that relies on it. M81 also adds a
 * `version` distinct from the magic: the magic guards *geometry* and a
 * mismatch can only ever be a reformat, while the version guards
 * *meaning* and is the field a future change that merely reinterprets
 * bytes can migrate across.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* M81: 27 -> 255, the number <dirent.h> has advertised as NAME_MAX since
 * M77 while noting that "leanfs will never produce" one that long. One of
 * those two numbers was a lie and this milestone decides which: a name is
 * now what POSIX says a name is, and the header stops apologising.
 *
 * The old 27 was not arbitrary - it was what made a directory record
 * exactly 32 bytes so sixteen fitted a block with nothing straddling.
 * That constraint is real and is kept; see leanfs_dirent_t, which buys
 * both a 255-byte name and the same no-straddling guarantee by making
 * records variable-length instead of making them bigger. */
#define LEANFS_MAX_NAME             255

/* M53: the longest absolute path this filesystem will resolve, NUL
 * included. Refused rather than truncated: a truncated path names a
 * different file.
 *
 * M81: 128 -> 4096, the number every program that has ever declared
 * `char path[PATH_MAX]` expects. M53's own reasoning for 128 was that the
 * syscall layer puts one on a kernel stack "which is 8 KiB, so this being
 * a number rather than 'however long the caller's string is' is
 * load-bearing rather than tidy." That reasoning is still exactly right,
 * which is why the stack moved rather than the argument: a kernel stack
 * is 32 KiB as of this milestone (kernel/sched/sched.c), because a path
 * is now 4 KiB and copy_path_from_user holds two of them at once. The
 * constraint did not go away - it got priced. */
#define LEANFS_MAX_PATH             4096
#define LEANFS_DIRECT_BLOCKS        16
#define LEANFS_BLOCK_SIZE           512
#define LEANFS_INDIRECT_POINTERS    (LEANFS_BLOCK_SIZE / (int)sizeof(uint32_t)) /* 128 */
/* M59: 16 direct + 128 indirect + 128 * 128 double-indirect blocks.
 * 16528 blocks, a shade over 8 MiB - up from a 72 KiB ceiling that was
 * smaller than this project's own largest source file, and which three
 * separate apps were visibly working around. */
#define LEANFS_DINDIRECT_BLOCKS     (LEANFS_INDIRECT_POINTERS * LEANFS_INDIRECT_POINTERS) /* 16384 */
#define LEANFS_MAX_FILE_BLOCKS      (LEANFS_DIRECT_BLOCKS + LEANFS_INDIRECT_POINTERS + LEANFS_DINDIRECT_BLOCKS)
#define LEANFS_MAX_FILE_SIZE        (LEANFS_MAX_FILE_BLOCKS * LEANFS_BLOCK_SIZE)

/* The number of files this filesystem can hold, in total, across every
 * directory. Its history is this project's most-repeated bug: 32 (M12),
 * 96 (M53), 192 (M74-M79), each one "enough for what is here now" and
 * each one exhausted by the next arc. M53's own note says so in as many
 * words and then M74 did it again, which is the strongest possible
 * argument that the number was never the problem.
 *
 * M81: 192 -> 8192, and this time the reasoning is not "enough for what
 * is here now". It is that the wall this cap creates is not a limit a
 * program can work around - a source tarball is thousands of files, a
 * language's standard library is thousands of files, and every one of
 * them has to exist at once or the program does not run at all. 192 is
 * not a small ceiling, it is a floor below every real workload. 8192 is
 * chosen to be past the largest thing this OS has any prospect of
 * holding (CPython's standard library is roughly 3000 files) rather than
 * past the largest thing it holds today.
 *
 * What it costs, stated because the last four bumps stated theirs: 1 MiB
 * of inode table on disk (2048 sectors, out of a 35 MiB image that has
 * room) and the same 1 MiB of kernel BSS, up from 16 KiB. The inode grew
 * to 128 bytes to get there - see leanfs_inode_t in leanfs.c for why a
 * power of two rather than the 84 bytes it packs into. */
#define LEANFS_MAX_INODES           8192

/* Total data region capacity. 65536 blocks = 32 MiB. Must be a multiple
 * of (LEANFS_BLOCK_SIZE * 8) so the bitmap lands on a whole number of
 * sectors. */
#define LEANFS_DATA_BLOCKS          65536u

/* ---- M81: a directory record, and why it is variable-length ----------
 *
 * Until M81 this was a fixed 32 bytes - a 27-char name and an inode
 * number - chosen so sixteen fitted exactly in a 512-byte block and no
 * record ever straddled one. That invariant is what lets every directory
 * operation work a block at a time, and it is worth more than it looks:
 * a lookup in a ten-entry directory reads one sector.
 *
 * A 255-byte name cannot keep that shape. A fixed record big enough for
 * the longest name is 260 bytes, which (a) no longer divides a block, and
 * (b) makes a directory of three thousand twenty-character filenames
 * 780 KiB that every lookup scans, where the names themselves are 60 KiB.
 * Paying eight times over for the one entry in a thousand that is long is
 * the wrong trade on the exact workload this milestone exists for.
 *
 * So records are variable-length, the way ext2's are, with the one rule
 * that keeps the block invariant intact: **`rec_len` of the last record
 * in a block is stretched to reach the end of that block**, so records
 * tile each block exactly and none ever crosses a boundary. A directory
 * is therefore always a whole number of blocks, and scanning one is
 * scanning a sector.
 *
 * `inode == 0` marks a record as free space. Zero is safe as a sentinel
 * for the same reason block 0 is: inode 0 is the root, and the root is
 * nobody's child, so no live record can ever name it.
 *
 * `type` is carried in the record so that listing a directory does not
 * have to fetch every inode to answer "is this a directory" - which is
 * precisely what <dirent.h>'s d_type is for, and what M77 had to
 * reconstruct from a trailing '/' because this record had nowhere to put
 * it.
 */
typedef struct __attribute__((packed)) {
    uint32_t inode;    /* 0 == free space; otherwise the inode this name refers to */
    uint16_t rec_len;  /* bytes from the start of this record to the next */
    uint8_t  name_len; /* 0..LEANFS_MAX_NAME */
    uint8_t  type;     /* LEANFS_TYPE_FILE / LEANFS_TYPE_DIR; meaningless when inode == 0 */
    /* char name[name_len] follows, unterminated and unpadded. */
} leanfs_dirent_t;

#define LEANFS_DIRENT_HDR   8u  /* sizeof(leanfs_dirent_t), asserted in leanfs.c */
#define LEANFS_DIRENT_ALIGN 4u

/* The smallest record that can hold a name this long. Records are padded
 * to LEANFS_DIRENT_ALIGN so that every rec_len keeps the next header
 * aligned, which is what lets this driver read one out of a block buffer
 * as a struct rather than byte by byte. */
#define LEANFS_DIRENT_NEED(name_len) \
    ((LEANFS_DIRENT_HDR + (uint32_t)(name_len) + LEANFS_DIRENT_ALIGN - 1u) & ~(LEANFS_DIRENT_ALIGN - 1u))

/* One unpacked entry, as everything above this driver wants to see it:
 * the name NUL-terminated, and the two facts a caller would otherwise
 * need a stat for. */
typedef struct {
    uint32_t inode;
    uint8_t  is_dir;
    char     name[LEANFS_MAX_NAME + 1];
} leanfs_dir_entry_t;

/* M81: read one entry at a time, so that a directory holding thousands of
 * them can be walked by a caller holding one.
 *
 * `*cookie` is an opaque position, zero to start; each call advances it
 * past the entry it returned. It is a byte offset into the directory
 * file, which is what makes resuming O(1) - the next call seeks straight
 * to the block rather than counting entries it has already seen.
 *
 * Returns 1 and fills `out` when there was an entry, 0 at the end of the
 * directory, and -1 if `path` is not a directory. Free records and the
 * padding at the end of a block are skipped internally: a caller sees
 * only real names.
 *
 * This is the interface leanfs_list should have had. That one is kept
 * because a terminal wanting a newline-separated blob is a real caller
 * and reimplementing it over this would be longer, not shorter - but it
 * is now written in terms of this, so there is one walk and not two. */
int leanfs_readdir(const char *path, uint32_t *cookie, leanfs_dir_entry_t *out);

/* The same walk, with the path resolved once instead of once per entry.
 *
 * leanfs_readdir is the convenient form and the right one for a caller
 * fetching a handful of entries. It is the wrong one for a caller walking
 * a directory of thousands, because resolving "/a/b/c" costs a block read
 * per component and doing that per *entry* is a hidden factor of the path
 * depth on the exact workload M81 exists for. So SYS_getdents opens once
 * and walks; `handle` is an inode index, the same thing leanfs_open has
 * returned since M59, and is re-validated on every call. */
int leanfs_dir_open(const char *path);
int leanfs_readdir_at(int handle, uint32_t *cookie, leanfs_dir_entry_t *out);

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

/* M59: the same gap unlink closed for files, left open there because
 * nothing had asked. A file manager that can delete a file but not the
 * folder it sits in is visibly half-finished.
 *
 * Empty directories only, and that is the whole design: recursive delete
 * is one keystroke away from losing everything under a path, and this
 * project has no trash to take it back out of. Refuses the root, which
 * has no parent to be removed from. Returns 0 or -1. */
int leanfs_rmdir(const char *path);

/* M59: what a caller needs to know about a path without reading it -
 * which the file manager's size and date columns are made of, and which
 * every "how big a buffer do I need" caller in this kernel was
 * previously answering with LEANFS_MAX_FILE_SIZE. */
typedef struct {
    uint32_t size;
    uint32_t mtime;  /* seconds since 1970, or 0 - see the inode's own note */
    uint8_t is_dir;
    /* M87: set only by leanfs_lstat, which is the only call that can see
     * a link - every other path operation follows one, so by the time
     * they answer there is nothing left to report. A caller that wants
     * to know whether a name IS a link rather than what it points at has
     * to ask the question that does not follow. */
    uint8_t is_link;
} leanfs_stat_t;

int leanfs_stat(const char *path, leanfs_stat_t *out);

/* ---- M87: symbolic links ----------------------------------------------
 *
 * A link's target lives in its data blocks the way a file's contents do,
 * which is why this needed no format change beyond a type value.
 *
 * leanfs_symlink creates one; the name must not already exist. readlink
 * and lstat are the two calls that do NOT follow a final link, because
 * they are about the link rather than about what it points at - that
 * distinction is the whole reason a program can tell the two apart.
 *
 * Every other path operation follows, with a hop limit: a chain longer
 * than LEANFS_MAX_LINK_HOPS, or a link pointing at itself, is refused
 * rather than walked further. */
int leanfs_symlink(const char *path, const char *target);
int64_t leanfs_readlink(const char *path, char *buf, size_t maxlen);
int leanfs_lstat(const char *path, leanfs_stat_t *out);

/* M77: the same three fields, for an already-open handle. A handle IS an
 * inode index here, so this is the path-resolution step of leanfs_stat
 * with the resolution already done - which is exactly why SYS_fstat can
 * answer for a file whose name has since changed. */
int leanfs_handle_stat(int handle, leanfs_stat_t *out);

/* ---- M59: descriptors ------------------------------------------------
 *
 * SYS_readfile's own comment has said "no open/close/fd-table/lseek yet"
 * since M13 and scoped that to what M13 needed. Six arcs later it was the
 * limit three apps apologised for in three different ways.
 *
 * The kernel-side shape of the fix is deliberately small: an *inode
 * handle* (an index into the inode table) plus byte-range read and write.
 * Everything else a descriptor is - a current offset, whether it may be
 * written, who holds it - belongs to the fd table in kernel/sched/sched.h,
 * because that is where the rest of this kernel's descriptors already
 * live. This file's job is the filesystem, not the process.
 *
 * leanfs_open resolves a path to a handle, creating an empty regular file
 * if `create` is set and nothing is there. Returns -1 for a malformed
 * path, a missing parent, a directory, or no free inode.
 *
 * M87: `create` is a small flag word rather than a boolean.
 * LEANFS_OPEN_EXCL means the caller wants to be the one who created the
 * file - an existing file is a failure rather than something to open.
 * That is what makes a lock file a lock, and the atomicity comes from
 * fs_lock rather than from anything here: the existence check and the
 * creation happen inside one critical section. */
#define LEANFS_OPEN_CREATE 1
#define LEANFS_OPEN_EXCL   2
int leanfs_open(const char *path, int create);

/* Byte-range read/write against an open handle. pwrite grows the file as
 * needed, up to LEANFS_MAX_FILE_SIZE, and updates its mtime. Both return
 * the number of bytes transferred, or -1. A read past the end returns 0. */
int64_t leanfs_handle_read(int handle, void *buf, size_t len, uint32_t off);
int64_t leanfs_handle_write(int handle, const void *buf, size_t len, uint32_t off);

/* The handle's current size - what SYS_lseek needs to resolve a seek
 * relative to the end. Returns 0 for an invalid handle, which is the same
 * answer an empty file gives; a caller that needs to tell them apart
 * checked when it opened. */
uint32_t leanfs_handle_size(int handle);

/* Drops a handle's contents back to zero bytes, freeing every block.
 * What opening for writing does to an existing file - the whole-file
 * overwrite semantics leanfs_write has always had, now reachable without
 * having the whole file in memory. */
int leanfs_handle_truncate(int handle);

/* M87: truncate to any length. Shrinking frees the blocks past the new
 * end; growing only changes the size, because an unallocated block
 * already reads as zeros - so a file extended this way is reserved
 * rather than allocated, which is what ftruncate promises. */
int leanfs_handle_truncate_to(int handle, uint32_t len);

/* M59: how many metadata sectors this filesystem has written since boot.
 * Exists so a test can assert what a one-byte save *costs* - the number
 * of PIO sector writes - rather than how long it took, which is a
 * property of the host and not of this code. See save_meta. */
uint32_t leanfs_meta_writes(void);

/* M71: rebuild the free-block bitmap from the inodes and reclaim anything
 * nothing points at. Called automatically on mount when the superblock
 * says the filesystem was never unmounted; exposed so a self-test can
 * drive it directly. See leanfs.c for what it does and does not check. */
void leanfs_check(void);

/* M71: mark this filesystem cleanly unmounted. Everything leanfs writes
 * is already write-through, so this flushes nothing - what it does is
 * record that the machine got here on purpose, which is the only way a
 * filesystem with no journal can tell a shutdown from a power cut. */
void leanfs_sync(void);

/* M71: rename that is allowed to replace an existing destination.
 *
 * leanfs_rename refuses one, and M56 was right about why: silently
 * replacing a file is a way to lose one. But that makes the
 * write-to-a-temp-then-rename dance impossible, and that dance is the
 * only way to replace a file's contents without a window in which
 * neither version exists. So the safe default keeps the refusal and the
 * save path gets this. */
int leanfs_rename_replace(const char *old_path, const char *new_path);

/* M71: a test hook - drop a file's directory entry and inode while
 * leaving its blocks marked used, which is exactly the leak a crash
 * between allocating blocks and recording them produces. See leanfs.c. */
void leanfs_debug_orphan(const char *path);
