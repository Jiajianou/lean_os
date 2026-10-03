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
 
   512 was four full descriptor tables when a table was 128. M187 made a
   table 1024, because Chromium's browser process alone needed more than
   128, and a machine-wide table smaller than one process's is the starvation
   M183 wrote down - so this is four full tables again, 4096, at about 1.1 MB.
   The condition for raising it again is unchanged: a program that reaches it
   and is not leaking, which [m183] answers on every boot. */
#define MAX_OPEN_FILES 4096

#define OPEN_FILE_PATH_MAX 256

typedef struct open_file {
    int handle;
    uint32_t offset;
    uint8_t writable;
    int refcount;
    char path[OPEN_FILE_PATH_MAX];
    uint8_t is_directory;
    uint8_t synchronous;
} open_file_t;

open_file_t *open_file_alloc(int handle, int writable, const char *path, int is_directory);

void open_file_reference(open_file_t *f);

void open_file_unref(open_file_t *f);

int open_file_in_use(void);

/* How many opens this machine has refused because the table above was full.
   Nothing above open(2) can tell that apart from a file that is not there,
   so the count is the only way to ask. */
int open_file_exhaustion_count(void);
