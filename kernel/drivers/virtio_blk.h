#pragma once

#include <stdint.h>

int virtio_blk_init(void);


int virtio_blk_read(uint64_t lba, uint32_t count, void *buf);
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buf);

