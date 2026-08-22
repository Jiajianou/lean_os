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

/* arg may be NULL (equivalent to an empty string) for a program that
 * doesn't take one. */
task_t *process_spawn(const uint8_t *image, size_t image_size, const char *arg);
