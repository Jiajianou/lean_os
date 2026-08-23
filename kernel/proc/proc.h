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

/* arg may be NULL (equivalent to an empty string) for a program that
 * doesn't take one. */
task_t *process_spawn(const uint8_t *image, size_t image_size, const char *arg);
