#pragma once

#include <stdint.h>

int usb_storage_init(void);

int usb_storage_present(void);

uint64_t usb_storage_sector_count(void);

int usb_storage_read(uint64_t lba, uint32_t count, void *buffer);

int usb_storage_write(uint64_t lba, uint32_t count, const void *buffer);
