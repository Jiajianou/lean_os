/* kernel/mm/filemap.h - M91 (second attempt)
 *
 * The frames that hold a file's pages while somebody has it mapped.
 *
 * ---- why this exists at all -------------------------------------------
 *
 * A private file mapping needs no table: each process reads the file
 * into its own frame and what it writes there is nobody else's business.
 * A **shared** one does, and that is the whole content of this file. Two
 * processes that map the same file with MAP_SHARED must see each other's
 * writes, which means they must be looking at the *same frame* - and
 * "the same frame for this (file, page index)" is a question nothing in
 * this kernel could previously answer.
 *
 * ---- what it is not ----------------------------------------------------
 *
 * It is **not a page cache**. A page cache holds pages a file has merely
 * been read through, so that the next read is free; this holds only
 * pages somebody has mapped, and a page leaves the moment the last
 * mapping does. M92 measured what a cache buys on this machine (2.2x
 * over DMA, once the driver stopped being PIO) and put one in front of
 * the block device, which is the layer where the measurement said it
 * belonged. Nothing has measured a second cache at the file layer, so
 * there is not one - this is a *sharing* table that happens to have to
 * read and write the file.
 *
 * ---- write-back, and the one place it is coarse ------------------------
 *
 * A frame handed out for a writable shared mapping is marked dirty at the
 * moment it is mapped, not at the moment it is written. The x86-64 page
 * table has a dirty bit that would be exact, and using it would mean
 * walking every mapper's tables at unmap time to collect it. The cost of
 * being coarse is that a shared page mapped writable and never written is
 * written back once; the cost of being exact is a walk per unmap of every
 * address space that ever touched the file. The first is a wasted block
 * write and the second is a data structure. Written down rather than
 * implied, because "dirty" here means "could have been written" and that
 * is a weaker statement than it looks.
 */
#pragma once

#include <stdint.h>

/* How many file pages may be shared at once, across the whole machine.
 *
 * 512 pages is 2 MiB of shared file mapping, which is more than anything
 * on this machine maps and small enough that the table is a linear scan
 * rather than a hash. A mapping that cannot get a slot fails at the
 * fault, which kills the process that asked - see filemap_get's return
 * and sched_fault_fill's handling of it. That is a real ceiling and it
 * is reported rather than worked around. */
#define FILEMAP_MAX_PAGES 512

/* The frame holding page `index` of `handle`, read in from the file if
 * this is the first mapper. Takes a reference. Returns 0 if the table is
 * full, if there is no frame, or if the file cannot be read.
 *
 * `writable` marks the page dirty - see the header note on why that is
 * at map time rather than at write time. */
uint64_t filemap_get(int handle, uint32_t index, int writable);

/* Drops a reference. Writes the page back to the file if it is dirty and
 * this was the last one, then frees the frame. */
void filemap_put(int handle, uint32_t index);

/* Writes back every dirty page of `handle` without dropping anything -
 * msync(MS_SYNC), and what SYS_fsync on a mapped file has to do before
 * it can claim the file on disk matches the file in memory. */
void filemap_sync(int handle);

/* How many slots are in use - for the boot self-test that asserts a
 * shared mapping gives its frames back. */
int filemap_in_use(void);
