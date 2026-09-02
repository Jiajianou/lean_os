/* user_space/libc/include/dirent.h - M77, rewritten in M81
 *
 * opendir/readdir/closedir over SYS_getdents (M81), which returns real
 * records: an inode number, a type, and a name. SYS_listdir (M53) is
 * still there and still returns newline-separated names with a '/' on
 * the directories - a shape built for a person reading a terminal, not
 * for a program walking a tree. This is the difference between the two.
 *
 * M77 fetched the whole listing in opendir and walked it in readdir, and
 * defended that as "a real design choice and not a shortcut" because "a
 * directory here holds at most LEANFS_MAX_DIRENTS entries, which is
 * kilobytes". M81 made that two megabytes - 8192 entries of up to 255
 * bytes - so the condition the defence rested on is gone, and with it the
 * design. This now streams through SYS_getdents a bufferful at a time,
 * which is the interface M77 said it would build "for a size this
 * filesystem cannot reach" on the day the filesystem could reach it.
 */
#pragma once

#include <sys/types.h>

/* M81: exactly leanfs's own limit, which is exactly the number every
 * program that walks a tree already assumes. This used to say "longer
 * than leanfs will ever produce (its own limit is 27)" - one of the two
 * numbers was a lie, and M81 decided which by moving the filesystem. */
#define NAME_MAX 255

/* d_type, as every program that avoids a stat per entry uses it. DT_DIR
 * and DT_REG are both genuinely known here - SYS_listdir's trailing '/'
 * is exactly this information - so this is not a field that always says
 * DT_UNKNOWN. */
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
/* M87 gave leanfs symbolic links and SYS_getdents reports them, so this
 * is a value a real entry can hold rather than one defined for
 * completeness. The three below are not: leanfs has no device nodes,
 * FIFOs or sockets, and nothing will ever set them. They are here so a
 * program that switches on d_type compiles. */
#define DT_LNK     10
#define DT_CHR     2
#define DT_BLK     6
#define DT_FIFO    1
#define DT_SOCK    12

struct dirent {
    /* M81: a real inode number.
     *
     * This field was documented as "always 0" for four milestones, with
     * the honest reason that leanfs had inode numbers and SYS_listdir
     * reported names. SYS_getdents reports both, so the apology is
     * retired: two entries with the same d_ino are the same file, and a
     * program that uses this to break a cycle or spot a repeat can now
     * do so without a stat per entry.
     *
     * Still no hard links in leanfs, so in practice two live names never
     * share one - which makes this useful for identity rather than for
     * counting. */
    ino_t d_ino;
    unsigned char d_type;
    char d_name[NAME_MAX + 1];
};

typedef struct DIR DIR;

/* NULL if `path` is not a directory, or if there is no memory for the
 * listing. */
DIR *opendir(const char *path);

/* The next entry, or NULL at the end. The returned pointer belongs to
 * the DIR and is overwritten by the next call - the standard contract,
 * and the reason a caller that keeps a name copies it. */
struct dirent *readdir(DIR *d);

void rewinddir(DIR *d);
int closedir(DIR *d);

/* M89: a stream over an already-open directory descriptor, which the
 * *at() family's users want so that one open serves both the walk and
 * the path resolution. The DIR takes ownership: closedir closes `fd`.
 *
 * `dirfd` reports that descriptor, and reports -1 for a stream opendir
 * made - which holds a path rather than a descriptor here. See
 * dirent.c. */
DIR *fdopendir(int fd);
int dirfd(DIR *d);
