/* tests/test_utf8.c - M88
 *
 * user_space/libc/src/wchar.c's UTF-8 conversions, compiled unmodified
 * and run on the host.
 *
 * ---- why this is a host test and not a boot marker -------------------
 *
 * The interesting half of a UTF-8 decoder is the set of sequences it
 * REFUSES, and a booted machine cannot reach any of them: nothing on
 * this disk contains an overlong encoding or a lone surrogate, because
 * nothing that produced those files would emit one. They have to be
 * written down as bytes by a test, which is exactly what this tier is
 * for.
 *
 * It matters more here than it usually does. An overlong 0xC0 0xAF is
 * "/" written the long way; a decoder that accepts it hands a path check
 * a slash it never saw. Every real UTF-8 security bug in the last thirty
 * years is in this family, and the only difference between a decoder
 * that has the rule and one that does not is a test that supplies the
 * bytes.
 *
 * The round trip is graded against an oracle rather than against a
 * table: every code point that survives encode-then-decode has to come
 * back identical, over the whole range including both sides of every
 * length boundary. That is a property the implementation cannot satisfy
 * by agreeing with a list this file wrote down.
 */
#include "check.h"

#include <wchar.h>  /* redirected to this project's own - see tests/fakes/wchar.h */
#include <errno.h>

/* Encodes `cp`, decodes what came back, and returns 1 if the round trip
 * is exact. Length is checked too: a decoder that consumed a different
 * number of bytes than the encoder wrote would still round-trip one
 * character and desynchronise the next one. */
static int roundtrips(unsigned int cp) {
    char buf[8];
    mbstate_t st = {0, 0, 0};
    size_t n = wcrtomb(buf, (wchar_t)cp, &st);
    if (n == (size_t)-1 || n > 4) {
        return 0;
    }
    wchar_t back = 0;
    mbstate_t rst = {0, 0, 0};
    size_t used = mbrtowc(&back, buf, n, &rst);
    if (cp == 0) {
        /* A completed L'\0' reports 0 bytes consumed, which is how a
         * caller walking a string knows it has ended. */
        return used == 0 && back == 0 && n == 1;
    }
    return used == n && (unsigned int)back == cp;
}

TEST(utf8, every_code_point_round_trips) {
    /* The whole range, in steps small enough to be quick and uneven
     * enough not to line up with any power of two - a stride of 1024
     * would test only code points whose low bits are zero, which is the
     * half of a shift-and-mask bug that always works. */
    for (unsigned int cp = 0; cp <= 0x10FFFFu; cp += 37) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) {
            continue; /* surrogates are refused, and have their own test */
        }
        if (!roundtrips(cp)) {
            /* Reported once and then abandoned: a decoder broken for one
             * length is broken for a third of this range, and thirty
             * thousand identical failure lines bury every other test in
             * the run. */
            test_fail(__FILE__, __LINE__, "U+%04X did not round trip", cp);
            break;
        }
    }
}

TEST(utf8, every_length_boundary_round_trips) {
    /* The values either side of each length change, which is where an
     * off-by-one in the encoder lives and which a stride will step over. */
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
    char buf[8];
    CHECK_EQ(wcrtomb(buf, (wchar_t)0x41u, NULL), (size_t)1);
    CHECK_EQ(wcrtomb(buf, (wchar_t)0xE9u, NULL), (size_t)2);   /* e-acute */
    CHECK_EQ(wcrtomb(buf, (wchar_t)0x20ACu, NULL), (size_t)3); /* euro sign */
    CHECK_EQ(wcrtomb(buf, (wchar_t)0x1F600u, NULL), (size_t)4);
}

TEST(utf8, the_encoding_is_the_one_on_the_wire) {
    /* Not just self-consistent - the actual bytes. A decoder and encoder
     * that agreed with each other on a private encoding would pass every
     * round-trip test above and produce files nothing else can read. */
    char buf[8];
    CHECK_EQ(wcrtomb(buf, (wchar_t)0xE9u, NULL), (size_t)2);
    CHECK_EQ((unsigned char)buf[0], 0xC3u);
    CHECK_EQ((unsigned char)buf[1], 0xA9u);
    CHECK_EQ(wcrtomb(buf, (wchar_t)0x20ACu, NULL), (size_t)3);
    CHECK_EQ((unsigned char)buf[0], 0xE2u);
    CHECK_EQ((unsigned char)buf[1], 0x82u);
    CHECK_EQ((unsigned char)buf[2], 0xACu);
}

/* Decodes a fixed byte string and returns what mbrtowc reported. */
static size_t decode(const char *bytes, size_t n, wchar_t *out) {
    mbstate_t st = {0, 0, 0};
    errno = 0;
    return mbrtowc(out, bytes, n, &st);
}

TEST(utf8, an_overlong_form_is_refused) {
    /* The one that matters: 0xC0 0xAF is '/' written in two bytes.
     * Accepting it is how a path check gets walked past. 0xC0 can begin
     * nothing else, so it is refused on the lead byte alone - and
     * 0xE0 0x80 0xAF is the same character written in three, which needs
     * the value check at the end of the sequence to catch. Both. */
    wchar_t wc = 0;
    CHECK_EQ(decode("\xC0\xAF", 2, &wc), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
    CHECK_EQ(decode("\xE0\x80\xAF", 3, &wc), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
    /* And the shortest overlong of all: NUL written as two bytes, which
     * is the trick that hides a terminator from a C string. */
    CHECK_EQ(decode("\xC0\x80", 2, &wc), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
}

TEST(utf8, a_surrogate_half_is_refused_both_ways) {
    wchar_t wc = 0;
    /* U+D800 encoded as if it were an ordinary three-byte character. */
    CHECK_EQ(decode("\xED\xA0\x80", 3, &wc), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
    char buf[8];
    errno = 0;
    CHECK_EQ(wcrtomb(buf, (wchar_t)0xD800u, NULL), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
}

TEST(utf8, past_the_last_code_point_is_refused) {
    wchar_t wc = 0;
    /* U+110000, one past the end, and a five-byte lead that could only
     * ever encode something larger. */
    CHECK_EQ(decode("\xF4\x90\x80\x80", 4, &wc), (size_t)-1);
    CHECK_EQ(decode("\xF8\x88\x80\x80\x80", 5, &wc), (size_t)-1);
    char buf[8];
    errno = 0;
    CHECK_EQ(wcrtomb(buf, (wchar_t)0x110000u, NULL), (size_t)-1);
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
    /* The distinction the whole restartable interface exists for. A
     * caller reading a stream in chunks must be able to tell "these
     * bytes are wrong" from "I have not given you enough yet", because
     * the second one is answered by reading more and the first one is
     * not. */
    wchar_t wc = 0;
    CHECK_EQ(decode("\xE2\x82", 2, &wc), (size_t)-2);
    CHECK_EQ(decode("\xF0\x9F", 2, &wc), (size_t)-2);
    /* And a lead byte where a continuation was owed is invalid rather
     * than incomplete - more bytes will not rescue it. */
    CHECK_EQ(decode("\xE2\x41", 2, &wc), (size_t)-1);
}

TEST(utf8, a_sequence_split_across_two_calls_completes) {
    /* The reason mbstate_t holds anything at all: a program reading a
     * file in fixed-size chunks will cut a character in half, and the
     * next call has to finish it. Split at every interior byte of a
     * four-byte character. */
    const char *seq = "\xF0\x9F\x98\x80"; /* U+1F600 */
    for (size_t cut = 1; cut < 4; cut++) {
        mbstate_t st = {0, 0, 0};
        wchar_t wc = 0;
        CHECK_EQ(mbrtowc(&wc, seq, cut, &st), (size_t)-2);
        size_t r = mbrtowc(&wc, seq + cut, 4 - cut, &st);
        CHECK_EQ(r, 4 - cut); /* only the bytes THIS call consumed */
        CHECK_EQ((unsigned int)wc, 0x1F600u);
    }
}

TEST(utf8, strings_convert_both_ways) {
    /* Two ASCII characters, a two-byte one, a three-byte one and a
     * four-byte one: "aé€😀b" - five characters, eleven bytes. The two
     * numbers being different is the whole point of the test. */
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

    /* And the measuring form: a NULL destination counts without writing,
     * which is how a caller sizes the buffer it is about to allocate. */
    CHECK_EQ(mbstowcs(NULL, s, 0), (size_t)5);
    CHECK_EQ(wcstombs(NULL, wide, 0), (size_t)11);
}

TEST(utf8, wcstombs_never_writes_a_partial_character) {
    /* The failure this exists to prevent: a buffer that ends mid-sequence
     * is not a shorter string, it is a string containing bytes that are
     * not text. So a character that does not fit whole is not written at
     * all, and the count reports what did fit. */
    wchar_t wide[3];
    wide[0] = (wchar_t)0x41u;    /* 1 byte */
    wide[1] = (wchar_t)0x20ACu;  /* 3 bytes */
    wide[2] = 0;
    char buf[8];
    for (size_t room = 1; room < 4; room++) {
        memset(buf, 0x7F, sizeof(buf));
        CHECK_EQ(wcstombs(buf, wide, room), (size_t)1);
        CHECK_EQ((unsigned char)buf[0], 0x41u);
        /* Nothing of the euro sign reached the buffer. */
        CHECK_EQ((unsigned char)buf[1], 0x7Fu);
    }
    /* With room for all four bytes plus the terminator it fits. */
    CHECK_EQ(wcstombs(buf, wide, 5), (size_t)4);
}

TEST(utf8, mblen_agrees_with_the_decoder) {
    CHECK_EQ(mblen("a", 1), 1);
    CHECK_EQ(mblen("\xC3\xA9", 2), 2);
    CHECK_EQ(mblen("\xE2\x82\xAC", 3), 3);
    CHECK_EQ(mblen("\xF0\x9F\x98\x80", 4), 4);
    CHECK_EQ(mblen("", 1), 0);          /* the terminator is zero bytes of character */
    CHECK_EQ(mblen("\xC3", 1), -1);     /* incomplete has no separate answer here */
    CHECK_EQ(mblen("\x80", 1), -1);
}

TEST(utf8, a_bad_byte_does_not_wedge_the_state) {
    /* After an error the caller must be able to skip the byte and carry
     * on. A decoder that kept its half-finished state would report
     * EILSEQ forever and turn one corrupt byte into an unreadable file. */
    mbstate_t st = {0, 0, 0};
    wchar_t wc = 0;
    CHECK_EQ(mbrtowc(&wc, "\xE2\x41", 2, &st), (size_t)-1);
    CHECK_EQ(mbrtowc(&wc, "A", 1, &st), (size_t)1);
    CHECK_EQ((unsigned int)wc, 0x41u);
}

TEST(utf8, the_restartable_string_form_resumes_where_it_stopped) {
    static const char *s = "\xC3\xA9\xC3\xA8"; /* two two-byte characters */
    const char *p = s;
    mbstate_t st = {0, 0, 0};
    wchar_t out[4];
    /* Room for one, so it stops after one and leaves the pointer at the
     * second - which is the entire difference between this and mbstowcs. */
    CHECK_EQ(mbsrtowcs(out, &p, 1, &st), (size_t)1);
    CHECK_EQ((unsigned int)out[0], 0xE9u);
    CHECK(p == s + 2);
    CHECK_EQ(mbsrtowcs(out, &p, 4, &st), (size_t)1);
    CHECK_EQ((unsigned int)out[0], 0xE8u);
    CHECK(p == NULL); /* the terminator was consumed */
}

/* ---- the cases the mutation harness found ---------------------------
 *
 * `make mutate FILE=user_space/libc/src/wchar.c` broke this file's UTF-8
 * half sixty ways and the tests above noticed eighteen of them. The
 * tests below are the difference, and what they have in common is worth
 * naming: **every one is an error return or a NULL-destination form.**
 * The tests above grade what the conversions produce; nothing graded
 * what they report when they cannot produce anything, and nothing at all
 * called `wcsrtombs`.
 *
 * That is the failure mode the harness exists to find. Coverage was
 * already total - `wcstombs`' error path runs in the round-trip test the
 * moment a surrogate reaches it - and total coverage with no assertion
 * on the returned value is a line that executes and proves nothing.
 */

TEST(utf8, the_string_conversions_report_a_bad_sequence) {
    /* Each of these executes its error path in the tests above and none
     * of those tests looks at what came back. A mutant returning
     * (size_t)-2 instead of (size_t)-1 passed all of them. */
    wchar_t wide[8];
    errno = 0;
    CHECK_EQ(mbstowcs(wide, "\xC0\xAF", 8), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);

    wchar_t bad[2];
    bad[0] = (wchar_t)0xD800u; /* a surrogate half cannot be encoded */
    bad[1] = 0;
    char buf[8];
    errno = 0;
    CHECK_EQ(wcstombs(buf, bad, sizeof(buf)), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);

    const char *p = "\xE2\x41";
    mbstate_t st = {0, 0, 0};
    errno = 0;
    CHECK_EQ(mbsrtowcs(wide, &p, 8, &st), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);

    /* And the NULL-source guards, which no caller above passes. */
    const char *nullsrc = NULL;
    CHECK_EQ(mbsrtowcs(wide, &nullsrc, 8, &st), (size_t)-1);
    const wchar_t *nullwsrc = NULL;
    CHECK_EQ(wcsrtombs(buf, &nullwsrc, sizeof(buf), &st), (size_t)-1);
}

TEST(utf8, wcsrtombs_converts_and_resumes) {
    /* Nothing called this at all before, which is why four consecutive
     * mutants inside it survived. It is wcstombs with a resumable
     * position, and both halves of that need saying. */
    static const wchar_t src[] = {0x41u, 0x20ACu, 0x42u, 0};
    const wchar_t *p = src;
    char buf[16];
    mbstate_t st = {0, 0, 0};
    memset(buf, 0x7F, sizeof(buf));
    CHECK_EQ(wcsrtombs(buf, &p, sizeof(buf), &st), (size_t)5); /* 1 + 3 + 1 */
    CHECK_EQ((unsigned char)buf[0], 0x41u);
    CHECK_EQ((unsigned char)buf[1], 0xE2u);
    CHECK_EQ((unsigned char)buf[4], 0x42u);
    CHECK_EQ(buf[5], 0);      /* the terminator was written */
    CHECK(p == NULL);         /* ...and the source was consumed to the end */

    /* Stopped short: the euro sign does not fit whole in the two bytes
     * left after 'A', so it is not written at all and the pointer is
     * left on it. A partial sequence in the buffer would be bytes that
     * are not text. */
    p = src;
    memset(buf, 0x7F, sizeof(buf));
    CHECK_EQ(wcsrtombs(buf, &p, 3, &st), (size_t)1);
    CHECK_EQ((unsigned char)buf[1], 0x7Fu);
    CHECK(p == src + 1);
}

TEST(utf8, the_null_destination_forms_answer_without_writing) {
    /* Every one of these is a documented form of the call that no test
     * above used, and each had a surviving mutant sitting in it. */

    /* mbrtowc(NULL, s, n, ps): decode and report, storing nothing. */
    mbstate_t st = {0, 0, 0};
    CHECK_EQ(mbrtowc(NULL, "\xE2\x82\xAC", 3, &st), (size_t)3);

    /* mbrtowc(dst, NULL, ...): reset the state and report that this
     * encoding is not stateful between characters. The reset is the part
     * that matters - a half-finished sequence must be abandoned. */
    wchar_t wc = 0;
    CHECK_EQ(mbrtowc(&wc, "\xE2\x82", 2, &st), (size_t)-2); /* two bytes owed one more */
    CHECK_EQ(mbrtowc(&wc, NULL, 0, &st), (size_t)0);
    /* If the state had survived, this 'A' would be read as a
     * continuation byte and refused. */
    CHECK_EQ(mbrtowc(&wc, "A", 1, &st), (size_t)1);
    CHECK_EQ((unsigned int)wc, 0x41u);

    /* wcrtomb(NULL, c, ps): "how many bytes to return to the initial
     * state, plus a terminator" - one, and the character is ignored. */
    CHECK_EQ(wcrtomb(NULL, (wchar_t)0x1F600u, &st), (size_t)1);

    /* mbtowc(NULL, NULL, 0) is the statefulness query, and wctomb(NULL)
     * is the same question for the other direction. */
    CHECK_EQ(mbtowc(NULL, NULL, 0), 0);
    CHECK_EQ(wctomb(NULL, (wchar_t)0x41u), 0);
}

TEST(utf8, wctomb_reports_what_it_cannot_encode) {
    /* wcrtomb's refusal is checked above; wctomb's is a separate return
     * statement with a separate value, and a mutant that changed -1 to
     * -2 survived because nothing looked at it. */
    char buf[8];
    CHECK_EQ(wctomb(buf, (wchar_t)0xD800u), -1);
    CHECK_EQ(wctomb(buf, (wchar_t)0x110000u), -1);
    CHECK_EQ(wctomb(buf, (wchar_t)0x41u), 1);
}

TEST(utf8, mbstowcs_terminates_only_when_there_is_room) {
    /* The `dst && out < n` guard on the terminator: with room for
     * exactly the characters and none for a NUL, the count is still
     * right and nothing is written past the end. */
    static const char *s = "ab";
    wchar_t w[4];
    for (size_t i = 0; i < 4; i++) {
        w[i] = (wchar_t)0x7Fu;
    }
    CHECK_EQ(mbstowcs(w, s, 2), (size_t)2);
    CHECK_EQ((unsigned int)w[0], 0x61u);
    CHECK_EQ((unsigned int)w[1], 0x62u);
    CHECK_EQ((unsigned int)w[2], 0x7Fu); /* untouched - there was no room for a terminator */
    /* One more slot, and the terminator lands. */
    CHECK_EQ(mbstowcs(w, s, 3), (size_t)2);
    CHECK_EQ((unsigned int)w[2], 0u);
}

TEST(utf8, the_terminators_land_only_where_they_fit_and_are_NUL) {
    /* A second pass with the harness, after the tests above. Three of
     * the survivors it still reported were terminator writes, and they
     * had all been *executed* without being *looked at* - the same shape
     * as the batch before, one layer in.
     *
     * The case none of the tests above reached: a string that fits
     * exactly, with no room left for a terminator. Every earlier
     * wcstombs test returned early from inside the loop because a
     * character did not fit, so the guard at the end never decided
     * anything. */
    static const wchar_t one[] = {0x41u, 0};
    char buf[8];
    memset(buf, 0x7F, sizeof(buf));
    CHECK_EQ(wcstombs(buf, one, 1), (size_t)1);
    CHECK_EQ((unsigned char)buf[0], 0x41u);
    CHECK_EQ((unsigned char)buf[1], 0x7Fu); /* no room, so nothing written */
    memset(buf, 0x7F, sizeof(buf));
    CHECK_EQ(wcstombs(buf, one, 2), (size_t)1);
    CHECK_EQ((unsigned char)buf[1], 0u);    /* room, so a NUL and not some other byte */

    /* And the same question of mbsrtowcs, whose terminator nothing had
     * inspected either. */
    static const char *s = "\xC3\xA9";
    const char *p = s;
    mbstate_t st = {0, 0, 0};
    wchar_t w[4];
    for (size_t i = 0; i < 4; i++) {
        w[i] = (wchar_t)0x7Fu;
    }
    CHECK_EQ(mbsrtowcs(w, &p, 4, &st), (size_t)1);
    CHECK_EQ((unsigned int)w[0], 0xE9u);
    CHECK_EQ((unsigned int)w[1], 0u); /* a NUL terminator, not a 1 */
}

TEST(utf8, wcsrtombs_measures_and_refuses) {
    /* The two forms of this call nothing had used: counting without a
     * destination, and meeting a code point that cannot be encoded. */
    static const wchar_t src[] = {0x41u, 0x20ACu, 0};
    const wchar_t *p = src;
    mbstate_t st = {0, 0, 0};
    /* NULL destination: report the byte count and leave the source
     * pointer where it was, because nothing was consumed. */
    CHECK_EQ(wcsrtombs(NULL, &p, 0, &st), (size_t)4);
    CHECK(p == src);

    /* And the exactly-no-room case, which is the mirror of the wcstombs
     * one above: the source fits, the terminator does not, so neither
     * the NUL nor the "source consumed" signal may be written. A
     * caller told the string was finished when the buffer merely ran
     * out would silently drop the rest of it. */
    static const wchar_t one[] = {0x41u, 0};
    p = one;
    char tight[4];
    memset(tight, 0x7F, sizeof(tight));
    CHECK_EQ(wcsrtombs(tight, &p, 1, &st), (size_t)1);
    CHECK_EQ((unsigned char)tight[1], 0x7Fu);
    CHECK(p != NULL);

    static const wchar_t bad[] = {0x41u, 0x110000u, 0};
    p = bad;
    char buf[8];
    errno = 0;
    CHECK_EQ(wcsrtombs(buf, &p, sizeof(buf), &st), (size_t)-1);
    CHECK_EQ(errno, EILSEQ);
}
