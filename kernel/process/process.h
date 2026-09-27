#pragma once

#include "memory_management/virtual_memory.h"

#include <stddef.h>
#include <stdint.h>

/* system_api's process.h, not this one - the angle form is what distinguishes
   them, because a quoted include from this file would find this file. */
#include <process.h>

#include "scheduler/scheduler.h"

#define PAGE_SIZE        4096ULL
#define GIB              (1024ULL * 1024 * 1024)
/* One definition, in system_api/include/process.h, because a C library that
   disagreed with the kernel about where the stack is would be wrong in a way
   nothing reports. */
#define USER_STACK_TOP   OS_MAIN_STACK_TOP
#define USER_STACK_PAGES 16
#define USER_STACK_MAX_BYTES OS_MAIN_STACK_MAX_BYTES
#define USER_STACK_LIMIT (USER_STACK_TOP - USER_STACK_MAX_BYTES)
#define USER_ARGUMENT_ADDRESS    USER_STACK_TOP
#define USER_ARGUMENT_PAGES   32
#define USER_ARGUMENT_BYTES   (USER_ARGUMENT_PAGES * PAGE_SIZE)

#define USER_ENV_MAX_VARS  64
#define USER_ENV_MAX_BYTES 4096
#define USER_HEAP_START  0x0000009000000000ULL
#define USER_HEAP_LIMIT  0x000000A000000000ULL
#define USER_MMAP_BASE   0x000000A000000000ULL
#define USER_MMAP_LIMIT  0x000000E000000000ULL
#define USER_SHARED_MEMORY_BASE    0x000000E000000000ULL
#define USER_SHARED_MEMORY_LIMIT   0x000000F000000000ULL
#define USER_FRAMEBUFFER_BASE     0x000000F000000000ULL

#define USER_REGION_BASE  0x0000008000000000ULL
#define USER_REGION_LIMIT 0x0000010000000000ULL

#define USER_IMAGE_BASE  0x0000008000000000ULL
#define USER_IMAGE_LIMIT 0x0000009000000000ULL

void process_destroy_address_space(uint64_t pml4_phys);

uint64_t process_fork_address_space(uint64_t source_pml4_phys,
                                    const virtual_memory_range_t *shared_ranges,
                                    int shared_count);

uint64_t process_build_address_space(const uint8_t *image, size_t image_size,
                                     const char *const *argv,
                                     const char *const *envp,
                                     uint64_t *out_entry);

task_t *process_spawnv(const char *name, const uint8_t *image, size_t image_size,
                        const char *const *argv);

task_t *process_spawnve(const char *name, const uint8_t *image, size_t image_size,
                        const char *const *argv, const char *const *envp);

task_t *process_spawn(const char *name, const uint8_t *image, size_t image_size, const char *arg);

task_t *process_spawnv_capped(const char *name, const uint8_t *image, size_t image_size,
                              const char *const *argv, uint32_t caps);

task_t *process_spawn_thread(const char *name, uint64_t entry, uint64_t stack_top,
                              uint64_t arg);

task_t *process_spawnve_capped(const char *name, const uint8_t *image, size_t image_size,
                               const char *const *argv, const char *const *envp, uint32_t caps);
