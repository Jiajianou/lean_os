/* kernel/proc/proc.h
 *
 * Turns an in-memory ELF64 image into a running ring-3 task: a fresh
 * private address space (vmm_create_address_space), the image's segments
 * (elf.h), a user stack, an optional single string argument (M13's
 * entire "argv" - see proc.c), and a task (sched.h) whose first action is
 * to drop to ring 3 at the image's entry point.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sched/sched.h"

/* Per-process virtual address space layout - every private (PML4[1])
 * region a process gets, in one place so proc.c (which sets it up) and
 * syscall.c (which bounds-checks growth into it - SYS_sbrk, SYS_shm_map)
 * can't drift apart. Generous gaps between regions rather than packed
 * tightly: nothing here needs to be dense, and it avoids ever having to
 * reason precisely about exact boundaries. */
#define PAGE_SIZE        4096ULL
#define USER_STACK_TOP   0x0000008000200000ULL /* 512 GiB + 2 MiB */
#define USER_STACK_PAGES 4                      /* 16 KiB */
#define USER_ARG_ADDR    USER_STACK_TOP
#define USER_HEAP_START  0x0000008000400000ULL /* 512 GiB + 4 MiB - M19 */
#define USER_HEAP_LIMIT  0x0000008010000000ULL /* 512 GiB + 256 MiB ceiling - M19 */
#define USER_SHM_BASE    0x0000008020000000ULL /* 512 GiB + 512 MiB - M19 */
#define USER_FB_BASE     0x0000008040000000ULL /* 512 GiB + 1 GiB - M20, SYS_fb_map's fixed target address */

/* M52: the whole private region, which is exactly what PML4 entry 1
 * covers - [512 GiB, 1 TiB). Every constant above lives inside it, and
 * nothing a process may legitimately hand the kernel a pointer to lives
 * outside it: PML4[0] is the shared kernel map (the identity map and the
 * kernel heap), so an address below this base is by definition either a
 * kernel address or a null-ish one, and either way not the caller's to
 * name. syscall.c's copy_from_user/copy_to_user check this first and the
 * page tables second - the range test is what makes "a kernel address"
 * and "one byte before the user region" errors rather than reads. */
#define USER_REGION_BASE  0x0000008000000000ULL
#define USER_REGION_LIMIT 0x0000010000000000ULL

/* M40: the window an ELF image's own PT_LOAD segments have to fit inside
 * - from the load address user_space/lib/user.ld links every program at,
 * up to (but not into) the stack this file places just above it. elf.c
 * checks every segment against these before mapping anything, because
 * until M40 it checked nothing at all: SYS_spawn will happily hand it any
 * file on disk, and a p_vaddr pointing into the identity-mapped low
 * memory would have mapped kernel pages into a user address space (or
 * panicked the machine on a huge-page collision). Not a security boundary
 * - nothing else in this project is either - but "a user program can take
 * down the kernel by spawning a text file" is a bug at any threat model. */
#define USER_IMAGE_BASE  0x0000008000000000ULL /* 512 GiB - matches user_space/lib/user.ld */
#define USER_IMAGE_LIMIT (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)

/* M54: hands `pml4_phys` and every frame the process itself owns back to
 * the allocator. Lives here rather than in vmm.c because the decision it
 * makes is entirely about *this* file's layout: the image, the stack and
 * the argument page are the process's, the heap is the process's, and
 * the shm and framebuffer windows are emphatically not - one holds
 * segments another process may still have mapped, the other holds device
 * physical addresses that were never pmm frames and would poison the
 * free list. vmm.c has no business knowing any of that.
 *
 * Never call this on the address space the calling CPU is running on;
 * task_exit_with_code switches to the kernel's own first. */
void process_destroy_address_space(uint64_t pml4_phys);

/* arg may be NULL (equivalent to an empty string) for a program that
 * doesn't take one.
 *
 * M45: `name` is what this process will be listed as (task_t.name,
 * sched.h) - the path it was loaded from, which is the only thing at
 * this layer that resembles a human-readable identity. Passed in rather
 * than derived here because this function is handed an in-memory image,
 * not a path: sys_spawn (which read the file) and kernel_main's own
 * self-tests (which read it via vfs_read) are the two callers that
 * actually know it. May be NULL. */
task_t *process_spawn(const char *name, const uint8_t *image, size_t image_size, const char *arg);
