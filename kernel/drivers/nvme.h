#pragma once

#include <stdint.h>

int nvme_init(void);

uint64_t nvme_capacity(void);

int nvme_read(uint64_t lba, uint32_t count, void *buf);
int nvme_write(uint64_t lba, uint32_t count, const void *buf);

uint32_t nvme_error_count(void);
