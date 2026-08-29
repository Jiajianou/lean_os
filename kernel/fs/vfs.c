#include "vfs.h"

#include "drivers/klog.h"
#include "leanfs.h"
#include "lib/spinlock.h"

/* ---- M67: fs_lock ----------------------------------------------------
 *
 * What it protects: every byte of mutable state in kernel/fs/leanfs.c -
 * the in-memory superblock, the inode table, the block bitmap, the
 * dirent scratch buffer and the per-sector dirty flags - plus the ATA
 * PIO driver underneath it, which is a sequence of port writes that
 * means nothing if a second caller interleaves its own.
 *
 * Against whom: for sixty-six milestones, nobody. `int 0x80` was an
 * *interrupt* gate, so IF was clear from the syscall instruction to the
 * iretq, and that was this filesystem's entire mutual exclusion - never
 * written down, because nothing had to write it down. M67 makes vector
 * 0x80 a trap gate, so a timer tick can now land between any two
 * instructions of a file write and hand the CPU to a task that calls
 * vfs_write itself. On a second core it never even needed the tick.
 *
 * One coarse lock, not a lock per inode. leanfs has exactly one writer's
 * worth of structure - a single shared bitmap and a single inode table
 * that any write may touch - so per-inode locking would buy nothing but
 * a lock-ordering problem. The cost is that two concurrent file
 * operations serialise, which is what they would do at the disk anyway:
 * there is one ATA channel and PIO is synchronous.
 *
 * Taken with interrupts off (spin_lock_irqsave), for the reason M56
 * wrote down for vmm_lock and which applies with more force here: a
 * fatal signal is delivered from the timer tick, so a task preempted
 * while holding this lock could be torn down by task_exit_with_code and
 * never release it - a permanent deadlock rather than a slow spin. Off
 * for the duration of one operation is the honest cost of that
 * guarantee; M68 converts this to a sleeping mutex once there is a
 * blocked state to sleep in, which is the real fix for the latency.
 *
 * vfs_init is deliberately NOT locked: it runs once, from kernel_main,
 * before any other task exists, and it formats and seeds the disk - tens
 * of thousands of PIO transfers. Holding IF off across that would stop
 * the timer for long enough to matter. There is nothing to race with.
 */
static spinlock_t fs_lock;

void vfs_init(void) {
    leanfs_init();
}

int64_t vfs_read(const char *path, void *buf, size_t maxlen) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int64_t r = leanfs_read(path, buf, maxlen);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_write(const char *path, const void *buf, size_t len) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_write(path, buf, len);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_exists(const char *path) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_exists(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_is_dir(const char *path) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_is_dir(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_mkdir(const char *path) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_mkdir(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

uint32_t vfs_free_blocks(void) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    uint32_t r = leanfs_free_blocks();
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_unlink(const char *path) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_unlink(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_rename(const char *old_path, const char *new_path) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_rename(old_path, new_path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

size_t vfs_list(const char *path, char *buf, size_t maxlen) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    size_t r = leanfs_list(path, buf, maxlen);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

void vfs_sync(void) {
    /* See vfs.h. Nothing to write back: leanfs_write_file's own
     * ata_write_sectors calls are synchronous, so a file is on the
     * platter by the time vfs_write returns to its caller. */
    klog_puts("[vfs] sync: leanfs is write-through, nothing buffered to flush.\n");
}

/* M59 */
int vfs_rmdir(const char *path) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_rmdir(path);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_stat(const char *path, leanfs_stat_t *out) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_stat(path, out);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_open(const char *path, int create) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_open(path, create);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int64_t vfs_handle_read(int handle, void *buf, size_t len, uint32_t off) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int64_t r = leanfs_handle_read(handle, buf, len, off);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int64_t vfs_handle_write(int handle, const void *buf, size_t len, uint32_t off) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int64_t r = leanfs_handle_write(handle, buf, len, off);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

uint32_t vfs_handle_size(int handle) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    uint32_t r = leanfs_handle_size(handle);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}

int vfs_handle_truncate(int handle) {
    uint64_t f = spin_lock_irqsave(&fs_lock);
    int r = leanfs_handle_truncate(handle);
    spin_unlock_irqrestore(&fs_lock, f);
    return r;
}
