/* user_space/libc/include/mntent.h - M89
 *
 * What is mounted, read as a file.
 *
 * This is the /etc/fstab and /proc/mounts parser every Unix ships, and
 * it is here because toybox's portability layer reaches for it to answer
 * "what filesystems are there" - `df` and `mount` are both built on it.
 *
 * The file it reads is `/proc/mounts`, which M89 also added: the kernel
 * has had a real mount table since M87 and no way to show it. So this
 * header is not a shim over a fiction - a program that opens this and
 * walks it sees leanfs at /, devfs at /dev and procfs at /proc, which is
 * exactly what is mounted.
 *
 * The two fields with nothing behind them are `mnt_freq` and
 * `mnt_passno`, both 0. They are dump-frequency and fsck-order, from a
 * 1980s /etc/fstab, and 0 is what they mean on a machine that dumps
 * nothing and checks nothing at boot - a true answer rather than a
 * placeholder.
 */
#pragma once

#include <stdio.h>

#define MOUNTED "/proc/mounts"
#define MNTTAB  "/proc/mounts"

struct mntent {
    char *mnt_fsname; /* the filesystem's own name - there is no block device here */
    char *mnt_dir;    /* where it is mounted */
    char *mnt_type;   /* leanfs / devfs / procfs */
    char *mnt_opts;   /* "rw" */
    int   mnt_freq;   /* 0 - see the header note */
    int   mnt_passno; /* 0 - same */
};

/* `mode` is accepted and ignored beyond its first character: this is a
 * read-only view of what the kernel says is mounted, so opening it "w"
 * to add a line has nothing to write to. setmntent returns NULL for a
 * write mode rather than a stream that silently discards. */
FILE *setmntent(const char *path, const char *mode);

/* The next line, or NULL at the end. The returned pointer and every
 * string in it belong to this library and are overwritten by the next
 * call - the standard contract, and the reason a caller that keeps a
 * field copies it. */
struct mntent *getmntent(FILE *f);

/* The reentrant form, which writes its strings into `buf`. */
struct mntent *getmntent_r(FILE *f, struct mntent *out, char *buf, int buflen);

int endmntent(FILE *f);

/* Looks up an option in an mnt_opts string: returns a pointer to it, or
 * NULL. Every mount here is "rw", so the only two answers this can give
 * are "rw" and NULL. */
char *hasmntopt(const struct mntent *me, const char *opt);
