#pragma once

#include <ctype.h>
#include <wchar.h>

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

#ifdef __cplusplus
}
#endif
