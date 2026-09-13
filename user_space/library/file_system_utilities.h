#pragma once

#include <stdint.h>

#include "syscall.h"

#define FILE_SYSTEM_UTILITIES_SIZE_MAX  12
#define FILE_SYSTEM_UTILITIES_EXACT_MAX 16
#define FILE_SYSTEM_UTILITIES_DATE_MAX  12

void file_system_utilities_format_size(uint32_t bytes, char *out);

void file_system_utilities_format_exact(uint32_t n, char *out);

void file_system_utilities_format_date(uint32_t mtime, char *out);

int file_system_utilities_name_ok(const char *name);

#define FILE_SYSTEM_UTILITIES_MAX_DEPTH 16

#define FILE_SYSTEM_UTILITIES_DIRENT_BUFFER 1024

typedef struct {
    uint32_t entries;
    uint32_t bytes;
    int      deep;
} file_system_utilities_tree_t;

int file_system_utilities_count_tree(const char *path, file_system_utilities_tree_t *out);

int file_system_utilities_remove_tree(const char *path);
