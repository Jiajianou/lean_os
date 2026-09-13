#pragma once

#include <stddef.h>
#include <stdint.h>

#include "scheduler/scheduler.h"

#define PAGE_SIZE        4096ULL
#define GIB              (1024ULL * 1024 * 1024)
#define USER_STACK_TOP   0x000000FC00000000ULL
#define USER_STACK_PAGES 16
#define USER_STACK_MAX_BYTES (64ULL * 1024 * 1024)
#define USER_STACK_LIMIT (USER_STACK_TOP - USER_STACK_MAX_BYTES)
#define USER_ARG_ADDR    USER_STACK_TOP
#define USER_ARG_PAGES   32
#define USER_ARG_BYTES   (USER_ARG_PAGES * PAGE_SIZE)

#define USER_ENV_MAX_VARS  64
#define USER_ENV_MAX_BYTES 4096
#define USER_HEAP_START  0x0000009000000000ULL
#define USER_HEAP_LIMIT  0x000000A000000000ULL
#define USER_MMAP_BASE   0x000000A000000000ULL
#define USER_MMAP_LIMIT  0x000000E000000000ULL
#define USER_SHM_BASE    0x000000E000000000ULL
#define USER_SHM_LIMIT   0x000000F000000000ULL
#define USER_FB_BASE     0x000000F000000000ULL

#define USER_REGION_BASE  0x0000008000000000ULL
#define USER_REGION_LIMIT 0x0000010000000000ULL

#define USER_IMAGE_BASE  0x0000008000000000ULL
#define USER_IMAGE_LIMIT 0x0000009000000000ULL

void process_destroy_address_space(uint64_t pml4_phys);

uint64_t process_fork_address_space(uint64_t source_pml4_phys);

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
