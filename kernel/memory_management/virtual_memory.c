#include "virtual_memory.h"

#include <stdint.h>

#include "drivers/kernel_log.h"
#include "library/spinlock.h"
#include "memory_management/e820.h"
#include "memory_management/physical_memory.h"
#include "panic.h"

static void virtual_memory_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

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
#define PTE_ADDRESS_MASK 0x000FFFFFFFFFF000ULL

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

void virtual_memory_enable_nx_this_cpu(void) {
    if (!nx_enabled) {
        return;
    }
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    lo |= (1u << 11);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080u));
}

int virtual_memory_nx_enabled(void) {
    return nx_enabled;
}

static uint64_t leaf_flags(uint64_t flags) {
    uint64_t e = (flags & (PTE_WRITABLE | PTE_USER | PTE_PCD)) | PTE_PRESENT;
    if (nx_enabled && !(flags & VIRTUAL_MEMORY_FLAG_EXEC)) {
        e |= PTE_NX;
    }
    return e;
}

static spinlock_t virtual_memory_lock;
static spinlock_t mmio_lock;

static inline uint64_t *phys_to_table(uint64_t phys) {
    return (uint64_t *)phys;
}

#define VIRTUAL_MEMORY_RSS_SLOTS 144

typedef struct {
    uint64_t pml4_phys;
    uint64_t pages;
    uint64_t peak;
} virtual_memory_rss_slot_t;

static virtual_memory_rss_slot_t rss_slots[VIRTUAL_MEMORY_RSS_SLOTS];

static int rss_recent;

static virtual_memory_rss_slot_t *rss_find(uint64_t pml4_phys) {
    if (rss_slots[rss_recent].pml4_phys == pml4_phys && pml4_phys != 0) {
        return &rss_slots[rss_recent];
    }
    for (int i = 0; i < VIRTUAL_MEMORY_RSS_SLOTS; i++) {
        if (rss_slots[i].pml4_phys == pml4_phys) {
            rss_recent = i;
            return &rss_slots[i];
        }
    }
    return (virtual_memory_rss_slot_t *)0;
}

static void rss_claim(uint64_t pml4_phys) {
    if (pml4_phys == kernel_pml4_phys || rss_find(pml4_phys)) {
        return;
    }
    for (int i = 0; i < VIRTUAL_MEMORY_RSS_SLOTS; i++) {
        if (rss_slots[i].pml4_phys == 0) {
            rss_slots[i].pml4_phys = pml4_phys;
            rss_slots[i].pages = 0;
            rss_slots[i].peak = 0;
            return;
        }
    }
}

static void rss_release(uint64_t pml4_phys) {
    virtual_memory_rss_slot_t *s = rss_find(pml4_phys);
    if (s) {
        s->pml4_phys = 0;
        s->pages = 0;
        s->peak = 0;
        rss_recent = 0;
    }
}

static void rss_charge(uint64_t pml4_phys, int64_t delta) {
    virtual_memory_rss_slot_t *s = rss_find(pml4_phys);
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

uint64_t virtual_memory_rss_pages(uint64_t pml4_phys) {
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
    virtual_memory_rss_slot_t *s = rss_find(pml4_phys);
    uint64_t n = s ? s->pages : 0;
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
    return n;
}

uint64_t virtual_memory_rss_peak_pages(uint64_t pml4_phys) {
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
    virtual_memory_rss_slot_t *s = rss_find(pml4_phys);
    uint64_t n = s ? s->peak : 0;
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
    return n;
}

static uint64_t try_alloc_table(void) {
    uint64_t phys = physical_memory_try_alloc_frame();
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
    return phys_to_table(table[index] & PTE_ADDRESS_MASK);
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

void virtual_memory_init(const uint32_t *e820_map) {
    nx_enabled = cpu_has_nx();
    virtual_memory_enable_nx_this_cpu();

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

    kernel_log_puts("[vmm] kernel-owned page tables installed (");
    kernel_log_put_hex64(identity_map_pages * 2);
    kernel_log_puts(" MiB identity-mapped in 2 MiB pages, NX ");
    kernel_log_puts(nx_enabled ? "on" : "unavailable");
    kernel_log_puts(")\n");
}

int virtual_memory_identity_covers(uint64_t phys, uint64_t length) {
    if (length == 0) {
        return 1;
    }
    uint64_t end = phys + length;
    if (end < phys) {
        return 0;
    }
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
    int ok = 1;
    for (uint64_t p = phys & ~(HUGE_PAGE_SIZE - 1); p < end; p += HUGE_PAGE_SIZE) {
        uint64_t *pdpt = table_walk(kernel_pml4, PML4_INDEX(p), 0, 0);
        uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(p), 0, 0) : (uint64_t *)0;
        if (!pd || !(pd[PD_INDEX(p)] & PTE_PRESENT) || !(pd[PD_INDEX(p)] & PTE_HUGE)) {
            ok = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
    return ok;
}

uint64_t virtual_memory_kernel_pml4_phys(void) {
    return kernel_pml4_phys;
}

uint64_t virtual_memory_kernel_heap_base(void) {
    return KERNEL_HEAP_VIRT_BASE;
}

int virtual_memory_unmap_page_in(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t *pdpt = table_walk(pml4, PML4_INDEX(virt), 0, 0);
    uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(virt), 0, 0) : (uint64_t *)0;
    if (!pd || (pd[PD_INDEX(virt)] & PTE_HUGE)) {
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return -1;
    }
    uint64_t *pt = table_walk(pd, PD_INDEX(virt), 0, 0);
    if (!pt || !(pt[PT_INDEX(virt)] & PTE_PRESENT)) {
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return -1;
    }
    pt[PT_INDEX(virt)] = 0;
    rss_charge(pml4_phys, -1);
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
    return 0;
}

uint64_t virtual_memory_unmap_page_take(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t *pdpt = table_walk(pml4, PML4_INDEX(virt), 0, 0);
    uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(virt), 0, 0) : (uint64_t *)0;
    if (!pd || (pd[PD_INDEX(virt)] & PTE_HUGE)) {
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return 0;
    }
    uint64_t *pt = table_walk(pd, PD_INDEX(virt), 0, 0);
    if (!pt || !(pt[PT_INDEX(virt)] & PTE_PRESENT)) {
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return 0;
    }
    uint64_t phys = pt[PT_INDEX(virt)] & PTE_ADDRESS_MASK;
    pt[PT_INDEX(virt)] = 0;
    rss_charge(pml4_phys, -1);
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
    return phys;
}

int virtual_memory_try_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t extra = flags & PTE_USER;

    uint64_t *pdpt = table_walk(pml4, PML4_INDEX(virt), 1, extra);
    uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(virt), 1, extra) : (uint64_t *)0;
    if (!pd) {
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return -1;
    }
    if (pd[PD_INDEX(virt)] & PTE_HUGE) {
        panic("vmm_map_page_in: address falls inside a 2 MiB huge-mapped range");
    }
    uint64_t *pt = table_walk(pd, PD_INDEX(virt), 1, extra);
    if (!pt) {
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return -1;
    }

    if (!(pt[PT_INDEX(virt)] & PTE_PRESENT)) {
        rss_charge(pml4_phys, 1);
    }
    pt[PT_INDEX(virt)] = (phys & PTE_ADDRESS_MASK) | leaf_flags(flags);
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
    return 0;
}

static void virtual_memory_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (virtual_memory_try_map_page_in(pml4_phys, virt, phys, flags) != 0) {
        panic("vmm_map_page_in: out of memory for a page table");
    }
}

uint64_t virtual_memory_protect_range_in(uint64_t pml4_phys, uint64_t start, uint64_t end,
                              uint64_t flags) {
    uint64_t changed = 0;
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
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
        pt[PT_INDEX(virt)] = (e & PTE_ADDRESS_MASK) | want;
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
        changed++;
    }
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
    return changed;
}

void virtual_memory_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    virtual_memory_map_page_in(kernel_pml4_phys, virt, phys, flags);
}

static uint64_t mmio_next = KERNEL_MMIO_VIRT_BASE;

void *virtual_memory_map_mmio(uint64_t phys, uint64_t length) {
    if (length == 0) {
        return (void *)0;
    }
    uint64_t start = phys & ~(PAGE_SIZE - 1);
    uint64_t offset = phys - start;
    uint64_t end = (phys + length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
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
        if (virtual_memory_try_map_page_in(kernel_pml4_phys, virt + i, start + i,
                                VIRTUAL_MEMORY_FLAG_WRITABLE | VIRTUAL_MEMORY_FLAG_NOCACHE) != 0) {
            return (void *)0;
        }
    }
    return (void *)(uintptr_t)(virt + offset);
}

void virtual_memory_unmap_page(uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
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
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
}

int virtual_memory_user_range_ok(uint64_t pml4_phys, uint64_t virt, uint64_t length, int need_write) {
    if (length == 0) {
        return 1;
    }
    uint64_t end = virt + length;
    if (end < virt) {
        return 0;
    }
    uint64_t need = PTE_PRESENT | PTE_USER | (need_write ? PTE_WRITABLE : 0);

    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    for (uint64_t page = virt & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        uint64_t e = pml4[PML4_INDEX(page)];
        if ((e & need) != need) {
            spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
            return 0;
        }
        uint64_t *pdpt = phys_to_table(e & PTE_ADDRESS_MASK);
        e = pdpt[PDPT_INDEX(page)];
        if ((e & need) != need || (e & PTE_HUGE)) {
            spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
            return 0;
        }
        uint64_t *pd = phys_to_table(e & PTE_ADDRESS_MASK);
        e = pd[PD_INDEX(page)];
        if ((e & need) != need || (e & PTE_HUGE)) {
            spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
            return 0;
        }
        uint64_t *pt = phys_to_table(e & PTE_ADDRESS_MASK);
        if ((pt[PT_INDEX(page)] & need) != need) {
            spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
    return 1;
}

static int address_in_owned(uint64_t virt, const virtual_memory_range_t *owned, int owned_count) {
    for (int i = 0; i < owned_count; i++) {
        if (virt >= owned[i].lo && virt < owned[i].hi) {
            return 1;
        }
    }
    return 0;
}

void virtual_memory_destroy_address_space(uint64_t pml4_phys, const virtual_memory_range_t *owned, int owned_count) {
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
    rss_release(pml4_phys);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    for (uint64_t i = 1; i < ENTRIES_PER_TABLE; i++) {
        if (!(pml4[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t *pdpt = phys_to_table(pml4[i] & PTE_ADDRESS_MASK);
        for (uint64_t j = 0; j < ENTRIES_PER_TABLE; j++) {
            if (!(pdpt[j] & PTE_PRESENT) || (pdpt[j] & PTE_HUGE)) {
                continue;
            }
            uint64_t *pd = phys_to_table(pdpt[j] & PTE_ADDRESS_MASK);
            for (uint64_t k = 0; k < ENTRIES_PER_TABLE; k++) {
                if (!(pd[k] & PTE_PRESENT) || (pd[k] & PTE_HUGE)) {
                    continue;
                }
                uint64_t *pt = phys_to_table(pd[k] & PTE_ADDRESS_MASK);
                for (uint64_t l = 0; l < ENTRIES_PER_TABLE; l++) {
                    if (!(pt[l] & PTE_PRESENT)) {
                        continue;
                    }
                    uint64_t virt = (i << 39) | (j << 30) | (k << 21) | (l << 12);
                    if (address_in_owned(virt, owned, owned_count)) {
                        pmm_free_site = "destroy:page";
                        pmm_free_virt = virt;
                        physical_memory_free_frame(pt[l] & PTE_ADDRESS_MASK);
                    }
                    pt[l] = 0;
                }
                pmm_free_site = "destroy:pt";
                physical_memory_free_frame(pd[k] & PTE_ADDRESS_MASK);
                pd[k] = 0;
            }
            pmm_free_site = "destroy:pd";
            physical_memory_free_frame(pdpt[j] & PTE_ADDRESS_MASK);
            pdpt[j] = 0;
        }
        pmm_free_site = "destroy:pdpt";
        physical_memory_free_frame(pml4[i] & PTE_ADDRESS_MASK);
        pml4[i] = 0;
    }
    pmm_free_site = "destroy:pml4";
    physical_memory_free_frame(pml4_phys);
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
}

uint64_t virtual_memory_unmap_range_free(uint64_t pml4_phys, uint64_t start, uint64_t end) {
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t freed = 0;

    for (uint64_t address = start; address < end; ) {
        uint64_t *pdpt = table_walk(pml4, PML4_INDEX(address), 0, 0);
        if (!pdpt) {
            address = (address + (1ULL << 39)) & ~((1ULL << 39) - 1);
            continue;
        }
        uint64_t *pd = table_walk(pdpt, PDPT_INDEX(address), 0, 0);
        if (!pd) {
            address = (address + (1ULL << 30)) & ~((1ULL << 30) - 1);
            continue;
        }
        if (pd[PD_INDEX(address)] & PTE_HUGE) {
            address = (address + (1ULL << 21)) & ~((1ULL << 21) - 1);
            continue;
        }
        uint64_t *pt = table_walk(pd, PD_INDEX(address), 0, 0);
        if (!pt) {
            address = (address + (1ULL << 21)) & ~((1ULL << 21) - 1);
            continue;
        }
        uint64_t pt_end = (address + (1ULL << 21)) & ~((1ULL << 21) - 1);
        if (pt_end > end) {
            pt_end = end;
        }
        for (; address < pt_end; address += PAGE_SIZE) {
            uint64_t entry = pt[PT_INDEX(address)];
            if (!(entry & PTE_PRESENT)) {
                continue;
            }
            pt[PT_INDEX(address)] = 0;
            __asm__ volatile("invlpg (%0)" : : "r"(address) : "memory");
            physical_memory_free_frame(entry & PTE_ADDRESS_MASK);
            freed++;
        }
    }

    rss_charge(pml4_phys, -(int64_t)freed);
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
    return freed;
}

uint64_t virtual_memory_fork_address_space(uint64_t source_pml4_phys, const virtual_memory_range_t *owned, int owned_count) {
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
    uint64_t *source = phys_to_table(source_pml4_phys);

    uint64_t destination_phys = physical_memory_try_alloc_frame();
    if (destination_phys == 0) {
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return 0;
    }
    uint64_t *destination = phys_to_table(destination_phys);
    for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        destination[i] = 0;
    }
    destination[0] = source[0];
    rss_claim(destination_phys);

    int ok = 1;
    for (uint64_t i = 1; ok && i < ENTRIES_PER_TABLE; i++) {
        if (!(source[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t *s_pdpt = phys_to_table(source[i] & PTE_ADDRESS_MASK);
        for (uint64_t j = 0; ok && j < ENTRIES_PER_TABLE; j++) {
            if (!(s_pdpt[j] & PTE_PRESENT) || (s_pdpt[j] & PTE_HUGE)) {
                continue;
            }
            uint64_t *s_pd = phys_to_table(s_pdpt[j] & PTE_ADDRESS_MASK);
            for (uint64_t k = 0; ok && k < ENTRIES_PER_TABLE; k++) {
                if (!(s_pd[k] & PTE_PRESENT) || (s_pd[k] & PTE_HUGE)) {
                    continue;
                }
                uint64_t *s_pt = phys_to_table(s_pd[k] & PTE_ADDRESS_MASK);
                for (uint64_t l = 0; ok && l < ENTRIES_PER_TABLE; l++) {
                    if (!(s_pt[l] & PTE_PRESENT)) {
                        continue;
                    }
                    uint64_t virt = (i << 39) | (j << 30) | (k << 21) | (l << 12);
                    if (!address_in_owned(virt, owned, owned_count)) {
                        continue;
                    }
                    uint64_t phys = s_pt[l] & PTE_ADDRESS_MASK;

                    uint64_t *d_pdpt = table_walk(destination, i, 1, PTE_USER);
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
                    rss_charge(destination_phys, 1);
                    physical_memory_frame_reference(phys);
                }
            }
        }
    }
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);

    if (!ok) {
        virtual_memory_destroy_address_space(destination_phys, owned, owned_count);
        return 0;
    }
    return destination_phys;
}

int virtual_memory_cow_break(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t *pdpt = table_walk(pml4, PML4_INDEX(virt), 0, 0);
    uint64_t *pd = pdpt ? table_walk(pdpt, PDPT_INDEX(virt), 0, 0) : (uint64_t *)0;
    if (!pd || (pd[PD_INDEX(virt)] & PTE_HUGE)) {
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return 0;
    }
    uint64_t *pt = table_walk(pd, PD_INDEX(virt), 0, 0);
    if (!pt) {
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return 0;
    }
    uint64_t entry = pt[PT_INDEX(virt)];
    if (!(entry & PTE_PRESENT)) {
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return 0;
    }
    if (!(entry & PTE_COW)) {
        /* Not copy-on-write any more. Until fork could be called from a
           process with more than one thread, that meant the fault was about
           something else and the caller was right to treat a zero as fatal.
           Now two siblings can take the same fault and the one that waited
           for this lock arrives to find the page already private - it is the
           only core still holding the read-only translation, so the fault is
           answered by discarding that rather than by copying a second time. */
        int already_broken = (entry & PTE_WRITABLE) != 0;
        if (already_broken) {
            __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
        }
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return already_broken;
    }

    uint64_t old_phys = entry & PTE_ADDRESS_MASK;
    if (physical_memory_frame_refs(old_phys) <= 1) {
        pt[PT_INDEX(virt)] = (entry & ~PTE_COW) | PTE_WRITABLE;
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return 1;
    }

    uint64_t new_phys = physical_memory_try_alloc_frame();
    if (new_phys == 0) {
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return 0;
    }
    const uint8_t *from = (const uint8_t *)old_phys;
    uint8_t *to = (uint8_t *)new_phys;
    for (uint64_t i = 0; i < PAGE_SIZE; i++) {
        to[i] = from[i];
    }
    pt[PT_INDEX(virt)] = new_phys | (entry & (PTE_USER | PTE_PRESENT | PTE_NX)) | PTE_WRITABLE;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);

    physical_memory_free_frame(old_phys);
    return 1;
}

uint64_t virtual_memory_create_address_space(void) {
    uint64_t irq_flags = spin_lock_irqsave(&virtual_memory_lock);
    uint64_t new_phys = try_alloc_table();
    if (!new_phys) {
        spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
        return 0;
    }
    uint64_t *new_pml4 = phys_to_table(new_phys);
    new_pml4[0] = kernel_pml4[0];
    rss_claim(new_phys);
    spin_unlock_irqrestore(&virtual_memory_lock, irq_flags);
    return new_phys;
}

void virtual_memory_switch_address_space(uint64_t pml4_phys) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

void virtual_memory_flush_local_tlb(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
