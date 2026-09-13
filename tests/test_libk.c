#include "check.h"
#include "library/kernel_library.h"

#define PAD 8
#define CANARY 0xA5

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

TEST(kernel_library, memset_fills_exactly_its_range) {
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

TEST(kernel_library, memset_truncates_its_value_to_a_byte) {
    guarded_t g;
    guard_init(&g, 4);
    k_memset(g.usable, 0x1FF, 4);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(g.usable[i], 0xFF);
    }
    CHECK(guard_intact(&g));
}

TEST(kernel_library, memcpy_copies_exactly_its_range) {
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

TEST(kernel_library, memmove_handles_every_overlap_direction) {
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

TEST(kernel_library, memmove_of_zero_bytes_and_onto_itself_touch_nothing) {
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

TEST(kernel_library, memmove_boundary_where_ranges_just_touch) {
    unsigned char buf[32];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (unsigned char)i;
    }
    k_memmove(buf + 8, buf, 8);
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(buf[8 + i], i);
    }
}

TEST(kernel_library, strlen_counts_to_the_terminator) {
    CHECK_EQ(k_strlen(""), 0);
    CHECK_EQ(k_strlen("a"), 1);
    CHECK_EQ(k_strlen("hello"), 5);
    CHECK_EQ(k_strlen("ab\0cd"), 2);
}

TEST(kernel_library, memcmp_reads_exactly_n_bytes_and_signs_correctly) {
    CHECK_EQ(k_memcmp("abc", "abc", 3), 0);
    CHECK_EQ(k_memcmp("", "", 0), 0);
    CHECK_EQ(k_memcmp("abcZ", "abcQ", 3), 0);
    CHECK(k_memcmp("abd", "abc", 3) > 0);
    CHECK(k_memcmp("abc", "abd", 3) < 0);
    CHECK(k_memcmp("\x80", "\x01", 1) > 0);
    CHECK(k_memcmp("\x01", "\x80", 1) < 0);
}

TEST(kernel_library, strcmp_signs_correctly_including_high_bytes) {
    CHECK_EQ(k_strcmp("", ""), 0);
    CHECK_EQ(k_strcmp("same", "same"), 0);
    CHECK(k_strcmp("abc", "abd") < 0);
    CHECK(k_strcmp("abd", "abc") > 0);
    CHECK(k_strcmp("ab", "abc") < 0);
    CHECK(k_strcmp("abc", "ab") > 0);
    CHECK(k_strcmp("\x80", "\x01") > 0);
}

TEST(kernel_library, strlcpy_always_terminates_and_never_overruns) {
    guarded_t g;
    guard_init(&g, 0);
    k_strlcpy((char *)g.usable, "anything", 0);
    CHECK(guard_intact(&g));

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

TEST(kernel_library, strstr_finds_at_every_position_and_refuses_the_rest) {
    const char *hay = "the quick brown fox";
    CHECK(k_strstr(hay, "the") == hay);
    CHECK(k_strstr(hay, "quick") == hay + 4);
    CHECK(k_strstr(hay, "fox") == hay + 16);
    CHECK(k_strstr(hay, "") == hay);
    CHECK(k_strstr(hay, "cat") == NULL);
    CHECK(k_strstr("ab", "abcdef") == NULL);
    CHECK(k_strstr("", "a") == NULL);
    CHECK_STREQ(k_strstr("aaab", "aab"), "aab");
}

TEST(kernel_library, memmove_at_exactly_one_byte_of_overlap) {
    for (int dir = 0; dir < 2; dir++) {
        unsigned char buf[32], expect[32];
        for (int i = 0; i < 32; i++) {
            buf[i] = expect[i] = (unsigned char)(i + 1);
        }
        const size_t n = 8;
        size_t source_off = dir ? 8 : 15;
        size_t destination_off = dir ? 15 : 8;
        for (size_t i = 0; i < n; i++) {
            expect[destination_off + i] = (unsigned char)(source_off + i + 1);
        }
        k_memmove(buf + destination_off, buf + source_off, n);
        CHECK_MEMEQ(buf, expect, sizeof(buf));
    }
}

TEST(kernel_library, memmove_of_a_single_byte_in_both_directions) {
    unsigned char buf[4] = {1, 2, 3, 4};
    k_memmove(buf + 1, buf, 1);
    CHECK_EQ(buf[1], 1);
    unsigned char buf2[4] = {1, 2, 3, 4};
    k_memmove(buf2, buf2 + 3, 1);
    CHECK_EQ(buf2[0], 4);
}

TEST(kernel_library, memmove_backwards_copies_the_first_byte_too) {
    unsigned char buf[16];
    for (int i = 0; i < 16; i++) {
        buf[i] = (unsigned char)(0xA0 + i);
    }
    k_memmove(buf + 1, buf, 15);
    CHECK_EQ(buf[1], 0xA0);
    for (int i = 0; i < 15; i++) {
        CHECK_EQ(buf[1 + i], 0xA0 + i);
    }
}

TEST(kernel_library, strlcpy_at_exactly_the_length_of_the_source) {
    char dst[6];
    k_strlcpy(dst, "hello", sizeof(dst));
    CHECK_STREQ(dst, "hello");

    char dst2[5];
    k_strlcpy(dst2, "hello", sizeof(dst2));
    CHECK_STREQ(dst2, "hell");
}
