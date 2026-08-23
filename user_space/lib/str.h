/* user_space/lib/str.h
 *
 * Hand-written string/mem helpers - no libc anywhere in this project
 * (see milestones.md's ground rules), so user programs need their own
 * memcpy/strlen/etc rather than linking a real one. Named "str.h" rather
 * than "string.h" to avoid any ambiguity with the standard header name,
 * even though nothing on this toolchain's include path would actually
 * collide with it (there's no libc installed to provide one).
 */
#pragma once

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
