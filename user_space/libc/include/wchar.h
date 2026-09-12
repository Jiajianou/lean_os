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
/* ---- M121: and the rest of the C99 numeric family -------------------
 *
 * M80 wrote the two above and stopped there, which was right while the
 * only wide strings on this machine were this project's own. libc++'s
 * std::stoll, std::stof and std::stod on a std::wstring are each one
 * call to one of these five, so `string.cpp` would not compile at all -
 * and the error was "reference to unresolved using declaration", from
 * <cwchar>, naming none of them.
 *
 * `wcstoll` is `wcstol` widened rather than a second parser, and on this
 * target that is exact rather than approximate: x86-64 is LP64, so
 * `long` and `long long` are both 64 bits. Written as a widening call
 * with this note instead of a copy of the digit loop, because a second
 * copy is a second place for a base-36 bug to live. */
long long          wcstoll(const wchar_t *s, wchar_t **end, int base);
unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base);
double             wcstod(const wchar_t *s, wchar_t **end);
float              wcstof(const wchar_t *s, wchar_t **end);
long double        wcstold(const wchar_t *s, wchar_t **end);

/* The re-entrant form only, which is the one C99 specifies - there is no
 * hidden static state here for a `wcstok(s, d)` to keep. */
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **saveptr);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);

/* ---- M121: the four wide-string searches C99 requires and this header
 * ---- did not have ----------------------------------------------------
 *
 * Found by building libc++ against this libc, and the way it was found
 * is worth keeping. libc++'s own <wchar.h> supplies the const-correct
 * C++ overloads of wcschr, wcspbrk, wcsrchr, wcsstr and wmemchr, and it
 * builds each one on top of the C function of the same name. Three of
 * the five were here; `wcsstr` and `wcspbrk` were not, so the overload
 * resolved to the C++ declaration libc++ had just made and the error
 * was about const-qualification in a file nobody here wrote - which is
 * a long way from "your libc is missing two functions".
 *
 * wcsspn and wcscspn come with them because wcspbrk is the third member
 * of that family and a header with two of the three is the same trap one
 * step further along.
 */
size_t   wcsspn(const wchar_t *s, const wchar_t *accept);
size_t   wcscspn(const wchar_t *s, const wchar_t *reject);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle);
/* POSIX's, not C's, and it allocates with malloc like strdup does. */
wchar_t *wcsdup(const wchar_t *s);

/* The wide half of the pair <string.h> already has, with the same note:
 * on a machine with one locale, collation is comparison and a transform
 * is a copy, which is what C says they must be in "C" and is the whole of
 * what this OS has to say about collation. libc++'s locale layer calls
 * both through wcscoll_l/wcsxfrm_l, which is where their absence showed
 * up - as "no member named 'wcscoll' in the global namespace" from a
 * header that supplies the _l forms by calling these. */
int      wcscoll(const wchar_t *a, const wchar_t *b);
size_t   wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n);

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

/* M111: nonzero if `ps` describes an initial conversion state, which for
 * a stateless-between-characters encoding means "no partial sequence in
 * flight". Nothing in this tree calls it; GNU grep's build *probes* for
 * it, and a failed probe made gnulib redefine mbstate_t as an int under
 * half the translation unit. See wchar.c for the whole story - it is the
 * clearest example this project has of a missing symbol costing far more
 * than the feature it names. */
int mbsinit(const mbstate_t *ps);

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

/* ---- M121: and the restartable/single-character forms ---------------
 *
 * `mbrlen` is `mbrtowc` with nowhere to put the result, which is exactly
 * how it is written; `btowc` and `wctob` are the single-byte special
 * cases a program uses when it knows the character is ASCII and wants
 * to say so. All three are C99 and all three are named in libc++'s
 * <cwchar>, which is where their absence was noticed.
 *
 * The `n`-limited string forms are POSIX rather than C, and they are
 * here for the reason the M111 note above gives about `mbsinit`: a
 * conversion loop over a buffer that must not be overrun cannot be
 * written with the C forms, so every library that has one of these
 * loops probes for them. */
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
wint_t btowc(int c);
int    wctob(wint_t c);
size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len,
                  mbstate_t *ps);
size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len,
                  mbstate_t *ps);

/* ---- M121: struct tm, and why an incomplete type is the point --------
 *
 * C requires <wchar.h> to declare `struct tm` - as an incomplete type,
 * which is all `wcsftime`'s prototype needs. This header did not, and
 * the failure was not a missing function: libc++'s <cwchar> and <ctime>
 * both say `using ::tm __attribute__((using_if_exists))`, so <cwchar>
 * bound the name to nothing and <ctime> then bound the same name to the
 * real struct, and clang reported "target of using declaration conflicts
 * with declaration already in scope" from inside <chrono>. One missing
 * line in a header, three files away from where it was reported.
 *
 * Not `#include <time.h>`: the standard asks for the tag to be visible,
 * not for the whole header, and a <wchar.h> that dragged in <time.h>
 * would be a difference from every other libc for no reason. */
struct tm;
size_t wcsftime(wchar_t *out, size_t n, const wchar_t *fmt,
                const struct tm *tm);

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

/* ---- M121: and the input half, which M94 did not build --------------
 *
 * M94 built the wide OUTPUT family because GNU hello's `wprintf` asked
 * for it. libc++ asks for the other half and asks unconditionally:
 * src/std_stream.h reads std::wcin through `getwc` and puts a character
 * back through `ungetwc`, and compiles both whatever this libc has.
 *
 * fgetwc decodes UTF-8 one byte at a time through mbrtowc's state, so a
 * character split across a read is finished rather than lost. `fwide`
 * answers 0 - "no orientation" - which is the truth here rather than a
 * stub; see wchar.c for why this libc has no orientation to report. */
wint_t   fgetwc(FILE *f);
wint_t   getwc(FILE *f);
wint_t   getwchar(void);
wint_t   ungetwc(wint_t c, FILE *f);
wchar_t *fgetws(wchar_t *ws, int n, FILE *f);
int      fwide(FILE *f, int mode);

/* M94: POSIX declares these in <wchar.h> and this project had them only
 * in <wctype.h>, which is where the ctype-shaped ones live. gnulib's
 * wcwidth replacement includes <wchar.h> and nothing else, and would not
 * compile - a header that has the function and does not declare it where
 * the standard says is indistinguishable, to a build, from not having
 * it. The definitions are in wctype.c and are unchanged. */
int wcwidth(wchar_t c);
int wcswidth(const wchar_t *s, size_t n);

#ifdef __cplusplus
}
#endif
