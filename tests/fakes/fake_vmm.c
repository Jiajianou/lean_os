#include "mm/vmm.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

void panic(const char *msg);

#define HEAP_SPAN (64ULL * 1024 * 1024)
#define PAGE 4096ULL
#define MAX_PAGES (HEAP_SPAN / PAGE)

static uint8_t *region;
static uint8_t mapped[MAX_PAGES];
static uint64_t mapped_phys[MAX_PAGES];
static uint64_t mapped_count;

static int map_fail_after = -1;
static int maps_attempted;

void fake_vmm_reset(void);
uint64_t fake_vmm_mapped_pages(void);
void fake_vmm_fail_map_after(int n);

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

uint64_t vmm_kernel_heap_base(void) {
    ensure_region();
    return (uint64_t)(uintptr_t)region;
}

void fake_vmm_reset(void) {
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

uint64_t fake_vmm_mapped_pages(void) { return mapped_count; }

void fake_vmm_fail_map_after(int n) {
    map_fail_after = n;
    maps_attempted = 0;
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

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)flags;
    uint64_t i = page_index(virt);
    if (mapped[i]) {
        panic("vmm_map_page: address already mapped");
    }
    mapped[i] = 1;
    mapped_phys[i] = phys;
    mapped_count++;
}

int vmm_try_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)pml4_phys;
    if (map_fail_after >= 0 && maps_attempted >= map_fail_after) {
        maps_attempted++;
        return -1;
    }
    maps_attempted++;
    vmm_map_page(virt, phys, flags);
    return 0;
}

uint64_t vmm_unmap_page_take(uint64_t pml4_phys, uint64_t virt) {
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

void vmm_unmap_page(uint64_t virt) {
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

uint64_t fake_vmm_cow_breaks(void) { return cow_breaks; }
uint64_t fake_vmm_unmaps_in(void) { return unmaps_in; }

int vmm_user_range_ok(uint64_t pml4_phys, uint64_t virt, uint64_t len, int need_write) {
    (void)pml4_phys;
    (void)need_write;
    if (len == 0) {
        return 1;
    }
    if (virt + len < virt) {
        return 0;
    }
    return 1;
}

int vmm_cow_break(uint64_t pml4_phys, uint64_t virt) {
    (void)pml4_phys;
    (void)virt;
    cow_breaks++;
    return 0;
}

int vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt) {
    (void)pml4_phys;
    (void)virt;
    unmaps_in++;
    return 0;
}

static uint64_t rss_peak;

void fake_vmm_set_rss_peak(uint64_t pages) { rss_peak = pages; }

uint64_t vmm_rss_pages(uint64_t pml4_phys) {
    (void)pml4_phys;
    return rss_peak;
}

uint64_t vmm_rss_peak_pages(uint64_t pml4_phys) {
    (void)pml4_phys;
    return rss_peak;
}

void vmm_init(const uint32_t *e820_map) { (void)e820_map; }
int vmm_identity_covers(uint64_t phys, uint64_t len) { (void)phys; (void)len; return 1; }
void vmm_enable_nx_this_cpu(void) {}
int vmm_nx_enabled(void) { return 1; }
uint64_t vmm_kernel_pml4_phys(void) { return 0; }
void vmm_switch_address_space(uint64_t p) { (void)p; }
