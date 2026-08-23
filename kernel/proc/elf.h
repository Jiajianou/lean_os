/* kernel/proc/elf.h
 *
 * Minimal ELF64 loader: maps an already-in-memory executable's PT_LOAD
 * segments into a process's address space.
 *
 * M40 made this validating. It used to `panic` on a malformed image,
 * which was written when the only source of one was a kernel-embedded
 * test blob - but SYS_spawn has been able to hand it any file on disk
 * since M13, so `sys_spawn("some_text_file", "")` from an ordinary user
 * program brought down the whole kernel. It also trusted every offset in
 * the headers, so a crafted (or merely truncated) file could read past
 * the image buffer or map a segment at a kernel address. Every check now
 * fails the load cleanly instead, and all of them run *before* the first
 * frame is allocated, so a rejected image costs nothing and leaves
 * nothing half-mapped.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Maps every PT_LOAD segment of `image` (image_size bytes) into the
 * address space rooted at pml4_phys, as user-accessible pages (BSS
 * portions zeroed). pml4_phys need not be the currently loaded CR3 - see
 * vmm_map_page_in.
 *
 * Returns the entry point (the ELF header's e_entry) on success, or 0 if
 * the image is malformed, unsupported (non-ET_EXEC, non-x86_64,
 * non-ELFCLASS64), inconsistent with its own declared size, or asks to be
 * loaded outside proc.h's USER_IMAGE_BASE..USER_IMAGE_LIMIT window. 0 is
 * an unambiguous sentinel: every image this project loads is linked at
 * 512 GiB (user_space/lib/user.ld), and a user entry point of 0 would be
 * unusable anyway. Nothing is allocated or mapped on the failing path. */
uint64_t elf_load(uint64_t pml4_phys, const uint8_t *image, size_t image_size);

/* Every check elf_load makes, with none of the mapping - returns the same
 * entry point (or the same 0 sentinel) without touching an address space
 * or allocating a frame. elf_load calls this itself, so it is never
 * possible for the two to disagree; it is separately exposed so
 * process_spawn can reject a bad image *before* building the address
 * space it would otherwise have to abandon (nothing in this project can
 * free one - see vmm.h). */
uint64_t elf_validate(const uint8_t *image, size_t image_size);
