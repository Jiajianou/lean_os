#include "vmm.h"

#include <stdint.h>

#include "drivers/klog.h"
#include "library/spinlock.h"
#include "memory_management/e820.h"
#include "memory_management/pmm.h"
#include "panic.h"

static void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

#define PAGE_SIZE 4096ULL
#define HUGE_PAGE_SIZE (2ULL * 1024 * 1024)
#define ENTRIES_PER_TABLE 512ULL

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PCD       (1ULL << 4)
#define PTE_HUGE      (1ULL << 7)
#define PTE_COW       (1ULL << 9)
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PML4_INDEX(v) (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((v) >> 12) & 0x1FF)

static uint64_t *kernel_pml4;
static uint64_t kernel_pml4_phys;

static int nx_enabled;

static int cpu_has_nx(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000000u));
    if (eax < 0x80000001u) {
        return 0;
    }
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000001u));
    return (edx & (1u << 20)) != 0;
}

void vmm_enable_nx_this_cpu(void) {
    if (!nx_enabled) {
        return;
    }
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    lo |= (1u << 11);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080u));
}

int vmm_nx_enabled(void) {
    return nx_enabled;
}

static uint64_t leaf_flags(uint64_t flags) {
    uint64_t e = (flags & (PTE_WRITABLE | PTE_USER | PTE_PCD)) | PTE_PRESENT;
    if (nx_enabled && !(flags & VMM_FLAG_EXEC)) {
        e |= PTE_NX;
    }
    return e;
}

static spinlock_t vmm_lock;
static spinlock_t mmio_lock;

static inline uint64_t *phys_to_table(uint64_t phys) {
    return (uint64_t *)phys;
}

#define VMM_RSS_SLOTS 144

typedef struct {
    uint64_t pml4_phys;
    uint64_t pages;
    uint64_t peak;
} vmm_rss_slot_t;

static vmm_rss_slot_t rss_slots[VMM_RSS_SLOTS];

static int rss_recent;

static vmm_rss_slot_t *rss_find(uint64_t pml4_phys) {
    if (rss_slots[rss_recent].pml4_phys == pml4_phys && pml4_phys != 0) {
        return &rss_slots[rss_recent];
    }
    for (int i = 0; i < VMM_RSS_SLOTS; i++) {
        if (rss_slots[i].pml4_phys == pml4_phys) {
            rss_recent = i;
            return &rss_slots[i];
        }
    }
    return (vmm_rss_slot_t *)0;
}

static void rss_claim(uint64_t pml4_phys) {
    if (pml4_phys == kernel_pml4_phys || rss_find(pml4_phys)) {
        return;
    }
    for (int i = 0; i < VMM_RSS_SLOTS; i++) {
        if (rss_slots[i].pml4_phys == 0) {
            rss_slots[i].pml4_phys = pml4_phys;
            rss_slots[i].pages = 0;
            rss_slots[i].peak = 0;
            return;
        }
    }
}

static void rss_release(uint64_t pml4_phys) {
    vmm_rss_slot_t *s = rss_find(pml4_phys);
    if (s) {
        s->pml4_phys = 0;
        s->pages = 0;
        s->peak = 0;
        rss_recent = 0;
    }
}

static void rss_charge(uint64_t pml4_phys, int64_t delta) {
    vmm_rss_slot_t *s = rss_find(pml4_phys);
    if (!s) {
        return;
    }
    if (delta < 0) {
        uint64_t take = (uint64_t)(-delta);
        s->pages = (s->pages > take) ? s->pages - take : 0;
    } else {
        s->pages += (uint64_t)delta;
        if (s->pages > s->peak) {
            s->peak = s->pages;
        }
    }
}

uint64_t vmm_rss_pages(uint64_t pml4_phys) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    vmm_rss_slot_t *s = rss_find(pml4_phys);
    uint64_t n = s ? s->pages : 0;
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return n;
}

uint64_t vmm_rss_peak_pages(uint64_t pml4_phys) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    vmm_rss_slot_t *s = rss_find(pml4_phys);
    uint64_t n = s ? s->peak : 0;
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return n;
}

static uint64_t try_alloc_table(void) {
    uint64_t phys = pmm_try_alloc_frame();
    if (!phys) {
        return 0;
    }
    uint64_t *table = phys_to_table(phys);
    for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        table[i] = 0;
    }
    return phys;
}

static uint64_t *table_walk(uint64_t *table, uint64_t index, int allocate, uint64_t extra_flags) {
    if (!(table[index] & PTE_PRESENT)) {
        if (!allocate) {
            return (uint64_t *)0;
        }
        uint64_t phys = try_alloc_table();
        if (!phys) {
            return (uint64_t *)0;
        }
        table[index] = phys | PTE_PRESENT | PTE_WRITABLE | extra_flags;
        return phys_to_table(phys);
    }
    return phys_to_table(table[index] & PTE_ADDR_MASK);
}

static uint64_t identity_map_pages;

static void identity_map_block(uint64_t phys_2m) {
    uint64_t *pdpt = table_walk(kernel_pml4, PML4_INDEX(phys_2m), 1, 0);
    uint64_t *pd = table_walk(pdpt, PDPT_INDEX(phys_2m), 1, 0);
    if (pd[PD_INDEX(phys_2m)] & PTE_PRESENT) {
        return;
    }
    pd[PD_INDEX(phys_2m)] = phys_2m | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;
    identity_map_pages++;
}

void vmm_init(const uint32_t *e820_map) {
    nx_enabled = cpu_has_nx();
    vmm_enable_nx_this_cpu();

    uint64_t pml4_phys = try_alloc_table();
    if (!pml4_phys) {
        panic("vmm_init: no frame for the kernel's own PML4");
    }
    kernel_pml4 = phys_to_table(pml4_phys);
    kernel_pml4_phys = pml4_phys;

    uint32_t count = e820_count(e820_map);
    const e820_entry_t *entries = e820_entries(e820_map);

    uint64_t ram_top = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].type != E820_TYPE_USABLE &&
            entries[i].type != E820_TYPE_ACPI_RECLAIM &&
            entries[i].type != E820_TYPE_ACPI_NVS) {
            continue;
        }
        uint64_t end = entries[i].base + entries[i].length;
        if (end > ram_top) ram_top = end;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!e820_is_ram(entries[i].type) || entries[i].base >= ram_top) {
            continue;
        }
        uint64_t limit = entries[i].base + entries[i].length;
        if (limit > ram_top) limit = ram_top;
        uint64_t start = entries[i].base & ~(HUGE_PAGE_SIZE - 1);
        uint64_t end = (limit + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
        for (uint64_t p = start; p < end; p += HUGE_PAGE_SIZE) {
            identity_map_block(p);
        }
    }

    identity_map_block(0);

    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");

    klog_puts("[vmm] kernel-owned page tables installed (");
    klog_put_hex64(identity_map_pages * 2);
    klog_puts(" MiB identity-mapped in 2 MiB pages, NX ");
    klog_puts(nx_enabled ? "on" : "unavailable");
    klog_puts(")\n");
}

int vmm_identity_covers(uint64_t phys, uint64_t len) {
    if (len == 0) {
        return 1;
    }
    uint64_t end = phys + len;
    if (end < phys) {
        return 0;
    }
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    int ok = 1;
    for (uint64_t p = phys & ~(HUGE_PAGE_SIZE - 1); p < end; p += HUGE_PAGE_SIZE) {
        uint64_t *pdpt = table_walk(kernel_pml4, PML4_INDEX(p), 0, 0);
        uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(p), 0, 0) : (uint64_t *)0;
        if (!pd || !(pd[PD_INDEX(p)] & PTE_PRESENT) || !(pd[PD_INDEX(p)] & PTE_HUGE)) {
            ok = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return ok;
}

uint64_t vmm_kernel_pml4_phys(void) {
    return kernel_pml4_phys;
}

uint64_t vmm_kernel_heap_base(void) {
    return KERNEL_HEAP_VIRT_BASE;
}

int vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t *pdpt = table_walk(pml4, PML4_INDEX(virt), 0, 0);
    uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(virt), 0, 0) : (uint64_t *)0;
    if (!pd || (pd[PD_INDEX(virt)] & PTE_HUGE)) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return -1;
    }
    uint64_t *pt = table_walk(pd, PD_INDEX(virt), 0, 0);
    if (!pt || !(pt[PT_INDEX(virt)] & PTE_PRESENT)) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return -1;
    }
    pt[PT_INDEX(virt)] = 0;
    rss_charge(pml4_phys, -1);
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return 0;
}

uint64_t vmm_unmap_page_take(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t *pdpt = table_walk(pml4, PML4_INDEX(virt), 0, 0);
    uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(virt), 0, 0) : (uint64_t *)0;
    if (!pd || (pd[PD_INDEX(virt)] & PTE_HUGE)) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return 0;
    }
    uint64_t *pt = table_walk(pd, PD_INDEX(virt), 0, 0);
    if (!pt || !(pt[PT_INDEX(virt)] & PTE_PRESENT)) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return 0;
    }
    uint64_t phys = pt[PT_INDEX(virt)] & PTE_ADDR_MASK;
    pt[PT_INDEX(virt)] = 0;
    rss_charge(pml4_phys, -1);
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return phys;
}

int vmm_try_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t extra = flags & PTE_USER;

    uint64_t *pdpt = table_walk(pml4, PML4_INDEX(virt), 1, extra);
    uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(virt), 1, extra) : (uint64_t *)0;
    if (!pd) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return -1;
    }
    if (pd[PD_INDEX(virt)] & PTE_HUGE) {
        panic("vmm_map_page_in: address falls inside a 2 MiB huge-mapped range");
    }
    uint64_t *pt = table_walk(pd, PD_INDEX(virt), 1, extra);
    if (!pt) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return -1;
    }

    if (!(pt[PT_INDEX(virt)] & PTE_PRESENT)) {
        rss_charge(pml4_phys, 1);
    }
    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | leaf_flags(flags);
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return 0;
}

static void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (vmm_try_map_page_in(pml4_phys, virt, phys, flags) != 0) {
        panic("vmm_map_page_in: out of memory for a page table");
    }
}

uint64_t vmm_protect_range_in(uint64_t pml4_phys, uint64_t start, uint64_t end,
                              uint64_t flags) {
    uint64_t changed = 0;
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    for (uint64_t virt = start; virt < end; virt += PAGE_SIZE) {
        uint64_t *pdpt = table_walk(pml4, PML4_INDEX(virt), 0, 0);
        uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(virt), 0, 0) : (uint64_t *)0;
        if (!pd || (pd[PD_INDEX(virt)] & PTE_HUGE)) {
            continue;
        }
        uint64_t *pt = table_walk(pd, PD_INDEX(virt), 0, 0);
        if (!pt) {
            continue;
        }
        uint64_t e = pt[PT_INDEX(virt)];
        if (!(e & PTE_PRESENT)) {
            continue;
        }
        uint64_t want = leaf_flags(flags);
        if (e & PTE_COW) {
            want = (want & ~PTE_WRITABLE) | PTE_COW;
        }
        pt[PT_INDEX(virt)] = (e & PTE_ADDR_MASK) | want;
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
        changed++;
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return changed;
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    vmm_map_page_in(kernel_pml4_phys, virt, phys, flags);
}

static uint64_t mmio_next = KERNEL_MMIO_VIRT_BASE;

void *vmm_map_mmio(uint64_t phys, uint64_t len) {
    if (len == 0) {
        return (void *)0;
    }
    uint64_t start = phys & ~(PAGE_SIZE - 1);
    uint64_t offset = phys - start;
    uint64_t end = (phys + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (end <= start) {
        return (void *)0;
    }
    uint64_t span = end - start;

    uint64_t irq = spin_lock_irqsave(&mmio_lock);
    if (span > KERNEL_MMIO_VIRT_SIZE ||
        mmio_next - KERNEL_MMIO_VIRT_BASE > KERNEL_MMIO_VIRT_SIZE - span) {
        spin_unlock_irqrestore(&mmio_lock, irq);
        return (void *)0;
    }
    uint64_t virt = mmio_next;
    mmio_next += span;
    spin_unlock_irqrestore(&mmio_lock, irq);

    for (uint64_t i = 0; i < span; i += PAGE_SIZE) {
        if (vmm_try_map_page_in(kernel_pml4_phys, virt + i, start + i,
                                VMM_FLAG_WRITABLE | VMM_FLAG_NOCACHE) != 0) {
            return (void *)0;
        }
    }
    return (void *)(uintptr_t)(virt + offset);
}

void vmm_unmap_page(uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
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
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
}

int vmm_user_range_ok(uint64_t pml4_phys, uint64_t virt, uint64_t len, int need_write) {
    if (len == 0) {
        return 1;
    }
    uint64_t end = virt + len;
    if (end < virt) {
        return 0;
    }
    uint64_t need = PTE_PRESENT | PTE_USER | (need_write ? PTE_WRITABLE : 0);

    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    for (uint64_t page = virt & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        uint64_t e = pml4[PML4_INDEX(page)];
        if ((e & need) != need) {
            spin_unlock_irqrestore(&vmm_lock, irq_flags);
            return 0;
        }
        uint64_t *pdpt = phys_to_table(e & PTE_ADDR_MASK);
        e = pdpt[PDPT_INDEX(page)];
        if ((e & need) != need || (e & PTE_HUGE)) {
            spin_unlock_irqrestore(&vmm_lock, irq_flags);
            return 0;
        }
        uint64_t *pd = phys_to_table(e & PTE_ADDR_MASK);
        e = pd[PD_INDEX(page)];
        if ((e & need) != need || (e & PTE_HUGE)) {
            spin_unlock_irqrestore(&vmm_lock, irq_flags);
            return 0;
        }
        uint64_t *pt = phys_to_table(e & PTE_ADDR_MASK);
        if ((pt[PT_INDEX(page)] & need) != need) {
            spin_unlock_irqrestore(&vmm_lock, irq_flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return 1;
}

static int addr_in_owned(uint64_t virt, const vmm_range_t *owned, int owned_count) {
    for (int i = 0; i < owned_count; i++) {
        if (virt >= owned[i].lo && virt < owned[i].hi) {
            return 1;
        }
    }
    return 0;
}

void vmm_destroy_address_space(uint64_t pml4_phys, const vmm_range_t *owned, int owned_count) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    rss_release(pml4_phys);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    for (uint64_t i = 1; i < ENTRIES_PER_TABLE; i++) {
        if (!(pml4[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t *pdpt = phys_to_table(pml4[i] & PTE_ADDR_MASK);
        for (uint64_t j = 0; j < ENTRIES_PER_TABLE; j++) {
            if (!(pdpt[j] & PTE_PRESENT) || (pdpt[j] & PTE_HUGE)) {
                continue;
            }
            uint64_t *pd = phys_to_table(pdpt[j] & PTE_ADDR_MASK);
            for (uint64_t k = 0; k < ENTRIES_PER_TABLE; k++) {
                if (!(pd[k] & PTE_PRESENT) || (pd[k] & PTE_HUGE)) {
                    continue;
                }
                uint64_t *pt = phys_to_table(pd[k] & PTE_ADDR_MASK);
                for (uint64_t l = 0; l < ENTRIES_PER_TABLE; l++) {
                    if (!(pt[l] & PTE_PRESENT)) {
                        continue;
                    }
                    uint64_t virt = (i << 39) | (j << 30) | (k << 21) | (l << 12);
                    if (addr_in_owned(virt, owned, owned_count)) {
                        pmm_free_frame(pt[l] & PTE_ADDR_MASK);
                    }
                    pt[l] = 0;
                }
                pmm_free_frame(pd[k] & PTE_ADDR_MASK);
                pd[k] = 0;
            }
            pmm_free_frame(pdpt[j] & PTE_ADDR_MASK);
            pdpt[j] = 0;
        }
        pmm_free_frame(pml4[i] & PTE_ADDR_MASK);
        pml4[i] = 0;
    }
    pmm_free_frame(pml4_phys);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
}

uint64_t vmm_unmap_range_free(uint64_t pml4_phys, uint64_t start, uint64_t end) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t freed = 0;

    for (uint64_t addr = start; addr < end; ) {
        uint64_t *pdpt = table_walk(pml4, PML4_INDEX(addr), 0, 0);
        if (!pdpt) {
            addr = (addr + (1ULL << 39)) & ~((1ULL << 39) - 1);
            continue;
        }
        uint64_t *pd = table_walk(pdpt, PDPT_INDEX(addr), 0, 0);
        if (!pd) {
            addr = (addr + (1ULL << 30)) & ~((1ULL << 30) - 1);
            continue;
        }
        if (pd[PD_INDEX(addr)] & PTE_HUGE) {
            addr = (addr + (1ULL << 21)) & ~((1ULL << 21) - 1);
            continue;
        }
        uint64_t *pt = table_walk(pd, PD_INDEX(addr), 0, 0);
        if (!pt) {
            addr = (addr + (1ULL << 21)) & ~((1ULL << 21) - 1);
            continue;
        }
        uint64_t pt_end = (addr + (1ULL << 21)) & ~((1ULL << 21) - 1);
        if (pt_end > end) {
            pt_end = end;
        }
        for (; addr < pt_end; addr += PAGE_SIZE) {
            uint64_t entry = pt[PT_INDEX(addr)];
            if (!(entry & PTE_PRESENT)) {
                continue;
            }
            pt[PT_INDEX(addr)] = 0;
            __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
            pmm_free_frame(entry & PTE_ADDR_MASK);
            freed++;
        }
    }

    rss_charge(pml4_phys, -(int64_t)freed);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return freed;
}

uint64_t vmm_fork_address_space(uint64_t src_pml4_phys, const vmm_range_t *owned, int owned_count) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t *src = phys_to_table(src_pml4_phys);

    uint64_t dst_phys = pmm_try_alloc_frame();
    if (dst_phys == 0) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return 0;
    }
    uint64_t *dst = phys_to_table(dst_phys);
    for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        dst[i] = 0;
    }
    dst[0] = src[0];
    rss_claim(dst_phys);

    int ok = 1;
    for (uint64_t i = 1; ok && i < ENTRIES_PER_TABLE; i++) {
        if (!(src[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t *s_pdpt = phys_to_table(src[i] & PTE_ADDR_MASK);
        for (uint64_t j = 0; ok && j < ENTRIES_PER_TABLE; j++) {
            if (!(s_pdpt[j] & PTE_PRESENT) || (s_pdpt[j] & PTE_HUGE)) {
                continue;
            }
            uint64_t *s_pd = phys_to_table(s_pdpt[j] & PTE_ADDR_MASK);
            for (uint64_t k = 0; ok && k < ENTRIES_PER_TABLE; k++) {
                if (!(s_pd[k] & PTE_PRESENT) || (s_pd[k] & PTE_HUGE)) {
                    continue;
                }
                uint64_t *s_pt = phys_to_table(s_pd[k] & PTE_ADDR_MASK);
                for (uint64_t l = 0; ok && l < ENTRIES_PER_TABLE; l++) {
                    if (!(s_pt[l] & PTE_PRESENT)) {
                        continue;
                    }
                    uint64_t virt = (i << 39) | (j << 30) | (k << 21) | (l << 12);
                    if (!addr_in_owned(virt, owned, owned_count)) {
                        continue;
                    }
                    uint64_t phys = s_pt[l] & PTE_ADDR_MASK;

                    uint64_t *d_pdpt = table_walk(dst, i, 1, PTE_USER);
                    uint64_t *d_pd = d_pdpt ? table_walk(d_pdpt, j, 1, PTE_USER) : (uint64_t *)0;
                    uint64_t *d_pt = d_pd ? table_walk(d_pd, k, 1, PTE_USER) : (uint64_t *)0;
                    if (!d_pt) {
                        ok = 0;
                        break;
                    }

                    uint64_t shared = s_pt[l];
                    if (shared & PTE_WRITABLE) {
                        shared = (shared & ~PTE_WRITABLE) | PTE_COW;
                        s_pt[l] = shared;
                        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
                    }
                    d_pt[l] = shared;
                    rss_charge(dst_phys, 1);
                    pmm_frame_ref(phys);
                }
            }
        }
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);

    if (!ok) {
        vmm_destroy_address_space(dst_phys, owned, owned_count);
        return 0;
    }
    return dst_phys;
}

int vmm_cow_break(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t *pdpt = table_walk(pml4, PML4_INDEX(virt), 0, 0);
    uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(virt), 0, 0) : (uint64_t *)0;
    if (!pd || (pd[PD_INDEX(virt)] & PTE_HUGE)) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return 0;
    }
    uint64_t *pt = table_walk(pd, PD_INDEX(virt), 0, 0);
    if (!pt) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return 0;
    }
    uint64_t entry = pt[PT_INDEX(virt)];
    if (!(entry & PTE_PRESENT) || !(entry & PTE_COW)) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return 0;
    }

    uint64_t old_phys = entry & PTE_ADDR_MASK;
    if (pmm_frame_refs(old_phys) <= 1) {
        pt[PT_INDEX(virt)] = (entry & ~PTE_COW) | PTE_WRITABLE;
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return 1;
    }

    uint64_t new_phys = pmm_try_alloc_frame();
    if (new_phys == 0) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return 0;
    }
    const uint8_t *from = (const uint8_t *)old_phys;
    uint8_t *to = (uint8_t *)new_phys;
    for (uint64_t i = 0; i < PAGE_SIZE; i++) {
        to[i] = from[i];
    }
    pt[PT_INDEX(virt)] = new_phys | (entry & (PTE_USER | PTE_PRESENT | PTE_NX)) | PTE_WRITABLE;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock_irqrestore(&vmm_lock, irq_flags);

    pmm_free_frame(old_phys);
    return 1;
}

uint64_t vmm_create_address_space(void) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t new_phys = try_alloc_table();
    if (!new_phys) {
        spin_unlock_irqrestore(&vmm_lock, irq_flags);
        return 0;
    }
    uint64_t *new_pml4 = phys_to_table(new_phys);
    new_pml4[0] = kernel_pml4[0];
    rss_claim(new_phys);
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return new_phys;
}

void vmm_switch_address_space(uint64_t pml4_phys) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}
