/* kernel/mm/vmm.h
 *
 * Kernel-owned page tables. vmm_init() replaces whatever paging the boot
 * loader left CR3 pointing at (firmware-owned, not the kernel's own
 * memory) with a fresh PML4 built from pmm_alloc_frame() frames: the same
 * "kernel takes ownership away from a bootloader-owned scratch structure"
 * pattern M4 already used for the GDT (gdt.c). It rebuilds the identical
 * 1 GiB identity map so nothing the kernel is currently running out of
 * moves.
 *
 * vmm_map_page/vmm_unmap_page give the rest of the kernel a real
 * map/unmap API for 4 KiB pages at virtual addresses outside that
 * bootstrap range - the kernel heap (heap.c) is the first user.
 */
#pragma once

#include <stdint.h>

#define VMM_FLAG_WRITABLE (1ULL << 1) /* matches the hardware PTE writable bit directly */
#define VMM_FLAG_USER     (1ULL << 2) /* matches the hardware PTE U/S bit - ring 3 may access the page (M9) */

void vmm_init(void);

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

/* Allocates a fresh PML4 with PML4[0] shared with the kernel's (so ring 0
 * code - interrupt/syscall handlers - keeps working no matter which
 * process's CR3 is loaded) and everything else zeroed, ready for
 * process-private mappings via vmm_map_page_in. */
uint64_t vmm_create_address_space(void);
void vmm_switch_address_space(uint64_t pml4_phys);
