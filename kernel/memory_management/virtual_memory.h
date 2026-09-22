#pragma once

#include <stdint.h>

#define VIRTUAL_MEMORY_FLAG_WRITABLE (1ULL << 1)
#define VIRTUAL_MEMORY_FLAG_USER     (1ULL << 2)
#define VIRTUAL_MEMORY_FLAG_EXEC     (1ULL << 3)

#define VIRTUAL_MEMORY_FLAG_NOCACHE  (1ULL << 4)

#define KERNEL_HEAP_VIRT_BASE 0x0000004000000000ULL

void virtual_memory_init(const uint32_t *e820_map);

int virtual_memory_identity_covers(uint64_t phys, uint64_t length);

void virtual_memory_enable_nx_this_cpu(void);
int virtual_memory_nx_enabled(void);

uint64_t virtual_memory_protect_range_in(uint64_t pml4_phys, uint64_t start, uint64_t end,
                              uint64_t flags);

void virtual_memory_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

#define KERNEL_MMIO_VIRT_BASE 0x0000006000000000ULL
#define KERNEL_MMIO_VIRT_SIZE 0x0000002000000000ULL
void *virtual_memory_map_mmio(uint64_t phys, uint64_t length);
void virtual_memory_unmap_page(uint64_t virt);
uint64_t virtual_memory_kernel_pml4_phys(void);

uint64_t virtual_memory_kernel_heap_base(void);


int virtual_memory_try_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

int virtual_memory_unmap_page_in(uint64_t pml4_phys, uint64_t virt);

uint64_t virtual_memory_unmap_page_take(uint64_t pml4_phys, uint64_t virt);

int virtual_memory_user_range_ok(uint64_t pml4_phys, uint64_t virt, uint64_t length, int need_write);

typedef struct {
    uint64_t lo, hi;
} virtual_memory_range_t;

void virtual_memory_destroy_address_space(uint64_t pml4_phys, const virtual_memory_range_t *owned, int owned_count);

uint64_t virtual_memory_unmap_range_free(uint64_t pml4_phys, uint64_t start, uint64_t end);

uint64_t virtual_memory_fork_address_space(uint64_t source_pml4_phys, const virtual_memory_range_t *owned, int owned_count);

uint64_t virtual_memory_rss_pages(uint64_t pml4_phys);
uint64_t virtual_memory_rss_peak_pages(uint64_t pml4_phys);

int virtual_memory_cow_break(uint64_t pml4_phys, uint64_t virt);

uint64_t virtual_memory_create_address_space(void);
void virtual_memory_switch_address_space(uint64_t pml4_phys);

/* Reload CR3 with what is already in it, which discards every non-global
   translation this core has cached. A core that has just been told another
   core changed the page tables under it calls this; there is nothing cheaper
   here, because invlpg needs the addresses and a shootdown carries none. */
void virtual_memory_flush_local_tlb(void);
uint64_t virtual_memory_lookup_frame(uint64_t pml4_phys, uint64_t virt);
uint64_t virtual_memory_lookup_frame(uint64_t pml4_phys, uint64_t virt);
