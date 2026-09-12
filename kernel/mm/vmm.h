/* kernel/mm/vmm.h
 *
 * Kernel-owned page tables. vmm_init() replaces whatever paging the boot
 * loader left CR3 pointing at (firmware-owned, not the kernel's own
 * memory) with a fresh PML4 built from pmm_alloc_frame() frames: the same
 * "kernel takes ownership away from a bootloader-owned scratch structure"
 * pattern M4 already used for the GDT (gdt.c).
 *
 * M90: the identity map it builds used to be a flat 1 GiB, chosen as a
 * constant and shared with pmm.h so the two could not drift. It is now
 * every RAM range in the e820 map the boot loader handed off, rounded
 * outward to 2 MiB pages - which is both larger and *smaller* than the
 * old map, and the smaller half matters as much: MMIO ranges the
 * firmware described are deliberately left out, because drivers/fb.c and
 * arch/x86_64/lapic.c map their own device pages 4 KiB at a time and
 * vmm_map_page panics on an address already covered by a huge page.
 *
 * vmm_map_page/vmm_unmap_page give the rest of the kernel a real
 * map/unmap API for 4 KiB pages at virtual addresses outside that
 * bootstrap range - the kernel heap (heap.c) is the first user.
 */
#pragma once

#include <stdint.h>

#define VMM_FLAG_WRITABLE (1ULL << 1) /* matches the hardware PTE writable bit directly */
#define VMM_FLAG_USER     (1ULL << 2) /* matches the hardware PTE U/S bit - ring 3 may access the page (M9) */
/* M91: may code be fetched from this page? Unlike the two above, this
 * does NOT match a hardware bit - the hardware bit is execute-*disable*
 * and lives at bit 63. It is spelled the positive way here because that
 * is how a caller thinks (PROT_EXEC), and because a flag whose absence is
 * the permissive case is a flag every existing call site would silently
 * get wrong. Absent means the page is mapped NX, which is why every
 * caller in this kernel had to be looked at when it was introduced. */
#define VMM_FLAG_EXEC     (1ULL << 3)

/* M107: PCD - "page cache disable" - and it matches the hardware bit
 * directly, like WRITABLE and USER rather than like EXEC.
 *
 * It exists because three drivers arrived at once that talk to their
 * controller through memory rather than through ports, and a device
 * register read out of a cache line is not a device register read. The
 * whole synchronisation model of AHCI, NVMe and xHCI is "write a
 * doorbell, then spin on a status word the device updates by DMA"; a
 * cached mapping is entitled to serve that spin from L1 forever.
 *
 * Nothing here caught it, and that is the point worth writing down:
 * QEMU's emulated MMIO is a trap into the hypervisor whatever the guest
 * PAT says, so every one of these drivers works perfectly with this flag
 * absent. It is a flag whose whole value is on hardware this project has
 * never booted - M110's business - which is exactly the class of
 * assumption M107's "graded under QEMU first" bullet is meant to stop
 * shipping unexamined. */
#define VMM_FLAG_NOCACHE  (1ULL << 4)

/* M90: where the kernel heap starts, and the one virtual address in this
 * kernel that is chosen rather than derived.
 *
 * heap.c used to start the heap at PMM_TRACKED_MEMORY - "the first
 * address above the 1 GiB vmm_init() identity-maps", which was a correct
 * derivation right up to the moment the identity map stopped being a
 * constant. A heap based on the size of the machine's RAM would move
 * whenever the RAM did, and would collide with the identity map on any
 * machine with more memory than the one it was built on.
 *
 * 256 GiB is above any physical memory this kernel will see and below
 * USER_REGION_BASE (512 GiB, kernel/proc/proc.h), so it stays inside
 * PML4[0] - the entry every address space shares - which is what makes
 * the kernel heap reachable no matter which CR3 is loaded. */
#define KERNEL_HEAP_VIRT_BASE 0x0000004000000000ULL /* 256 GiB */

/* M90: takes the e820 map (kernel/mm/e820.h) it builds the identity map
 * from. Must be called after pmm_init, which is where the frames for its
 * own page tables come from. */
void vmm_init(const uint32_t *e820_map);

/* M90: is [phys, phys + len) inside the identity map? Walks the tables
 * rather than comparing against a limit, because the map is not one range
 * starting at zero on a machine with a PCI hole in the middle of it.
 * kernel/acpi/acpi.c is the caller. */
int vmm_identity_covers(uint64_t phys, uint64_t len);

/* M91: execute-disable. vmm_init turns it on for the BSP if the CPU has
 * it; smp.c calls the per-CPU half for every AP, because EFER is per-CPU
 * state and the page tables carrying the bit are not - an AP that skipped
 * it would fault on a reserved bit at the first user page it touched.
 * vmm_nx_enabled reports whether any of it is real, so a self-test can
 * say what it actually proved on this machine. */
void vmm_enable_nx_this_cpu(void);
int vmm_nx_enabled(void);

/* M91: rewrite the permissions of every page already present in
 * [start, end) without changing what is mapped there; returns how many
 * entries changed. Pages with no entry yet are left alone - in a
 * demand-paged address space that is most of a mapping, and the authority
 * on what they will become is the region's own prot. A copy-on-write page
 * keeps its mark and stays hardware-read-only whatever is asked, because
 * the mechanism depends on the write still faulting. */
uint64_t vmm_protect_range_in(uint64_t pml4_phys, uint64_t start, uint64_t end,
                              uint64_t flags);

/* The kernel's own address space: PML4[0], covering the 1 GiB identity
 * map and the heap. Every per-process address space (vmm_create_address_
 * space) shares this exact slot, so kernel code/data/heap stay reachable
 * regardless of which CR3 is loaded - user mappings live in a separate,
 * private PML4 slot instead (see proc.c). */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* ---- M107: mapping a device's memory BAR ------------------------------
 *
 * Maps [phys, phys + len) uncacheable and returns a pointer to it. `len`
 * is rounded up to whole pages, because a BAR is the device's to own.
 *
 * ---- the window, which is the whole reason this is not identity-mapped
 *
 * The first version of this mapped virt == phys, the way drivers/fb.c
 * maps the framebuffer, and it worked perfectly for AHCI and then page-
 * faulted the machine on the first NVMe doorbell written after the
 * scheduler had switched to a user process.
 *
 * The reason is the note on KERNEL_HEAP_VIRT_BASE above, read the other
 * way round. Every address space shares PML4[0] and nothing else, so a
 * kernel mapping is only reachable from a process's CR3 if it lives below
 * 512 GiB. QEMU puts a 64-bit PCI BAR at 768 GiB. An identity mapping of
 * that BAR is a kernel-PML4 entry under PML4[1], which is visible while
 * the kernel's own address space is loaded - that is, throughout init,
 * which is exactly why every register access during probing worked - and
 * invisible the moment anything else runs.
 *
 * So MMIO gets a window of its own inside PML4[0], and virt != phys. That
 * costs nothing: a BAR is registers, and registers are never handed back
 * to a device as a DMA address - the buffers that are come from
 * pmm_alloc_contiguous and are identity-mapped like all of low memory.
 *
 * 384 GiB is the base: 128 GiB above the kernel heap and 128 GiB below
 * USER_REGION_BASE. The heap is backed one-to-one by physical frames, so
 * reaching this window would mean a machine with 128 GiB of RAM entirely
 * inside the kernel heap, which is not a configuration that boots for
 * other reasons first.
 *
 * Returns 0 if the window is exhausted or a page table cannot be
 * allocated - a refusal rather than a panic, for Q16's reason: a card
 * this kernel cannot address is a fact about the machine, and the machine
 * should still boot without it. */
#define KERNEL_MMIO_VIRT_BASE 0x0000006000000000ULL /* 384 GiB */
#define KERNEL_MMIO_VIRT_SIZE 0x0000002000000000ULL /* 128 GiB of window */
void *vmm_map_mmio(uint64_t phys, uint64_t len);
void vmm_unmap_page(uint64_t virt);
uint64_t vmm_kernel_pml4_phys(void);

/* Q2: where the kernel heap starts, asked rather than assumed.
 *
 * heap.c used to expand KERNEL_HEAP_VIRT_BASE itself, and heap.c's own
 * M90 comment is a paragraph about how deriving that address wrongly put
 * the heap inside the identity map on a machine with 2 GiB. An address
 * the heap gets from the module that owns the address space is one fewer
 * place for that to happen again.
 *
 * It is also what makes the heap testable off the machine: 256 GiB is not
 * an address a host process can map, so a heap that hardcodes it can only
 * ever be exercised by booting. See tests/fakes/fake_vmm.c. */
uint64_t vmm_kernel_heap_base(void);

/* Per-address-space variant: pml4_phys need not be the currently loaded
 * CR3 - safe to call from kernel context to build up a new process's
 * mappings before ever switching to it (every pmm-allocated frame,
 * including a fresh PML4's own storage, lives inside the always-identity-
 * mapped first 1 GiB - see vmm.c's phys_to_table). */
void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

/* M102: the same mapping, returning -1 instead of halting the machine
 * when there is no frame left for a page table. 0 on success.
 *
 * The panicking form above stays, and which one a call site uses is a
 * statement about that site: the framebuffer and the Local APIC are
 * mapped once at boot and a failure there means the machine cannot run,
 * while a process growing its heap is an ordinary thing that can
 * ordinarily fail. Same split as pmm_alloc_frame and
 * pmm_try_alloc_frame, which this mirrors deliberately. */
int vmm_try_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

/* M50: vmm_map_page_in's counterpart, and the piece SYS_shm_free needed.
 * vmm_unmap_page above walks the *kernel* PML4 and panics on anything it
 * doesn't find, which is right for kernel mappings and exactly wrong here:
 * the address comes from a user process, so "not mapped" is ordinary bad
 * input rather than a kernel bug. Returns 0 if a page was unmapped and -1
 * if there was nothing there - never panics, which is the whole
 * difference. */
int vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt);

/* M78: unmap `virt` in `pml4_phys` and return the physical frame that was
 * mapped there, or 0 if nothing was. One step rather than a lookup
 * followed by an unmap, so there is no window in which the mapping could
 * change between the two - see the implementation's own note. The caller
 * owns the returned frame and is the one that hands it to
 * pmm_free_frame; this function deliberately does not, because the same
 * primitive is the right one for a caller that wants to move a frame
 * rather than release it. */
uint64_t vmm_unmap_page_take(uint64_t pml4_phys, uint64_t virt);

/* M52: is every page of [virt, virt + len) present, user-accessible and
 * (if need_write) writable in the address space rooted at pml4_phys?
 *
 * This is the primitive `copy_from_user`/`copy_to_user` are built on, and
 * therefore the thing that finally makes a bad pointer from a user
 * program an error return rather than a kernel fault. It answers the
 * question the hardware would answer by faulting, by doing the same walk
 * the hardware does - PRESENT and U/S required at *every* level, which is
 * how x86 paging itself computes access rights, so a page whose leaf
 * entry looks fine under a kernel-only PDPT is correctly refused.
 *
 * len == 0 is true for any address (there is nothing to touch). A range
 * that wraps past the top of the address space is false rather than
 * being wrapped around, which is the "a length that overflows the range"
 * case in M52's garbage-argument matrix. */
int vmm_user_range_ok(uint64_t pml4_phys, uint64_t virt, uint64_t len, int need_write);

/* M54: one virtual-address range whose *leaf* frames belong to the
 * process and are therefore this allocator's to hand back - [lo, hi). */
typedef struct {
    uint64_t lo, hi;
} vmm_range_t;

/* M54: tears down a per-process address space - the thing M29 documented
 * as missing, M50 measured at ~15 frames per dead process, and neither
 * fixed. Frees every page table in the PML4[1..511] subtree, and every
 * leaf frame whose virtual address falls inside one of `owned`.
 *
 * PML4[0] is deliberately untouched: it is the shared kernel map, the
 * same subtree in *every* address space, and freeing it would take the
 * machine with it.
 *
 * The `owned` list is why this takes an argument instead of freeing
 * every leaf it finds. A process's address space contains mappings whose
 * frames are emphatically not its own - an shm segment created by another
 * process (kernel/ipc/shm.c owns those, and one of them may still be
 * mapped by somebody else) and the linear framebuffer, whose physical
 * addresses are device memory that was never a pmm frame at all and
 * would poison the free list. The caller that knows the layout
 * (kernel/proc/proc.c) says which ranges are genuinely the process's;
 * everything else is unmapped by discarding the page tables and nothing
 * more.
 *
 * The pml4 frame itself is freed last. Never call this on the address
 * space the calling CPU is currently running on - see
 * task_exit_with_code, which switches to the kernel's own first. */
void vmm_destroy_address_space(uint64_t pml4_phys, const vmm_range_t *owned, int owned_count);

/* M83: clone `src_pml4_phys` copy-on-write. Every page in `owned` becomes
 * read-only and PTE_COW in both address spaces and gains an owner; the
 * first write to one takes the fault that separates them. Returns the new
 * PML4's physical address, or 0.
 *
 * `owned` must be the same list vmm_destroy_address_space is given for
 * these address spaces - see the implementation for why that is an
 * invariant and not a convention. kernel/proc/proc.c holds the one copy
 * and process_fork_address_space is the call that should be used. */
/* M82: unmap every present page in [start, end) and give its frame back,
 * returning how many there were. Walks page tables rather than addresses,
 * so a sparse range costs what is mapped rather than what was reserved -
 * see the implementation for the twelve seconds of boot that bought. */
uint64_t vmm_unmap_range_free(uint64_t pml4_phys, uint64_t start, uint64_t end);

uint64_t vmm_fork_address_space(uint64_t src_pml4_phys, const vmm_range_t *owned, int owned_count);

/* ---- M98: the resident set --------------------------------------------
 *
 * How many present user pages an address space has now, and the most it
 * has ever had. Both are maintained inside vmm.c at the four places a
 * leaf entry is written or cleared, so they cost an add and a compare on
 * a path that has just walked four page tables.
 *
 * Keyed on the PML4's physical address because that is an address
 * space's only identity here: threads share one and would otherwise be
 * counted once each. An address space with no accounting slot - and the
 * kernel's own, which is deliberately never given one - answers 0, which
 * the one caller that reports these numbers prints as "not measured"
 * rather than as "no memory". */
uint64_t vmm_rss_pages(uint64_t pml4_phys);
uint64_t vmm_rss_peak_pages(uint64_t pml4_phys);

/* M83: a write to a page shared by a fork. Returns 1 if `virt` was a
 * copy-on-write page and this address space now has a private writable
 * copy, 0 if it was not - in which case the fault is as fatal as it has
 * always been. */
int vmm_cow_break(uint64_t pml4_phys, uint64_t virt);

/* Allocates a fresh PML4 with PML4[0] shared with the kernel's (so ring 0
 * code - interrupt/syscall handlers - keeps working no matter which
 * process's CR3 is loaded) and everything else zeroed, ready for
 * process-private mappings via vmm_map_page_in. */
/* A fresh address space, or **0** when there is no frame for its PML4.
 * M102: it used to panic, and every caller has to check now. */
uint64_t vmm_create_address_space(void);
void vmm_switch_address_space(uint64_t pml4_phys);
