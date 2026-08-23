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

/* Bounded copy that always NUL-terminates: copies at most n-1 bytes of
 * src into dst and terminates. n == 0 does nothing. The same contract
 * kernel/lib/libk.h's k_strlcpy already has on the other side of the
 * syscall boundary. M41: added once the menu protocol
 * (system_api/include/wm.h's fixed-width wm_menu_t fields) gave user
 * space several places that all need exactly this and would otherwise
 * each hand-roll a bounded copy loop. */
void strlcpy(char *dst, const char *src, size_t n);
