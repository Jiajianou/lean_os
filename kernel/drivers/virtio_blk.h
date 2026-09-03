/* kernel/drivers/virtio_blk.h - M92
 *
 * A virtio block device over the legacy (0.9.5) PCI transport: BAR0 is an
 * I/O port window, the virtqueue is one physically contiguous ring the
 * device walks itself, and a request is three descriptors chained -
 * header, data, status.
 *
 * Why this and not AHCI first. Both are real; virtio-blk is the smaller
 * one by a wide margin (there is no port/command-list/FIS layering, and
 * no ATA command set), QEMU has always had it, and the thing M92 is
 * actually after is the transfer model rather than the controller: ATA
 * PIO moves one sector at a time through `insw` on the CPU, sixteen
 * thousand port reads per 4 KiB block group. This moves a whole request
 * with one notification and no CPU cycles per byte. AHCI is what a real
 * machine needs and is deliberately not in this milestone - drivers/ata.c
 * stays as the fallback that boots anything, and virtio is what the
 * emulator gets.
 *
 * Deliberately polled rather than interrupt-driven, and the reason is the
 * one drivers/ata.h already gives for not handling IRQ14: "nothing here
 * needs to overlap disk I/O with other work yet." That is still true -
 * leanfs holds one lock across a request - so an interrupt would buy a
 * context switch this kernel has nothing to switch to. What made PIO slow
 * was never the polling, it was the per-word port I/O.
 */
#pragma once

#include <stdint.h>

/* Finds and initialises the device. Returns 1 if this machine has one and
 * it came up, 0 otherwise - a machine without it falls back to ATA, the
 * same "degrade rather than panic" rule M27 set for the NIC. */
int virtio_blk_init(void);

/* Capacity in 512-byte sectors, as the device reports it. */
uint64_t virtio_blk_capacity(void);

/* Both panic on a device-reported error, matching ata.c: there is no
 * retry story anywhere in this kernel and inventing one for a single
 * driver would be a rule that lives in one place. */
/* Q16: 0 on success, -1 on a device error or a request that never
 * completed. Both of those used to be a panic - see the note in
 * submit() for why halting on a bad sector is the more expensive of the
 * two answers, and kernel/drivers/blk.h for what the caller does with
 * this one. */
int virtio_blk_read(uint64_t lba, uint32_t count, void *buf);
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buf);

/* Q16: commands this driver has failed since boot. See ata_error_count. */
uint32_t virtio_blk_error_count(void);
