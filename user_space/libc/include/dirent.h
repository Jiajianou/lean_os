/* user_space/libc/include/dirent.h - M77
 *
 * opendir/readdir/closedir over SYS_listdir (M53), which returns
 * newline-separated names with a '/' appended to each one that is itself
 * a directory - a shape built for a person reading a terminal, not for a
 * program walking a tree. This is the difference between the two.
 *
 * The whole listing is fetched by opendir and readdir walks it. That is a
 * real design choice and not a shortcut: SYS_listdir is whole-shot with
 * no iterator and no handle, so there is nothing to stream - and a
 * directory here holds at most LEANFS_MAX_DIRENTS entries, which is
 * kilobytes. A streaming readdir would be a second kernel interface
 * built for a size this filesystem cannot reach.
 */
#pragma once

#include <sys/types.h>

/* Longer than leanfs will ever produce (its own limit is 27), sized to
 * the number every program that walks a tree already assumes. */
#define NAME_MAX 255

/* d_type, as every program that avoids a stat per entry uses it. DT_DIR
 * and DT_REG are both genuinely known here - SYS_listdir's trailing '/'
 * is exactly this information - so this is not a field that always says
 * DT_UNKNOWN. */
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

struct dirent {
    /* Always 0. leanfs has inode numbers, and SYS_listdir does not
     * report them - it reports names. Present because a program that
     * reads the field compiles, zero because zero is the conventional
     * "this filesystem is not telling you", and a fabricated number
     * would be worse than an honest one. A program that needs identity
     * calls stat. */
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
