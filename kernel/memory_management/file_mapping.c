#include "file_mapping.h"

#include "file_system/virtual_file_system.h"
#include "library/kernel_library.h"
#include "library/spinlock.h"
#include "memory_management/physical_memory.h"
#include "process/process.h"

typedef struct {
    int handle;
    uint32_t index;
    uint64_t phys;
    int refs;
    uint8_t dirty;
} file_mapping_page_t;

static file_mapping_page_t table[FILEMAP_MAX_PAGES];
static int initialized;

static spinlock_t file_mapping_lock;

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

uint64_t file_mapping_get(int handle, uint32_t index, int writable) {
    if (handle < 0) {
        return 0;
    }
    uint64_t flags = spin_lock_irqsave(&file_mapping_lock);
    ensure_init();
    int slot = find_slot(handle, index);
    if (slot >= 0) {
        table[slot].refs++;
        if (writable) {
            table[slot].dirty = 1;
        }
        uint64_t phys = table[slot].phys;
        spin_unlock_irqrestore(&file_mapping_lock, flags);
        return phys;
    }
    spin_unlock_irqrestore(&file_mapping_lock, flags);

    uint64_t phys = physical_memory_try_alloc_frame();
    if (phys == 0) {
        return 0;
    }
    k_memset((void *)phys, 0, PAGE_SIZE);
    int64_t n = virtual_file_system_handle_read(handle, (void *)phys, PAGE_SIZE,
                                index * PAGE_SIZE);
    if (n < 0) {
        physical_memory_free_frame(phys);
        return 0;
    }

    flags = spin_lock_irqsave(&file_mapping_lock);
    slot = find_slot(handle, index);
    if (slot >= 0) {
        table[slot].refs++;
        if (writable) {
            table[slot].dirty = 1;
        }
        uint64_t theirs = table[slot].phys;
        spin_unlock_irqrestore(&file_mapping_lock, flags);
        physical_memory_free_frame(phys);
        return theirs;
    }
    for (int i = 0; i < FILEMAP_MAX_PAGES; i++) {
        if (table[i].handle < 0) {
            table[i].handle = handle;
            table[i].index = index;
            table[i].phys = phys;
            table[i].refs = 1;
            table[i].dirty = (uint8_t)(writable != 0);
            spin_unlock_irqrestore(&file_mapping_lock, flags);
            return phys;
        }
    }
    spin_unlock_irqrestore(&file_mapping_lock, flags);
    physical_memory_free_frame(phys);
    return 0;
}

void file_mapping_put(int handle, uint32_t index) {
    if (handle < 0) {
        return;
    }
    uint64_t flags = spin_lock_irqsave(&file_mapping_lock);
    ensure_init();
    int slot = find_slot(handle, index);
    if (slot < 0) {
        spin_unlock_irqrestore(&file_mapping_lock, flags);
        return;
    }
    if (--table[slot].refs > 0) {
        spin_unlock_irqrestore(&file_mapping_lock, flags);
        return;
    }
    uint64_t phys = table[slot].phys;
    int dirty = table[slot].dirty;
    table[slot].handle = -1;
    spin_unlock_irqrestore(&file_mapping_lock, flags);

    if (dirty) {
        virtual_file_system_handle_write(handle, (const void *)phys, PAGE_SIZE,
                         index * PAGE_SIZE);
    }
    physical_memory_free_frame(phys);
}

void file_mapping_sync(int handle) {
    if (handle < 0) {
        return;
    }
    for (int i = 0; i < FILEMAP_MAX_PAGES; i++) {
        uint64_t flags = spin_lock_irqsave(&file_mapping_lock);
        ensure_init();
        int dirty = (table[i].handle == handle && table[i].dirty);
        uint64_t phys = table[i].phys;
        uint32_t index = table[i].index;
        if (dirty) {
            table[i].dirty = 0;
        }
        spin_unlock_irqrestore(&file_mapping_lock, flags);
        if (dirty) {
            virtual_file_system_handle_write(handle, (const void *)phys, PAGE_SIZE,
                             index * PAGE_SIZE);
        }
    }
}

int file_mapping_in_use(void) {
    uint64_t flags = spin_lock_irqsave(&file_mapping_lock);
    ensure_init();
    int n = 0;
    for (int i = 0; i < FILEMAP_MAX_PAGES; i++) {
        if (table[i].handle >= 0) {
            n++;
        }
    }
    spin_unlock_irqrestore(&file_mapping_lock, flags);
    return n;
}
