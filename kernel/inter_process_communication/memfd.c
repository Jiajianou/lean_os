#include "memfd.h"

#include "library/libk.h"
#include "library/spinlock.h"
#include "memory_management/heap.h"
#include "memory_management/pmm.h"
#include "process/proc.h"

static spinlock_t memfd_lock;

typedef struct memfd {
    int used;
    int refs;
    uint16_t generation;
    uint32_t pages;
    uint64_t size;
    uint64_t *frames;
    uint32_t seals;
    char name[MEMFD_NAME_MAX];
} memfd_t;

static memfd_t table[MEMFD_MAX];

void memfd_init(void) {
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    for (int i = 0; i < MEMFD_MAX; i++) {
        table[i].used = 0;
    }
    spin_unlock_irqrestore(&memfd_lock, f);
}

struct memfd *memfd_create_obj(const char *name) {
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    for (int i = 0; i < MEMFD_MAX; i++) {
        if (table[i].used) {
            continue;
        }
        table[i].used = 1;
        table[i].refs = 1;
        table[i].pages = 0;
        table[i].size = 0;
        table[i].frames = (uint64_t *)0;
        table[i].seals = 0;
        if (name) {
            k_strlcpy(table[i].name, name, MEMFD_NAME_MAX);
        } else {
            table[i].name[0] = '\0';
        }
        spin_unlock_irqrestore(&memfd_lock, f);
        return &table[i];
    }
    spin_unlock_irqrestore(&memfd_lock, f);
    return (memfd_t *)0;
}

uint8_t memfd_slot(const struct memfd *m) {
    return m ? (uint8_t)(m - table) : 0;
}

uint16_t memfd_generation(const struct memfd *m) {
    return m ? m->generation : 0;
}

struct memfd *memfd_by_tag(uint8_t slot, uint16_t generation) {
    if (slot >= MEMFD_MAX) {
        return (memfd_t *)0;
    }
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    memfd_t *m = &table[slot];
    int ok = m->used && m->generation == generation;
    spin_unlock_irqrestore(&memfd_lock, f);
    return ok ? m : (memfd_t *)0;
}

void memfd_ref(struct memfd *m) {
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
        pmm_free_frame(frames[i]);
    }
    kfree(frames);
}

void memfd_region_ref(struct memfd *m) { memfd_ref(m); }
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
        uint64_t phys = pmm_try_alloc_frame();
        if (phys == 0) {
            for (uint64_t j = m->pages; j < i; j++) {
                pmm_free_frame(grown[j]);
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
    for (int i = 0; i < MEMFD_MAX; i++) {
        if (table[i].used) {
            n++;
        }
    }
    spin_unlock_irqrestore(&memfd_lock, f);
    return n;
}

const char *memfd_first_live_name(void) {
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    const char *name = "";
    for (int i = 0; i < MEMFD_MAX; i++) {
        if (table[i].used) {
            name = table[i].name;
            break;
        }
    }
    spin_unlock_irqrestore(&memfd_lock, f);
    return name;
}

uint32_t memfd_pages_held(void) {
    uint64_t f = spin_lock_irqsave(&memfd_lock);
    uint32_t n = 0;
    for (int i = 0; i < MEMFD_MAX; i++) {
        if (table[i].used) {
            n += table[i].pages;
        }
    }
    spin_unlock_irqrestore(&memfd_lock, f);
    return n;
}
