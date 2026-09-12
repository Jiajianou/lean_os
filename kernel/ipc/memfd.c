/* kernel/ipc/memfd.c - M120. See memfd.h for what this is for, why shm
 * was not it, and why the lifetime rule is the whole of the difficulty. */
#include "memfd.h"

#include "lib/libk.h"
#include "lib/spinlock.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "proc/proc.h" /* PAGE_SIZE - the same 4096 every other file in this kernel takes from here */

/* One lock for the table and every object in it. Interrupts off while
 * held: a dying task unrefs its descriptors and its regions, and that path
 * is reachable from a timer tick delivering SIGKILL. */
static spinlock_t memfd_lock;

typedef struct memfd {
    int used;
    int refs;            /* descriptors + regions, which are the same count - see memfd.h */
    uint16_t generation; /* incremented each time this slot is handed out again (M54's trick) */
    uint32_t pages;
    uint64_t size;       /* bytes, which is NOT pages * PAGE_SIZE: ftruncate takes a byte count and fstat has to report it back */
    uint64_t *frames;    /* `pages` physical addresses, each zeroed when it was claimed */
    uint32_t seals;
    char name[MEMFD_NAME_MAX];
} memfd_t;

static memfd_t table[MEMFD_MAX];

/* Marks every slot free, and nothing else - which is less than it looks
 * like it should do, and deliberately.
 *
 * Every other field is set by memfd_create_obj on the way in and cleared by
 * memfd_unref on the way out, so a free slot is already clean and clearing
 * it again here would be code that cannot be wrong and cannot be right.
 * (The mutation harness is what made that concrete: it changed two of those
 * redundant assignments and nothing could notice, which is the definition
 * of a line with nothing behind it.)
 *
 * The generation is deliberately NOT reset: at boot every slot is zero
 * anyway, and a reset that happened while anything still held a tag would
 * be the one thing the generation exists to prevent. */
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
    /* Both halves, and the generation is the one that matters: a slot
     * reused by a different program must not answer for the one that asked.
     * A stale tag is NULL and the caller's fault becomes a SIGSEGV rather
     * than a window into somebody else's memory. */
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
    /* The last holder - descriptor or mapping - has gone. Everything is
     * taken out of the slot under the lock and freed after it, so that no
     * other task can find a half-dismantled object through a tag. */
    uint64_t *frames = m->frames;
    uint32_t pages = m->pages;
    m->used = 0;
    m->frames = (uint64_t *)0;
    m->pages = 0;
    m->size = 0;
    m->seals = 0;
    m->generation++; /* every tag naming this slot is now stale, which is the point */
    spin_unlock_irqrestore(&memfd_lock, f);

    for (uint32_t i = 0; i < pages; i++) {
        pmm_free_frame(frames[i]);
    }
    kfree(frames);
}

/* memfd_ref and memfd_unref under the other name. Two spellings of one
 * count, so that a call site says which lifetime it is about - see
 * memfd.h. A comment rather than a distinction. */
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
        /* Refused, seal or no seal. A mapping whose frames were freed
         * under it would read whatever those frames become next, and this
         * kernel has no SIGBUS-on-truncated-page machinery to answer with
         * instead - see memfd.h. A caller that wants the memory back
         * closes the descriptor. */
        spin_unlock_irqrestore(&memfd_lock, f);
        return -1;
    }
    if (want_pages == m->pages) {
        /* A size change within the last page: real, and recorded, because
         * fstat reports bytes and a caller may have asked for 100 and then
         * 200 of the same page. */
        m->size = size;
        spin_unlock_irqrestore(&memfd_lock, f);
        return 0;
    }
    /* Growing. The new array is built beside the old one and swapped in,
     * so a failure part-way leaves the object exactly as it was - which
     * matters because the frames are the thing a mapping is already
     * looking at. */
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
        /* **Zeroed before anybody can see it.** A frame this machine used
         * for something else reaching a process that may not read a file
         * is the worst thing this object could do, and this is the one
         * line that stops it. */
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
        return -1; /* a seal this kernel does not implement is refused, not ignored - a promise nobody keeps is worse than none */
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
    /* No lock and no copy: the name is written once, before any other task
     * can reach the object, and never again. Saying so here is cheaper than
     * a buffer the caller would have to own. */
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
