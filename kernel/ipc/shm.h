#pragma once

#include <stddef.h>
#include <stdint.h>

#define SHM_MAX_SEGMENT_BYTES (256ULL * 1024 * 1024)

int shm_create(size_t size, int owner_task_id);

void shm_free_by_owner(int owner_task_id);

int shm_free(int id, int owner_task_id);

int64_t shm_page_count(int id);

int shm_count_by_owner(int owner_task_id);

int64_t shm_get_size(int id);

int shm_map_into(int id, uint64_t pml4_phys, uint64_t vaddr, uint64_t flags);
