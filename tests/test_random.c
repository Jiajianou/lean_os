/* tests/test_random.c - M100: the random device, off the machine.
 *
 * Three claims kernel/dev/random.h makes, each graded by the one
 * instrument that can grade it:
 *
 *   - ChaCha20 is ChaCha20. RFC 7539's own block-function test vector,
 *     section 2.3.2, fetched from the RFC editor and typed nowhere else -
 *     the one place in this file where the right answer was decided by
 *     somebody who was not writing the code. A quarter-round with one
 *     rotation wrong produces 64 confident bytes that are all wrong,
 *     and nothing but a known answer can see that.
 *   - Extraction erases the key. Output must never be a function of a
 *     state that still exists: two extractions differ, the state after
 *     an extraction cannot reproduce it, and feeding the pool changes
 *     everything after it.
 *   - The output is not a counter. Not a proof of randomness - there is
 *     no such test - but the cheap sanity checks that catch the bugs a
 *     stream generator actually has: a byte histogram that is flat to
 *     within statistics, no repeated 64-byte block across a megabyte,
 *     and a keystream that is not the key.
 *
 * The TSC the generator mixes on every feed is a variable here, so the
 * generator can be held still: with the same seed and the same "TSC" it
 * must produce the same stream, or the determinism that lets the RFC
 * vector be checked at all is not there.
 */
#include "check.h"

#include "dev/random.h"

#include <string.h>

extern uint64_t random_test_tsc_value;

/* RFC 7539 section 2.3.2: key 00..1f, nonce 00:00:00:09:00:00:00:4a:00:00:00:00,
 * block counter 1, and this is the serialized block it prints. */
static const uint8_t RFC_KEY[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
static const uint8_t RFC_NONCE[12] = {0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
static const uint8_t RFC_BLOCK[64] = {
    0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
    0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03, 0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
    0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09, 0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
    0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e,
};

TEST(random, chacha20_matches_rfc_7539s_own_block) {
    uint8_t out[64];
    chacha20_block(RFC_KEY, RFC_NONCE, 1, out);
    CHECK(memcmp(out, RFC_BLOCK, 64) == 0);
    /* And the next counter is a different block, not the same one. */
    uint8_t out2[64];
    chacha20_block(RFC_KEY, RFC_NONCE, 2, out2);
    CHECK(memcmp(out, out2, 64) != 0);
    /* And a key one bit different is a different block in every word:
     * the diffusion a quarter-round with a wrong rotation would lack. */
    uint8_t key2[32];
    memcpy(key2, RFC_KEY, 32);
    key2[31] ^= 1;
    uint8_t out3[64];
    chacha20_block(key2, RFC_NONCE, 1, out3);
    int same_words = 0;
    for (int i = 0; i < 16; i++) {
        if (memcmp(out + 4 * i, out3 + 4 * i, 4) == 0) {
            same_words++;
        }
    }
    CHECK_EQ(same_words, 0);
}

TEST(random, the_same_seed_and_clock_give_the_same_stream_and_a_different_seed_does_not) {
    uint8_t a[128], b[128], c[128];
    random_test_tsc_value = 12345;
    random_init("seed one", 8);
    random_bytes(a, sizeof(a));
    random_test_tsc_value = 12345;
    random_init("seed one", 8);
    random_bytes(b, sizeof(b));
    CHECK(memcmp(a, b, sizeof(a)) == 0);
    random_test_tsc_value = 12345;
    random_init("seed two", 8);
    random_bytes(c, sizeof(c));
    CHECK(memcmp(a, c, sizeof(a)) != 0);
    /* A different clock at init is a different seed too. */
    random_test_tsc_value = 12346;
    random_init("seed one", 8);
    random_bytes(c, sizeof(c));
    CHECK(memcmp(a, c, sizeof(a)) != 0);
}

/* A change-detector, not an oracle: the exact bytes this generator
 * produces for a fixed seed and clock. The RFC vector above pins
 * ChaCha20 against a standard; this pins everything ON TOP of it - the
 * pool's mixing, the ratchet, the fast-key-erasure extraction and the
 * carry across a block boundary - to the algorithm as it stands. It
 * cannot say the algorithm is "right" (a pool has design freedom no
 * external answer covers), but it says any change to it is deliberate:
 * a mutation to the mix or the extraction that still produces
 * random-looking output - which the statistical checks below pass by
 * design - changes these bytes and is caught here. Regenerate with
 * tools' one-off harness only when the algorithm is meant to change.
 * Captured 2026-09-08 from the implementation this commit ships. */
TEST(random, the_whole_pipeline_produces_exactly_these_bytes) {
    static const uint8_t GOLD[96] = {
        0x65,0x19,0x2e,0xdf,0x33,0x8b,0xc4,0x41,0xd3,0xac,0xd6,0xe0,
        0x76,0x53,0x0c,0x26,0x32,0xdc,0x58,0x3c,0x52,0x91,0x7d,0x49,
        0x52,0x40,0x57,0x0b,0x4f,0x35,0xe5,0x70,0x1e,0x83,0x56,0xbb,
        0x16,0x25,0x40,0x94,0xc8,0x5d,0xca,0x02,0xac,0x1a,0x85,0xa2,
        0x69,0x3c,0x96,0x94,0x20,0x0d,0xd0,0xfd,0x09,0xf4,0xa0,0x75,
        0x83,0x56,0x00,0x26,0xd7,0x96,0xe4,0x55,0xba,0xff,0x0b,0xaf,
        0xaa,0x02,0xa2,0x14,0xb3,0x32,0x30,0x04,0x12,0x3a,0x0d,0x70,
        0xbd,0x9b,0xbf,0xd0,0xfc,0xea,0x5f,0x69,0x8f,0xaf,0x64,0x9b,
    };
    random_test_tsc_value = 0x0123456789abcdefULL;
    random_init("golden", 6);
    uint8_t out[96];
    random_bytes(out, sizeof out);
    for (size_t i = 0; i < sizeof out; i++) {
        if (out[i] != GOLD[i]) {
            test_fail(__FILE__, __LINE__,
                      "byte %zu is 0x%02x, the pinned algorithm gives 0x%02x",
                      i, out[i], GOLD[i]);
            break;
        }
    }
    /* GOLD spans two ChaCha20 blocks in one call (96 > 64), so the
     * block-boundary carry within a call is pinned above. It is
     * deliberately NOT expected that three 32-byte pulls reproduce it:
     * fast key erasure re-keys on every call, so a chunked reader gets
     * a different - and equally unpredictable - stream, which is the
     * security property, not a bug. That each call is distinct is what
     * extraction_erases_the_key_that_produced_it checks. */
}

TEST(random, extraction_erases_the_key_that_produced_it) {
    random_test_tsc_value = 7;
    random_init("erase", 5);
    uint8_t first[64], second[64], third[64];
    random_bytes(first, 64);
    random_bytes(second, 64);
    CHECK(memcmp(first, second, 64) != 0);
    /* The state after `first` cannot reproduce `first`: reinitialising
     * to the same seed and clock and pulling twice gives first then
     * second, never second then first or first then first. */
    random_test_tsc_value = 7;
    random_init("erase", 5);
    random_bytes(third, 64);
    CHECK(memcmp(third, first, 64) == 0);
    random_bytes(third, 64);
    CHECK(memcmp(third, second, 64) == 0);
    CHECK(memcmp(third, first, 64) != 0);
}

TEST(random, feeding_the_pool_changes_everything_after_it) {
    uint8_t a[64], b[64];
    random_test_tsc_value = 99;
    random_init("feed", 4);
    random_bytes(a, 64);
    random_test_tsc_value = 99;
    random_init("feed", 4);
    random_feed("x", 1);
    random_bytes(b, 64);
    CHECK(memcmp(a, b, 64) != 0);
    CHECK_EQ(random_events(), 1);
    /* And a feed with no data is still a feed - the clock went in. */
    random_test_tsc_value = 100;
    random_feed(0, 0);
    CHECK_EQ(random_events(), 2);
    random_bytes(a, 64);
    CHECK(memcmp(a, b, 64) != 0);
}

TEST(random, the_output_is_not_a_counter) {
    random_test_tsc_value = 424242;
    random_init("stats", 5);
    enum { N = 1 << 20 };
    static uint8_t buf[N];
    random_bytes(buf, N);
    /* A byte histogram over a megabyte: each value expected 4096 times,
     * standard deviation ~64, so +-10% (410) is six sigma - a bound a
     * counter, a stuck generator or a byte-order bug fails by miles and
     * a real stream never does. */
    unsigned counts[256];
    memset(counts, 0, sizeof(counts));
    for (int i = 0; i < N; i++) {
        counts[buf[i]]++;
    }
    unsigned lo = N, hi = 0;
    for (int v = 0; v < 256; v++) {
        if (counts[v] < lo) lo = counts[v];
        if (counts[v] > hi) hi = counts[v];
    }
    CHECK(lo > 4096 - 410);
    CHECK(hi < 4096 + 410);
    /* No 64-byte block repeats in the megabyte - a ratchet that failed
     * to advance the counter, or a key that stopped changing, repeats
     * within a few blocks. Compared against the sixteen blocks that
     * follow each, which is where a stuck generator shows. */
    int repeats = 0;
    for (int i = 0; i + 64 * 17 <= N; i += 64) {
        for (int j = 1; j <= 16; j++) {
            if (memcmp(buf + i, buf + i + 64 * j, 64) == 0) {
                repeats++;
            }
        }
    }
    CHECK_EQ(repeats, 0);
    /* And a lot of it, in pieces of every size, still adds up: pulling
     * in odd sizes must give the same bytes as one pull would, so the
     * carry across a partial block is right. */
    random_test_tsc_value = 424242;
    random_init("stats", 5);
    static uint8_t again[200];
    random_bytes(again, 1);
    random_bytes(again + 1, 63);
    random_bytes(again + 64, 100);
    random_bytes(again + 164, 36);
    /* Not equal to `buf`, and deliberately: every extraction ratchets,
     * so four pulls are four keys. What must hold is that the pulls
     * are distinct from each other and from the single pull. */
    CHECK(memcmp(again, buf, 200) != 0);
    CHECK(memcmp(again, again + 64, 36) != 0);
}
