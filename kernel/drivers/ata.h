/* kernel/drivers/ata.h
 *
 * ATA PIO-mode driver: primary bus, master drive, 28-bit LBA addressing,
 * polling (no IRQ14 handling - PIO mode is inherently CPU-driven anyway,
 * and nothing here needs to overlap disk I/O with other work yet).
 * Assumes the boot disk itself is the primary master, which is what QEMU
 * gives a bare `-drive` on the default `pc` machine - the same drive
 * stage1/stage2 already read from via BIOS `INT 13h`, just addressed
 * directly by the kernel now instead of through firmware.
 */
#pragma once

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

/* Reads/writes `count` consecutive 512-byte sectors starting at `lba`
 * into/from `buf` (must be at least count * ATA_SECTOR_SIZE bytes).
 * Panics on a command timeout or an ERR status bit - there's no
 * retry/error-recovery story yet, matching every other driver in this
 * kernel so far. */
void ata_read_sectors(uint32_t lba, uint8_t count, void *buf);
void ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);
