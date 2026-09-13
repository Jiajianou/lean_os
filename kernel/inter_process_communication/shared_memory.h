#pragma once

#include <stddef.h>
#include <stdint.h>

#define SHARED_MEMORY_MAX_SEGMENT_BYTES (256ULL * 1024 * 1024)

int shared_memory_create(size_t size, int owner_task_id);

void shared_memory_free_by_owner(int owner_task_id);

int shared_memory_free(int id, int owner_task_id);

int64_t shared_memory_page_count(int id);

int shared_memory_count_by_owner(int owner_task_id);

int64_t shared_memory_get_size(int id);

int shared_memory_map_into(int id, uint64_t pml4_phys, uint64_t vaddr, uint64_t flags);
