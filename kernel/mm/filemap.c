#include "filemap.h"

#include "fs/vfs.h"
#include "lib/libk.h"
#include "lib/spinlock.h"
#include "mm/pmm.h"
#include "proc/proc.h" /* PAGE_SIZE */

typedef struct {
    int handle;      /* -1 when this slot is free */
    uint32_t index;  /* which page of the file */
    uint64_t phys;
    int refs;
    uint8_t dirty;
} filemap_page_t;

static filemap_page_t table[FILEMAP_MAX_PAGES];
static int initialized;

/* ---- filemap_lock ------------------------------------------------------
 *
 * What it protects: the table above and every refcount in it.
 *
 * Against whom: two page faults on two CPUs for the same (file, page) -
 * which is exactly the case this table exists to make coherent, so it is
 * not a theoretical race. Interrupts off, because a fault is where this
 * is called from and a fault can arrive on any core at any time.
 *
 * Lock order: filemap_lock is taken and DROPPED before vfs_handle_read or
 * vfs_handle_write, which take fs_lock. openfile_unref established the
 * same rule for the same reason - one order, or a deadlock waiting for
 * the second CPU. The consequence is that two callers can both decide to
 * read the same page in; the second one finds the slot already populated
 * and frees the frame it allocated, which is a wasted read and never a
 * wrong answer. */
static spinlock_t filemap_lock;

static void ensure_init(void) {
    if (initialized) {
        return;
    }
    for (int i = 0; i < FILEMAP_MAX_PAGES; i++) {
        table[i].handle = -1;
    }
    initialized = 1;
}

static int find_slot(int handle, uint32_t index) {
    for (int i = 0; i < FILEMAP_MAX_PAGES; i++) {
        if (table[i].handle == handle && table[i].index == index) {
            return i;
        }
    }
    return -1;
}

uint64_t filemap_get(int handle, uint32_t index, int writable) {
    if (handle < 0) {
        return 0;
    }
    uint64_t flags = spin_lock_irqsave(&filemap_lock);
    ensure_init();
    int slot = find_slot(handle, index);
    if (slot >= 0) {
        table[slot].refs++;
        if (writable) {
            table[slot].dirty = 1;
        }
        uint64_t phys = table[slot].phys;
        spin_unlock_irqrestore(&filemap_lock, flags);
        return phys;
    }
    spin_unlock_irqrestore(&filemap_lock, flags);

    /* Read the page with the lock dropped - see the lock note. */
    uint64_t phys = pmm_try_alloc_frame();
    if (phys == 0) {
        return 0;
    }
    /* Zeroed first: a file may be shorter than the mapping, and the tail
     * of the last page must read as zeros rather than as whatever the
     * frame held before. POSIX specifies exactly this. */
    k_memset((void *)phys, 0, PAGE_SIZE);
    int64_t n = vfs_handle_read(handle, (void *)phys, PAGE_SIZE,
                                index * PAGE_SIZE);
    if (n < 0) {
        pmm_free_frame(phys);
        return 0;
    }

    flags = spin_lock_irqsave(&filemap_lock);
    slot = find_slot(handle, index);
    if (slot >= 0) {
        /* Somebody else won the race. Theirs is the one everyone must
         * share, so this one is given back - a wasted read, never a
         * second frame for one page. */
        table[slot].refs++;
        if (writable) {
            table[slot].dirty = 1;
        }
        uint64_t theirs = table[slot].phys;
        spin_unlock_irqrestore(&filemap_lock, flags);
        pmm_free_frame(phys);
        return theirs;
    }
    for (int i = 0; i < FILEMAP_MAX_PAGES; i++) {
        if (table[i].handle < 0) {
            table[i].handle = handle;
            table[i].index = index;
            table[i].phys = phys;
            table[i].refs = 1;
            table[i].dirty = (uint8_t)(writable != 0);
            spin_unlock_irqrestore(&filemap_lock, flags);
            return phys;
        }
    }
    spin_unlock_irqrestore(&filemap_lock, flags);
    pmm_free_frame(phys); /* the table is full - see FILEMAP_MAX_PAGES */
    return 0;
}

void filemap_put(int handle, uint32_t index) {
    if (handle < 0) {
        return;
    }
    uint64_t flags = spin_lock_irqsave(&filemap_lock);
    ensure_init();
    int slot = find_slot(handle, index);
    if (slot < 0) {
        spin_unlock_irqrestore(&filemap_lock, flags);
        return;
    }
    if (--table[slot].refs > 0) {
        spin_unlock_irqrestore(&filemap_lock, flags);
        return;
    }
    /* Last mapper. Take the slot out of the table before writing it back,
     * so that a new mapper arriving during the write does not find a
     * frame that is about to be freed - it reads the page in fresh
     * instead, which is correct because the write below is what makes
     * that read see the right bytes. */
    uint64_t phys = table[slot].phys;
    int dirty = table[slot].dirty;
    table[slot].handle = -1;
    spin_unlock_irqrestore(&filemap_lock, flags);

    if (dirty) {
        vfs_handle_write(handle, (const void *)phys, PAGE_SIZE,
                         index * PAGE_SIZE);
    }
    pmm_free_frame(phys);
}

void filemap_sync(int handle) {
    if (handle < 0) {
        return;
    }
    /* Two passes, because the write has to happen with the lock dropped:
     * the first collects what is dirty, the second writes it. A page that
     * becomes dirty between them is missed by this call and caught by the
     * next one or by the unmap - which is what msync means everywhere,
     * since a write racing an msync has no defined ordering anyway. */
    for (int i = 0; i < FILEMAP_MAX_PAGES; i++) {
        uint64_t flags = spin_lock_irqsave(&filemap_lock);
        ensure_init();
        int dirty = (table[i].handle == handle && table[i].dirty);
        uint64_t phys = table[i].phys;
        uint32_t index = table[i].index;
        if (dirty) {
            table[i].dirty = 0;
        }
        spin_unlock_irqrestore(&filemap_lock, flags);
        if (dirty) {
            vfs_handle_write(handle, (const void *)phys, PAGE_SIZE,
                             index * PAGE_SIZE);
        }
    }
}

int filemap_in_use(void) {
    uint64_t flags = spin_lock_irqsave(&filemap_lock);
    ensure_init();
    int n = 0;
    for (int i = 0; i < FILEMAP_MAX_PAGES; i++) {
        if (table[i].handle >= 0) {
            n++;
        }
    }
    spin_unlock_irqrestore(&filemap_lock, flags);
    return n;
}
