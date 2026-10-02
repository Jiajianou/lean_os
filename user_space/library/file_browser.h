#pragma once

#include <stddef.h>
#include <stdint.h>

#include "os_time.h"
#include "paths.h"

#define FILE_BROWSER_NAME_MAX 256
#define FILE_BROWSER_DEPTH_MAX 16

#define FILE_BROWSER_TRASH          PATH_HOME_DIRECTORY ".Trash"
#define FILE_BROWSER_TRASH_ORIGINS  PATH_HOME_DIRECTORY ".Trash/.origins"
#define FILE_BROWSER_ORIGINS_NAME   ".origins"

#define FILE_BROWSER_OK                  0
#define FILE_BROWSER_ERROR_TOO_LONG     -1
#define FILE_BROWSER_ERROR_INTO_ITSELF  -2
#define FILE_BROWSER_ERROR_EXISTS       -3
#define FILE_BROWSER_ERROR_READ         -4
#define FILE_BROWSER_ERROR_WRITE        -5
#define FILE_BROWSER_ERROR_NOT_FOUND    -6
#define FILE_BROWSER_ERROR_IN_TRASH     -7
#define FILE_BROWSER_ERROR_NO_ORIGIN    -8
#define FILE_BROWSER_ERROR_ORIGIN_GONE  -9
#define FILE_BROWSER_ERROR_TOO_DEEP    -10
#define FILE_BROWSER_ERROR_RENAME      -11
#define FILE_BROWSER_ERROR_DELETE      -12
#define FILE_BROWSER_ERROR_NAME        -13
#define FILE_BROWSER_ERROR_SAME        -14

typedef enum {
    FILE_BROWSER_KIND_FOLDER = 0,
    FILE_BROWSER_KIND_PROGRAM,
    FILE_BROWSER_KIND_TEXT,
    FILE_BROWSER_KIND_SOURCE,
    FILE_BROWSER_KIND_IMAGE,
    FILE_BROWSER_KIND_ARCHIVE,
    FILE_BROWSER_KIND_FONT,
    FILE_BROWSER_KIND_DOCUMENT,
    FILE_BROWSER_KIND_LINK,
    FILE_BROWSER_KIND_DATA,
    FILE_BROWSER_KIND_COUNT,
} file_browser_kind_t;

typedef enum {
    FILE_BROWSER_PREVIEW_NONE = 0,
    FILE_BROWSER_PREVIEW_TEXT,
    FILE_BROWSER_PREVIEW_IMAGE,
    FILE_BROWSER_PREVIEW_BYTES,
} file_browser_preview_t;

typedef enum {
    FILE_BROWSER_SORT_NAME = 0,
    FILE_BROWSER_SORT_DATE,
    FILE_BROWSER_SORT_SIZE,
    FILE_BROWSER_SORT_KIND,
    FILE_BROWSER_SORT_COUNT,
} file_browser_sort_t;

typedef struct {
    char name[FILE_BROWSER_NAME_MAX];
    const char *kind_name;
    uint32_t size;
    uint32_t mtime;
    uint8_t is_directory;
    uint8_t is_link;
    uint8_t kind;
} file_browser_entry_t;

const char *file_browser_error_message(int error);

int file_browser_join(const char *directory, const char *name, char *out, size_t capacity);

const char *file_browser_basename(const char *path);

int file_browser_parent(const char *path, char *out, size_t capacity);

int file_browser_is_inside(const char *path, const char *ancestor);

int file_browser_compare_names(const char *a, const char *b);

int file_browser_name_matches(const char *name, const char *query);

int file_browser_looks_like_text(const unsigned char *bytes, size_t length);

file_browser_kind_t file_browser_classify(const char *name, int is_directory, int is_link,
                                          const unsigned char *head, size_t head_length,
                                          const char **kind_name);

file_browser_preview_t file_browser_preview_kind(const unsigned char *head, size_t head_length);

int file_browser_exists(const char *path);

int file_browser_available_name(const char *directory, const char *wanted, char *out,
                                size_t capacity);

int file_browser_copy_name(const char *directory, const char *source_name, char *out,
                           size_t capacity);

#define FILE_BROWSER_LIST_HIDDEN 1
#define FILE_BROWSER_LIST_SNIFF  2

int file_browser_list(const char *directory, int flags, file_browser_entry_t *out, int maximum);

int file_browser_needs_sniff(const file_browser_entry_t *entry);

void file_browser_sniff_entry(const char *path, file_browser_entry_t *entry);

void file_browser_sort(const file_browser_entry_t *entries, int *order, int count,
                       file_browser_sort_t key, int descending);

int file_browser_copy(const char *from, const char *to);

int file_browser_copy_into(const char *from, const char *directory, char *made,
                           size_t capacity);

int file_browser_move_into(const char *from, const char *directory, char *moved,
                           size_t capacity);

int file_browser_delete(const char *path);

int file_browser_trash(const char *path, char *trashed, size_t capacity);

int file_browser_trash_origin(const char *trashed_path, char *out, size_t capacity);

int file_browser_put_back(const char *trashed_path, char *restored, size_t capacity);

int file_browser_empty_trash(void);

typedef int (*file_browser_found_t)(void *context, const char *path, const os_stat_t *status);

long file_browser_search(const char *root, const char *query, int include_hidden,
                         long visit_budget, file_browser_found_t found, void *context);
