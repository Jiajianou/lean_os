#pragma once

#include <stddef.h>
#include <stdint.h>


uint64_t elf_validate(const uint8_t *image, size_t image_size);

uint64_t elf_load_at(uint64_t pml4_phys, const uint8_t *image, size_t image_size,
                     uint64_t bias);

int elf_is_dyn(const uint8_t *image, size_t image_size);

int elf_interp(const uint8_t *image, size_t image_size, char *out, size_t cap);

uint64_t elf_phdr_vaddr(const uint8_t *image, size_t image_size, uint64_t bias);
uint16_t elf_phnum(const uint8_t *image, size_t image_size);
