/* kernel/drivers/ahci.h - M107
 *
 * AHCI: the SATA controller interface every machine built between about
 * 2005 and about 2015 has, and which virtio_blk.h explicitly declined.
 *
 * ---- what M92 said, and why this is the milestone that pays it --------
 *
 * virtio_blk.h's own header says it: "AHCI is what a real machine needs
 * and is deliberately not in this milestone - drivers/ata.c stays as the
 * fallback that boots anything, and virtio is what the emulator gets."
 * That was the right call for a machine that had only ever booted under
 * an emulator. It stops being right the moment the target is a physical
 * machine, because on one of those `drivers/ata.c` is not a fallback that
 * boots anything - it is a driver for a controller that is not there.
 *
 * ---- the layering, which is the whole difference from virtio ---------
 *
 * virtio-blk is one ring and three descriptors. AHCI is four levels deep
 * and every one of them is a separate allocation the device walks by
 * physical address:
 *
 *   HBA registers (a memory BAR)
 *     -> per-port command list: 32 command headers, 1 KiB aligned
 *       -> per-slot command table: a command FIS, then a PRDT
 *         -> PRDT entry: a physical address and a byte count
 *
 * plus a *fifth* region per port, the FIS receive area, which the device
 * writes and the driver mostly ignores - its presence is mandatory even
 * for a driver that never reads it, because the controller will not leave
 * its idle state until it has somewhere to put received FISes.
 *
 * ---- what is deliberately not here -----------------------------------
 *
 * **NCQ.** M107's own bullet asks for "NCQ deep enough to matter" and
 * this driver has a queue depth of one, which is the opposite. The reason
 * is M69's rule rather than difficulty: NCQ buys overlapping commands,
 * and there is nothing here to overlap. leanfs holds one lock across a
 * request, so the second command cannot be issued until the first has
 * returned to the caller, and 32 command slots would sit 31 empty. The
 * 32-slot command list is allocated anyway - it is mandatory - and slot 0
 * is the only one ever used. When there is a second writer, the change is
 * a free-slot search in issue_command() and a per-slot completion, and
 * this note is what it is conditioned on.
 *
 * **Hotplug**, ignored on purpose, which the bullet asked for. A disk
 * that arrives after boot is a disk leanfs has not mounted.
 *
 * **Port multipliers and ATAPI.** One SATA disk on one port is the
 * machine this OS boots on.
 *
 * ---- polled, like every other disk driver here -----------------------
 *
 * Same argument virtio_blk.h makes and for the same reason: an interrupt
 * would buy a context switch this kernel has nothing to switch to while
 * one lock is held across the whole request. milestones.md already
 * carries "interrupt-driven virtio" as deferred against M110's
 * measurements; this driver joins that deferral rather than opening a
 * second one. The MSI-X wiring M107's bullet mentions is in nvme.h's
 * note, which is where the same question came up with a real answer.
 */
#pragma once

#include <stdint.h>

/* Finds an AHCI controller, brings up the first port with a SATA disk on
 * it, and returns 1 if this machine has one. 0 means no controller, no
 * populated port, or a controller that would not leave its reset - all
 * three of which are "the next backend gets a turn" rather than errors,
 * the same degrade-rather-than-panic rule M27 set for the NIC. */
int ahci_init(void);

/* Capacity in 512-byte sectors, from IDENTIFY DEVICE's 48-bit field. */
uint64_t ahci_capacity(void);

/* 0 on success, -1 on a device error or a command that never completed.
 * Same contract as virtio_blk_read/write and for the same reason - see
 * drivers/blk.h for what the caller does with it. */
int ahci_read(uint64_t lba, uint32_t count, void *buf);
int ahci_write(uint64_t lba, uint32_t count, const void *buf);

/* Commands this driver has failed since boot. */
uint32_t ahci_error_count(void);
