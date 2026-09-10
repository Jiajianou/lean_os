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

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

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

/* ---- M100: scandir, named by NetSurf's file: fetcher -----------------
 *
 * The whole directory, filtered and sorted, in one call, with each entry
 * in its own allocation. NetSurf builds the HTML index page for a
 * `file:///` directory out of it (content/fetchers/file/file.c), which
 * is how a browser shows you a folder.
 *
 * The awkward part of the interface is the ownership and it is worth
 * stating rather than discovering: on success the caller owns `*namelist`
 * AND every pointer in it, and frees all of them. On failure it owns
 * nothing - which is the clause that makes the implementation's error
 * path the interesting half of the function.
 *
 * The comparison function is `int (*)(const struct dirent **, const
 * struct dirent **)`. That is POSIX's signature since 2008 and it is
 * NOT what qsort takes, so the sort here cannot be a bare qsort call
 * without a cast that lies about the argument types. See dirent.c. */
int scandir(const char *path, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **));

/* Name order, via strcoll - which in this one-locale libc is strcmp.
 * `versionsort` is its usual companion and is NOT here: nothing has
 * asked for it, and the rule this project keeps is that the port which
 * needs a thing is the one that pays for it. */
int alphasort(const struct dirent **a, const struct dirent **b);

#ifdef __cplusplus
}
#endif
