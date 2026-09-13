#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOUNTED "/proc/mounts"
#define MNTTAB  "/proc/mounts"

struct mntent {
    char *mnt_fsname;
    char *mnt_dir;
    char *mnt_type;
    char *mnt_opts;
    int   mnt_freq;
    int   mnt_passno;
};

FILE *setmntent(const char *path, const char *mode);

struct mntent *getmntent(FILE *f);

struct mntent *getmntent_r(FILE *f, struct mntent *out, char *buf, int buflen);

int endmntent(FILE *f);

char *hasmntopt(const struct mntent *me, const char *opt);

#ifdef __cplusplus
}
#endif
