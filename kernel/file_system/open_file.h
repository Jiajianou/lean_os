#pragma once

#include <stdint.h>

#define MAX_OPEN_FILES 64

#define OPENFILE_PATH_MAX 256

typedef struct openfile {
    int handle;
    uint32_t offset;
    uint8_t writable;
    int refcount;
    char path[OPENFILE_PATH_MAX];
    uint8_t is_dir;
} openfile_t;

openfile_t *openfile_alloc(int handle, int writable, const char *path, int is_dir);

void openfile_ref(openfile_t *f);

void openfile_unref(openfile_t *f);

int openfile_in_use(void);
