#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int wint_t;

#define WEOF ((wint_t)-1)

typedef struct {
    unsigned int wc;
    unsigned char owed;
    unsigned char total;
} mbstate_t;

size_t wcslen(const wchar_t *s);
wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dst, const wchar_t *src);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
long wcstol(const wchar_t *s, wchar_t **end, int base);
unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base);
long long          wcstoll(const wchar_t *s, wchar_t **end, int base);
unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base);
double             wcstod(const wchar_t *s, wchar_t **end);
float              wcstof(const wchar_t *s, wchar_t **end);
long double        wcstold(const wchar_t *s, wchar_t **end);

wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **saveptr);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);

size_t   wcsspn(const wchar_t *s, const wchar_t *accept);
size_t   wcscspn(const wchar_t *s, const wchar_t *reject);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle);
wchar_t *wcsdup(const wchar_t *s);

int      wcscoll(const wchar_t *a, const wchar_t *b);
size_t   wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n);

size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);
int    mbtowc(wchar_t *dst, const char *src, size_t n);
int    wctomb(char *dst, wchar_t c);
size_t mbrtowc(wchar_t *dst, const char *src, size_t n, mbstate_t *ps);
size_t wcrtomb(char *dst, wchar_t c, mbstate_t *ps);

int mbsinit(const mbstate_t *ps);

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t n, mbstate_t *ps);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t n, mbstate_t *ps);

int mblen(const char *s, size_t n);

size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
wint_t btowc(int c);
int    wctob(wint_t c);
size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len,
                  mbstate_t *ps);
size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len,
                  mbstate_t *ps);

struct tm;
size_t wcsftime(wchar_t *out, size_t n, const wchar_t *fmt,
                const struct tm *tm);

#include <stdarg.h>
#include <stdio.h>

int wprintf(const wchar_t *fmt, ...);
int fwprintf(FILE *f, const wchar_t *fmt, ...);
int swprintf(wchar_t *out, size_t n, const wchar_t *fmt, ...);
int vwprintf(const wchar_t *fmt, va_list ap);
int vfwprintf(FILE *f, const wchar_t *fmt, va_list ap);
int vswprintf(wchar_t *out, size_t n, const wchar_t *fmt, va_list ap);

int fputwc(wchar_t c, FILE *f);
int putwc(wchar_t c, FILE *f);
int putwchar(wchar_t c);
int fputws(const wchar_t *ws, FILE *f);

wint_t   fgetwc(FILE *f);
wint_t   getwc(FILE *f);
wint_t   getwchar(void);
wint_t   ungetwc(wint_t c, FILE *f);
wchar_t *fgetws(wchar_t *ws, int n, FILE *f);
int      fwide(FILE *f, int mode);

int wcwidth(wchar_t c);
int wcswidth(const wchar_t *s, size_t n);

#ifdef __cplusplus
}
#endif
