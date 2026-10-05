#include "memory_management/virtual_memory.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

void panic(const char *message);

#define HEAP_SPAN (64ULL * 1024 * 1024)
#define PAGE 4096ULL
#define MAX_PAGES (HEAP_SPAN / PAGE)

static uint8_t *region;
static uint8_t mapped[MAX_PAGES];
static uint64_t mapped_phys[MAX_PAGES];
static uint64_t mapped_count;

static int map_fail_after = -1;
static int maps_attempted;

void fake_virtual_memory_reset(void);
uint64_t fake_virtual_memory_mapped_pages(void);
void fake_virtual_memory_fail_map_after(int n);

static void ensure_region(void) {
    if (region) {
        return;
    }
    void *p = mmap(NULL, HEAP_SPAN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        panic("fake_vmm: could not reserve a heap region");
    }
    region = (uint8_t *)p;
}

uint64_t virtual_memory_kernel_heap_base(void) {
    ensure_region();
    return (uint64_t)(uintptr_t)region;
}

void fake_virtual_memory_reset(void) {
    ensure_region();
    for (uint64_t i = 0; i < MAX_PAGES; i++) {
        mapped[i] = 0;
        mapped_phys[i] = 0;
    }
    mapped_count = 0;
    map_fail_after = -1;
    maps_attempted = 0;
    if (mmap(region, HEAP_SPAN, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) {
        panic("fake_vmm: could not reset the heap region");
    }
}

uint64_t fake_virtual_memory_mapped_pages(void) { return mapped_count; }

void fake_virtual_memory_fail_map_after(int n) {
    map_fail_after = n;
    maps_attempted = 0;
}

/* M225: user pages, modelled - opt in (fake_virtual_memory_model_user_pages).
   The fault path asks whether a page is present, maps it if absent, and
   since M225 looks again at what it mapped; with every user range answered
   "mapped", as this fake has always done, none of that can be followed. */
#define USER_MODEL_PAGES 64
static struct {
    uint64_t pml4;
    uint64_t virt;
    uint64_t phys;
    uint64_t flags;
    int used;
} user_pages[USER_MODEL_PAGES];
static int user_model_on;

static int in_heap_region(uint64_t virt) {
    ensure_region();
    uint64_t base = (uint64_t)(uintptr_t)region;
    return virt >= base && virt < base + HEAP_SPAN;
}

static int modelled(uint64_t virt) {
    return user_model_on && !in_heap_region(virt);
}

static int user_page_find(uint64_t pml4, uint64_t virt) {
    uint64_t page = virt & ~(PAGE - 1);
    for (int i = 0; i < USER_MODEL_PAGES; i++) {
        if (user_pages[i].used && user_pages[i].pml4 == pml4 && user_pages[i].virt == page) {
            return i;
        }
    }
    return -1;
}

void fake_virtual_memory_model_user_pages(int on);
static void model_off(void) {
    fake_virtual_memory_model_user_pages(0);
}

void fake_spinlock_at_test_end(void (*clear)(void));
void fake_virtual_memory_model_user_pages(int on) {
    for (int i = 0; i < USER_MODEL_PAGES; i++) {
        user_pages[i].used = 0;
    }
    user_model_on = on;
    if (on) {
        fake_spinlock_at_test_end(model_off);
    }
}

int fake_virtual_memory_user_page(uint64_t virt, uint64_t *phys_out, uint64_t *flags_out);
int fake_virtual_memory_user_page(uint64_t virt, uint64_t *phys_out, uint64_t *flags_out) {
    uint64_t page = virt & ~(PAGE - 1);
    for (int i = 0; i < USER_MODEL_PAGES; i++) {
        if (user_pages[i].used && user_pages[i].virt == page) {
            if (phys_out) {
                *phys_out = user_pages[i].phys;
            }
            if (flags_out) {
                *flags_out = user_pages[i].flags;
            }
            return 1;
        }
    }
    return 0;
}

uint64_t virtual_memory_lookup_frame(uint64_t pml4_phys, uint64_t virt) {
    int i = modelled(virt) ? user_page_find(pml4_phys, virt) : -1;
    if (i < 0) {
        return 0;
    }
    /* The hardware's own bits, which is what the real one returns. */
    return user_pages[i].phys | 1u |
           ((user_pages[i].flags & VIRTUAL_MEMORY_FLAG_WRITABLE) ? 2u : 0u) |
           ((user_pages[i].flags & VIRTUAL_MEMORY_FLAG_USER) ? 4u : 0u);
}

uint64_t virtual_memory_protect_range_in(uint64_t pml4_phys, uint64_t start, uint64_t end,
                                         uint64_t flags) {
    uint64_t changed = 0;
    for (int i = 0; i < USER_MODEL_PAGES; i++) {
        if (user_pages[i].used && user_pages[i].pml4 == pml4_phys &&
            user_pages[i].virt >= start && user_pages[i].virt < end) {
            user_pages[i].flags = flags;
            changed++;
        }
    }
    return changed;
}

static uint64_t page_index(uint64_t virt) {
    ensure_region();
    uint64_t base = (uint64_t)(uintptr_t)region;
    if (virt < base || virt >= base + HEAP_SPAN) {
        panic("fake_vmm: mapping outside the reserved heap region "
              "(the test asked for more heap than the fake reserves)");
    }
    return (virt - base) / PAGE;
}

void virtual_memory_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)flags;
    uint64_t i = page_index(virt);
    if (mapped[i]) {
        panic("vmm_map_page: address already mapped");
    }
    mapped[i] = 1;
    mapped_phys[i] = phys;
    mapped_count++;
}

int virtual_memory_try_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)pml4_phys;
    if (map_fail_after >= 0 && maps_attempted >= map_fail_after) {
        maps_attempted++;
        return -1;
    }
    maps_attempted++;
    virtual_memory_map_page(virt, phys, flags);
    return 0;
}

int virtual_memory_try_map_page_if_absent(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                                          uint64_t flags) {
    if (modelled(virt)) {
        if (user_page_find(pml4_phys, virt) >= 0) {
            return 1;
        }
        for (int i = 0; i < USER_MODEL_PAGES; i++) {
            if (!user_pages[i].used) {
                user_pages[i].used = 1;
                user_pages[i].pml4 = pml4_phys;
                user_pages[i].virt = virt & ~(PAGE - 1);
                user_pages[i].phys = phys;
                user_pages[i].flags = flags;
                return 0;
            }
        }
        return -1;
    }
    return virtual_memory_try_map_page_in(pml4_phys, virt, phys, flags);
}

uint64_t virtual_memory_unmap_page_take(uint64_t pml4_phys, uint64_t virt) {
    if (modelled(virt)) {
        int at = user_page_find(pml4_phys, virt);
        if (at < 0) {
            return 0;
        }
        user_pages[at].used = 0;
        return user_pages[at].phys;
    }
    (void)pml4_phys;
    uint64_t i = page_index(virt);
    if (!mapped[i]) {
        return 0;
    }
    uint64_t phys = mapped_phys[i];
    mapped[i] = 0;
    mapped_phys[i] = 0;
    mapped_count--;
    return phys;
}

void virtual_memory_unmap_page(uint64_t virt) {
    uint64_t i = page_index(virt);
    if (!mapped[i]) {
        panic("vmm_unmap_page: address not mapped");
    }
    mapped[i] = 0;
    mapped_phys[i] = 0;
    mapped_count--;
}

static uint64_t cow_breaks;
static uint64_t unmaps_in;

uint64_t fake_virtual_memory_cow_breaks(void) { return cow_breaks; }
uint64_t fake_virtual_memory_unmaps_in(void) { return unmaps_in; }

int virtual_memory_user_range_ok(uint64_t pml4_phys, uint64_t virt, uint64_t length, int need_write) {
    if (length == 0) {
        return 1;
    }
    if (virt + length < virt) {
        return 0;
    }
    if (modelled(virt)) {
        for (uint64_t p = virt & ~(PAGE - 1); p < virt + length; p += PAGE) {
            int i = user_page_find(pml4_phys, p);
            if (i < 0 || (need_write && !(user_pages[i].flags & VIRTUAL_MEMORY_FLAG_WRITABLE))) {
                return 0;
            }
        }
    }
    return 1;
}

int virtual_memory_cow_break(uint64_t pml4_phys, uint64_t virt) {
    (void)pml4_phys;
    (void)virt;
    cow_breaks++;
    return 0;
}

int virtual_memory_unmap_page_in(uint64_t pml4_phys, uint64_t virt) {
    (void)pml4_phys;
    (void)virt;
    unmaps_in++;
    return 0;
}

static int other_cpu_flushes;

void virtual_memory_flush_other_cpus(uint64_t pml4_phys) {
    (void)pml4_phys;
    other_cpu_flushes++;
}

int fake_vmm_other_cpu_flushes(void) { return other_cpu_flushes; }

static uint64_t rss_peak;

void fake_virtual_memory_set_rss_peak(uint64_t pages) { rss_peak = pages; }

uint64_t virtual_memory_rss_pages(uint64_t pml4_phys) {
    (void)pml4_phys;
    return rss_peak;
}

uint64_t virtual_memory_rss_peak_pages(uint64_t pml4_phys) {
    (void)pml4_phys;
    return rss_peak;
}

void virtual_memory_init(const uint32_t *e820_map) { (void)e820_map; }
int virtual_memory_identity_covers(uint64_t phys, uint64_t length) { (void)phys; (void)length; return 1; }
void virtual_memory_enable_nx_this_cpu(void) {}
int virtual_memory_nx_enabled(void) { return 1; }
uint64_t virtual_memory_kernel_pml4_phys(void) { return 0; }
void virtual_memory_switch_address_space(uint64_t p) { (void)p; }
