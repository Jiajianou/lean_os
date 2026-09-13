#pragma once

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

#ifdef __cplusplus
}
#endif
