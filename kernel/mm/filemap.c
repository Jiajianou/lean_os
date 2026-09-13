#include "filemap.h"

#include "fs/vfs.h"
#include "lib/libk.h"
#include "lib/spinlock.h"
#include "mm/pmm.h"
#include "proc/proc.h"

typedef struct {
    int handle;
    uint32_t index;
    uint64_t phys;
    int refs;
    uint8_t dirty;
} filemap_page_t;

static filemap_page_t table[FILEMAP_MAX_PAGES];
static int initialized;

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

    uint64_t phys = pmm_try_alloc_frame();
    if (phys == 0) {
        return 0;
    }
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
    pmm_free_frame(phys);
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
