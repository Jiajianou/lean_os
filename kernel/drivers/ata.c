#include "ata.h"

#include "architecture/x86_64/io.h"

#define ATA_IO_BASE 0x1F0

#define ATA_REG_DATA     (ATA_IO_BASE + 0)
#define ATA_REG_ERROR    (ATA_IO_BASE + 1)
#define ATA_REG_SECCOUNT (ATA_IO_BASE + 2)
#define ATA_REG_LBA_LOW  (ATA_IO_BASE + 3)
#define ATA_REG_LBA_MID  (ATA_IO_BASE + 4)
#define ATA_REG_LBA_HIGH (ATA_IO_BASE + 5)
#define ATA_REG_DRIVE    (ATA_IO_BASE + 6)
#define ATA_REG_STATUS   (ATA_IO_BASE + 7)
#define ATA_REG_COMMAND  (ATA_IO_BASE + 7)

#define ATA_COMMAND_READ_SECTORS  0x20
#define ATA_COMMAND_WRITE_SECTORS 0x30
#define ATA_COMMAND_CACHE_FLUSH   0xE7

#define ATA_SR_ERROR 0x01
#define ATA_SR_DRQ 0x08
#define ATA_SR_BSY 0x80

#define ATA_DRIVE_MASTER_LBA 0xE0

#define ATA_POLL_LIMIT 100000

static void ata_select(uint32_t lba, uint8_t count) {
    outb(ATA_REG_DRIVE, (uint8_t)(ATA_DRIVE_MASTER_LBA | ((lba >> 24) & 0x0F)));
    outb(ATA_REG_SECCOUNT, count);
    outb(ATA_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
}

static uint32_t errors;

static int ata_wait_ready(void) {
    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t status = inb(ATA_REG_STATUS);
        if (status & ATA_SR_BSY) {
            continue;
        }
        if (status & ATA_SR_ERROR) {
            errors++;
            return -1;
        }
        return 0;
    }
    errors++;
    return -1;
}

static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t status = inb(ATA_REG_STATUS);
        if (status & ATA_SR_ERROR) {
            errors++;
            return -1;
        }
        if (status & ATA_SR_DRQ) {
            return 0;
        }
    }
    errors++;
    return -1;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer) {
    if (ata_wait_ready() != 0) {
        return -1;
    }
    ata_select(lba, count);
    outb(ATA_REG_COMMAND, ATA_COMMAND_READ_SECTORS);

    uint16_t *destination = (uint16_t *)buffer;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_ready() != 0 || ata_wait_drq() != 0) {
            return -1;
        }
        insw(ATA_REG_DATA, destination, ATA_SECTOR_SIZE / 2);
        destination += ATA_SECTOR_SIZE / 2;
    }
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    if (ata_wait_ready() != 0) {
        return -1;
    }
    ata_select(lba, count);
    outb(ATA_REG_COMMAND, ATA_COMMAND_WRITE_SECTORS);

    const uint16_t *source = (const uint16_t *)buffer;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_ready() != 0 || ata_wait_drq() != 0) {
            return -1;
        }
        outsw(ATA_REG_DATA, source, ATA_SECTOR_SIZE / 2);
        source += ATA_SECTOR_SIZE / 2;
    }

    outb(ATA_REG_COMMAND, ATA_COMMAND_CACHE_FLUSH);
    return ata_wait_ready();
}
