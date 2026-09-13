#pragma once

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

int ata_read_sectors(uint32_t lba, uint8_t count, void *buf);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);

