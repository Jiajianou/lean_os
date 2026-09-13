#include "check.h"

#include <wchar.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static void widen(const char *s, wchar_t *out) {
    size_t i = 0;
    for (; s[i]; i++) {
        out[i] = (wchar_t)(unsigned char)s[i];
    }
    out[i] = 0;
}

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

TEST(wcs, the_conversions_accept_a_null_end_pointer) {
    CHECK_EQ((long)wcstoll(L"17", NULL, 10), 17L);
    CHECK(wcstod(L"2.5", NULL) == 2.5);
    CHECK(wcstof(L"2.5", NULL) == 2.5f);
    CHECK(wcstold(L"2.5", NULL) == 2.5L);
}

TEST(wcs, wcsdup_copies_and_is_independent) {
    wchar_t source[] = L"lean_os";
    wchar_t *copy = wcsdup(source);
    CHECK(copy != NULL);
    CHECK(copy != source);
    CHECK_EQ(wcscmp(copy, source), 0);
    copy[0] = L'L';
    CHECK_EQ(source[0], (wchar_t)L'l');
    free(copy);

    wchar_t *empty = wcsdup(L"");
    CHECK(empty != NULL);
    CHECK_EQ((long)wcslen(empty), 0L);
    free(empty);
}

TEST(wcs, wcscoll_orders_the_same_way_wcscmp_does) {
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
    CHECK_EQ((long)wcsxfrm(a, L"apple", 16), 5L);
    CHECK_EQ((long)wcsxfrm(b, L"banana", 16), 6L);
    CHECK(wcscmp(a, b) < 0);
    CHECK(wcscoll(L"apple", L"banana") < 0);

    wchar_t small[4];
    small[3] = L'!';
    CHECK_EQ((long)wcsxfrm(small, L"abcdefgh", 4), 8L);
    CHECK_EQ(small[3], (wchar_t)0);
    CHECK_EQ(small[0], (wchar_t)L'a');

    wchar_t untouched = L'#';
    CHECK_EQ((long)wcsxfrm(&untouched, L"xyz", 0), 3L);
    CHECK_EQ(untouched, (wchar_t)L'#');
}

TEST(wcs, mbsnrtowcs_stops_at_the_input_limit) {
    const char *source = "abcdef";
    const char *p = source;
    wchar_t out[8];
    mbstate_t st = {0, 0, 0};
    CHECK_EQ((long)mbsnrtowcs(out, &p, 3, 8, &st), 3L);
    CHECK_EQ(out[0], (wchar_t)L'a');
    CHECK_EQ(out[2], (wchar_t)L'c');
    CHECK_EQ((long)(p - source), 3L);
    CHECK_EQ((long)mbsnrtowcs(out, &p, 3, 8, &st), 3L);
    CHECK_EQ(out[0], (wchar_t)L'd');
    CHECK_EQ((long)(p - source), 6L);
}

TEST(wcs, mbsnrtowcs_stops_at_the_output_limit) {
    const char *source = "abcdef";
    const char *p = source;
    wchar_t out[3];
    mbstate_t st = {0, 0, 0};
    CHECK_EQ((long)mbsnrtowcs(out, &p, 6, 2, &st), 2L);
    CHECK_EQ((long)(p - source), 2L);
}

TEST(wcs, mbsnrtowcs_consumes_the_terminator_and_says_so) {
    const char *source = "ab";
    const char *p = source;
    wchar_t out[8];
    mbstate_t st = {0, 0, 0};
    CHECK_EQ((long)mbsnrtowcs(out, &p, 8, 8, &st), 2L);
    CHECK_EQ(out[2], (wchar_t)0);
    CHECK(p == NULL);
}

TEST(wcs, a_window_ending_mid_character_is_not_an_error) {
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
    const wchar_t *source = L"abcdef";
    const wchar_t *p = source;
    char out[16];
    mbstate_t st = {0, 0, 0};
    CHECK_EQ((long)wcsnrtombs(out, &p, 3, 16, &st), 3L);
    CHECK_EQ((long)(p - source), 3L);

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
    const char two[] = {(char)0xC3, (char)0xA9};
    CHECK_EQ((long)mbrlen(two, 2, &st), 2L);
    mbstate_t st2 = {0, 0, 0};
    CHECK((long)mbrlen(two, 1, &st2) == (long)(size_t)-2);
    CHECK_EQ((long)mbrlen(two + 1, 1, &st2), 1L);
    mbstate_t st3 = {0, 0, 0};
    CHECK_EQ((long)mbrlen("", 1, &st3), 0L);
}

TEST(wcs, wcsftime_agrees_with_the_hosts_strftime) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = 126;
    t.tm_mon = 8;
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
    wchar_t out[4];
    CHECK_EQ((long)wcsftime(out, 4, L"%Y-%m-%d", &t), 0L);
    CHECK_EQ((long)wcsftime(out, 0, L"%Y", &t), 0L);
}
