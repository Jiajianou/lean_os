/* kernel/proc/elf.h
 *
 * Minimal ELF64 loader: maps a trusted, already-in-memory executable's
 * PT_LOAD segments into a process's address space. Nothing validates
 * this against a hostile input - the only source of an image right now
 * is a kernel-embedded test blob (see kernel/proc/init_program/); once
 * M12 adds a real filesystem, loading an arbitrary on-disk binary will
 * need real validation this doesn't attempt yet.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Maps every PT_LOAD segment of `image` (image_size bytes) into the
 * address space rooted at pml4_phys, as user-accessible pages (BSS
 * portions zeroed). pml4_phys need not be the currently loaded CR3 - see
 * vmm_map_page_in. Panics on a malformed or unsupported (non-ET_EXEC,
 * non-x86_64) image. Returns the entry point (ELF header's e_entry). */
uint64_t elf_load(uint64_t pml4_phys, const uint8_t *image, size_t image_size);
