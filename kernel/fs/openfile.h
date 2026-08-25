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

typedef struct openfile {
    int handle;        /* leanfs inode handle, -1 when this entry is free */
    uint32_t offset;   /* the shared file position - the whole reason this is not in fd_slot_t */
    uint8_t writable;
    int refcount;      /* how many fd-table slots, across all tasks, point here */
} openfile_t;

/* Claims a free entry for `handle`, refcount 1. NULL if the table is
 * full. */
openfile_t *openfile_alloc(int handle, int writable);

/* One more descriptor now names this file (SYS_dup2, and a spawn
 * inheriting its parent's table). */
void openfile_ref(openfile_t *f);

/* One fewer. Frees the entry at zero. */
void openfile_unref(openfile_t *f);

/* How many entries are in use - for the boot self-test that asserts a
 * process's files are given back when it exits. */
int openfile_in_use(void);
