/* kernel/fs/openfile.h
 *
 * M59: the kernel's shared table of open files - what an FD_FILE
 * descriptor points at.
 *
 * Why a shared table rather than putting the inode and offset straight
 * into fd_slot_t: SYS_dup2 exists, and two descriptors made by dup2 have
 * to share one file position. Writing through both otherwise interleaves
 * silently, which is the exact bug shell redirection would hit first.
 * This is the same open-file-description split every Unix has, kept to
 * the three fields this OS can actually justify.
 *
 * Small and fixed, like every other table in this kernel. `refcount` is
 * what makes "the last fd naming this file went away" knowable - the same
 * thing M59 gave pipes, for the same reason.
 */
#pragma once

#include <stdint.h>

/* Deliberately modest. Every process here holds a handful of files at
 * most, and a table that ran out would be reported (SYS_open returns -1)
 * rather than silently reused. */
#define MAX_OPEN_FILES 64

/* ---- M89: the path an open file was opened with ----------------------
 *
 * The *at() family resolves a relative name against a directory named by
 * a descriptor, and nothing in this kernel could answer "which directory
 * is that" - a descriptor pointed at a leanfs inode handle and an
 * offset, and an inode has no name. So the open path is recorded here,
 * already absolute and normalized by copy_path_from_user, and SYS_fdpath
 * hands it back.
 *
 * **256 rather than LEANFS_MAX_PATH.** A full 4096 here would be 256 KiB
 * of kernel data for a table of 64 entries, to hold paths that in a real
 * source tree run to sixty or eighty characters. A path longer than this
 * is stored truncated-to-nothing rather than truncated-to-wrong: the
 * first byte is set to 0 and SYS_fdpath reports failure, so an *at()
 * call against such a descriptor gets ENAMETOOLONG instead of resolving
 * against a shorter directory that also exists. A wrong directory is the
 * one outcome this must not have.
 *
 * **And what it does not give.** This is a name, not a reference. If the
 * directory is renamed after being opened, the stored path names
 * whatever is at the old location now. See <fcntl.h>'s note for why that
 * is the right trade on a machine with one principal and what would have
 * to change for it not to be. */
#define OPENFILE_PATH_MAX 256

typedef struct openfile {
    int handle;        /* leanfs inode handle, -1 when this entry is free */
    uint32_t offset;   /* the shared file position - the whole reason this is not in fd_slot_t */
    uint8_t writable;
    int refcount;      /* how many fd-table slots, across all tasks, point here */
    char path[OPENFILE_PATH_MAX]; /* absolute and normalized; "" if it did not fit - M89 */
} openfile_t;

/* Claims a free entry for `handle`, refcount 1. NULL if the table is
 * full. `path` is the absolute path it was opened with and may be NULL
 * for a file that has none. */
openfile_t *openfile_alloc(int handle, int writable, const char *path);

/* One more descriptor now names this file (SYS_dup2, and a spawn
 * inheriting its parent's table). */
void openfile_ref(openfile_t *f);

/* One fewer. Frees the entry at zero. */
void openfile_unref(openfile_t *f);

/* How many entries are in use - for the boot self-test that asserts a
 * process's files are given back when it exits. */
int openfile_in_use(void);
