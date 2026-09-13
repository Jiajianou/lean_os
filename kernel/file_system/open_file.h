#pragma once

#include <stdint.h>

#define MAX_OPEN_FILES 64

#define OPENFILE_PATH_MAX 256

typedef struct open_file {
    int handle;
    uint32_t offset;
    uint8_t writable;
    int refcount;
    char path[OPENFILE_PATH_MAX];
    uint8_t is_dir;
} open_file_t;

open_file_t *open_file_alloc(int handle, int writable, const char *path, int is_dir);

void open_file_reference(open_file_t *f);

void open_file_unref(open_file_t *f);

int open_file_in_use(void);
