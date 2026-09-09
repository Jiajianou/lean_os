/* user_space/libc/include/ctype.h - M63. Inline, because every one of
 * these is a comparison and a call would be more code than the work.
 *
 * ---- M111: inline, AND a real symbol, and why both --------------------
 *
 * Every function here was `static inline` for forty-eight milestones,
 * which means not one of them existed in libc.a. That is invisible to
 * any program that includes this header - the definition is right
 * there - and it is exactly what a configure script cannot see, because
 * a configure script does not include the header: it LINKS.
 *
 *   configure: checking for isblank... no
 *
 * and then gnulib compiles its own isblank.c, which includes <ctype.h>
 * and defines `isblank` - colliding with the static inline that was
 * there all along. So the build stops with "redefinition of 'isblank'"
 * pointing at a header that has it, in a file gnulib only wrote because
 * the header appeared not to. GNU grep 3.11 found it; the same trap was
 * set for isupper, toupper, isalnum and every other name here.
 *
 * This is M94's `wcwidth` lesson with the sign flipped. That one was a
 * function this libc had and declared in the wrong header; this is a
 * function this libc has and does not *export*. Both are indistinguish-
 * able from absent to a build system, which is the only observer whose
 * opinion decides whether a port compiles.
 *
 * ---- the fix, and the one that did not work --------------------------
 *
 * The obvious answer is C99's `extern inline` idiom: plain `inline`
 * definitions here, and one `extern inline int isblank(int);` in a .c
 * file to emit the out-of-line copy. **It does not work for these
 * names**, and finding out why cost an hour worth writing down.
 *
 * GCC knows `isspace`, `toupper` and the rest as built-in library
 * functions, which means every translation unit already carries an
 * implicit `extern` declaration of them. C99 says an inline definition
 * plus an external declaration in the same unit is an *external
 * definition* - so every object file that included this header defined
 * all sixteen, strongly, and two of them in one link is "multiple
 * definition of `isascii'". The same header with a name GCC does not
 * know as a builtin behaves perfectly, which is what makes the failure
 * so confusing to look at.
 *
 * So the definitions stay `static inline` - a call site still gets the
 * comparison - and the bodies now live in one X-macro list that
 * user_space/libc/src/ctype.c expands a second time, without `static
 * inline`, to put a real strong symbol for each of them in libc.a.
 * One source of truth for sixteen expressions, and both properties.
 */
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

/* The list. `c` is the argument's name in every expression below, and
 * the order matters: the later ones are written in terms of the earlier
 * ones, which is only legal if the earlier one is already defined.
 *
 * M89: isgraph is isprint without the space - "has ink" - and isblank is
 * the space and the tab and nothing else, which is the distinction a
 * program splitting fields cares about. Both are asked for by name by
 * <regex.h>'s [[:graph:]] and [[:blank:]].
 *
 * M98: isascii and toascii are XPG's pair, asked for by name by GMP's
 * own printf. isascii is defined on EVERY int, unlike the
 * classifications above whose argument must be unsigned-char-or-EOF -
 * that wider domain is its whole reason to exist, and why it is not
 * isprint with different bounds. */
#define LEANOS_CTYPE_EACH(F)                                                  \
    F(isspace,  c == ' ' || (c >= '\t' && c <= '\r'))                         \
    F(isdigit,  c >= '0' && c <= '9')                                         \
    F(isxdigit, isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) \
    F(isupper,  c >= 'A' && c <= 'Z')                                         \
    F(islower,  c >= 'a' && c <= 'z')                                         \
    F(isalpha,  isupper(c) || islower(c))                                     \
    F(isalnum,  isalpha(c) || isdigit(c))                                     \
    F(isprint,  c >= 0x20 && c < 0x7F)                                        \
    F(ispunct,  isprint(c) && c != ' ' && !isalnum(c))                        \
    F(iscntrl,  c < 0x20 || c == 0x7F)                                        \
    F(isgraph,  isprint(c) && c != ' ')                                       \
    F(isblank,  c == ' ' || c == '\t')                                        \
    F(isascii,  (c & ~0x7F) == 0)                                             \
    F(toascii,  c & 0x7F)                                                     \
    F(toupper,  islower(c) ? c - 'a' + 'A' : c)                               \
    F(tolower,  isupper(c) ? c - 'A' + 'a' : c)

/* Included normally: the inline definitions, exactly as before.
 * Included by user_space/libc/src/ctype.c, which defines
 * LEANOS_CTYPE_OUT_OF_LINE first: the external definitions instead. */
#ifdef LEANOS_CTYPE_OUT_OF_LINE
#define LEANOS_CTYPE_DEFINE(name, expr) int name(int c) { return (expr); }
#else
#define LEANOS_CTYPE_DEFINE(name, expr) static inline int name(int c) { return (expr); }
#endif

LEANOS_CTYPE_EACH(LEANOS_CTYPE_DEFINE)

#undef LEANOS_CTYPE_DEFINE

#ifdef __cplusplus
}
#endif
