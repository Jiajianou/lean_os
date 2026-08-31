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

    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | (flags & (PTE_WRITABLE | PTE_USER)) | PTE_PRESENT;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    spin_unlock_irqrestore(&vmm_lock, irq_flags);
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
    pt[PT_INDEX(virt)] = new_phys | (entry & (PTE_USER | PTE_PRESENT)) | PTE_WRITABLE;
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
