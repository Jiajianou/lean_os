/* user_space/libc/include/wchar.h - M80 groundwork, M88 rewrite
 *
 * Wide characters, and - since M88 - a real multibyte encoding under
 * them.
 *
 * **What changed, and why the old note is worth keeping in mind.** Until
 * M88 this header opened by saying this system had no multibyte encoding
 * at all, and that the conversions below treated a byte string as
 * Latin-1: "it is wrong for anything above U+00FF, deliberately, and
 * this comment is where that is written down." That was the honest thing
 * to say while the whole system was one byte per character. It stopped
 * being the right trade the moment a source tree written by somebody
 * else had to reach this disk - every one of them has non-ASCII bytes in
 * it, and a Latin-1 round trip turns a UTF-8 file into a different file.
 *
 * So: `wchar_t` is a 32-bit code point and the byte encoding is UTF-8.
 * The conversions decode and encode it properly - rejecting overlong
 * forms, surrogates and anything above U+10FFFF with EILSEQ rather than
 * accepting them - and `mbstate_t` genuinely carries a partial sequence
 * across calls, which is what makes converting a stream in fixed-size
 * chunks work at all.
 *
 * **What did not change: the glyphs.** The font is one byte per glyph
 * (M39/M57), and a font covering more than Latin-1 is a font project
 * rather than a libc one. The split is exact and worth stating: the
 * *encoding* is correct end to end and text round-trips through this
 * system unmangled, while text above U+00FF draws as a replacement box.
 * That is a rendering limitation a program can be told about, which is
 * categorically better than an encoding limitation that corrupts its
 * data.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int wint_t;

#define WEOF ((wint_t)-1)
/* WCHAR_MIN/WCHAR_MAX are the compiler's, from <stdint.h>, which knows
 * the target's actual wchar_t signedness. Redefining them here would be
 * this file guessing at something GCC already knows. */

/* M88: a conversion state that genuinely holds something.
 *
 * UTF-8 is stateless between characters and stateful *within* one, which
 * is the case that matters: a program reading a file in 4 KiB chunks
 * will eventually cut a three-byte character across a boundary, and the
 * whole reason `mbrtowc` takes an `mbstate_t` is so the next call can
 * finish it. The Latin-1 version of this struct held an `int dummy`;
 * this one holds the partial code point, how many continuation bytes are
 * still owed, and how many the sequence had in total - the last one
 * because an overlong encoding can only be detected once the value is
 * complete and its length is known.
 *
 * A zeroed mbstate_t is the initial state, which is what the standard
 * requires and what a static one gets for free. */
typedef struct {
    unsigned int wc;     /* the code point accumulated so far */
    unsigned char owed;  /* continuation bytes still expected */
    unsigned char total; /* continuation bytes this sequence has in all */
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

/* UTF-8 both ways - see the header note.
 *
 * Every one of these reports a malformed sequence rather than
 * substituting something for it: (size_t)-1 or -1, with errno set to
 * EILSEQ. A conversion that silently replaced bad bytes with U+FFFD
 * would make a corrupt file indistinguishable from a valid one, and
 * this is a libc rather than a text editor - the program on top gets to
 * decide what to do about it.
 *
 * The restartable forms take an `mbstate_t` and use it; a NULL `ps`
 * means "use the library's own", which is what the standard specifies
 * and which is only safe from one thread at a time. */
size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);
int    mbtowc(wchar_t *dst, const char *src, size_t n);
int    wctomb(char *dst, wchar_t c);
size_t mbrtowc(wchar_t *dst, const char *src, size_t n, mbstate_t *ps);
size_t wcrtomb(char *dst, wchar_t c, mbstate_t *ps);

/* M88: the restartable string forms, which is what a program converting
 * a stream actually calls - `*src` is advanced to the first byte not
 * consumed, so a partial character at the end of a buffer is left for
 * the next call rather than being an error. */
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t n, mbstate_t *ps);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t n, mbstate_t *ps);

/* How many bytes the next character occupies, or -1 for a malformed or
 * incomplete one. The oldest of these calls and the one a configure
 * script probes for by name. */
int mblen(const char *s, size_t n);

/* ---- M94: the wide stdio family ---------------------------------------
 *
 * Built on the narrow printf rather than beside it - see wchar.c for why
 * that is the design and not a shortcut, and for what the stream
 * orientation rules would mean on a stdio with no buffering.
 *
 * Asked for by GNU hello, whose `wprintf (L"%ls\n", ...)` is the first
 * thing in this tree to need one. M63's rule, arriving at the
 * formatter.
 */
#include <stdarg.h>
#include <stdio.h>

int wprintf(const wchar_t *fmt, ...);
int fwprintf(FILE *f, const wchar_t *fmt, ...);
int swprintf(wchar_t *out, size_t n, const wchar_t *fmt, ...);
int vwprintf(const wchar_t *fmt, va_list ap);
int vfwprintf(FILE *f, const wchar_t *fmt, va_list ap);
/* Note `n` is a count of WIDE CHARACTERS including the NUL, and that a
 * result which does not fit is -1 rather than the length it would have
 * needed. Both differ from snprintf, and both are what C specifies. */
int vswprintf(wchar_t *out, size_t n, const wchar_t *fmt, va_list ap);

int fputwc(wchar_t c, FILE *f);
int putwc(wchar_t c, FILE *f);
int putwchar(wchar_t c);
int fputws(const wchar_t *ws, FILE *f);

/* M94: POSIX declares these in <wchar.h> and this project had them only
 * in <wctype.h>, which is where the ctype-shaped ones live. gnulib's
 * wcwidth replacement includes <wchar.h> and nothing else, and would not
 * compile - a header that has the function and does not declare it where
 * the standard says is indistinguishable, to a build, from not having
 * it. The definitions are in wctype.c and are unchanged. */
int wcwidth(wchar_t c);
int wcswidth(const wchar_t *s, size_t n);
