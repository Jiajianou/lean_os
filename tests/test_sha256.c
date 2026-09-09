/* tests/test_sha256.c - M111
 *
 * The hash the package manager's every claim rests on, graded against
 * the vectors in the standard rather than against itself.
 *
 * That distinction is the whole reason this file is worth its length. A
 * hash function is the easiest thing in this tree to write a *vacuous*
 * test for: hash something, record what came out, assert it comes out
 * again. That test passes for a SHA-256 with the padding length in bytes
 * instead of bits, for one with a transposed constant, and for one that
 * is simply a different function - it only ever proves the code is
 * deterministic. So every expected value below comes from FIPS 180-4 or
 * from the NIST example set, and none of them was produced by running
 * this implementation.
 *
 * tools/pkg-test.sh does the other half, which no fixture can do: it
 * hashes several hundred real files from this tree with this code and
 * with the host's own `shasum -a 256`, and requires every pair to agree.
 */
#include "check.h"

#include "sha256.h"

#include <string.h>

static const char *hex_of(const void *data, size_t len) {
    static char out[65];
    uint8_t d[SHA256_DIGEST_BYTES];
    sha256(data, len, d);
    sha256_hex(d, out);
    return out;
}

/* FIPS 180-4 appendix B.1: the one-block message "abc". */
TEST(sha256, abc) {
    CHECK(strcmp(hex_of("abc", 3),
                 "ba7816bf8f01cfea414140de5dae2223"
                 "b00361a396177a9cb410ff61f20015ad") == 0);
}

/* The empty message. Not in the appendix, and the case an implementation
 * with an off-by-one in its padding loop gets wrong while getting "abc"
 * right - the length is zero and the padding block is the entire
 * message. */
TEST(sha256, empty) {
    CHECK(strcmp(hex_of("", 0),
                 "e3b0c44298fc1c149afbf4c8996fb924"
                 "27ae41e4649b934ca495991b7852b855") == 0);
}

/* FIPS 180-4 appendix B.2: 448 bits, so the length does not fit in the
 * last block and a second one is needed for the padding alone. This is
 * the case a naive final() gets wrong. */
TEST(sha256, two_block) {
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(strcmp(hex_of(m, strlen(m)),
                 "248d6a61d20638b8e5c026930c3e6039"
                 "a33ce45964ff2167f6ecedd419db06c1") == 0);
}

/* Exactly 55, 56, 63, 64 and 65 bytes: the boundaries either side of
 * "the length still fits in this block" and "this block is full". Every
 * padding bug this file could have lives at one of these five lengths,
 * and the expected values are the host's - see the note at the top about
 * where they are NOT from. */
TEST(sha256, block_boundaries) {
    char buf[65];
    memset(buf, 'a', sizeof(buf));
    /* 55: the last length that fits with its 8-byte count in one block. */
    CHECK(strcmp(hex_of(buf, 55),
                 "9f4390f8d30c2dd92ec9f095b65e2b9a"
                 "e9b0a925a5258e241c9f1e910f734318") == 0);
    /* 56: one byte too many, so the count spills into a second block. */
    CHECK(strcmp(hex_of(buf, 56),
                 "b35439a4ac6f0948b6d6f9e3c6af0f5f"
                 "590ce20f1bde7090ef7970686ec6738a") == 0);
    CHECK(strcmp(hex_of(buf, 63),
                 "7d3e74a05d7db15bce4ad9ec0658ea98"
                 "e3f06eeecf16b4c6fff2da457ddc2f34") == 0);
    /* 64: a full block, and the padding is a whole second one. */
    CHECK(strcmp(hex_of(buf, 64),
                 "ffe054fe7ae0cb6dc65c3af9b61d5209"
                 "f439851db43d0ba5997337df154668eb") == 0);
    CHECK(strcmp(hex_of(buf, 65),
                 "635361c48bb9eab14198e76ea8ab7f1a"
                 "41685d6ad62aa9146d301d4f17eb0ae0") == 0);
}

/* The streaming interface has to agree with the one-shot one whatever
 * the chunk sizes are - which is the property the package reader
 * depends on and the one an implementation that resets its buffer at
 * the wrong moment breaks. Every split of a 200-byte message. */
TEST(sha256, streaming_matches_one_shot) {
    unsigned char msg[200];
    for (int i = 0; i < 200; i++) {
        msg[i] = (unsigned char)(i * 7 + 3);
    }
    uint8_t once[SHA256_DIGEST_BYTES];
    sha256(msg, sizeof(msg), once);

    for (size_t split = 0; split <= sizeof(msg); split++) {
        sha256_t s;
        sha256_init(&s);
        sha256_update(&s, msg, split);
        sha256_update(&s, msg + split, sizeof(msg) - split);
        uint8_t twice[SHA256_DIGEST_BYTES];
        sha256_final(&s, twice);
        if (!sha256_equal(once, twice)) {
            test_fail(__FILE__, __LINE__, "split at %zu disagrees", split);
            return;
        }
    }
}

/* One byte at a time, over a message long enough to cross several
 * blocks. The slowest possible use of the streaming interface and the
 * one that exercises the buffered path on every single call. */
TEST(sha256, byte_at_a_time) {
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_t s;
    sha256_init(&s);
    for (const char *p = m; *p; p++) {
        sha256_update(&s, p, 1);
    }
    uint8_t d[SHA256_DIGEST_BYTES];
    sha256_final(&s, d);
    char hex[65];
    sha256_hex(d, hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039"
                      "a33ce45964ff2167f6ecedd419db06c1") == 0);
}

/* A one-bit change anywhere must change the digest. Not a proof of
 * anything cryptographic - it is a check that the input is actually
 * being read, which is the failure mode of a hash whose update() has a
 * length bug: it agrees with itself on inputs it never looked at. */
TEST(sha256, every_byte_reaches_the_digest) {
    unsigned char msg[130];
    memset(msg, 0, sizeof(msg));
    uint8_t base[SHA256_DIGEST_BYTES];
    sha256(msg, sizeof(msg), base);
    for (size_t i = 0; i < sizeof(msg); i++) {
        msg[i] = 1;
        uint8_t d[SHA256_DIGEST_BYTES];
        sha256(msg, sizeof(msg), d);
        msg[i] = 0;
        if (sha256_equal(base, d)) {
            test_fail(__FILE__, __LINE__, "byte %zu does not reach the digest", i);
            return;
        }
    }
}

TEST(sha256, hex_round_trip) {
    uint8_t d[SHA256_DIGEST_BYTES];
    sha256("abc", 3, d);
    char hex[65];
    sha256_hex(d, hex);
    uint8_t back[SHA256_DIGEST_BYTES];
    CHECK_EQ(sha256_unhex(hex, back), 0);
    CHECK(sha256_equal(d, back));
    /* Uppercase is accepted - a digest copied out of another tool's
     * output should not fail to parse over a case difference. */
    char upper[65];
    for (int i = 0; i < 64; i++) {
        upper[i] = (hex[i] >= 'a' && hex[i] <= 'f') ? (char)(hex[i] - 32) : hex[i];
    }
    upper[64] = '\0';
    CHECK_EQ(sha256_unhex(upper, back), 0);
    CHECK(sha256_equal(d, back));
}

/* The refusals. Each of these would, if accepted, mean a package
 * verifying against a digest that is not one. */
TEST(sha256, unhex_refuses_what_is_not_a_digest) {
    uint8_t d[SHA256_DIGEST_BYTES];
    /* 63 digits. */
    CHECK_EQ(sha256_unhex("ba7816bf8f01cfea414140de5dae2223"
                          "b00361a396177a9cb410ff61f20015a", d), -1);
    /* 65 - the trailing character is the one an implementation that
     * stops after 64 would never look at. */
    CHECK_EQ(sha256_unhex("ba7816bf8f01cfea414140de5dae2223"
                          "b00361a396177a9cb410ff61f20015adx", d), -1);
    /* A non-hex digit in the middle. */
    CHECK_EQ(sha256_unhex("ba7816bf8f01cfea414140de5dae2223"
                          "b00361a396177a9cbz10ff61f20015ad", d), -1);
    CHECK_EQ(sha256_unhex("", d), -1);
}

TEST(sha256, equal_is_equality) {
    uint8_t a[SHA256_DIGEST_BYTES], b[SHA256_DIGEST_BYTES];
    sha256("abc", 3, a);
    sha256("abc", 3, b);
    CHECK_EQ(sha256_equal(a, b), 1);
    /* Differing in the LAST byte, which is what an early-exit comparison
     * written the other way round would get wrong. */
    b[SHA256_DIGEST_BYTES - 1] ^= 1;
    CHECK_EQ(sha256_equal(a, b), 0);
    sha256("abc", 3, b);
    b[0] ^= 0x80;
    CHECK_EQ(sha256_equal(a, b), 0);
}
