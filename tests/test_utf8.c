#include "check.h"

#include <wchar.h>
#include <errno.h>

static int roundtrips(unsigned int cp) {
    char buffer[8];
    mbstate_t st = {0, 0, 0};
    size_t n = wcrtomb(buffer, (wchar_t)cp, &st);
    if (n == (size_t)-1 || n > 4) {
        return 0;
    }
    wchar_t back = 0;
    mbstate_t rst = {0, 0, 0};
    size_t used = mbrtowc(&back, buffer, n, &rst);
    if (cp == 0) {
        return used == 0 && back == 0 && n == 1;
    }
    return used == n && (unsigned int)back == cp;
}

TEST(utf8, every_code_point_round_trips) {
    for (unsigned int cp = 0; cp <= 0x10FFFFu; cp += 37) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) {
            continue;
        }
        if (!roundtrips(cp)) {
            test_fail(__FILE__, __LINE__, "U+%04X did not round trip", cp);
            break;
        }
    }
}

TEST(utf8, every_length_boundary_round_trips) {
    static const unsigned int edges[] = {
        0x00, 0x7Fu, 0x80u, 0x7FFu, 0x800u, 0xFFFFu,
        0xD7FFu, 0xE000u, 0x10000u, 0x10FFFFu,
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        if (!roundtrips(edges[i])) {
            test_fail(__FILE__, __LINE__, "boundary U+%04X did not round trip", edges[i]);
        }
    }
}

TEST(utf8, encoded_lengths_are_the_specified_ones) {
    char buffer[8];
    CHECK_EQ(wcrtomb(buffer, (wchar_t)0x41u, NULL), (size_t)1);
    CHECK_EQ(wcrtomb(buffer, (wchar_t)0xE9u, NULL), (size_t)2);
    CHECK_EQ(wcrtomb(buffer, (wchar_t)0x20ACu, NULL), (size_t)3);
    CHECK_EQ(wcrtomb(buffer, (wchar_t)0x1F600u, NULL), (size_t)4);
}

TEST(utf8, the_encoding_is_the_one_on_the_wire) {
    char buffer[8];
    CHECK_EQ(wcrtomb(buffer, (wchar_t)0xE9u, NULL), (size_t)2);
    CHECK_EQ((unsigned char)buffer[0], 0xC3u);
    CHECK_EQ((unsigned char)buffer[1], 0xA9u);
    CHECK_EQ(wcrtomb(buffer, (wchar_t)0x20ACu, NULL), (size_t)3);
    CHECK_EQ((unsigned char)buffer[0], 0xE2u);
    CHECK_EQ((unsigned char)buffer[1], 0x82u);
    CHECK_EQ((unsigned char)buffer[2], 0xACu);
}

static size_t decode(const char *bytes, size_t n, wchar_t *out) {
    mbstate_t st = {0, 0, 0};
    errno = 0;
    return mbrtowc(out, bytes, n, &st);
}

TEST(utf8, an_overlong_form_is_refused) {
    wchar_t wc = 0;
    CHECK_EQ(decode("\xC0\xAF", 2, &wc), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
    CHECK_EQ(decode("\xE0\x80\xAF", 3, &wc), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
    CHECK_EQ(decode("\xC0\x80", 2, &wc), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
}

TEST(utf8, a_surrogate_half_is_refused_both_ways) {
    wchar_t wc = 0;
    CHECK_EQ(decode("\xED\xA0\x80", 3, &wc), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
    char buffer[8];
    errno = 0;
    CHECK_EQ(wcrtomb(buffer, (wchar_t)0xD800u, NULL), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
}

TEST(utf8, past_the_last_code_point_is_refused) {
    wchar_t wc = 0;
    CHECK_EQ(decode("\xF4\x90\x80\x80", 4, &wc), (size_t)-1);
    CHECK_EQ(decode("\xF8\x88\x80\x80\x80", 5, &wc), (size_t)-1);
    char buffer[8];
    errno = 0;
    CHECK_EQ(wcrtomb(buffer, (wchar_t)0x110000u, NULL), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
}

TEST(utf8, a_lone_continuation_byte_is_refused) {
    wchar_t wc = 0;
    CHECK_EQ(decode("\x80", 1, &wc), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
    CHECK_EQ(decode("\xBF", 1, &wc), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
}

TEST(utf8, a_truncated_sequence_is_incomplete_not_invalid) {
    wchar_t wc = 0;
    CHECK_EQ(decode("\xE2\x82", 2, &wc), (size_t)-2);
    CHECK_EQ(decode("\xF0\x9F", 2, &wc), (size_t)-2);
    CHECK_EQ(decode("\xE2\x41", 2, &wc), (size_t)-1);
}

TEST(utf8, a_sequence_split_across_two_calls_completes) {
    const char *seq = "\xF0\x9F\x98\x80";
    for (size_t cut = 1; cut < 4; cut++) {
        mbstate_t st = {0, 0, 0};
        wchar_t wc = 0;
        CHECK_EQ(mbrtowc(&wc, seq, cut, &st), (size_t)-2);
        size_t r = mbrtowc(&wc, seq + cut, 4 - cut, &st);
        CHECK_EQ(r, 4 - cut);
        CHECK_EQ((unsigned int)wc, 0x1F600u);
    }
}

TEST(utf8, strings_convert_both_ways) {
    static const char *s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" "b";
    wchar_t wide[16];
    CHECK_EQ(mbstowcs(wide, s, 16), (size_t)5);
    CHECK_EQ((unsigned int)wide[0], 0x61u);
    CHECK_EQ((unsigned int)wide[1], 0xE9u);
    CHECK_EQ((unsigned int)wide[2], 0x20ACu);
    CHECK_EQ((unsigned int)wide[3], 0x1F600u);
    CHECK_EQ((unsigned int)wide[4], 0x62u);

    char back[32];
    CHECK_EQ(wcstombs(back, wide, sizeof(back)), (size_t)11);
    CHECK_MEMEQ(back, s, 11);
    CHECK_EQ(back[11], 0);

    CHECK_EQ(mbstowcs(NULL, s, 0), (size_t)5);
    CHECK_EQ(wcstombs(NULL, wide, 0), (size_t)11);
}

TEST(utf8, wcstombs_never_writes_a_partial_character) {
    wchar_t wide[3];
    wide[0] = (wchar_t)0x41u;
    wide[1] = (wchar_t)0x20ACu;
    wide[2] = 0;
    char buffer[8];
    for (size_t room = 1; room < 4; room++) {
        memset(buffer, 0x7F, sizeof(buffer));
        CHECK_EQ(wcstombs(buffer, wide, room), (size_t)1);
        CHECK_EQ((unsigned char)buffer[0], 0x41u);
        CHECK_EQ((unsigned char)buffer[1], 0x7Fu);
    }
    CHECK_EQ(wcstombs(buffer, wide, 5), (size_t)4);
}

TEST(utf8, mblen_agrees_with_the_decoder) {
    CHECK_EQ(mblen("a", 1), 1);
    CHECK_EQ(mblen("\xC3\xA9", 2), 2);
    CHECK_EQ(mblen("\xE2\x82\xAC", 3), 3);
    CHECK_EQ(mblen("\xF0\x9F\x98\x80", 4), 4);
    CHECK_EQ(mblen("", 1), 0);
    CHECK_EQ(mblen("\xC3", 1), -1);
    CHECK_EQ(mblen("\x80", 1), -1);
}

TEST(utf8, a_bad_byte_does_not_wedge_the_state) {
    mbstate_t st = {0, 0, 0};
    wchar_t wc = 0;
    CHECK_EQ(mbrtowc(&wc, "\xE2\x41", 2, &st), (size_t)-1);
    CHECK_EQ(mbrtowc(&wc, "A", 1, &st), (size_t)1);
    CHECK_EQ((unsigned int)wc, 0x41u);
}

TEST(utf8, the_restartable_string_form_resumes_where_it_stopped) {
    static const char *s = "\xC3\xA9\xC3\xA8";
    const char *p = s;
    mbstate_t st = {0, 0, 0};
    wchar_t out[4];
    CHECK_EQ(mbsrtowcs(out, &p, 1, &st), (size_t)1);
    CHECK_EQ((unsigned int)out[0], 0xE9u);
    CHECK(p == s + 2);
    CHECK_EQ(mbsrtowcs(out, &p, 4, &st), (size_t)1);
    CHECK_EQ((unsigned int)out[0], 0xE8u);
    CHECK(p == NULL);
}

TEST(utf8, the_string_conversions_report_a_bad_sequence) {
    wchar_t wide[8];
    errno = 0;
    CHECK_EQ(mbstowcs(wide, "\xC0\xAF", 8), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);

    wchar_t bad[2];
    bad[0] = (wchar_t)0xD800u;
    bad[1] = 0;
    char buffer[8];
    errno = 0;
    CHECK_EQ(wcstombs(buffer, bad, sizeof(buffer)), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);

    const char *p = "\xE2\x41";
    mbstate_t st = {0, 0, 0};
    errno = 0;
    CHECK_EQ(mbsrtowcs(wide, &p, 8, &st), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);

    const char *nullsrc = NULL;
    CHECK_EQ(mbsrtowcs(wide, &nullsrc, 8, &st), (size_t)-1);
    const wchar_t *nullwsrc = NULL;
    CHECK_EQ(wcsrtombs(buffer, &nullwsrc, sizeof(buffer), &st), (size_t)-1);
}

TEST(utf8, wcsrtombs_converts_and_resumes) {
    static const wchar_t source[] = {0x41u, 0x20ACu, 0x42u, 0};
    const wchar_t *p = source;
    char buffer[16];
    mbstate_t st = {0, 0, 0};
    memset(buffer, 0x7F, sizeof(buffer));
    CHECK_EQ(wcsrtombs(buffer, &p, sizeof(buffer), &st), (size_t)5);
    CHECK_EQ((unsigned char)buffer[0], 0x41u);
    CHECK_EQ((unsigned char)buffer[1], 0xE2u);
    CHECK_EQ((unsigned char)buffer[4], 0x42u);
    CHECK_EQ(buffer[5], 0);
    CHECK(p == NULL);

    p = source;
    memset(buffer, 0x7F, sizeof(buffer));
    CHECK_EQ(wcsrtombs(buffer, &p, 3, &st), (size_t)1);
    CHECK_EQ((unsigned char)buffer[1], 0x7Fu);
    CHECK(p == source + 1);
}

TEST(utf8, the_null_destination_forms_answer_without_writing) {

    mbstate_t st = {0, 0, 0};
    CHECK_EQ(mbrtowc(NULL, "\xE2\x82\xAC", 3, &st), (size_t)3);

    wchar_t wc = 0;
    CHECK_EQ(mbrtowc(&wc, "\xE2\x82", 2, &st), (size_t)-2);
    CHECK_EQ(mbrtowc(&wc, NULL, 0, &st), (size_t)0);
    CHECK_EQ(mbrtowc(&wc, "A", 1, &st), (size_t)1);
    CHECK_EQ((unsigned int)wc, 0x41u);

    CHECK_EQ(wcrtomb(NULL, (wchar_t)0x1F600u, &st), (size_t)1);

    CHECK_EQ(mbtowc(NULL, NULL, 0), 0);
    CHECK_EQ(wctomb(NULL, (wchar_t)0x41u), 0);
}

TEST(utf8, wctomb_reports_what_it_cannot_encode) {
    char buffer[8];
    CHECK_EQ(wctomb(buffer, (wchar_t)0xD800u), -1);
    CHECK_EQ(wctomb(buffer, (wchar_t)0x110000u), -1);
    CHECK_EQ(wctomb(buffer, (wchar_t)0x41u), 1);
}

TEST(utf8, mbstowcs_terminates_only_when_there_is_room) {
    static const char *s = "ab";
    wchar_t w[4];
    for (size_t i = 0; i < 4; i++) {
        w[i] = (wchar_t)0x7Fu;
    }
    CHECK_EQ(mbstowcs(w, s, 2), (size_t)2);
    CHECK_EQ((unsigned int)w[0], 0x61u);
    CHECK_EQ((unsigned int)w[1], 0x62u);
    CHECK_EQ((unsigned int)w[2], 0x7Fu);
    CHECK_EQ(mbstowcs(w, s, 3), (size_t)2);
    CHECK_EQ((unsigned int)w[2], 0u);
}

TEST(utf8, the_terminators_land_only_where_they_fit_and_are_NUL) {
    static const wchar_t one[] = {0x41u, 0};
    char buffer[8];
    memset(buffer, 0x7F, sizeof(buffer));
    CHECK_EQ(wcstombs(buffer, one, 1), (size_t)1);
    CHECK_EQ((unsigned char)buffer[0], 0x41u);
    CHECK_EQ((unsigned char)buffer[1], 0x7Fu);
    memset(buffer, 0x7F, sizeof(buffer));
    CHECK_EQ(wcstombs(buffer, one, 2), (size_t)1);
    CHECK_EQ((unsigned char)buffer[1], 0u);

    static const char *s = "\xC3\xA9";
    const char *p = s;
    mbstate_t st = {0, 0, 0};
    wchar_t w[4];
    for (size_t i = 0; i < 4; i++) {
        w[i] = (wchar_t)0x7Fu;
    }
    CHECK_EQ(mbsrtowcs(w, &p, 4, &st), (size_t)1);
    CHECK_EQ((unsigned int)w[0], 0xE9u);
    CHECK_EQ((unsigned int)w[1], 0u);
}

TEST(utf8, wcsrtombs_measures_and_refuses) {
    static const wchar_t source[] = {0x41u, 0x20ACu, 0};
    const wchar_t *p = source;
    mbstate_t st = {0, 0, 0};
    CHECK_EQ(wcsrtombs(NULL, &p, 0, &st), (size_t)4);
    CHECK(p == source);

    static const wchar_t one[] = {0x41u, 0};
    p = one;
    char tight[4];
    memset(tight, 0x7F, sizeof(tight));
    CHECK_EQ(wcsrtombs(tight, &p, 1, &st), (size_t)1);
    CHECK_EQ((unsigned char)tight[1], 0x7Fu);
    CHECK(p != NULL);

    static const wchar_t bad[] = {0x41u, 0x110000u, 0};
    p = bad;
    char buffer[8];
    errno = 0;
    CHECK_EQ(wcsrtombs(buffer, &p, sizeof(buffer), &st), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
}
