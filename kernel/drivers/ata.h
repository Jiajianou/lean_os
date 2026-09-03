/* kernel/drivers/ata.h
 *
 * ATA PIO-mode driver: primary bus, master drive, 28-bit LBA addressing,
 * polling (no IRQ14 handling - PIO mode is inherently CPU-driven anyway,
 * and nothing here needs to overlap disk I/O with other work yet).
 * Assumes the boot disk itself is the primary master, which is what QEMU
 * gives a bare `-drive` on the default `pc` machine - the same drive the
 * boot loader already read the kernel from via EFI_BLOCK_IO_PROTOCOL, just
 * addressed directly by the kernel now instead of through firmware.
 */
#pragma once

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

/* Reads/writes `count` consecutive 512-byte sectors starting at `lba`
 * into/from `buf` (must be at least count * ATA_SECTOR_SIZE bytes).
 *
 * ---- Q16: these used to panic, and now they return ---------------------
 *
 * "Panics on a command timeout or an ERR status bit - there's no
 * retry/error-recovery story yet, matching every other driver in this
 * kernel so far" is what this comment said, and it was an accurate
 * description of a machine that had only ever run under an emulator
 * whose disk does not fail. On hardware a bad sector is the likeliest
 * first failure there is, and halting the machine is a strictly worse
 * answer than telling the caller.
 *
 * 0 on success, -1 on a timeout or an ERR status. A failed read leaves
 * `buf` in an unspecified state and a failed write may have written some
 * of the sectors - both of which are what the hardware actually
 * guarantees, and saying so is the point. The caller decides; see
 * kernel/drivers/blk.h for what this kernel's caller decides.
 *
 * There is still no retry and that is deliberate: a retry policy with no
 * measurement behind it is a number somebody invented, and the layer
 * that knows whether a retry is worth it is the one that knows what the
 * read was for. */
int ata_read_sectors(uint32_t lba, uint8_t count, void *buf);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);

/* Q16: how many commands this driver has failed since boot.
 *
 * A machine whose disk is failing occasionally has to be
 * distinguishable from one whose disk is fine, and an error that
 * propagates is invisible from outside the caller that saw it. This is
 * the counter the boot self-test reads and the one a `/proc` reader
 * would want. */
uint32_t ata_error_count(void);
