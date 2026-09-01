/* tests/test_libk.c - Q2
 *
 * kernel/lib/libk.c, compiled unmodified and run on the host.
 *
 * These are the primitives every other subsystem is built on: leanfs
 * copies directory records with them, TCP compacts its send and receive
 * buffers with them on every ACK, and the ELF loader lays out program
 * images with them. A one-byte error here is a one-byte error everywhere,
 * and -Werror cannot see any of it.
 *
 * Every buffer below is canary-guarded, because the bug worth catching in
 * a memory primitive is not "it returned the wrong answer" - it is "it
 * wrote one byte past the end and the answer was right". A test that only
 * checks the result cannot tell those apart. */
#include "check.h"
#include "lib/libk.h"

#define PAD 8
#define CANARY 0xA5

/* A buffer with CANARY-filled margins either side. `usable` points at the
 * middle; guard_intact() says whether anything wrote outside it. */
typedef struct {
    unsigned char raw[PAD + 256 + PAD];
    unsigned char *usable;
    size_t len;
} guarded_t;

static void guard_init(guarded_t *g, size_t len) {
    REQUIRE(len <= 256);
    for (size_t i = 0; i < sizeof(g->raw); i++) {
        g->raw[i] = CANARY;
    }
    g->usable = g->raw + PAD;
    g->len = len;
}

static int guard_intact(const guarded_t *g) {
    for (size_t i = 0; i < PAD; i++) {
        if (g->raw[i] != CANARY) {
            return 0;
        }
        if (g->raw[PAD + g->len + i] != CANARY) {
            return 0;
        }
    }
    return 1;
}

TEST(libk, memset_fills_exactly_its_range) {
    guarded_t g;
    for (size_t n = 0; n <= 64; n++) {
        guard_init(&g, n);
        CHECK(k_memset(g.usable, 0x5A, n) == g.usable);
        for (size_t i = 0; i < n; i++) {
            CHECK_EQ(g.usable[i], 0x5A);
        }
        CHECK(guard_intact(&g));
    }
}

TEST(libk, memset_truncates_its_value_to_a_byte) {
    /* The signature takes an int, like memset. Passing 0x1FF must store
     * 0xFF and not overflow into the next byte. */
    guarded_t g;
    guard_init(&g, 4);
    k_memset(g.usable, 0x1FF, 4);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(g.usable[i], 0xFF);
    }
    CHECK(guard_intact(&g));
}

TEST(libk, memcpy_copies_exactly_its_range) {
    guarded_t dst;
    unsigned char src[64];
    for (size_t i = 0; i < sizeof(src); i++) {
        src[i] = (unsigned char)(i * 7 + 1);
    }
    for (size_t n = 0; n <= 64; n++) {
        guard_init(&dst, n);
        CHECK(k_memcpy(dst.usable, src, n) == dst.usable);
        CHECK_MEMEQ(dst.usable, src, n);
        CHECK(guard_intact(&dst));
    }
}

TEST(libk, memmove_handles_every_overlap_direction) {
    /* The case k_memmove exists for, and the case it was added in M66 to
     * fix: a buffer compacted toward its own start, and the mirror of it.
     * Both directions, at every offset up to the buffer length, against a
     * reference computed the obvious way. */
    for (int shift = -16; shift <= 16; shift++) {
        unsigned char buf[64];
        unsigned char expect[64];
        const size_t n = 32;
        const size_t base = 16;

        for (size_t i = 0; i < sizeof(buf); i++) {
            buf[i] = (unsigned char)(i + 100);
            expect[i] = (unsigned char)(i + 100);
        }

        unsigned char *dst = buf + base + shift;
        const unsigned char *src = buf + base;
        for (size_t i = 0; i < n; i++) {
            expect[(base + shift) + i] = (unsigned char)(base + i + 100);
        }

        CHECK(k_memmove(dst, src, n) == dst);
        CHECK_MEMEQ(buf, expect, sizeof(buf));
    }
}

TEST(libk, memmove_of_zero_bytes_and_onto_itself_touch_nothing) {
    guarded_t g;
    guard_init(&g, 16);
    for (int i = 0; i < 16; i++) {
        g.usable[i] = (unsigned char)i;
    }
    CHECK(k_memmove(g.usable, g.usable, 16) == g.usable);
    CHECK(k_memmove(g.usable, g.usable + 4, 0) == g.usable);
    for (int i = 0; i < 16; i++) {
        CHECK_EQ(g.usable[i], i);
    }
    CHECK(guard_intact(&g));
}

TEST(libk, memmove_boundary_where_ranges_just_touch) {
    /* dst == src + n exactly: adjacent, not overlapping. The forward path
     * is correct here and the backward path would also be - this is the
     * off-by-one in k_memmove's own `d < s + n` condition, checked. */
    unsigned char buf[32];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (unsigned char)i;
    }
    k_memmove(buf + 8, buf, 8);
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(buf[8 + i], i);
    }
}

TEST(libk, strlen_counts_to_the_terminator) {
    CHECK_EQ(k_strlen(""), 0);
    CHECK_EQ(k_strlen("a"), 1);
    CHECK_EQ(k_strlen("hello"), 5);
    /* An embedded NUL stops it - the contract, and the reason leanfs
     * directory records use k_memcmp instead. */
    CHECK_EQ(k_strlen("ab\0cd"), 2);
}

TEST(libk, memcmp_reads_exactly_n_bytes_and_signs_correctly) {
    CHECK_EQ(k_memcmp("abc", "abc", 3), 0);
    CHECK_EQ(k_memcmp("", "", 0), 0);
    /* Differs only past n - must still compare equal. This is the whole
     * point of a length-bounded compare and the property leanfs relies on
     * for names that are stored without a terminator. */
    CHECK_EQ(k_memcmp("abcZ", "abcQ", 3), 0);
    CHECK(k_memcmp("abd", "abc", 3) > 0);
    CHECK(k_memcmp("abc", "abd", 3) < 0);
    /* High bytes must compare as unsigned. Signed chars would make 0x80
     * sort below 0x01, which is the classic version of this bug. */
    CHECK(k_memcmp("\x80", "\x01", 1) > 0);
    CHECK(k_memcmp("\x01", "\x80", 1) < 0);
}

TEST(libk, strcmp_signs_correctly_including_high_bytes) {
    CHECK_EQ(k_strcmp("", ""), 0);
    CHECK_EQ(k_strcmp("same", "same"), 0);
    CHECK(k_strcmp("abc", "abd") < 0);
    CHECK(k_strcmp("abd", "abc") > 0);
    /* A prefix sorts before the longer string. */
    CHECK(k_strcmp("ab", "abc") < 0);
    CHECK(k_strcmp("abc", "ab") > 0);
    CHECK(k_strcmp("\x80", "\x01") > 0);
}

TEST(libk, strlcpy_always_terminates_and_never_overruns) {
    guarded_t g;
    /* n == 0 must write nothing at all, terminator included. */
    guard_init(&g, 0);
    k_strlcpy((char *)g.usable, "anything", 0);
    CHECK(guard_intact(&g));

    /* Exact fit, one short, and far too small. The terminator lands at
     * min(n-1, strlen(src)) - NOT always at n-1, which is the difference
     * between this and a memset-then-copy and the thing a careless test
     * gets wrong before the code does. */
    for (size_t n = 1; n <= 12; n++) {
        guard_init(&g, n);
        k_strlcpy((char *)g.usable, "abcdefgh", n);
        CHECK(guard_intact(&g));
        size_t expect = (n - 1 < 8) ? n - 1 : 8;
        CHECK_EQ(g.usable[expect], 0);
        CHECK_EQ(k_strlen((const char *)g.usable), expect);
        CHECK_MEMEQ(g.usable, "abcdefgh", expect);
    }
}

TEST(libk, strstr_finds_at_every_position_and_refuses_the_rest) {
    const char *hay = "the quick brown fox";
    CHECK(k_strstr(hay, "the") == hay);
    CHECK(k_strstr(hay, "quick") == hay + 4);
    CHECK(k_strstr(hay, "fox") == hay + 16);
    CHECK(k_strstr(hay, "") == hay);       /* an empty needle matches at 0 */
    CHECK(k_strstr(hay, "cat") == NULL);
    /* A needle longer than the haystack must not read past the
     * terminator looking for it. */
    CHECK(k_strstr("ab", "abcdef") == NULL);
    CHECK(k_strstr("", "a") == NULL);
    /* A near-miss that shares a prefix with the real match - the case a
     * naive scanner that does not restart correctly gets wrong. */
    CHECK_STREQ(k_strstr("aaab", "aab"), "aab");
}

/* ---- Q12: what the mutation census found here -------------------------
 *
 * libk.c had 100% line coverage and a 78.6% mutation score. The
 * survivors were all in k_memmove and k_strlcpy, and all of the same
 * shape: a boundary that every existing test straddled without ever
 * landing on. */

TEST(libk, memmove_at_exactly_one_byte_of_overlap) {
    /* `d < s + n` mutated to `<=` survived, which means no test placed
     * the destination exactly at the far edge of the source. These do:
     * one byte of overlap in each direction, where the forward and
     * backward paths give different answers. */
    for (int dir = 0; dir < 2; dir++) {
        unsigned char buf[32], expect[32];
        for (int i = 0; i < 32; i++) {
            buf[i] = expect[i] = (unsigned char)(i + 1);
        }
        const size_t n = 8;
        size_t src_off = dir ? 8 : 15;
        size_t dst_off = dir ? 15 : 8;   /* exactly one byte of overlap */
        for (size_t i = 0; i < n; i++) {
            expect[dst_off + i] = (unsigned char)(src_off + i + 1);
        }
        k_memmove(buf + dst_off, buf + src_off, n);
        CHECK_MEMEQ(buf, expect, sizeof(buf));
    }
}

TEST(libk, memmove_of_a_single_byte_in_both_directions) {
    /* `n == 0` mutated to `n == 1` survived: a one-byte move would
     * become a no-op and nothing noticed, because no test moved exactly
     * one byte. */
    unsigned char buf[4] = {1, 2, 3, 4};
    k_memmove(buf + 1, buf, 1);
    CHECK_EQ(buf[1], 1);
    unsigned char buf2[4] = {1, 2, 3, 4};
    k_memmove(buf2, buf2 + 3, 1);
    CHECK_EQ(buf2[0], 4);
}

TEST(libk, memmove_backwards_copies_the_first_byte_too) {
    /* `for (size_t i = n; i > 0; i--)` is the backward loop, and its
     * bound had a survivor. The byte it would drop is the first one, so
     * a test whose source bytes are all identical cannot see it. These
     * are all different. */
    unsigned char buf[16];
    for (int i = 0; i < 16; i++) {
        buf[i] = (unsigned char)(0xA0 + i);
    }
    k_memmove(buf + 1, buf, 15);
    CHECK_EQ(buf[1], 0xA0);            /* the first source byte, moved */
    for (int i = 0; i < 15; i++) {
        CHECK_EQ(buf[1 + i], 0xA0 + i);
    }
}

TEST(libk, strlcpy_at_exactly_the_length_of_the_source) {
    /* `i < n - 1` had a survivor: the case where n is exactly
     * strlen(src) + 1, which is the commonest sizing a caller writes and
     * the one place an off-by-one truncates a string that fits. */
    char dst[6];
    k_strlcpy(dst, "hello", sizeof(dst));   /* 5 chars + NUL, exactly */
    CHECK_STREQ(dst, "hello");

    char dst2[5];
    k_strlcpy(dst2, "hello", sizeof(dst2)); /* one short */
    CHECK_STREQ(dst2, "hell");
}
