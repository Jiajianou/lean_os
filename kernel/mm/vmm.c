#include "vmm.h"

#include <stdint.h>

#include "drivers/klog.h"
#include "lib/spinlock.h"
#include "mm/pmm.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL
#define HUGE_PAGE_SIZE (2ULL * 1024 * 1024)
#define ENTRIES_PER_TABLE 512ULL
/* PMM_TRACKED_MEMORY / HUGE_PAGE_SIZE, i.e. 512 - computed from the shared
 * constant (pmm.h) rather than hardcoded again, so the identity map this
 * builds can't silently drift out of sync with what the frame allocator
 * tracks. */
#define IDENTITY_MAP_ENTRIES (PMM_TRACKED_MEMORY / HUGE_PAGE_SIZE)

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2) /* U/S bit: ring 3 may access this translation */
#define PTE_HUGE      (1ULL << 7) /* PS bit: this PDE maps a 2 MiB page directly, no PT below it */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PML4_INDEX(v) (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((v) >> 12) & 0x1FF)

static uint64_t *kernel_pml4;
static uint64_t kernel_pml4_phys;

/* SMP: guards every page-table-mutating call below (vmm_map_page_in,
 * vmm_unmap_page, vmm_create_address_space) - a multi-level table walk
 * that allocates and links intermediate tables as it goes is a real
 * read-modify-write on shared structure (kernel_pml4[0]'s subtree is
 * literally shared across every address space, see vmm_create_address_
 * space), not safe from two CPUs at once. One coarse global lock rather
 * than per-table locking: this kernel has no concurrent-heavy-mapping
 * workload to make finer-grained locking worth the added complexity.
 * vmm_init/vmm_map_page/vmm_switch_address_space don't need it -
 * vmm_init runs single-threaded before SMP exists, vmm_map_page is a thin
 * wrapper whose real work happens (and is already locked) inside
 * vmm_map_page_in, and vmm_switch_address_space only ever touches this
 * CPU's own CR3, never shared table contents. */
static spinlock_t vmm_lock;

/* Every frame pmm_alloc_frame() can return lives within the 1 GiB this
 * file identity-maps, under whichever page tables are currently active
 * (the bootstrap ones during vmm_init, the kernel's own ones after) - so a
 * physical frame address is always safe to dereference directly as a
 * pointer. That stops being true the day physical memory tracking or
 * kernel mappings grow past 1 GiB. */
static inline uint64_t *phys_to_table(uint64_t phys) {
    return (uint64_t *)phys;
}

static uint64_t alloc_table(void) {
    uint64_t phys = pmm_alloc_frame();
    uint64_t *table = phys_to_table(phys);
    for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        table[i] = 0;
    }
    return phys;
}

/* Walks one level: returns the next-level table, allocating and linking a
 * fresh one if the entry is empty and allocate is set. Returns NULL if the
 * entry is empty and allocate is not set (used by unmap, which must never
 * create new page-table structure for an address it's removing).
 * extra_flags (e.g. PTE_USER) is OR'd into a newly allocated intermediate
 * entry only - x86 paging ANDs the U/S bit across every level, so a
 * process-private hierarchy (built entirely through vmm_map_page_in with
 * VMM_FLAG_USER) needs it set at every level, while the kernel's own
 * hierarchy needs it clear at every level; an existing entry was already
 * created with the right flags the first time, so this never needs to
 * patch one up. */
static uint64_t *table_walk(uint64_t *table, uint64_t index, int allocate, uint64_t extra_flags) {
    if (!(table[index] & PTE_PRESENT)) {
        if (!allocate) {
            return (uint64_t *)0;
        }
        uint64_t phys = alloc_table();
        table[index] = phys | PTE_PRESENT | PTE_WRITABLE | extra_flags;
        return phys_to_table(phys);
    }
    return phys_to_table(table[index] & PTE_ADDR_MASK);
}

void vmm_init(void) {
    uint64_t pml4_phys = alloc_table();
    kernel_pml4 = phys_to_table(pml4_phys);
    kernel_pml4_phys = pml4_phys;

    uint64_t pdpt_phys = alloc_table();
    kernel_pml4[0] = pdpt_phys | PTE_PRESENT | PTE_WRITABLE;
    uint64_t *pdpt = phys_to_table(pdpt_phys);

    uint64_t pd_phys = alloc_table();
    pdpt[0] = pd_phys | PTE_PRESENT | PTE_WRITABLE;
    uint64_t *pd = phys_to_table(pd_phys);

    for (uint64_t i = 0; i < IDENTITY_MAP_ENTRIES; i++) {
        pd[i] = (i * HUGE_PAGE_SIZE) | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;
    }

    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");

    klog_puts("[vmm] kernel-owned page tables installed (1 GiB identity map)\n");
}

uint64_t vmm_kernel_pml4_phys(void) {
    return kernel_pml4_phys;
}

int vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt) {
    spin_lock(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    /* Every walk here is create = 0: a missing level means the address
     * was never mapped in this address space, which is a -1, not a
     * reason to build page tables for it. */
    uint64_t *pdpt = table_walk(pml4, PML4_INDEX(virt), 0, 0);
    uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(virt), 0, 0) : (uint64_t *)0;
    if (!pd || (pd[PD_INDEX(virt)] & PTE_HUGE)) {
        spin_unlock(&vmm_lock);
        return -1;
    }
    uint64_t *pt = table_walk(pd, PD_INDEX(virt), 0, 0);
    if (!pt || !(pt[PT_INDEX(virt)] & PTE_PRESENT)) {
        spin_unlock(&vmm_lock);
        return -1;
    }
    pt[PT_INDEX(virt)] = 0;
    /* invlpg only touches this CPU's TLB, and this address is private to
     * one process's address space (PML4[1]) - so the only CPU that can
     * have it cached is one that has run this task, and it will reload
     * CR3 before running any other address space anyway. */
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock(&vmm_lock);
    return 0;
}

void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    spin_lock(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t extra = flags & PTE_USER;

    uint64_t *pdpt = table_walk(pml4, PML4_INDEX(virt), 1, extra);
    uint64_t *pd = table_walk(pdpt, PDPT_INDEX(virt), 1, extra);
    if (pd[PD_INDEX(virt)] & PTE_HUGE) {
        panic("vmm_map_page_in: address falls inside a 2 MiB huge-mapped range");
    }
    uint64_t *pt = table_walk(pd, PD_INDEX(virt), 1, extra);

    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | (flags & (PTE_WRITABLE | PTE_USER)) | PTE_PRESENT;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock(&vmm_lock);
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    vmm_map_page_in(kernel_pml4_phys, virt, phys, flags);
}

void vmm_unmap_page(uint64_t virt) {
    spin_lock(&vmm_lock);
    uint64_t *pdpt = table_walk(kernel_pml4, PML4_INDEX(virt), 0, 0);
    uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(virt), 0, 0) : (uint64_t *)0;
    if (!pd || (pd[PD_INDEX(virt)] & PTE_HUGE)) {
        panic("vmm_unmap_page: address not individually mapped");
    }
    uint64_t *pt = table_walk(pd, PD_INDEX(virt), 0, 0);
    if (!pt || !(pt[PT_INDEX(virt)] & PTE_PRESENT)) {
        panic("vmm_unmap_page: address not mapped");
    }

    pt[PT_INDEX(virt)] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock(&vmm_lock);
}

uint64_t vmm_create_address_space(void) {
    spin_lock(&vmm_lock);
    uint64_t new_phys = alloc_table();
    uint64_t *new_pml4 = phys_to_table(new_phys);
    new_pml4[0] = kernel_pml4[0]; /* share the kernel's identity map + heap */
    spin_unlock(&vmm_lock);
    return new_phys;
}

void vmm_switch_address_space(uint64_t pml4_phys) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}
