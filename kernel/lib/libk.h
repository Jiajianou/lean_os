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

/* M66: the overlapping-safe copy. k_memcpy copies forward, which is
 * wrong exactly when a buffer is being compacted toward its own start -
 * which is what TCP's send and receive buffers do on every ACK and every
 * read. Added when that was needed rather than speculatively, which is
 * why it took sixty-six milestones. */
void *k_memmove(void *dst, const void *src, size_t n);
void *k_memset(void *dst, int c, size_t n);
size_t k_strlen(const char *s);

/* M81: the one string primitive this kernel had never needed, asked for
 * by leanfs's directory records - whose names are stored with a length
 * and no NUL, so k_strcmp has nothing to stop at. Same contract as the C
 * library's: negative, zero or positive, and it reads exactly n bytes. */
int k_memcmp(const void *a, const void *b, size_t n);
int k_strcmp(const char *a, const char *b);
/* Copies up to n-1 bytes of src plus a null terminator into dst, always
 * null-terminating (unlike the standard strncpy, which doesn't if src is
 * >= n bytes) - the only behavior any caller here actually wants. */
void k_strlcpy(char *dst, const char *src, size_t n);

/* M60: the first substring search this kernel has needed. A self-test
 * that reads a directory listing back off disk has to ask "is this name
 * in it" without asserting on the order leanfs happens to return - which
 * is a property of the filesystem, not of the thing under test. */
const char *k_strstr(const char *haystack, const char *needle);
