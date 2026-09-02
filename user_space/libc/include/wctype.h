/* user_space/libc/include/wctype.h - M89
 *
 * Wide-character classification.
 *
 * **These answer for ASCII and say so, which is the whole content of
 * this header.** The encoding is UTF-8 since M88 and `wchar_t` is a real
 * code point, so a program can hold U+00E9 here - but deciding whether
 * U+00E9 is a letter needs a Unicode character database, and that is a
 * table of thousands of ranges that this project has no user for. Above
 * U+007F every classification below reports false, which is wrong for
 * `iswalpha(L'é')` and is wrong in the safe direction: a program that
 * asks gets "not a letter" rather than a wrong yes, and text it does not
 * classify it still copies through unharmed.
 *
 * The same split as the font, and for the same reason - see <wchar.h>:
 * the *encoding* is correct end to end, and what sits on top of it
 * (glyphs there, character properties here) is a table nobody here has
 * needed yet.
 */
#pragma once

#include <ctype.h>
#include <wchar.h>

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

/* Case mapping, ASCII only for the reason above: a code point outside it
 * is returned unchanged, which is correct for every character that has
 * no case and wrong only for those that do. */
static inline wint_t towupper(wint_t c) { return c < 128 ? (wint_t)toupper((int)c) : c; }
static inline wint_t towlower(wint_t c) { return c < 128 ? (wint_t)tolower((int)c) : c; }

/* The programmable forms, which look a class up by name. Both are real -
 * `wctype("digit")` returns something `iswctype` understands - because a
 * program that builds a class name at runtime is doing something this
 * can answer. */
wctype_t wctype(const char *name);
int      iswctype(wint_t c, wctype_t type);
wctrans_t wctrans(const char *name);
wint_t    towctrans(wint_t c, wctrans_t desc);

/* How many columns a character occupies when drawn. This machine draws
 * one glyph per character and has no double-width glyphs, so the answer
 * is 1 for anything printable and -1 for a control character - which is
 * what a program laying out a terminal needs and is true of what the
 * compositor actually draws (M88's replacement box is one cell too). */
int wcwidth(wchar_t c);
int wcswidth(const wchar_t *s, size_t n);
