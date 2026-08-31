#include "vmm.h"

#include <stdint.h>

#include "drivers/klog.h"
#include "lib/spinlock.h"
#include "mm/e820.h"
#include "mm/pmm.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL
#define HUGE_PAGE_SIZE (2ULL * 1024 * 1024)
#define ENTRIES_PER_TABLE 512ULL

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2) /* U/S bit: ring 3 may access this translation */
#define PTE_HUGE      (1ULL << 7) /* PS bit: this PDE maps a 2 MiB page directly, no PT below it */
/* M83: bit 9 is one of the three bits the CPU ignores in a page table
 * entry and leaves entirely to the operating system. This one marks a
 * page as copy-on-write: present, mapped read-only in two or more
 * address spaces, and to be duplicated by the fault handler the first
 * time anyone writes to it.
 *
 * A flag is needed rather than inference, because "present and not
 * writable" already means something else - a page of a PROT_READ mapping,
 * which must stay unwritable however many times it is written to. The two
 * look identical in the hardware bits and must not be treated alike. */
#define PTE_COW       (1ULL << 9)
/* M91: the execute-disable bit, and the first page-table bit in this
 * kernel that the hardware only honours if it is asked to. It lives at
 * the top of the entry rather than the bottom, and it is *inverted* -
 * set means "may not execute" - so a kernel that never sets it maps
 * everything executable, which is what this one did for ninety
 * milestones.
 *
 * Setting it without EFER.NXE first is not a no-op: the CPU treats bit 63
 * as reserved-must-be-zero and faults on any translation that uses the
 * entry. So nx_enabled below is checked at every write rather than
 * assumed, and vmm_init turns the feature on before it builds a single
 * table. */
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PML4_INDEX(v) (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((v) >> 12) & 0x1FF)

static uint64_t *kernel_pml4;
static uint64_t kernel_pml4_phys;

/* M91: whether this CPU family has execute-disable and EFER.NXE is on.
 * Read on every leaf write; 0 makes PTE_NX unreachable, so a machine
 * without the feature maps everything executable exactly as this kernel
 * always did rather than faulting on a reserved bit. */
static int nx_enabled;

/* CPUID leaf 0x80000001, EDX bit 20 - "execute disable bit available". */
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

/* IA32_EFER bit 11. Per-CPU state, so this is called on the BSP by
 * vmm_init and on every AP by smp.c - an AP that skipped it would fault
 * on the first user page it touched, because the entries are shared and
 * already carry the bit. */
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

/* The leaf-entry bits a caller's VMM_FLAG_* map to. One function so that
 * every place that writes a leaf - map, cow_break, protect - agrees, which
 * is the bug M91 would otherwise ship: a page that loses its NX bit on
 * the first write to it is a W^X hole that only appears after a fork. */
static uint64_t leaf_flags(uint64_t flags) {
    uint64_t e = (flags & (PTE_WRITABLE | PTE_USER)) | PTE_PRESENT;
    if (nx_enabled && !(flags & VMM_FLAG_EXEC)) {
        e |= PTE_NX;
    }
    return e;
}

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

/* Every frame pmm_alloc_frame() can return lives within the identity map
 * this file builds, under whichever page tables are currently active (the
 * bootstrap ones during vmm_init, the kernel's own ones after) - so a
 * physical frame address is always safe to dereference directly as a
 * pointer.
 *
 * M90: that used to be true because both numbers were the same constant,
 * 1 GiB. It is now true because both come from the same e820 map: pmm.c
 * tracks frames up to the highest usable address in it, and vmm_init maps
 * every RAM range in it. The invariant is unchanged and the reason for it
 * is stronger - it is derived rather than asserted.
 *
 * There is one ordering subtlety worth stating, because it looks like a
 * bug and is not: vmm_init runs *after* pmm_init and allocates its page
 * tables from frames the identity map does not cover yet. Those writes go
 * through the boot loader's own page tables, which map all of low memory
 * 1:1 - and pmm's own metadata placement puts every early allocation near
 * the bottom of RAM. A machine whose firmware handed off a map covering
 * less than that would fault here rather than corrupt anything, which is
 * the failure mode to want. */
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

/* M90: one 2 MiB identity page, allocating the PDPT/PD structure above it
 * on the way down. Idempotent - e820 ranges rounded outward to 2 MiB can
 * overlap each other, and mapping the same block twice has to be free
 * rather than an error.
 *
 * Deliberately 2 MiB pages and not 1 GiB ones, which the CPU almost
 * certainly supports and which would cut the page tables for 8 GiB from
 * 40 KiB to 4 KiB. Every walk in this file tests for PTE_HUGE at exactly
 * one level - the PD - and a 1 GiB page puts one at the PDPT level
 * instead. Supporting both would mean auditing nine functions
 * (map/unmap/take, user_range_ok, destroy, fork, cow_break,
 * unmap_range_free, and this) for a second huge-page case, to save 36 KiB
 * on a machine with gigabytes. The saving is not the point of the
 * milestone and the audit is exactly the kind of thing that gets one
 * function wrong. */
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
    /* Before any table is built, because a leaf written with PTE_NX under
     * a CPU whose EFER.NXE is clear faults on a reserved bit rather than
     * being ignored. */
    nx_enabled = cpu_has_nx();
    vmm_enable_nx_this_cpu();

    uint64_t pml4_phys = alloc_table();
    kernel_pml4 = phys_to_table(pml4_phys);
    kernel_pml4_phys = pml4_phys;

    /* Every RAM range the firmware described, rounded outward to whole
     * 2 MiB pages - not every *usable* range, which is a different and
     * wrong set. kernel/acpi/acpi.c reads ACPI tables at the physical
     * addresses the RSDT points at, and those live in firmware-reserved
     * RAM that no allocator will ever hand out; before M90 they were
     * covered by accident, because a flat 1 GiB map covered everything
     * low regardless of type. What must stay *out* is MMIO: drivers/fb.c
     * and arch/x86_64/lapic.c map their own device pages 4 KiB at a time,
     * and vmm_map_page_in panics outright on an address already inside a
     * huge page. e820.h is where those three cases became three types.
     *
     * And one bound that the types alone do not give: nothing above the
     * last byte of real memory is mapped, whatever its type says. OVMF on
     * this machine reports a 12 GiB EfiReservedMemoryType range at
     * 1012 GiB - the address-space window the PCIe hierarchy lives in,
     * reserved so that nothing allocates over it, and emphatically not
     * memory. The first version of this loop mapped it, and reported a
     * 16 GiB identity map on a 4 GiB machine. "Reserved" answers *may
     * anything allocate here*; it does not answer *is there RAM here*, and
     * the only entries that answer the second one affirmatively are the
     * ones a frame allocator or an ACPI table could occupy. So the
     * ceiling comes from those, and a reserved range above every byte of
     * real memory is read as what it is: an address-space reservation. */
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

    /* The first 2 MiB unconditionally. The real-mode IVT/BDA, the VGA
     * text buffer at 0xB8000 and the AP trampoline's landing address are
     * all in it, and a firmware that describes the first page as
     * something other than RAM would otherwise leave them unmapped. */
    identity_map_block(0);

    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");

    klog_puts("[vmm] kernel-owned page tables installed (");
    klog_put_hex64(identity_map_pages * 2);
    klog_puts(" MiB identity-mapped in 2 MiB pages, NX ");
    klog_puts(nx_enabled ? "on" : "unavailable");
    klog_puts(")\n");
}

/* M90: does the identity map cover [phys, phys + len)?
 *
 * Answered by walking the tables rather than by remembering a limit,
 * because the map is no longer a single range starting at zero - a
 * machine with more than ~3 GiB has RAM below the PCI hole and RAM above
 * 4 GiB with nothing in between, and a limit would call the hole mapped.
 * kernel/acpi/acpi.c is the caller: it dereferences table addresses the
 * firmware chose and has degraded gracefully on anything out of reach
 * since M29, which is a check it could only make against a constant
 * until now. */
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

int vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    /* Every walk here is create = 0: a missing level means the address
     * was never mapped in this address space, which is a -1, not a
     * reason to build page tables for it. */
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
    /* invlpg only touches this CPU's TLB, and this address is private to
     * one process's address space (PML4[1]) - so the only CPU that can
     * have it cached is one that has run this task, and it will reload
     * CR3 before running any other address space anyway. */
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return 0;
}

uint64_t vmm_unmap_page_take(uint64_t pml4_phys, uint64_t virt) {
    /* M78: unmap and hand back the frame that was there, in one step and
     * under one acquisition of the lock. Two calls - "what is mapped
     * here" then "unmap it" - would be the same thing with a window in
     * the middle in which another CPU could have replaced the mapping,
     * and the caller would then free a frame somebody else is using. The
     * one caller that needs this (SYS_munmap) owns both halves, so
     * fusing them costs nothing and removes the window entirely. */
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
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory"); /* see vmm_unmap_page_in on why one CPU is enough */
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return phys;
}

void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t extra = flags & PTE_USER;

    uint64_t *pdpt = table_walk(pml4, PML4_INDEX(virt), 1, extra);
    uint64_t *pd = table_walk(pdpt, PDPT_INDEX(virt), 1, extra);
    if (pd[PD_INDEX(virt)] & PTE_HUGE) {
        panic("vmm_map_page_in: address falls inside a 2 MiB huge-mapped range");
    }
    uint64_t *pt = table_walk(pd, PD_INDEX(virt), 1, extra);

    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | leaf_flags(flags);
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
}

/* M91: change the permissions of every page already present in
 * [start, end) without touching what is mapped there. Returns how many
 * entries it rewrote.
 *
 * Pages inside the range that are NOT present are deliberately left
 * alone: in a demand-paged address space most of a mapping has no entry
 * at all, and the authority on what those pages will become is the
 * region's own `prot` (sched.h's mmap_region_t), which mprotect updates
 * separately. Writing entries for them here would defeat M82 by
 * materialising a mapping the moment anybody adjusted its permissions.
 *
 * A copy-on-write page is the one case that needs care and gets it: a
 * PTE_COW page stays read-only in the hardware whatever the caller asks
 * for, because the whole mechanism depends on the write faulting. It
 * keeps its mark, so vmm_cow_break still separates it later - and it is
 * given the *new* NX bit immediately, because that one has nothing to do
 * with the fault. */
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
        return 0; /* the length wrapped the address space - never a real range */
    }
    uint64_t need = PTE_PRESENT | PTE_USER | (need_write ? PTE_WRITABLE : 0);

    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    for (uint64_t page = virt & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        /* Deliberately not table_walk: that helper masks the entry down
         * to its address and hands back the next table, which is exactly
         * the flag information this check exists to look at. The walk is
         * the same shape; what differs is that every level's flags are
         * required rather than assumed. */
        uint64_t e = pml4[PML4_INDEX(page)];
        if ((e & need) != need) {
            spin_unlock_irqrestore(&vmm_lock, irq_flags);
            return 0;
        }
        uint64_t *pdpt = phys_to_table(e & PTE_ADDR_MASK);
        e = pdpt[PDPT_INDEX(page)];
        if ((e & need) != need || (e & PTE_HUGE)) {
            spin_unlock_irqrestore(&vmm_lock, irq_flags);
            return 0; /* a 1 GiB page is never something a user process owns here */
        }
        uint64_t *pd = phys_to_table(e & PTE_ADDR_MASK);
        e = pd[PD_INDEX(page)];
        if ((e & need) != need || (e & PTE_HUGE)) {
            spin_unlock_irqrestore(&vmm_lock, irq_flags);
            return 0; /* likewise 2 MiB - the identity map's huge pages are kernel-only and live under PML4[0] */
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
    uint64_t *pml4 = phys_to_table(pml4_phys);
    /* From 1, not 0. PML4[0] is the kernel's own map, shared by reference
     * with every address space - see this function's header. */
    for (uint64_t i = 1; i < ENTRIES_PER_TABLE; i++) {
        if (!(pml4[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t *pdpt = phys_to_table(pml4[i] & PTE_ADDR_MASK);
        for (uint64_t j = 0; j < ENTRIES_PER_TABLE; j++) {
            if (!(pdpt[j] & PTE_PRESENT) || (pdpt[j] & PTE_HUGE)) {
                continue; /* nothing here builds 1 GiB pages in a user space */
            }
            uint64_t *pd = phys_to_table(pdpt[j] & PTE_ADDR_MASK);
            for (uint64_t k = 0; k < ENTRIES_PER_TABLE; k++) {
                if (!(pd[k] & PTE_PRESENT) || (pd[k] & PTE_HUGE)) {
                    continue; /* likewise 2 MiB - only the identity map has those, and it is under PML4[0] */
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

/* ---- M83: cloning an address space, copy-on-write ---------------------
 *
 * Walks every user translation in `src` and installs the same physical
 * page in a fresh address space, with both copies made read-only and
 * marked PTE_COW, and the frame's owner count raised by one. Neither
 * process can write to a shared page without faulting, and the fault is
 * where the copy actually happens (vmm_cow_break).
 *
 * PML4[0] is shared by reference, exactly as vmm_create_address_space
 * does it: that is the kernel's own identity map and is not this
 * process's to copy. `owned` names which user ranges ARE - see the note
 * at the addr_in_owned check below for why that list has to be the same
 * one teardown uses.
 *
 * Only pages that were WRITABLE are marked, and that distinction is the
 * one thing here that has to be right. A page that was already read-only
 * - the text of the program, a PROT_READ mapping - is shared as it
 * stands, with no mark: nobody can write to it, so there is nothing to
 * copy, and more importantly a write to it must keep being fatal. If
 * read-only pages were marked too, the fault handler would have no way
 * left to tell "this was writable before the fork" from "this was never
 * writable", and breaking the copy would silently hand a process write
 * access to its own machine code.
 *
 * Teardown does not need the mark either way: an unmarked shared page
 * still has two owners in the frame's own count, and
 * vmm_destroy_address_space drops one per address space exactly as it
 * does for an unshared one.
 *
 * Returns the new PML4's physical address, or 0 if it ran out of memory
 * partway - in which case it has already given back everything it built,
 * because a half-cloned address space is not something a caller can do
 * anything sensible with.
 */
/* ---- M82: unmapping a range by walking tables, not addresses ----------
 *
 * Unmaps every present page in [start, end) and returns each one's frame
 * to the allocator, reporting how many that was.
 *
 * The obvious implementation - a loop over addresses calling
 * vmm_unmap_page_take - is what SYS_munmap did, and demand paging is what
 * made it wrong. Before M82 a mapping was fully backed, so "pages in the
 * range" and "pages actually mapped" were the same number. Now a program
 * can reserve 144 MiB, touch two pages of it, and unmap the lot - and the
 * address loop does thirty-six thousand four-level page-table walks to
 * free two frames.
 *
 * This walks the tables instead: an absent PDPT entry skips 512 GiB, an
 * absent PD entry skips 1 GiB, an absent PT entry skips 2 MiB. The cost
 * becomes proportional to what is mapped rather than to what was
 * reserved, which is the shape every operation on a sparse address space
 * should have.
 *
 * Honest postscript: this was written expecting to recover the twelve
 * seconds M82's self-test costs, and it recovered none of them - the
 * measurement before and after is 12.19 s against 12.29 s, which is
 * noise. So the twelve seconds are somewhere else and this is not the
 * fix for them. It is kept because the complexity argument stands on its
 * own: unmapping a sparse gigabyte should not do a quarter of a million
 * page-table walks to free two frames, whatever the profile of one
 * self-test happens to say. The thing that would be wrong is leaving
 * this comment claiming a saving it did not make.
 */
uint64_t vmm_unmap_range_free(uint64_t pml4_phys, uint64_t start, uint64_t end) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t *pml4 = phys_to_table(pml4_phys);
    uint64_t freed = 0;

    for (uint64_t addr = start; addr < end; ) {
        uint64_t *pdpt = table_walk(pml4, PML4_INDEX(addr), 0, 0);
        if (!pdpt) {
            /* Nothing under this PML4 entry at all - jump to the next. */
            addr = (addr + (1ULL << 39)) & ~((1ULL << 39) - 1);
            continue;
        }
        uint64_t *pd = table_walk(pdpt, PDPT_INDEX(addr), 0, 0);
        if (!pd) {
            addr = (addr + (1ULL << 30)) & ~((1ULL << 30) - 1);
            continue;
        }
        if (pd[PD_INDEX(addr)] & PTE_HUGE) {
            /* Nothing builds 2 MiB user pages, and stepping over one is
             * the honest thing to do rather than taking it apart. */
            addr = (addr + (1ULL << 21)) & ~((1ULL << 21) - 1);
            continue;
        }
        uint64_t *pt = table_walk(pd, PD_INDEX(addr), 0, 0);
        if (!pt) {
            addr = (addr + (1ULL << 21)) & ~((1ULL << 21) - 1);
            continue;
        }
        /* A real page table: walk the entries this range covers. */
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
    dst[0] = src[0]; /* the kernel's map, shared by reference */

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
                        /* Not this process's page to copy. The shm window
                         * and the framebuffer live in a user address
                         * space without belonging to it - shm.c and the
                         * compositor own those frames and free them on
                         * their own terms, which is exactly why
                         * process_destroy_address_space leaves them
                         * alone.
                         *
                         * Fork has to use the same list, and the reason is
                         * an invariant rather than a preference: this
                         * function takes an owner reference on every page
                         * it copies, and teardown drops one for every page
                         * it frees. Copy a page teardown will not free and
                         * that reference is never given back - a frame
                         * pinned for the machine's uptime with nothing
                         * pointing at it. So fork copies exactly what
                         * teardown frees, and the two take the same list
                         * from the same place (proc.c). */
                        continue;
                    }
                    uint64_t phys = s_pt[l] & PTE_ADDR_MASK;

                    /* Build the child's path down to this page. */
                    uint64_t *d_pdpt = table_walk(dst, i, 1, PTE_USER);
                    uint64_t *d_pd = d_pdpt ? table_walk(d_pdpt, j, 1, PTE_USER) : (uint64_t *)0;
                    uint64_t *d_pt = d_pd ? table_walk(d_pd, k, 1, PTE_USER) : (uint64_t *)0;
                    if (!d_pt) {
                        /* Not reachable today and kept anyway: table_walk
                         * allocates through pmm_alloc_frame, which panics
                         * on exhaustion rather than returning nothing -
                         * the same behaviour every other allocation in
                         * this file has. If that ever becomes a return
                         * instead of a panic, this is the path that has
                         * to be right, and a rollback written after the
                         * fact is a rollback nobody tested. */
                        ok = 0;
                        break;
                    }

                    /* One more owner on the frame either way. A writable
                     * page becomes read-only and marked on both sides, so
                     * the next write to either takes the fault that
                     * separates them; a read-only page is simply shared.
                     * The parent's entry is rewritten in place in the
                     * first case, which is why its TLB entry has to go. */
                    uint64_t shared = s_pt[l];
                    if (shared & PTE_WRITABLE) {
                        shared = (shared & ~PTE_WRITABLE) | PTE_COW;
                        s_pt[l] = shared;
                        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
                    }
                    d_pt[l] = shared;
                    pmm_frame_ref(phys);
                }
            }
        }
    }
    spin_unlock_irqrestore(&vmm_lock, irq_flags);

    if (!ok) {
        /* Everything built so far is a legitimate address space holding
         * legitimate references, so the ordinary teardown is exactly the
         * right way to give it back - including dropping the owner counts
         * this function raised. The parent's own entries stay read-only
         * and marked, which is harmless: the first write to one finds a
         * frame with a single owner and takes it back (vmm_cow_break). */
        vmm_destroy_address_space(dst_phys, owned, owned_count);
        return 0;
    }
    return dst_phys;
}

/* ---- M83: taking a copy-on-write page apart ---------------------------
 *
 * Called from the page-fault path for a WRITE to a page that is present.
 * Returns 1 if this was a copy-on-write page and the caller now has a
 * private, writable copy of it; 0 if it was not, which leaves the fault
 * exactly as fatal as it was before.
 *
 * The single-owner case is not an optimization, it is the common one: a
 * child that has exited, or a parent that has written to every page
 * already, leaves the other side holding a frame nobody else has. Copying
 * it would be copying a page to itself. Taking the mark off and making it
 * writable again is the whole operation.
 */
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
        return 0; /* not a shared page - somebody else's problem, and still fatal */
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
        return 0; /* out of memory - the write cannot be completed, so the process dies */
    }
    const uint8_t *from = (const uint8_t *)old_phys;
    uint8_t *to = (uint8_t *)new_phys;
    for (uint64_t i = 0; i < PAGE_SIZE; i++) {
        to[i] = from[i];
    }
    /* M91: PTE_NX carried over with the rest. A copy-on-write break that
     * dropped it would hand back an executable copy of a page that was
     * not executable before - a W^X hole that appears only after a fork,
     * which is exactly the kind of thing nobody would find by running a
     * program. Every other place that writes a leaf goes through
     * leaf_flags for the same reason; this one cannot, because it is
     * preserving an entry rather than building one. */
    pt[PT_INDEX(virt)] = new_phys | (entry & (PTE_USER | PTE_PRESENT | PTE_NX)) | PTE_WRITABLE;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock_irqrestore(&vmm_lock, irq_flags);

    /* Outside the lock: this drops one owner from the frame this address
     * space has just stopped using, and pmm has its own. */
    pmm_free_frame(old_phys);
    return 1;
}

uint64_t vmm_create_address_space(void) {
    uint64_t irq_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t new_phys = alloc_table();
    uint64_t *new_pml4 = phys_to_table(new_phys);
    new_pml4[0] = kernel_pml4[0]; /* share the kernel's identity map + heap */
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
    return new_phys;
}

void vmm_switch_address_space(uint64_t pml4_phys) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}
