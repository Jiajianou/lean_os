#include "drivers/ata.h"
#include "drivers/ahci.h"
#include "drivers/nvme.h"
#include "drivers/virtio_block.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void panic(const char *message);

void fake_backend_reset(uint32_t sectors);
void fake_backend_reset_counters(void);
uint64_t fake_backend_read_calls(void);
uint64_t fake_backend_write_calls(void);
uint8_t *fake_backend_sector(uint32_t lba);

static uint8_t *disk;
static uint32_t disk_sectors;
static uint64_t read_calls;
static uint64_t write_calls;

void fake_backend_reset(uint32_t sectors) {
    free(disk);
    disk = (uint8_t *)calloc(sectors, ATA_SECTOR_SIZE);
    if (!disk) {
        panic("fake_block_backends: could not allocate the RAM disk");
    }
    disk_sectors = sectors;
    read_calls = 0;
    write_calls = 0;
}

void fake_backend_reset_counters(void) {
    read_calls = 0;
    write_calls = 0;
}

uint64_t fake_backend_read_calls(void) {
    return read_calls;
}

uint64_t fake_backend_write_calls(void) {
    return write_calls;
}

uint8_t *fake_backend_sector(uint32_t lba) {
    if (lba >= disk_sectors) {
        panic("fake_block_backends: sector out of range");
    }
    return disk + (uint64_t)lba * ATA_SECTOR_SIZE;
}

int nvme_init(void) {
    return 0;
}

int ahci_init(void) {
    return 0;
}

int virtio_block_device_init(void) {
    return 0;
}

int nvme_read(uint64_t lba, uint32_t count, void *buffer) {
    (void)lba;
    (void)count;
    (void)buffer;
    return -1;
}

int nvme_write(uint64_t lba, uint32_t count, const void *buffer) {
    (void)lba;
    (void)count;
    (void)buffer;
    return -1;
}

int ahci_read(uint64_t lba, uint32_t count, void *buffer) {
    (void)lba;
    (void)count;
    (void)buffer;
    return -1;
}

int ahci_write(uint64_t lba, uint32_t count, const void *buffer) {
    (void)lba;
    (void)count;
    (void)buffer;
    return -1;
}

int virtio_block_device_read(uint64_t lba, uint32_t count, void *buffer) {
    (void)lba;
    (void)count;
    (void)buffer;
    return -1;
}

int virtio_block_device_write(uint64_t lba, uint32_t count, const void *buffer) {
    (void)lba;
    (void)count;
    (void)buffer;
    return -1;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer) {
    if ((uint64_t)lba + count > disk_sectors) {
        return -1;
    }
    read_calls++;
    memcpy(buffer, disk + (uint64_t)lba * ATA_SECTOR_SIZE,
           (uint64_t)count * ATA_SECTOR_SIZE);
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    if ((uint64_t)lba + count > disk_sectors) {
        return -1;
    }
    write_calls++;
    memcpy(disk + (uint64_t)lba * ATA_SECTOR_SIZE, buffer,
           (uint64_t)count * ATA_SECTOR_SIZE);
    return 0;
}
