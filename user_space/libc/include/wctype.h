#pragma once

#include <ctype.h>
#include <wchar.h>

#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long wctype_t;
typedef unsigned long wctrans_t;

static inline int iswalpha(wint_t c)  { return c < 128 && isalpha((int)c); }
static inline int iswdigit(wint_t c)  { return c < 128 && isdigit((int)c); }
static inline int iswalnum(wint_t c)  { return c < 128 && isalnum((int)c); }
static inline int iswspace(wint_t c)  { return c < 128 && isspace((int)c); }
static inline int iswupper(wint_t c)  { return c < 128 && isupper((int)c); }
static inline int iswlower(wint_t c)  { return c < 128 && islower((int)c); }
static inline int iswpunct(wint_t c)  { return c < 128 && ispunct((int)c); }
static inline int iswprint(wint_t c)  { return c < 128 && isprint((int)c); }
static inline int iswgraph(wint_t c)  { return c < 128 && isgraph((int)c); }
static inline int iswcntrl(wint_t c)  { return c < 128 && iscntrl((int)c); }
static inline int iswblank(wint_t c)  { return c < 128 && isblank((int)c); }
static inline int iswxdigit(wint_t c) { return c < 128 && isxdigit((int)c); }

static inline wint_t towupper(wint_t c) { return c < 128 ? (wint_t)toupper((int)c) : c; }
static inline wint_t towlower(wint_t c) { return c < 128 ? (wint_t)tolower((int)c) : c; }

wctype_t wctype(const char *name);
int      iswctype(wint_t c, wctype_t type);
wctrans_t wctrans(const char *name);
wint_t    towctrans(wint_t c, wctrans_t desc);

int wcwidth(wchar_t c);
int wcswidth(const wchar_t *s, size_t n);


/* POSIX 2008's locale-argument forms. See
   user_space/libc/src/locale_functions.c for why they are what they are on
   a machine with exactly one locale - and for what they do NOT do, which is
   accept a locale this machine never made. */
int iswalnum_l(wint_t c, locale_t locale);
int iswalpha_l(wint_t c, locale_t locale);
int iswblank_l(wint_t c, locale_t locale);
int iswcntrl_l(wint_t c, locale_t locale);
int iswdigit_l(wint_t c, locale_t locale);
int iswgraph_l(wint_t c, locale_t locale);
int iswlower_l(wint_t c, locale_t locale);
int iswprint_l(wint_t c, locale_t locale);
int iswpunct_l(wint_t c, locale_t locale);
int iswspace_l(wint_t c, locale_t locale);
int iswupper_l(wint_t c, locale_t locale);
int iswxdigit_l(wint_t c, locale_t locale);
int iswctype_l(wint_t c, wctype_t type, locale_t locale);
wctype_t wctype_l(const char *name, locale_t locale);
wint_t towlower_l(wint_t c, locale_t locale);
wint_t towupper_l(wint_t c, locale_t locale);
wctrans_t wctrans_l(const char *name, locale_t locale);
wint_t towctrans_l(wint_t c, wctrans_t transform, locale_t locale);

#ifdef __cplusplus
}
#endif
