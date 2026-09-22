#include "memfd.h"

#include "library/kernel_library.h"
#include "library/spinlock.h"
#include "memory_management/heap.h"
#include "memory_management/physical_memory.h"
#include "process/process.h"

static spinlock_t memfd_lock;

typedef struct memfd {
    int used;
    int refs;
    uint32_t slot;
    uint16_t generation;
    uint32_t pages;
    uint64_t size;
    uint64_t *frames;
    uint32_t seals;
    char name[MEMFD_NAME_MAX];
} memfd_t;

/* An array of pointers rather than of objects, so that a struct memfd * a
   caller holds stays valid across a growth, and each object lives for the
   rest of the boot once made: its generation is what lets a stale tag in a
   region table be told apart from a live one, and a generation that
   restarted at zero would say "live" about a memfd that is long gone. */
static memfd_t **table;
static uint32_t capacity;

static int memfd_grow_locked(void) {
    if (capacity >= MEMFD_MAX) {
        return -1;
    }
    uint32_t want = capacity ? capacity * 2 : MEMFD_INITIAL;
    if (want > MEMFD_MAX) {
        want = MEMFD_MAX;
    }
    memfd_t **grown = (memfd_t **)kmalloc((size_t)want * sizeof(memfd_t *));
    if (!grown) {
        return -1;
    }
    for (uint32_t i = 0; i < capacity; i++) {
        grown[i] = table[i];
    }
    for (uint32_t i = capacity; i < want; i++) {
        grown[i] = (memfd_t *)0;
    }
    memfd_t **old = table;
    table = grown;
    capacity = want;
    if (old) {
        kfree(old);
    }
    return 0;
}

void memfd_init(void) {
    /* Once, before the heap holds anything of ours; a host test that resets
       the heap under it calls this again, and a table from before the reset
       would be a pointer into memory that is no longer the table. */
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    table = (memfd_t **)0;
    capacity = 0;
    spin_unlock_irqrestore(&memfd_lock, f);
}

struct memfd *memfd_create_object(const char *name) {
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    uint32_t i = 0;
    for (;;) {
        for (; i < capacity; i++) {
            if (!table[i] || !table[i]->used) {
                break;
            }
        }
        if (i < capacity) {
            break;
        }
        if (memfd_grow_locked() != 0) {
            spin_unlock_irqrestore(&memfd_lock, f);
            return (memfd_t *)0;
        }
    }
    memfd_t *m = table[i];
    if (!m) {
        /* Objects arrive MEMFD_INITIAL at a time - one page's worth - because
           this kernel's kmalloc hands a fresh growth the whole page it
           mapped and never splits it, so 65,536 objects made one call each
           would be 65,536 pages. The capacity is always a multiple of
           MEMFD_INITIAL, so a chunk never crosses it. */
        uint32_t first = i - (i % MEMFD_INITIAL);
        memfd_t *chunk = (memfd_t *)kmalloc(sizeof(memfd_t) * MEMFD_INITIAL);
        if (!chunk) {
            spin_unlock_irqrestore(&memfd_lock, f);
            return (memfd_t *)0;
        }
        for (uint32_t j = 0; j < MEMFD_INITIAL; j++) {
            chunk[j].used = 0;
            chunk[j].generation = 0;
            chunk[j].slot = first + j;
            table[first + j] = &chunk[j];
        }
        m = table[i];
    }
    m->used = 1;
    m->refs = 1;
    m->pages = 0;
    m->size = 0;
    m->frames = (uint64_t *)0;
    m->seals = 0;
    if (name) {
        k_strlcpy(m->name, name, MEMFD_NAME_MAX);
    } else {
        m->name[0] = '\0';
    }
    spin_unlock_irqrestore(&memfd_lock, f);
    return m;
}

uint32_t memfd_slot(const struct memfd *m) {
    return m ? m->slot : 0;
}

uint16_t memfd_generation(const struct memfd *m) {
    return m ? m->generation : 0;
}

struct memfd *memfd_by_tag(uint32_t slot, uint16_t generation) {
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    memfd_t *m = slot < capacity ? table[slot] : (memfd_t *)0;
    int ok = m && m->used && m->generation == generation;
    spin_unlock_irqrestore(&memfd_lock, f);
    return ok ? m : (memfd_t *)0;
}

void memfd_reference(struct memfd *m) {
    if (!m) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    m->refs++;
    spin_unlock_irqrestore(&memfd_lock, f);
}

void memfd_unref(struct memfd *m) {
    if (!m) {
        return;
    }
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    if (--m->refs > 0) {
        spin_unlock_irqrestore(&memfd_lock, f);
        return;
    }
    uint64_t *frames = m->frames;
    uint32_t pages = m->pages;
    m->used = 0;
    m->frames = (uint64_t *)0;
    m->pages = 0;
    m->size = 0;
    m->seals = 0;
    m->generation++;
    spin_unlock_irqrestore(&memfd_lock, f);

    for (uint32_t i = 0; i < pages; i++) {
        physical_memory_free_frame(frames[i]);
    }
    kfree(frames);
}

void memfd_region_reference(struct memfd *m) { memfd_reference(m); }
void memfd_region_unref(struct memfd *m) { memfd_unref(m); }

int memfd_truncate(struct memfd *m, uint64_t size) {
    if (!m) {
        return -1;
    }
    uint64_t want_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (want_pages > MEMFD_MAX_PAGES) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    if (!m->used) {
        spin_unlock_irqrestore(&memfd_lock, f);
        return -1;
    }
    if (want_pages > m->pages && (m->seals & MEMFD_SEAL_GROW)) {
        spin_unlock_irqrestore(&memfd_lock, f);
        return -1;
    }
    if (want_pages < m->pages) {
        spin_unlock_irqrestore(&memfd_lock, f);
        return -1;
    }
    if (want_pages == m->pages) {
        m->size = size;
        spin_unlock_irqrestore(&memfd_lock, f);
        return 0;
    }
    uint64_t *grown = (uint64_t *)kmalloc((size_t)want_pages * sizeof(uint64_t));
    if (!grown) {
        spin_unlock_irqrestore(&memfd_lock, f);
        return -1;
    }
    for (uint32_t i = 0; i < m->pages; i++) {
        grown[i] = m->frames[i];
    }
    for (uint64_t i = m->pages; i < want_pages; i++) {
        uint64_t phys = physical_memory_try_alloc_frame();
        if (phys == 0) {
            for (uint64_t j = m->pages; j < i; j++) {
                physical_memory_free_frame(grown[j]);
            }
            kfree(grown);
            spin_unlock_irqrestore(&memfd_lock, f);
            return -1;
        }
        k_memset((void *)phys, 0, PAGE_SIZE);
        grown[i] = phys;
    }
    uint64_t *old = m->frames;
    m->frames = grown;
    m->pages = (uint32_t)want_pages;
    m->size = size;
    spin_unlock_irqrestore(&memfd_lock, f);
    kfree(old);
    return 0;
}

uint64_t memfd_size(const struct memfd *m) {
    if (!m) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    uint64_t s = m->used ? m->size : 0;
    spin_unlock_irqrestore(&memfd_lock, f);
    return s;
}

uint64_t memfd_frame(const struct memfd *m, uint32_t index) {
    if (!m) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    uint64_t phys = (m->used && index < m->pages) ? m->frames[index] : 0;
    spin_unlock_irqrestore(&memfd_lock, f);
    return phys;
}

int memfd_add_seals(struct memfd *m, uint32_t seals) {
    if (!m) {
        return -1;
    }
    uint32_t known = MEMFD_SEAL_SEAL | MEMFD_SEAL_SHRINK | MEMFD_SEAL_GROW |
                     MEMFD_SEAL_WRITE;
    if (seals & ~known) {
        return -1;
    }
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    if (!m->used || (m->seals & MEMFD_SEAL_SEAL)) {
        spin_unlock_irqrestore(&memfd_lock, f);
        return -1;
    }
    m->seals |= seals;
    spin_unlock_irqrestore(&memfd_lock, f);
    return 0;
}

uint32_t memfd_get_seals(const struct memfd *m) {
    if (!m) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    uint32_t s = m->used ? m->seals : 0;
    spin_unlock_irqrestore(&memfd_lock, f);
    return s;
}

int memfd_may_write(const struct memfd *m) {
    if (!m) {
        return 0;
    }
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    int may = m->used && !(m->seals & MEMFD_SEAL_WRITE);
    spin_unlock_irqrestore(&memfd_lock, f);
    return may;
}

const char *memfd_name(const struct memfd *m) {
    return (m && m->used) ? m->name : "";
}

int memfd_in_use(void) {
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    int n = 0;
    for (uint32_t i = 0; i < capacity; i++) {
        if (table[i] && table[i]->used) {
            n++;
        }
    }
    spin_unlock_irqrestore(&memfd_lock, f);
    return n;
}

const char *memfd_first_live_name(void) {
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    const char *name = "";
    for (uint32_t i = 0; i < capacity; i++) {
        if (table[i] && table[i]->used) {
            name = table[i]->name;
            break;
        }
    }
    spin_unlock_irqrestore(&memfd_lock, f);
    return name;
}

uint32_t memfd_pages_held(void) {
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    uint32_t n = 0;
    for (uint32_t i = 0; i < capacity; i++) {
        if (table[i] && table[i]->used) {
            n += table[i]->pages;
        }
    }
    spin_unlock_irqrestore(&memfd_lock, f);
    return n;
}

uint32_t memfd_capacity(void) {
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    uint32_t c = capacity;
    spin_unlock_irqrestore(&memfd_lock, f);
    return c;
}
