#pragma once

#include <stdint.h>

#define FILEMAP_MAX_PAGES 8192

uint64_t filemap_get(int handle, uint32_t index, int writable);

void filemap_put(int handle, uint32_t index);

void filemap_sync(int handle);

int filemap_in_use(void);
