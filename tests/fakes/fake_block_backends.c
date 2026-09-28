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

/* M186: the block layer now asks each disk whose boot sector it carries, so
   the fake has to be able to be more than one disk. Which of them answers,
   and which of them is this OS's, is what a selection test varies. */
static int nvme_present;
static int usb_present;
static uint8_t usb_disk[8 * ATA_SECTOR_SIZE];
static uint64_t usb_sectors;

void fake_backend_present(int nvme, int usb);
void fake_backend_label_usb(int labelled);
void fake_backend_label_primary(int labelled);

static void write_label(uint8_t *sector, int labelled) {
    static const char label[8] = {'L', 'E', 'A', 'N', '_', 'O', 'S', '1'};
    for (int i = 0; i < 8; i++) {
        sector[0x1A0 + i] = labelled ? (uint8_t)label[i] : 0;
    }
    sector[510] = labelled ? 0x55 : 0;
    sector[511] = labelled ? 0xAA : 0;
}

void fake_backend_present(int nvme, int usb) {
    nvme_present = nvme;
    usb_present = usb;
    usb_sectors = sizeof(usb_disk) / ATA_SECTOR_SIZE;
    memset(usb_disk, 0, sizeof(usb_disk));
}

void fake_backend_label_usb(int labelled) {
    write_label(usb_disk, labelled);
}

void fake_backend_label_primary(int labelled) {
    if (disk && disk_sectors > 0) {
        write_label(disk, labelled);
    }
}

int usb_storage_init(void) {
    return usb_present;
}

int usb_storage_present(void) {
    return usb_present;
}

uint64_t usb_storage_sector_count(void) {
    return usb_present ? usb_sectors : 0;
}

int usb_storage_read(uint64_t lba, uint32_t count, void *buffer) {
    if (!usb_present || lba + count > usb_sectors) {
        return 0;
    }
    memcpy(buffer, usb_disk + lba * ATA_SECTOR_SIZE, (size_t)count * ATA_SECTOR_SIZE);
    return 1;
}

int usb_storage_write(uint64_t lba, uint32_t count, const void *buffer) {
    if (!usb_present || lba + count > usb_sectors) {
        return 0;
    }
    memcpy(usb_disk + lba * ATA_SECTOR_SIZE, buffer, (size_t)count * ATA_SECTOR_SIZE);
    return 1;
}

int nvme_init(void) {
    return nvme_present;
}

int ahci_init(void) {
    return 0;
}

int virtio_block_device_init(void) {
    return 0;
}

int nvme_read(uint64_t lba, uint32_t count, void *buffer) {
    if (nvme_present && disk && lba + count <= disk_sectors) {
        memcpy(buffer, disk + lba * ATA_SECTOR_SIZE, (size_t)count * ATA_SECTOR_SIZE);
        return 0;
    }
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

/* M194: a power cut, sector-granular. After the budget runs out a write keeps
   only the sectors that fit and fails, and every write after it is lost -
   which is what a disk does when the power goes in the middle of a command. */
static int64_t cut_budget = -1;
static int cut_happened;

void fake_backend_power_cut_after_sectors(int64_t sectors);
int fake_backend_power_was_cut(void);
uint64_t fake_backend_sectors_written(void);
static uint64_t sectors_written;

void fake_backend_power_cut_after_sectors(int64_t sectors) {
    cut_budget = sectors;
    cut_happened = 0;
    sectors_written = 0;
}

int fake_backend_power_was_cut(void) {
    return cut_happened;
}

uint64_t fake_backend_sectors_written(void) {
    return sectors_written;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    if ((uint64_t)lba + count > disk_sectors) {
        return -1;
    }
    write_calls++;
    uint32_t keep = count;
    if (cut_budget >= 0) {
        if (cut_happened || (int64_t)sectors_written + count > cut_budget) {
            keep = cut_happened ? 0 : (uint32_t)(cut_budget - (int64_t)sectors_written);
            cut_happened = 1;
        }
    }
    memcpy(disk + (uint64_t)lba * ATA_SECTOR_SIZE, buffer, (uint64_t)keep * ATA_SECTOR_SIZE);
    sectors_written += keep;
    return keep == count ? 0 : -1;
}
