#pragma once

#include <stdint.h>

#define VMM_FLAG_WRITABLE (1ULL << 1)
#define VMM_FLAG_USER     (1ULL << 2)
#define VMM_FLAG_EXEC     (1ULL << 3)

#define VMM_FLAG_NOCACHE  (1ULL << 4)

#define KERNEL_HEAP_VIRT_BASE 0x0000004000000000ULL

void vmm_init(const uint32_t *e820_map);

int vmm_identity_covers(uint64_t phys, uint64_t len);

void vmm_enable_nx_this_cpu(void);
int vmm_nx_enabled(void);

uint64_t vmm_protect_range_in(uint64_t pml4_phys, uint64_t start, uint64_t end,
                              uint64_t flags);

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

#define KERNEL_MMIO_VIRT_BASE 0x0000006000000000ULL
#define KERNEL_MMIO_VIRT_SIZE 0x0000002000000000ULL
void *vmm_map_mmio(uint64_t phys, uint64_t len);
void vmm_unmap_page(uint64_t virt);
uint64_t vmm_kernel_pml4_phys(void);

uint64_t vmm_kernel_heap_base(void);


int vmm_try_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

int vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt);

uint64_t vmm_unmap_page_take(uint64_t pml4_phys, uint64_t virt);

int vmm_user_range_ok(uint64_t pml4_phys, uint64_t virt, uint64_t len, int need_write);

typedef struct {
    uint64_t lo, hi;
} vmm_range_t;

void vmm_destroy_address_space(uint64_t pml4_phys, const vmm_range_t *owned, int owned_count);

uint64_t vmm_unmap_range_free(uint64_t pml4_phys, uint64_t start, uint64_t end);

uint64_t vmm_fork_address_space(uint64_t src_pml4_phys, const vmm_range_t *owned, int owned_count);

uint64_t vmm_rss_pages(uint64_t pml4_phys);
uint64_t vmm_rss_peak_pages(uint64_t pml4_phys);

int vmm_cow_break(uint64_t pml4_phys, uint64_t virt);

uint64_t vmm_create_address_space(void);
void vmm_switch_address_space(uint64_t pml4_phys);
