/* tests/test_wcs.c - M121
 *
 * The wide-string functions this milestone added to
 * user_space/libc/src/wchar.c, compiled unmodified and run on the host.
 *
 * ---- what the oracle is, and why it is not a table -------------------
 *
 * Every function here exists to agree with every other implementation of
 * itself, which is the same standard tools/sh-test.sh, regex-test.sh,
 * printf-test.sh, scanf-test.sh, math-test.sh and iconv-test.sh are held
 * to. So almost nothing below says what the right answer is: **the
 * host's own NARROW function decides.** `wcsstr(L"aaab", L"aab")` has to
 * find its match at the index the host's `strstr("aaab", "aab")` finds
 * one, and `wcstod(L"0x1p-3", &e)` has to return what the host's
 * `strtod` returns and leave `e` the same number of characters along.
 *
 * The narrow function is a better oracle here than the host's own wide
 * one, for two reasons. It cannot be shadowed - this file's `wcsstr` IS
 * this project's, because wchar.c is compiled into this binary, so a
 * differential against `::wcsstr` would silently grade this
 * implementation against itself. And for ASCII input the two are
 * *required* to agree character for character, which makes the
 * comparison exact rather than approximate: one wide character is one
 * byte, so an index is an index.
 *
 * The corpus is chosen for the cases a hand-written search gets wrong -
 * an empty needle, a needle longer than the haystack, a match at the
 * very end, and a repeated prefix (`aab` in `aaab`, where a naive scan
 * that does not restart correctly finds nothing).
 *
 * ---- what is NOT here, and where it is graded instead ---------------
 *
 * `asprintf`, `strerror_r` and the six C99 `lconv` fields live in
 * stdio.c, string.c and locale.c, and none of those three is in the
 * Makefile's TEST_USER_SRCS - they are too tangled in syscalls for this
 * tier. They are graded on the machine: libc++ itself calls all three
 * (`__libcpp_asprintf_l`, `std::system_error`, `std::moneypunct`), and
 * /bin/clangcxxtest under the `[m121]` marker is what runs it. The wide
 * *input* family is the same case - `fgetwc` and `ungetwc` are in
 * stdio.c because they touch `struct FILE`.
 */
#include "check.h"

#include <wchar.h>  /* redirected to this project's own - tests/fakes/wchar.h */

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- the corpus ------------------------------------------------------
 *
 * ASCII on purpose: the point is that one wide character is one byte, so
 * an offset in the wide string is the same number as an offset in the
 * narrow one and the two functions can be compared exactly. The UTF-8
 * paths have their own test (tests/test_utf8.c, M88).
 */
static const char *const HAYSTACKS[] = {
    "", "a", "abc", "aaab", "abcabcabd", "hello world",
    "  \t 42abc", "banana", "xyzzy", "aaaaaaaa",
};
static const char *const NEEDLES[] = {
    "", "a", "b", "abc", "aab", "abd", "world", "ana", "zzz",
    "aaaaaaaaa", "o w",
};
static const char *const SETS[] = {
    "", "a", "ab", "xyz", " \t", "abcdefghijklmnop", "0123456789",
};

/* Widens an ASCII string into `out` (which must have room for len+1). */
static void widen(const char *s, wchar_t *out) {
    size_t i = 0;
    for (; s[i]; i++) {
        out[i] = (wchar_t)(unsigned char)s[i];
    }
    out[i] = 0;
}

/* The offset a search returned, or -1 for "no match" - which is the
 * form that can be compared between a char* and a wchar_t* result. */
static long offset_of(const void *found, const void *base, size_t width) {
    if (!found) {
        return -1;
    }
    return (long)(((const char *)found - (const char *)base) / (long)width);
}

TEST(wcs, wcsstr_agrees_with_the_hosts_strstr) {
    wchar_t wh[64], wn[64];
    for (size_t h = 0; h < sizeof(HAYSTACKS) / sizeof(HAYSTACKS[0]); h++) {
        widen(HAYSTACKS[h], wh);
        for (size_t n = 0; n < sizeof(NEEDLES) / sizeof(NEEDLES[0]); n++) {
            widen(NEEDLES[n], wn);
            long got = offset_of(wcsstr(wh, wn), wh, sizeof(wchar_t));
            long want = offset_of(strstr(HAYSTACKS[h], NEEDLES[n]),
                                  HAYSTACKS[h], 1);
            CHECK_EQ(got, want);
        }
    }
}

TEST(wcs, wcspbrk_agrees_with_the_hosts_strpbrk) {
    wchar_t wh[64], ws[64];
    for (size_t h = 0; h < sizeof(HAYSTACKS) / sizeof(HAYSTACKS[0]); h++) {
        widen(HAYSTACKS[h], wh);
        for (size_t s = 0; s < sizeof(SETS) / sizeof(SETS[0]); s++) {
            widen(SETS[s], ws);
            long got = offset_of(wcspbrk(wh, ws), wh, sizeof(wchar_t));
            long want = offset_of(strpbrk(HAYSTACKS[h], SETS[s]),
                                  HAYSTACKS[h], 1);
            CHECK_EQ(got, want);
        }
    }
}

TEST(wcs, wcsspn_and_wcscspn_agree_with_the_hosts) {
    wchar_t wh[64], ws[64];
    for (size_t h = 0; h < sizeof(HAYSTACKS) / sizeof(HAYSTACKS[0]); h++) {
        widen(HAYSTACKS[h], wh);
        for (size_t s = 0; s < sizeof(SETS) / sizeof(SETS[0]); s++) {
            widen(SETS[s], ws);
            CHECK_EQ((long)wcsspn(wh, ws), (long)strspn(HAYSTACKS[h], SETS[s]));
            CHECK_EQ((long)wcscspn(wh, ws),
                     (long)strcspn(HAYSTACKS[h], SETS[s]));
        }
    }
}

/* ---- the numeric conversions ---------------------------------------
 *
 * Value AND end pointer, because the end pointer is half of what these
 * are for: a caller parsing a list depends on it, and a parser that
 * returns the right number and the wrong end is the harder bug.
 */
static const char *const NUMBERS[] = {
    "0", "1", "-1", "+42", "  7", "\t\n 99", "2147483648",
    "9223372036854775807", "-9223372036854775808",
    "0x1f", "0X10", "017", "ff", "z", "", "   ", "-",
    "12abc", "0x", "1e3", "3.5", "-0.125", ".5", "5.",
    "1e-3", "0x1p-3", "inf", "-inf", "nan", "1.7976931348623157e308",
};

TEST(wcs, wcstoll_and_wcstoull_agree_with_the_hosts) {
    wchar_t w[64];
    const int BASES[] = {0, 8, 10, 16, 36};
    for (size_t i = 0; i < sizeof(NUMBERS) / sizeof(NUMBERS[0]); i++) {
        widen(NUMBERS[i], w);
        for (size_t b = 0; b < sizeof(BASES) / sizeof(BASES[0]); b++) {
            wchar_t *we = NULL;
            char *ne = NULL;
            long long got = wcstoll(w, &we, BASES[b]);
            long long want = strtoll(NUMBERS[i], &ne, BASES[b]);
            CHECK_EQ(got, want);
            CHECK_EQ((long)(we - w), (long)(ne - NUMBERS[i]));

            unsigned long long ugot = wcstoull(w, &we, BASES[b]);
            unsigned long long uwant = strtoull(NUMBERS[i], &ne, BASES[b]);
            CHECK(ugot == uwant);
            CHECK_EQ((long)(we - w), (long)(ne - NUMBERS[i]));
        }
    }
}

/* A NaN is not equal to itself, so "both are NaN" is the agreement to
 * check rather than equality - and a function that returned 0 where the
 * host returned NaN would otherwise pass. */
static int same_double(double a, double b) {
    if (a != a || b != b) {
        return (a != a) && (b != b);
    }
    return a == b;
}

TEST(wcs, wcstod_and_wcstof_agree_with_the_hosts) {
    wchar_t w[64];
    for (size_t i = 0; i < sizeof(NUMBERS) / sizeof(NUMBERS[0]); i++) {
        widen(NUMBERS[i], w);
        wchar_t *we = NULL;
        char *ne = NULL;

        double got = wcstod(w, &we);
        double want = strtod(NUMBERS[i], &ne);
        CHECK(same_double(got, want));
        CHECK_EQ((long)(we - w), (long)(ne - NUMBERS[i]));

        float fgot = wcstof(w, &we);
        float fwant = strtof(NUMBERS[i], &ne);
        CHECK(same_double((double)fgot, (double)fwant));
        CHECK_EQ((long)(we - w), (long)(ne - NUMBERS[i]));

        long double lgot = wcstold(w, &we);
        long double lwant = strtold(NUMBERS[i], &ne);
        CHECK(same_double((double)lgot, (double)lwant));
        CHECK_EQ((long)(we - w), (long)(ne - NUMBERS[i]));
    }
}

/* A NULL end pointer is legal and is what most callers pass. */
TEST(wcs, the_conversions_accept_a_null_end_pointer) {
    CHECK_EQ((long)wcstoll(L"17", NULL, 10), 17L);
    CHECK(wcstod(L"2.5", NULL) == 2.5);
    CHECK(wcstof(L"2.5", NULL) == 2.5f);
    CHECK(wcstold(L"2.5", NULL) == 2.5L);
}

TEST(wcs, wcsdup_copies_and_is_independent) {
    wchar_t src[] = L"lean_os";
    wchar_t *copy = wcsdup(src);
    CHECK(copy != NULL);
    CHECK(copy != src);
    CHECK_EQ(wcscmp(copy, src), 0);
    /* Independent: editing one must not touch the other, which is what
     * distinguishes a copy from a pointer that happened to work. */
    copy[0] = L'L';
    CHECK_EQ(src[0], (wchar_t)L'l');
    free(copy);

    wchar_t *empty = wcsdup(L"");
    CHECK(empty != NULL);
    CHECK_EQ((long)wcslen(empty), 0L);
    free(empty);
}

TEST(wcs, wcscoll_orders_the_same_way_wcscmp_does) {
    /* In the C locale collation IS comparison - see the note in
     * <wchar.h>. What is checked is the SIGN, not the value: the
     * standard specifies only the sign, and an implementation that
     * returned a different magnitude would still be right. */
    static const wchar_t *const PAIRS[][2] = {
        {L"", L""},       {L"a", L"a"},   {L"a", L"b"},
        {L"b", L"a"},     {L"a", L"ab"},  {L"ab", L"a"},
        {L"Z", L"a"},     {L"lean", L"lean_os"},
    };
    for (size_t i = 0; i < sizeof(PAIRS) / sizeof(PAIRS[0]); i++) {
        int coll = wcscoll(PAIRS[i][0], PAIRS[i][1]);
        int cmp = wcscmp(PAIRS[i][0], PAIRS[i][1]);
        CHECK((coll < 0) == (cmp < 0));
        CHECK((coll > 0) == (cmp > 0));
        CHECK((coll == 0) == (cmp == 0));
    }
}

TEST(wcs, wcsxfrm_preserves_order_and_reports_the_length_it_needed) {
    wchar_t a[16], b[16];
    /* The only requirement on a transform: comparing two transformed
     * strings with wcscmp gives the same answer as wcscoll on the
     * originals. */
    CHECK_EQ((long)wcsxfrm(a, L"apple", 16), 5L);
    CHECK_EQ((long)wcsxfrm(b, L"banana", 16), 6L);
    CHECK(wcscmp(a, b) < 0);
    CHECK(wcscoll(L"apple", L"banana") < 0);

    /* A result that does not fit still reports the length it needed -
     * which is how a caller knows to allocate and try again - and
     * writes a terminated prefix rather than overrunning. */
    wchar_t small[4];
    small[3] = L'!';
    CHECK_EQ((long)wcsxfrm(small, L"abcdefgh", 4), 8L);
    CHECK_EQ(small[3], (wchar_t)0);
    CHECK_EQ(small[0], (wchar_t)L'a');

    /* n == 0 writes nothing at all and still reports the length. */
    wchar_t untouched = L'#';
    CHECK_EQ((long)wcsxfrm(&untouched, L"xyz", 0), 3L);
    CHECK_EQ(untouched, (wchar_t)L'#');
}

/* ---- the n-limited restartable conversions --------------------------
 *
 * The reason these exist is an input that is not NUL-terminated, so the
 * interesting cases are all about the limit: an input window that ends
 * exactly on a character boundary, one that ends INSIDE a character, and
 * an output buffer that fills first.
 */
TEST(wcs, mbsnrtowcs_stops_at_the_input_limit) {
    const char *src = "abcdef";
    const char *p = src;
    wchar_t out[8];
    mbstate_t st = {0, 0, 0};
    CHECK_EQ((long)mbsnrtowcs(out, &p, 3, 8, &st), 3L);
    CHECK_EQ(out[0], (wchar_t)L'a');
    CHECK_EQ(out[2], (wchar_t)L'c');
    /* `*src` points at the first unconsumed byte, which is what makes a
     * second call possible at all. */
    CHECK_EQ((long)(p - src), 3L);
    CHECK_EQ((long)mbsnrtowcs(out, &p, 3, 8, &st), 3L);
    CHECK_EQ(out[0], (wchar_t)L'd');
    CHECK_EQ((long)(p - src), 6L);
}

TEST(wcs, mbsnrtowcs_stops_at_the_output_limit) {
    const char *src = "abcdef";
    const char *p = src;
    wchar_t out[3];
    mbstate_t st = {0, 0, 0};
    CHECK_EQ((long)mbsnrtowcs(out, &p, 6, 2, &st), 2L);
    CHECK_EQ((long)(p - src), 2L);
}

TEST(wcs, mbsnrtowcs_consumes_the_terminator_and_says_so) {
    const char *src = "ab";
    const char *p = src;
    wchar_t out[8];
    mbstate_t st = {0, 0, 0};
    CHECK_EQ((long)mbsnrtowcs(out, &p, 8, 8, &st), 2L);
    CHECK_EQ(out[2], (wchar_t)0);
    /* NULL means "the terminator was consumed", which is how a caller
     * distinguishes a finished string from a filled buffer. */
    CHECK(p == NULL);
}

TEST(wcs, a_window_ending_mid_character_is_not_an_error) {
    /* U+00E9 is two bytes in UTF-8 (0xC3 0xA9). A window that ends
     * between them is exactly the case this function exists for: the
     * partial sequence stays in the state and the next call finishes it.
     * An implementation that reported EILSEQ here would make converting
     * a file in fixed-size chunks impossible. */
    const char full[] = {(char)0xC3, (char)0xA9, 'z', 0};
    const char *p = full;
    wchar_t out[8];
    mbstate_t st = {0, 0, 0};

    size_t first = mbsnrtowcs(out, &p, 1, 8, &st);
    CHECK_EQ((long)first, 0L);
    CHECK(p == full + 1);

    size_t second = mbsnrtowcs(out, &p, 3, 8, &st);
    CHECK_EQ((long)second, 2L);
    CHECK_EQ(out[0], (wchar_t)0x00E9);
    CHECK_EQ(out[1], (wchar_t)L'z');
}

TEST(wcs, wcsnrtombs_stops_at_both_limits_and_round_trips) {
    const wchar_t *src = L"abcdef";
    const wchar_t *p = src;
    char out[16];
    mbstate_t st = {0, 0, 0};
    CHECK_EQ((long)wcsnrtombs(out, &p, 3, 16, &st), 3L);
    CHECK_EQ((long)(p - src), 3L);

    /* An output buffer too small for the next character stops before it
     * rather than writing a partial one - the property that makes the
     * byte stream always valid. 0x00E9 needs two bytes. */
    const wchar_t two[] = {0x00E9, 0x00E9, 0};
    const wchar_t *q = two;
    char tight[3];
    mbstate_t st2 = {0, 0, 0};
    CHECK_EQ((long)wcsnrtombs(tight, &q, 2, 3, &st2), 2L);
    CHECK_EQ((long)(q - two), 1L);
    CHECK_EQ((unsigned char)tight[0], 0xC3u);
    CHECK_EQ((unsigned char)tight[1], 0xA9u);
}

TEST(wcs, btowc_and_wctob_are_ascii_only) {
    /* On a UTF-8 system a single byte is a complete character only if it
     * is ASCII. Returning the byte's value for 0x80..0xFF would be the
     * Latin-1 answer M88 deliberately stopped giving. */
    CHECK_EQ((long)btowc('A'), (long)L'A');
    CHECK_EQ((long)btowc(0), 0L);
    CHECK_EQ((long)btowc(0x7F), 0x7FL);
    CHECK((long)btowc(0x80) == (long)WEOF);
    CHECK((long)btowc(0xC3) == (long)WEOF);
    CHECK((long)btowc(EOF) == (long)WEOF);

    CHECK_EQ((long)wctob(L'A'), (long)'A');
    CHECK_EQ((long)wctob(0), 0L);
    CHECK_EQ((long)wctob(0x7F), 0x7FL);
    CHECK_EQ((long)wctob(0x00E9), (long)EOF);
    CHECK_EQ((long)wctob(0x10FFFF), (long)EOF);
    CHECK_EQ((long)wctob(WEOF), (long)EOF);
}

TEST(wcs, mbrlen_counts_the_bytes_mbrtowc_would_consume) {
    mbstate_t st = {0, 0, 0};
    CHECK_EQ((long)mbrlen("a", 1, &st), 1L);
    /* Two-byte character, complete. */
    const char two[] = {(char)0xC3, (char)0xA9};
    CHECK_EQ((long)mbrlen(two, 2, &st), 2L);
    /* The same character, one byte at a time: incomplete then finished,
     * which is the whole reason it takes an mbstate_t. */
    mbstate_t st2 = {0, 0, 0};
    CHECK((long)mbrlen(two, 1, &st2) == (long)(size_t)-2);
    CHECK_EQ((long)mbrlen(two + 1, 1, &st2), 1L);
    /* A completed L'\0' reports 0 bytes, not 1. */
    mbstate_t st3 = {0, 0, 0};
    CHECK_EQ((long)mbrlen("", 1, &st3), 0L);
}

TEST(wcs, wcsftime_agrees_with_the_hosts_strftime) {
    /* Narrowed into strftime by construction (see wchar.c), so what this
     * grades is the narrowing and the widening either side of it - which
     * is where an off-by-one in the buffer sizes would live. The oracle
     * is the host's strftime on the same fields. */
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = 126; /* 2026 */
    t.tm_mon = 8;    /* September */
    t.tm_mday = 12;
    t.tm_hour = 13;
    t.tm_min = 45;
    t.tm_sec = 5;
    t.tm_wday = 6;
    t.tm_yday = 254;

    static const char *const FORMATS[] = {
        "%Y-%m-%d", "%H:%M:%S", "%Y", "", "no conversions at all",
        "%Y-%m-%dT%H:%M:%S",
    };
    for (size_t i = 0; i < sizeof(FORMATS) / sizeof(FORMATS[0]); i++) {
        wchar_t wfmt[64], wout[128];
        char nout[128];
        widen(FORMATS[i], wfmt);
        size_t wn = wcsftime(wout, 128, wfmt, &t);
        size_t nn = strftime(nout, 128, FORMATS[i], &t);
        CHECK_EQ((long)wn, (long)nn);
        for (size_t k = 0; k < nn; k++) {
            CHECK_EQ((long)wout[k], (long)(unsigned char)nout[k]);
        }
        CHECK_EQ((long)wout[wn], 0L);
    }
}

TEST(wcs, wcsftime_reports_zero_when_the_result_does_not_fit) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = 126;
    t.tm_mon = 8;
    t.tm_mday = 12;
    /* "2026-09-12" is ten characters plus a terminator; four is not
     * enough, and the answer is 0 with the contents unspecified rather
     * than a truncated date that reads as a real one. */
    wchar_t out[4];
    CHECK_EQ((long)wcsftime(out, 4, L"%Y-%m-%d", &t), 0L);
    /* n == 0 writes nothing. */
    CHECK_EQ((long)wcsftime(out, 0, L"%Y", &t), 0L);
}
