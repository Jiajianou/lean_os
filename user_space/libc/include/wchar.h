/* user_space/libc/include/wchar.h - M80 groundwork
 *
 * Wide characters, to the extent this system has any.
 *
 * It has none, and that is worth saying at the top rather than burying:
 * every string this OS handles - filenames, the console, the terminal,
 * every label the compositor draws - is bytes, and the font (M39/M57) has
 * one glyph per byte. There is no locale, no multibyte encoding and no
 * `mbstate_t` that means anything.
 *
 * What is here therefore has a narrow and honest purpose: `wchar_t` is a
 * 32-bit code point, the functions that are pure arithmetic over arrays
 * of them are real, and the conversions between wide and multibyte
 * treat the byte string as Latin-1 - which is not an encoding this
 * system chose so much as the only one under which "one byte is one
 * character" is true, and it is true here.
 *
 * A program that needs real UTF-8 conversion will find this wrong rather
 * than missing, so: it is wrong for anything above U+00FF, deliberately,
 * and this comment is where that is written down.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int wint_t;

#define WEOF ((wint_t)-1)
/* WCHAR_MIN/WCHAR_MAX are the compiler's, from <stdint.h>, which knows
 * the target's actual wchar_t signedness. Redefining them here would be
 * this file guessing at something GCC already knows. */

/* An opaque conversion state that has nothing to hold, because Latin-1
 * is stateless. Present because the standard's signatures take one. */
typedef struct {
    int dummy;
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
/* M80 groundwork. Latin-1 in, so a wide digit is a byte digit and the
 * conversion is the narrow one applied per character. */
long wcstol(const wchar_t *s, wchar_t **end, int base);
unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base);
/* The re-entrant form only, which is the one C99 specifies - there is no
 * hidden static state here for a `wcstok(s, d)` to keep. */
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **saveptr);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);

/* Latin-1 both ways - see the header note. `ps` is accepted and ignored,
 * because there is no state to carry. */
size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);
int    mbtowc(wchar_t *dst, const char *src, size_t n);
int    wctomb(char *dst, wchar_t c);
size_t mbrtowc(wchar_t *dst, const char *src, size_t n, mbstate_t *ps);
size_t wcrtomb(char *dst, wchar_t c, mbstate_t *ps);
