#pragma once

#include <stdint.h>

int virtio_block_device_init(void);


int virtio_block_device_read(uint64_t lba, uint32_t count, void *buf);
int virtio_block_device_write(uint64_t lba, uint32_t count, const void *buf);

