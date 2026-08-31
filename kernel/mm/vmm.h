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

/* The kernel's own address space: PML4[0], covering the 1 GiB identity
 * map and the heap. Every per-process address space (vmm_create_address_
 * space) shares this exact slot, so kernel code/data/heap stay reachable
 * regardless of which CR3 is loaded - user mappings live in a separate,
 * private PML4 slot instead (see proc.c). */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_page(uint64_t virt);
uint64_t vmm_kernel_pml4_phys(void);

/* Per-address-space variant: pml4_phys need not be the currently loaded
 * CR3 - safe to call from kernel context to build up a new process's
 * mappings before ever switching to it (every pmm-allocated frame,
 * including a fresh PML4's own storage, lives inside the always-identity-
 * mapped first 1 GiB - see vmm.c's phys_to_table). */
void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

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

/* M83: a write to a page shared by a fork. Returns 1 if `virt` was a
 * copy-on-write page and this address space now has a private writable
 * copy, 0 if it was not - in which case the fault is as fatal as it has
 * always been. */
int vmm_cow_break(uint64_t pml4_phys, uint64_t virt);

/* Allocates a fresh PML4 with PML4[0] shared with the kernel's (so ring 0
 * code - interrupt/syscall handlers - keeps working no matter which
 * process's CR3 is loaded) and everything else zeroed, ready for
 * process-private mappings via vmm_map_page_in. */
uint64_t vmm_create_address_space(void);
void vmm_switch_address_space(uint64_t pml4_phys);
