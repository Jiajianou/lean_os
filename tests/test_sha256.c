#include "check.h"

#include "sha256.h"

#include <string.h>

static const char *hex_of(const void *data, size_t length) {
    static char out[65];
    uint8_t d[SHA256_DIGEST_BYTES];
    sha256(data, length, d);
    sha256_hex(d, out);
    return out;
}

TEST(sha256, abc) {
    CHECK(strcmp(hex_of("abc", 3),
                 "ba7816bf8f01cfea414140de5dae2223"
                 "b00361a396177a9cb410ff61f20015ad") == 0);
}

TEST(sha256, empty) {
    CHECK(strcmp(hex_of("", 0),
                 "e3b0c44298fc1c149afbf4c8996fb924"
                 "27ae41e4649b934ca495991b7852b855") == 0);
}

TEST(sha256, two_block) {
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(strcmp(hex_of(m, strlen(m)),
                 "248d6a61d20638b8e5c026930c3e6039"
                 "a33ce45964ff2167f6ecedd419db06c1") == 0);
}

TEST(sha256, block_boundaries) {
    char buffer[65];
    memset(buffer, 'a', sizeof(buffer));
    CHECK(strcmp(hex_of(buffer, 55),
                 "9f4390f8d30c2dd92ec9f095b65e2b9a"
                 "e9b0a925a5258e241c9f1e910f734318") == 0);
    CHECK(strcmp(hex_of(buffer, 56),
                 "b35439a4ac6f0948b6d6f9e3c6af0f5f"
                 "590ce20f1bde7090ef7970686ec6738a") == 0);
    CHECK(strcmp(hex_of(buffer, 63),
                 "7d3e74a05d7db15bce4ad9ec0658ea98"
                 "e3f06eeecf16b4c6fff2da457ddc2f34") == 0);
    CHECK(strcmp(hex_of(buffer, 64),
                 "ffe054fe7ae0cb6dc65c3af9b61d5209"
                 "f439851db43d0ba5997337df154668eb") == 0);
    CHECK(strcmp(hex_of(buffer, 65),
                 "635361c48bb9eab14198e76ea8ab7f1a"
                 "41685d6ad62aa9146d301d4f17eb0ae0") == 0);
}

TEST(sha256, streaming_matches_one_shot) {
    unsigned char message[200];
    for (int i = 0; i < 200; i++) {
        message[i] = (unsigned char)(i * 7 + 3);
    }
    uint8_t once[SHA256_DIGEST_BYTES];
    sha256(message, sizeof(message), once);

    for (size_t split = 0; split <= sizeof(message); split++) {
        sha256_t s;
        sha256_init(&s);
        sha256_update(&s, message, split);
        sha256_update(&s, message + split, sizeof(message) - split);
        uint8_t twice[SHA256_DIGEST_BYTES];
        sha256_final(&s, twice);
        if (!sha256_equal(once, twice)) {
            test_fail(__FILE__, __LINE__, "split at %zu disagrees", split);
            return;
        }
    }
}

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

TEST(sha256, every_byte_reaches_the_digest) {
    unsigned char message[130];
    memset(message, 0, sizeof(message));
    uint8_t base[SHA256_DIGEST_BYTES];
    sha256(message, sizeof(message), base);
    for (size_t i = 0; i < sizeof(message); i++) {
        message[i] = 1;
        uint8_t d[SHA256_DIGEST_BYTES];
        sha256(message, sizeof(message), d);
        message[i] = 0;
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
    char upper[65];
    for (int i = 0; i < 64; i++) {
        upper[i] = (hex[i] >= 'a' && hex[i] <= 'f') ? (char)(hex[i] - 32) : hex[i];
    }
    upper[64] = '\0';
    CHECK_EQ(sha256_unhex(upper, back), 0);
    CHECK(sha256_equal(d, back));
}

TEST(sha256, unhex_refuses_what_is_not_a_digest) {
    uint8_t d[SHA256_DIGEST_BYTES];
    CHECK_EQ(sha256_unhex("ba7816bf8f01cfea414140de5dae2223"
                          "b00361a396177a9cb410ff61f20015a", d), -1);
    CHECK_EQ(sha256_unhex("ba7816bf8f01cfea414140de5dae2223"
                          "b00361a396177a9cb410ff61f20015adx", d), -1);
    CHECK_EQ(sha256_unhex("ba7816bf8f01cfea414140de5dae2223"
                          "b00361a396177a9cbz10ff61f20015ad", d), -1);
    CHECK_EQ(sha256_unhex("", d), -1);
}

TEST(sha256, equal_is_equality) {
    uint8_t a[SHA256_DIGEST_BYTES], b[SHA256_DIGEST_BYTES];
    sha256("abc", 3, a);
    sha256("abc", 3, b);
    CHECK_EQ(sha256_equal(a, b), 1);
    b[SHA256_DIGEST_BYTES - 1] ^= 1;
    CHECK_EQ(sha256_equal(a, b), 0);
    sha256("abc", 3, b);
    b[0] ^= 0x80;
    CHECK_EQ(sha256_equal(a, b), 0);
}
