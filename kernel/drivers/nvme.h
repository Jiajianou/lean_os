/* kernel/drivers/nvme.h - M107
 *
 * NVMe: the disk a machine built in the last ten years actually has, and
 * the one the laptop this milestone exists for has *only* - a ThinkPad of
 * that generation has no SATA controller at all, so ahci.c and ata.c are
 * both drivers for hardware that is not in the box.
 *
 * ---- why this is the simplest of the three, not the hardest ----------
 *
 * It reads like the most modern and is by a wide margin the least code,
 * because NVMe was designed after everyone had agreed what a queue is.
 * There is no command list, no FIS, no task file, and no ATA command set
 * underneath it: there is a submission queue, a completion queue, a
 * doorbell register, and a 64-byte command whose opcode is 0x01 for write
 * and 0x02 for read. AHCI is four levels of indirection because it is a
 * 2005 interface wrapped around a 1994 command set. This is one level.
 *
 * The one thing it does ask for that the others do not is a PRP list: a
 * transfer bigger than two pages is described by a page of physical
 * addresses rather than by a scatter-gather list with lengths in it. Every
 * entry is exactly one page, which makes the list arithmetic rather than
 * a structure - and makes it free here, because this driver's bounce
 * buffer is physically contiguous, so the list is built once at init and
 * never changes.
 *
 * ---- queues, and how many of them --------------------------------
 *
 * One admin queue pair and one I/O queue pair, both 32 deep, and only one
 * command in flight at a time. The queues are the mechanism NVMe uses for
 * everything including its own configuration, so even a driver that wants
 * no parallelism needs two of them.
 *
 * A real NVMe driver creates one I/O queue pair per CPU, which is the
 * whole point of the interface - no lock, no shared doorbell, no cache
 * line bouncing between cores. This one does not, for the reason M69
 * sets: leanfs holds one lock across a request, so a second queue would
 * have nothing to put in it. The measurement that would change this is a
 * filesystem with per-inode locking, not a faster disk.
 *
 * ---- MSI-X, which M107's bullet asks for by name ---------------------
 *
 * Not wired up, and this is the deferral rather than an oversight. The
 * bullet reads "MSI-X completions from M103", and the honest finding on
 * getting here is that MSI-X is the second half of a change whose first
 * half nobody needs yet: an interrupt that completes a command is only
 * worth having if the CPU has something else to do while the command
 * runs, and it does not - `blk_read` is called with one lock held and
 * returns to a caller that cannot proceed without the bytes.
 *
 * milestones.md already carries "interrupt-driven virtio" as deferred
 * against M110's measurements. This joins that deferral on the same
 * condition rather than opening a second one: when a disk command's
 * latency is measured on metal and the machine is shown to be idle across
 * it, the change is one vector, one handler, and a completion queue this
 * driver already maintains the phase tag of. INTMS is masked at init so
 * the controller cannot raise a line nothing is listening on.
 */
#pragma once

#include <stdint.h>

/* Finds an NVMe controller, resets it, creates its queues, and picks the
 * first active namespace. 1 if this machine has one that came up. 0 for
 * every failure - no controller, a controller that never became ready, a
 * namespace with a block size this driver does not speak - so the next
 * backend gets its turn rather than the machine halting. */
int nvme_init(void);

/* Capacity in 512-byte sectors. A namespace formatted with 4096-byte
 * blocks reports its size in those, and this converts - see the note in
 * nvme.c on why 512 is the unit at this boundary even when it is not the
 * unit on the disk. */
uint64_t nvme_capacity(void);

/* 0 on success, -1 on a controller error or a command that never
 * completed. Same contract as the other three backends. */
int nvme_read(uint64_t lba, uint32_t count, void *buf);
int nvme_write(uint64_t lba, uint32_t count, const void *buf);

uint32_t nvme_error_count(void);
