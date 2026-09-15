#pragma once

#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef LEANOS_CTYPE_OUT_OF_LINE
#define LEANOS_CTYPE_DEFINE(name, expr) int name(int c) { return (expr); }
#else
#define LEANOS_CTYPE_DEFINE(name, expr) static inline int name(int c) { return (expr); }
#endif

LEANOS_CTYPE_EACH(LEANOS_CTYPE_DEFINE)

#undef LEANOS_CTYPE_DEFINE


/* POSIX 2008's locale-argument forms. See
   user_space/libc/src/locale_functions.c for why they are what they are on
   a machine with exactly one locale - and for what they do NOT do, which is
   accept a locale this machine never made. */
int isalnum_l(int c, locale_t locale);
int isalpha_l(int c, locale_t locale);
int isblank_l(int c, locale_t locale);
int iscntrl_l(int c, locale_t locale);
int isdigit_l(int c, locale_t locale);
int isgraph_l(int c, locale_t locale);
int islower_l(int c, locale_t locale);
int isprint_l(int c, locale_t locale);
int ispunct_l(int c, locale_t locale);
int isspace_l(int c, locale_t locale);
int isupper_l(int c, locale_t locale);
int isxdigit_l(int c, locale_t locale);
int tolower_l(int c, locale_t locale);
int toupper_l(int c, locale_t locale);

#ifdef __cplusplus
}
#endif
