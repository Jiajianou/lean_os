#include "ata.h"

#include "arch/x86_64/io.h"

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

#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH   0xE7

#define ATA_SR_ERR 0x01
#define ATA_SR_DRQ 0x08
#define ATA_SR_BSY 0x80

#define ATA_DRIVE_MASTER_LBA 0xE0 /* bits 7,5 always 1 (legacy); bit 6 = LBA mode; bit 4 = 0 (master) */

#define ATA_POLL_LIMIT 100000 /* generous bound so a wedged/missing drive reports instead of hanging boot forever (Q16: it used to panic) */

static void ata_select(uint32_t lba, uint8_t count) {
    outb(ATA_REG_DRIVE, (uint8_t)(ATA_DRIVE_MASTER_LBA | ((lba >> 24) & 0x0F)));
    outb(ATA_REG_SECCOUNT, count);
    outb(ATA_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
}

/* Q16: the four halts that used to be here.
 *
 * Every one of them was a device failure - an ERR status or a poll that
 * ran out - and every one stopped the machine. Under QEMU they never
 * fire, which is exactly why they survived ninety milestones; on the
 * hardware M28 is still waiting for, they are the likeliest first thing
 * to happen. They report now, and the report is counted so that a disk
 * which is failing occasionally is distinguishable from one that is not
 * failing at all: see ata_error_count and the [q16] self-test.
 *
 * Returns 0 ready, -1 failed. */
static uint32_t errors;

uint32_t ata_error_count(void) {
    return errors;
}

static int ata_wait_ready(void) {
    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t status = inb(ATA_REG_STATUS);
        if (status & ATA_SR_BSY) {
            continue;
        }
        if (status & ATA_SR_ERR) {
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
        if (status & ATA_SR_ERR) {
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

int ata_read_sectors(uint32_t lba, uint8_t count, void *buf) {
    if (ata_wait_ready() != 0) {
        return -1;
    }
    ata_select(lba, count);
    outb(ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    uint16_t *dst = (uint16_t *)buf;
    for (uint8_t s = 0; s < count; s++) {
        /* Bailing out mid-transfer leaves the drive with sectors it still
         * wants to hand over, and the next command re-selects and
         * re-issues - which is what the ata_wait_ready at the top of
         * every entry point is for. It was there before this change and
         * it is load-bearing now rather than merely tidy. */
        if (ata_wait_ready() != 0 || ata_wait_drq() != 0) {
            return -1;
        }
        insw(ATA_REG_DATA, dst, ATA_SECTOR_SIZE / 2);
        dst += ATA_SECTOR_SIZE / 2;
    }
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf) {
    if (ata_wait_ready() != 0) {
        return -1;
    }
    ata_select(lba, count);
    outb(ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    const uint16_t *src = (const uint16_t *)buf;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_ready() != 0 || ata_wait_drq() != 0) {
            return -1;
        }
        outsw(ATA_REG_DATA, src, ATA_SECTOR_SIZE / 2);
        src += ATA_SECTOR_SIZE / 2;
    }

    outb(ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    /* The flush is where a write that the drive accepted and could not
     * commit finally reports itself, so its answer is the call's answer.
     * A write that returned success before the flush completed would be
     * exactly the lie M71's ordering guarantee is built on top of. */
    return ata_wait_ready();
}
