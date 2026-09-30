#include "image_cache.h"

#include "drivers/kernel_log.h"
#include "drivers/pit.h"
#include "file_system/virtual_file_system.h"
#include "library/kernel_library.h"
#include "memory_management/heap.h"
#include "memory_management/physical_memory.h"
#include "memory_management/virtual_memory.h"
#include "paths.h"
#include "process/process.h"
#include "scheduler/scheduler.h"

/* M198. Every exec read the whole program into a kernel allocation and then
   copied it, a page at a time, into frames of the new process's own - 306 MB
   of browser, eight times for one browser launch, each copy a second of a
   core and 300 MB of memory for text that is the same in every one of them.
   A program's read-only segments are now read once, into frames kept here,
   and every process running it maps those frames: each mapping holds a
   reference, so a process exiting, forking or execing drops only its own, and
   the frames go back when the last mapping and this cache have both let go.
   Its writable segments are copied from the kept frames, a page each, because
   every process must have its own.

   An entry is keyed by the inode, the file's size and the inode's write
   generation, which leanfs bumps on every write, truncation and release - so
   a program rewritten on disk is read again, and a stale copy can never be
   handed out, only left unused until it is evicted. Anything this does not
   understand - an interpreter, segments that share a page, program headers
   past the first page - is left to the loader that copies. */

#define IMAGE_CACHE_ENTRIES 16
#define IMAGE_CACHE_MAX_SEGMENTS 8
#define IMAGE_CACHE_HEADER_BYTES 4096u
#define IMAGE_CACHE_READ_CHUNK (1024u * 1024u)
/* A frame's reference count is eight bits; a page shared by more mappings
   than this is copied rather than shared, so a count never reaches the
   ceiling that panics. */
#define IMAGE_CACHE_SHARE_LIMIT 200u

#define PT_LOAD 1
#define PT_INTERP 3
#define PF_X 0x1
#define PF_W 0x2
#define ET_EXEC 2

typedef struct __attribute__((packed)) {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} image_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} image_phdr_t;

typedef struct {
    uint64_t start;
    uint64_t pages;
    uint64_t *frames;
    uint64_t frames_table_pages;
    int writable;
    int executable;
} image_segment_t;

struct image_cache_entry {
    int used;
    /* M201: being read, outside cache_lock. Its users count holds it against
       eviction; anybody asking for the same file waits for it to finish. */
    int loading;
    uint64_t reserved_pages;
    uint32_t inode;
    uint32_t generation;
    uint32_t size;
    uint64_t last_used;
    int users;
    uint64_t pages;
    int segment_count;
    image_segment_t segments[IMAGE_CACHE_MAX_SEGMENTS];
    uint8_t header[IMAGE_CACHE_HEADER_BYTES];
};

static image_cache_entry_t entries[IMAGE_CACHE_ENTRIES];
static sleep_lock_t cache_lock;
static uint64_t use_clock;
static uint64_t cached_pages;
static image_cache_statistics_t statistics;

static uint64_t budget_pages(void) {
    return physical_memory_total_frame_count() / 8u;
}

/* Gives back an entry's frames and tables without touching cached_pages,
   which is the caller's to settle - a loading entry's pages are counted in
   its reservation, a published one's in cached_pages itself. */
static void release_frames(image_cache_entry_t *e) {
    for (int s = 0; s < e->segment_count; s++) {
        image_segment_t *segment = &e->segments[s];
        if (segment->frames) {
            for (uint64_t p = 0; p < segment->pages; p++) {
                if (segment->frames[p]) {
                    physical_memory_free_frame(segment->frames[p]);
                }
            }
            physical_memory_free_contiguous((uint64_t)(uintptr_t)segment->frames,
                                            segment->frames_table_pages);
            segment->frames = (uint64_t *)0;
        }
    }
    e->pages = 0;
    e->segment_count = 0;
}

static void free_entry(image_cache_entry_t *e) {
    cached_pages -= e->pages;
    release_frames(e);
    e->used = 0;
}

static int evict_one(void) {
    image_cache_entry_t *victim = (image_cache_entry_t *)0;
    for (int i = 0; i < IMAGE_CACHE_ENTRIES; i++) {
        image_cache_entry_t *e = &entries[i];
        if (e->used && !e->loading && e->users == 0 &&
            (!victim || e->last_used < victim->last_used)) {
            victim = e;
        }
    }
    if (!victim) {
        return 0;
    }
    free_entry(victim);
    statistics.evictions++;
    return 1;
}

static int path_is_on_leanfs(const char *path) {
    uint32_t n = (uint32_t)k_strlen(PATH_DEV);
    if (k_memcmp(path, PATH_DEV, n) == 0 && (path[n] == '\0' || path[n] == '/')) {
        return 0;
    }
    n = (uint32_t)k_strlen(PATH_PROCESS);
    if (k_memcmp(path, PATH_PROCESS, n) == 0 && (path[n] == '\0' || path[n] == '/')) {
        return 0;
    }
    return 1;
}

static int read_exact_at(int handle, uint8_t *buffer, uint64_t length, uint64_t offset) {
    uint64_t done = 0;
    while (done < length) {
        int64_t n = virtual_file_system_handle_read(handle, buffer + done, (size_t)(length - done),
                                                    (uint32_t)(offset + done));
        if (n <= 0) {
            return -1;
        }
        done += (uint64_t)n;
    }
    return 0;
}

/* The segment's bytes go from the file into its frames a chunk at a time,
   through one bounce buffer: the program is never whole in kernel memory. */
static int fill_segment(int handle, image_segment_t *segment, const image_phdr_t *ph,
                        uint8_t *bounce, uint64_t bounce_bytes) {
    uint64_t done = 0;
    uint64_t lead = ph->p_vaddr - segment->start;
    while (done < ph->p_filesz) {
        uint64_t piece = ph->p_filesz - done;
        if (piece > bounce_bytes) {
            piece = bounce_bytes;
        }
        if (read_exact_at(handle, bounce, piece, ph->p_offset + done) != 0) {
            return -1;
        }
        uint64_t copied = 0;
        while (copied < piece) {
            uint64_t at = lead + done + copied;
            uint64_t page = at / PAGE_SIZE;
            uint64_t within = at % PAGE_SIZE;
            uint64_t take = PAGE_SIZE - within;
            if (take > piece - copied) {
                take = piece - copied;
            }
            k_memcpy((uint8_t *)segment->frames[page] + within, bounce + copied, take);
            copied += take;
        }
        done += piece;
    }
    return 0;
}

/* Reads the header and lays the segments out; returns the pages the entry
   will take - frames and frame tables - or 0 for a program this cache does
   not take. Nothing is allocated. */
static uint64_t plan_entry(image_cache_entry_t *e, int handle, const leanfs_stat_t *st) {
    uint32_t header_bytes = st->size < IMAGE_CACHE_HEADER_BYTES ? st->size : IMAGE_CACHE_HEADER_BYTES;
    if (header_bytes < sizeof(image_ehdr_t) ||
        read_exact_at(handle, e->header, header_bytes, 0) != 0) {
        return 0;
    }
    const image_ehdr_t *eh = (const image_ehdr_t *)e->header;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
        eh->e_ident[3] != 'F' || eh->e_type != ET_EXEC ||
        eh->e_phentsize != sizeof(image_phdr_t) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(image_phdr_t) > header_bytes) {
        return 0;
    }
    const image_phdr_t *ph = (const image_phdr_t *)(e->header + eh->e_phoff);
    uint64_t pages = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            e->segment_count = 0;
            return 0;
        }
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) {
            continue;
        }
        if (e->segment_count == IMAGE_CACHE_MAX_SEGMENTS || ph[i].p_filesz > ph[i].p_memsz ||
            ph[i].p_offset + ph[i].p_filesz > st->size || ph[i].p_vaddr < USER_IMAGE_BASE ||
            ph[i].p_memsz > USER_IMAGE_LIMIT - ph[i].p_vaddr) {
            e->segment_count = 0;
            return 0;
        }
        image_segment_t *segment = &e->segments[e->segment_count];
        segment->start = ph[i].p_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t end = (ph[i].p_vaddr + ph[i].p_memsz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        segment->pages = (end - segment->start) / PAGE_SIZE;
        segment->writable = (ph[i].p_flags & PF_W) != 0;
        segment->executable = (ph[i].p_flags & PF_X) != 0;
        for (int other = 0; other < e->segment_count; other++) {
            const image_segment_t *o = &e->segments[other];
            if (segment->start < o->start + o->pages * PAGE_SIZE &&
                o->start < segment->start + segment->pages * PAGE_SIZE) {
                e->segment_count = 0;
                return 0;
            }
        }
        pages += segment->pages + (segment->pages * sizeof(uint64_t) + PAGE_SIZE - 1) / PAGE_SIZE;
        e->segment_count++;
    }
    return e->segment_count ? pages : 0;
}

/* Allocates the frames the plan asked for and reads the file into them.
   Runs without cache_lock: the entry is the loader's alone until it is
   published, and its pages are already counted in its reservation. */
static int fill_entry(image_cache_entry_t *e, int handle) {
    /* The bounce buffer is frames, not heap: the kernel heap keeps whatever
       it grows to, and a megabyte of it per first exec is a megabyte the
       machine never gets back. A fragmented machine gets a smaller one. */
    uint64_t bounce_pages = IMAGE_CACHE_READ_CHUNK / PAGE_SIZE;
    uint64_t bounce_phys = 0;
    while (bounce_pages > 0) {
        bounce_phys = physical_memory_try_alloc_contiguous_anywhere(bounce_pages);
        if (bounce_phys) {
            break;
        }
        bounce_pages /= 4;
    }
    if (!bounce_phys) {
        return -1;
    }
    uint8_t *bounce = (uint8_t *)(uintptr_t)bounce_phys;
    uint64_t bounce_bytes = bounce_pages * PAGE_SIZE;
    const image_ehdr_t *eh = (const image_ehdr_t *)e->header;
    const image_phdr_t *ph = (const image_phdr_t *)(e->header + eh->e_phoff);
    int failed = 0;
    int load = 0;
    for (uint16_t i = 0; i < eh->e_phnum && !failed; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) {
            continue;
        }
        image_segment_t *segment = &e->segments[load++];
        /* The table of a segment's frames is frames too, for the reason the
           bounce buffer is: the heap never shrinks, and a table the cache
           keeps for as long as the program stays cached would pin whatever
           the heap grew to around it. */
        segment->frames_table_pages = (segment->pages * sizeof(uint64_t) + PAGE_SIZE - 1) / PAGE_SIZE;
        segment->frames = (uint64_t *)(uintptr_t)physical_memory_try_alloc_contiguous_anywhere(
            segment->frames_table_pages);
        if (!segment->frames) {
            segment->frames_table_pages = 0;
            failed = 1;
            break;
        }
        k_memset(segment->frames, 0, segment->frames_table_pages * PAGE_SIZE);
        e->pages += segment->frames_table_pages;
        for (uint64_t p = 0; p < segment->pages; p++) {
            segment->frames[p] = physical_memory_try_alloc_frame();
            if (!segment->frames[p]) {
                failed = 1;
                break;
            }
            e->pages++;
            k_memset((void *)segment->frames[p], 0, PAGE_SIZE);
        }
        if (!failed && fill_segment(handle, segment, &ph[i], bounce, bounce_bytes) != 0) {
            failed = 1;
        }
    }
    physical_memory_free_contiguous(bounce_phys, bounce_pages);
    return failed ? -1 : 0;
}

/* M201. The whole program used to be read with cache_lock held - three
   hundred megabytes of browser, a couple of seconds off a USB stick - and
   every other exec on the machine waited behind it, whatever it was
   launching. The slot is claimed and its pages reserved under the lock, the
   file is read without it, and the entry is published under it again.
   Anybody asking for the same file meanwhile waits for that one load rather
   than starting a second. */
#define IMAGE_CACHE_LOAD_POLL_MS 2u

image_cache_entry_t *image_cache_acquire(const char *path) {
    if (!path || !path_is_on_leanfs(path)) {
        return (image_cache_entry_t *)0;
    }
    int handle = virtual_file_system_open(path, 0);
    if (handle < 0) {
        return (image_cache_entry_t *)0;
    }
    leanfs_stat_t st;
    if (virtual_file_system_handle_stat(handle, &st) != 0 || st.is_directory) {
        virtual_file_system_handle_close(handle);
        return (image_cache_entry_t *)0;
    }

    sleep_lock_acquire(&cache_lock);
    image_cache_entry_t *found = (image_cache_entry_t *)0;
    for (;;) {
        int waiting = 0;
        for (int i = 0; i < IMAGE_CACHE_ENTRIES; i++) {
            image_cache_entry_t *e = &entries[i];
            if (!e->used || e->inode != st.inode) {
                continue;
            }
            if (e->generation == st.generation && e->size == st.size) {
                if (e->loading) {
                    waiting = 1;
                } else {
                    found = e;
                }
                break;
            }
            if (e->users == 0 && !e->loading) {
                free_entry(e);
            }
        }
        if (!waiting) {
            break;
        }
        sleep_lock_release(&cache_lock);
        scheduler_sleep_ms(IMAGE_CACHE_LOAD_POLL_MS);
        sleep_lock_acquire(&cache_lock);
    }
    if (found) {
        statistics.hits++;
        found->users++;
        found->last_used = ++use_clock;
        sleep_lock_release(&cache_lock);
        virtual_file_system_handle_close(handle);
        return found;
    }

    image_cache_entry_t *slot = (image_cache_entry_t *)0;
    for (int i = 0; i < IMAGE_CACHE_ENTRIES && !slot; i++) {
        if (!entries[i].used) {
            slot = &entries[i];
        }
    }
    if (!slot && evict_one()) {
        for (int i = 0; i < IMAGE_CACHE_ENTRIES && !slot; i++) {
            if (!entries[i].used) {
                slot = &entries[i];
            }
        }
    }
    if (!slot) {
        statistics.refusals++;
        sleep_lock_release(&cache_lock);
        virtual_file_system_handle_close(handle);
        return (image_cache_entry_t *)0;
    }
    k_memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->loading = 1;
    slot->users = 1;
    slot->inode = st.inode;
    slot->generation = st.generation;
    slot->size = st.size;
    sleep_lock_release(&cache_lock);

    int ok = 0;
    uint64_t pages = plan_entry(slot, handle, &st);
    if (pages) {
        sleep_lock_acquire(&cache_lock);
        while (cached_pages + pages > budget_pages() && evict_one()) {
        }
        if (cached_pages + pages <= budget_pages()) {
            cached_pages += pages;
            slot->reserved_pages = pages;
            ok = 1;
        }
        sleep_lock_release(&cache_lock);
    }
    if (ok && fill_entry(slot, handle) != 0) {
        ok = 0;
    }
    if (ok) {
        leanfs_stat_t after;
        ok = virtual_file_system_handle_stat(handle, &after) == 0 &&
             after.generation == st.generation && after.size == st.size;
    }

    sleep_lock_acquire(&cache_lock);
    cached_pages -= slot->reserved_pages;
    slot->reserved_pages = 0;
    if (ok) {
        cached_pages += slot->pages;
        slot->loading = 0;
        slot->last_used = ++use_clock;
        statistics.misses++;
        found = slot;
    } else {
        release_frames(slot);
        slot->loading = 0;
        slot->users = 0;
        slot->used = 0;
        statistics.refusals++;
    }
    sleep_lock_release(&cache_lock);
    virtual_file_system_handle_close(handle);
    return found;
}

void image_cache_release(image_cache_entry_t *e) {
    if (!e) {
        return;
    }
    sleep_lock_acquire(&cache_lock);
    e->users--;
    sleep_lock_release(&cache_lock);
}

const uint8_t *image_cache_header(const image_cache_entry_t *e, size_t *file_size) {
    *file_size = e->size;
    return e->header;
}

uint64_t image_cache_map(uint64_t pml4_phys, const image_cache_entry_t *e) {
    for (int s = 0; s < e->segment_count; s++) {
        const image_segment_t *segment = &e->segments[s];
        uint64_t flags = VIRTUAL_MEMORY_FLAG_USER;
        if (segment->writable) {
            flags |= VIRTUAL_MEMORY_FLAG_WRITABLE;
        }
        if (segment->executable) {
            flags |= VIRTUAL_MEMORY_FLAG_EXEC;
        }
        for (uint64_t p = 0; p < segment->pages; p++) {
            uint64_t kept = segment->frames[p];
            uint64_t phys;
            if (!segment->writable && physical_memory_frame_refs(kept) < IMAGE_CACHE_SHARE_LIMIT) {
                physical_memory_frame_reference(kept);
                phys = kept;
            } else {
                phys = physical_memory_try_alloc_frame();
                if (phys == 0) {
                    return 0;
                }
                k_memcpy((void *)phys, (const void *)kept, PAGE_SIZE);
            }
            if (virtual_memory_try_map_page_in(pml4_phys, segment->start + p * PAGE_SIZE, phys,
                                               flags) != 0) {
                physical_memory_free_frame(phys);
                return 0;
            }
        }
    }
    const image_ehdr_t *eh = (const image_ehdr_t *)e->header;
    kernel_log_debug("[elf] loaded, entry = 0x");
    kernel_log_log_hex64(KERNEL_LOG_DEBUG, eh->e_entry);
    kernel_log_debug("\n");
    return eh->e_entry;
}

/* Frames nobody but the cache holds: what an eviction would give back. A
   frame a running process also maps is that process's as much as the
   cache's, and stays in use whether or not the cache lets go of it. */
uint64_t image_cache_private_pages(void) {
    sleep_lock_acquire(&cache_lock);
    uint64_t count = 0;
    for (int i = 0; i < IMAGE_CACHE_ENTRIES; i++) {
        const image_cache_entry_t *e = &entries[i];
        if (!e->used || e->loading) {
            continue;
        }
        for (int s = 0; s < e->segment_count; s++) {
            const image_segment_t *segment = &e->segments[s];
            count += segment->frames_table_pages;
            for (uint64_t p = 0; p < segment->pages; p++) {
                if (segment->frames[p] && physical_memory_frame_refs(segment->frames[p]) == 1) {
                    count++;
                }
            }
        }
    }
    sleep_lock_release(&cache_lock);
    return count;
}

void image_cache_statistics(image_cache_statistics_t *out) {
    sleep_lock_acquire(&cache_lock);
    *out = statistics;
    out->cached_pages = cached_pages;
    sleep_lock_release(&cache_lock);
}
