#pragma once

#include <stdint.h>

/* Pages of files mapped MAP_SHARED, for the whole machine. 8192 - 32 MB - until
   M187: Chromium maps resources.pak, 20 MB, MAP_SHARED in every one of its
   processes, and a page is kept per open handle, so two processes filled the
   table and every page fault in a mapped file after that was reported as out
   of memory and killed the process that took it. 131072 is 512 MB of mapped
   file, in a hash table from the heap rather than an array searched end to
   end on every fault. */
#define FILE_MAPPING_MAX_PAGES 131072

uint64_t file_mapping_get(int handle, uint32_t index, int writable);

void file_mapping_put(int handle, uint32_t index);

void file_mapping_sync(int handle);

int file_mapping_in_use(void);
