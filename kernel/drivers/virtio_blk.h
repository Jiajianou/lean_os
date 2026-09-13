#pragma once

#include <stdint.h>

int virtio_blk_init(void);

uint64_t virtio_blk_capacity(void);

int virtio_blk_read(uint64_t lba, uint32_t count, void *buf);
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buf);

uint32_t virtio_blk_error_count(void);
