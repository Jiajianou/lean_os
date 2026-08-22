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

/* arg may be NULL (equivalent to an empty string) for a program that
 * doesn't take one. */
task_t *process_spawn(const uint8_t *image, size_t image_size, const char *arg);
