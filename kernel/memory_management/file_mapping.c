#include "file_mapping.h"

#include "drivers/kernel_log.h"
#include "file_system/virtual_file_system.h"
#include "library/kernel_library.h"
#include "library/spinlock.h"
#include "memory_management/heap.h"
#include "memory_management/physical_memory.h"
#include "panic.h"
#include "process/process.h"

typedef struct {
    int handle;
    uint32_t index;
    uint64_t phys;
    int refs;
    uint8_t dirty;
} file_mapping_page_t;

/* Open addressing with linear probing, keyed by (handle, index): a fault is a
   few probes rather than a walk of the whole table. Deletion shifts the run
   back rather than leaving a tombstone, so a table that has been through a
   million maps and unmaps probes as short as a fresh one. */
_Static_assert((FILE_MAPPING_MAX_PAGES & (FILE_MAPPING_MAX_PAGES - 1)) == 0,
               "the table is indexed by a mask");

static file_mapping_page_t *table;
static int initialized;
static int in_use;
static int refusals;

static spinlock_t file_mapping_lock;

static void ensure_init(void) {
    if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE)) {
        return;
    }
    file_mapping_page_t *fresh =
        (file_mapping_page_t *)kmalloc(sizeof(file_mapping_page_t) * FILE_MAPPING_MAX_PAGES);
    if (!fresh) {
        panic("file_mapping: no memory for the table of shared file pages");
    }
    for (int i = 0; i < FILE_MAPPING_MAX_PAGES; i++) {
        fresh[i].handle = -1;
    }
    uint64_t flags = spin_lock_irqsave(&file_mapping_lock);
    if (!initialized) {
        table = fresh;
        fresh = (file_mapping_page_t *)0;
        __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&file_mapping_lock, flags);
    if (fresh) {
        kfree(fresh);
    }
}

static uint32_t home_of(int handle, uint32_t index) {
    uint64_t key = ((uint64_t)(uint32_t)handle << 32) | index;
    key ^= key >> 33;
    key *= 0xFF51AFD7ED558CCDULL;
    key ^= key >> 33;
    return (uint32_t)(key & (FILE_MAPPING_MAX_PAGES - 1));
}

static int find_slot(int handle, uint32_t index) {
    uint32_t i = home_of(handle, index);
    for (int probes = 0; probes < FILE_MAPPING_MAX_PAGES; probes++) {
        if (table[i].handle < 0) {
            return -1;
        }
        if (table[i].handle == handle && table[i].index == index) {
            return (int)i;
        }
        i = (i + 1) & (FILE_MAPPING_MAX_PAGES - 1);
    }
    return -1;
}

static int free_slot_for(int handle, uint32_t index) {
    if (in_use >= FILE_MAPPING_MAX_PAGES - 1) {
        return -1;
    }
    uint32_t i = home_of(handle, index);
    while (table[i].handle >= 0) {
        i = (i + 1) & (FILE_MAPPING_MAX_PAGES - 1);
    }
    return (int)i;
}

static void remove_slot(uint32_t hole) {
    table[hole].handle = -1;
    in_use--;
    uint32_t i = (hole + 1) & (FILE_MAPPING_MAX_PAGES - 1);
    while (table[i].handle >= 0) {
        uint32_t home = home_of(table[i].handle, table[i].index);
        /* An entry moves back into the hole if the hole lies on the way from
           its home to where it is now - cyclically. */
        int between = (hole <= i) ? (home <= hole || home > i)
                                  : (home <= hole && home > i);
        if (between) {
            table[hole] = table[i];
            table[i].handle = -1;
            hole = i;
        }
        i = (i + 1) & (FILE_MAPPING_MAX_PAGES - 1);
    }
}

uint64_t file_mapping_get(int handle, uint32_t index, int writable) {
    if (handle < 0) {
        return 0;
    }
    ensure_init();
    uint64_t flags = spin_lock_irqsave(&file_mapping_lock);
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
    int free_slot = free_slot_for(handle, index);
    if (free_slot >= 0) {
        table[free_slot].handle = handle;
        table[free_slot].index = index;
        table[free_slot].phys = phys;
        table[free_slot].refs = 1;
        table[free_slot].dirty = (uint8_t)(writable != 0);
        in_use++;
        spin_unlock_irqrestore(&file_mapping_lock, flags);
        return phys;
    }
    refusals++;
    int report = refusals == 1 || refusals % 64 == 0;
    spin_unlock_irqrestore(&file_mapping_lock, flags);
    physical_memory_free_frame(phys);
    /* The fault path reports this as out of memory and kills the process,
       which is true of the process and says nothing about why. */
    if (report) {
        kernel_log_puts("[mmap] the machine is at its ceiling of ");
        kernel_log_put_dec(FILE_MAPPING_MAX_PAGES);
        kernel_log_puts(" shared file pages (");
        kernel_log_put_dec((uint32_t)refusals);
        kernel_log_puts(" refusal(s) since boot)\n");
    }
    return 0;
}

void file_mapping_put(int handle, uint32_t index) {
    if (handle < 0) {
        return;
    }
    ensure_init();
    uint64_t flags = spin_lock_irqsave(&file_mapping_lock);
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
    remove_slot((uint32_t)slot);
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
    ensure_init();
    for (int i = 0; i < FILE_MAPPING_MAX_PAGES; i++) {
        uint64_t flags = spin_lock_irqsave(&file_mapping_lock);
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
    ensure_init();
    uint64_t flags = spin_lock_irqsave(&file_mapping_lock);
    int n = in_use;
    spin_unlock_irqrestore(&file_mapping_lock, flags);
    return n;
}
