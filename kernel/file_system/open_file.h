#pragma once

#include <stdint.h>

/* Open files, for the whole machine.
 
   64 until M183, and a number smaller than ONE process's descriptor table is
   a number a single program can starve the machine with - which is what a
   browser did. What it looked like from outside was not "out of files": it
   was Chromium's launcher opening /dev/null in a forked child, getting
   nothing and _exit(127)ing, so every renderer died at birth; and the
   browser's font service failing to open DejaVuSans.ttf, so Blink found no
   font for a run of text and crashed in the diagnostic it calls before
   giving up. Two unrelated-looking deaths, one table.
 
   512 is four full descriptor tables (MAX_FILE_DESCRIPTORS is 128), which is
   the browser's four processes with the desktop's beside them, and it costs
   139 KB. The condition for raising it again is a program that reaches it
   and is not leaking - which is a question [m183] now answers on every boot
   rather than one that has to be guessed at. */
#define MAX_OPEN_FILES 512

#define OPEN_FILE_PATH_MAX 256

typedef struct open_file {
    int handle;
    uint32_t offset;
    uint8_t writable;
    int refcount;
    char path[OPEN_FILE_PATH_MAX];
    uint8_t is_directory;
} open_file_t;

open_file_t *open_file_alloc(int handle, int writable, const char *path, int is_directory);

void open_file_reference(open_file_t *f);

void open_file_unref(open_file_t *f);

int open_file_in_use(void);

/* How many opens this machine has refused because the table above was full.
   Nothing above open(2) can tell that apart from a file that is not there,
   so the count is the only way to ask. */
int open_file_exhaustion_count(void);
