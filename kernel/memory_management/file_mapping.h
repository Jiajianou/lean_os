#pragma once

#include <stdint.h>

#define FILE_MAPPING_MAX_PAGES 8192

uint64_t file_mapping_get(int handle, uint32_t index, int writable);

void file_mapping_put(int handle, uint32_t index);

void file_mapping_sync(int handle);

int file_mapping_in_use(void);
