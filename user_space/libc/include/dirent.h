#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAME_MAX 255

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
#define DT_LNK     10
#define DT_CHR     2
#define DT_BLK     6
#define DT_FIFO    1
#define DT_SOCK    12

struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[NAME_MAX + 1];
};

typedef struct DIR DIR;

DIR *opendir(const char *path);

struct dirent *readdir(DIR *d);

void rewinddir(DIR *d);
int closedir(DIR *d);

DIR *fdopendir(int fd);
int dirfd(DIR *d);

int scandir(const char *path, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **));

int alphasort(const struct dirent **a, const struct dirent **b);

#ifdef __cplusplus
}
#endif
