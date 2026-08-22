/* kernel/lib/libk.h
 *
 * Hand-written mem/string primitives for the kernel's own internal use -
 * no libc anywhere in this project (see milestones.md's ground rules).
 * Distinct from user_space/lib/str.h: that one ships inside user
 * binaries, this one is linked into the kernel image only. Split out
 * once a second kernel-side call site (kernel/fs/leanfs.c) needed the
 * same handful of functions kernel/proc/elf.c had already hand-rolled
 * locally - not written speculatively ahead of a real need.
 */
#pragma once

#include <stddef.h>

void *k_memcpy(void *dst, const void *src, size_t n);
void *k_memset(void *dst, int c, size_t n);
size_t k_strlen(const char *s);
int k_strcmp(const char *a, const char *b);
/* Copies up to n-1 bytes of src plus a null terminator into dst, always
 * null-terminating (unlike the standard strncpy, which doesn't if src is
 * >= n bytes) - the only behavior any caller here actually wants. */
void k_strlcpy(char *dst, const char *src, size_t n);
