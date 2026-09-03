/* user_space/libc/include/ctype.h - M63. Inline, because every one of
 * these is a comparison and a call would be more code than the work. */
#pragma once

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

static inline int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c) { return isupper(c) || islower(c); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isprint(int c) { return c >= 0x20 && c < 0x7F; }
static inline int ispunct(int c) { return isprint(c) && c != ' ' && !isalnum(c); }
static inline int iscntrl(int c) { return c < 0x20 || c == 0x7F; }
/* M89: the two C89 classifications this header had never needed. isgraph
 * is isprint without the space - "has ink" - and isblank is the space and
 * the tab and nothing else, which is the distinction a program splitting
 * fields cares about. Both are asked for by name by <regex.h>'s
 * [[:graph:]] and [[:blank:]]. */
static inline int isgraph(int c) { return isprint(c) && c != ' '; }
static inline int isblank(int c) { return c == ' ' || c == '\t'; }
/* M98: XPG's pair, asked for by name by GMP's own printf. isascii is
 * defined on EVERY int, unlike the classifications above whose argument
 * must be unsigned-char-or-EOF - that wider domain is its whole reason
 * to exist, and why it is not isprint with different bounds. */
static inline int isascii(int c) { return (c & ~0x7F) == 0; }
static inline int toascii(int c) { return c & 0x7F; }
static inline int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
static inline int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }

#ifdef __cplusplus
}
#endif
