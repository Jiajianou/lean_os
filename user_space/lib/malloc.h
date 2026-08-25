/* user_space/lib/malloc.h
 *
 * M19: the first dynamic allocator any user program in this project has
 * had access to - every program before this milestone was limited to
 * static buffers and its own fixed-size stack. First-fit free list over
 * pages grown on demand via sys_sbrk (syscall_wrappers.h); deliberately
 * mirrors kernel/mm/heap.c's design (same split/coalesce rules) rather
 * than inventing a different shape for what's conceptually the same
 * problem one level up.
 */
#pragma once

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);

/* M63: how many usable bytes the block at `ptr` actually has. The header
 * that knows has always been right there, one word before the pointer -
 * what was missing was anybody who needed to ask. `realloc` does: with
 * no way to know the old size it would have to copy the *new* size,
 * which reads past the end of a block being grown. Returns 0 for NULL. */
size_t malloc_usable_size(void *ptr);
