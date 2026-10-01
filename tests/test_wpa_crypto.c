#include "check.h"

#include <stdlib.h>

#include "network/wpa_crypto.h"

/* Every expected value here was published by somebody else: FIPS 180 and
   RFC 2202 for SHA-1 and HMAC, RFC 6070 and IEEE 802.11 Annex J for PBKDF2
   and the PRF, FIPS-197 for AES, RFC 3394 for key wrap and RFC 4493 for
   CMAC. The two PRF and PBKDF2 values not in a standard came from Python's
   hmac and hashlib. */

static void from_hex(const char *hex, uint8_t *out) {
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned value;
        sscanf(hex + i * 2, "%2x", &value);
        out[i] = (uint8_t)value;
    }
}

static int equals_hex(const uint8_t *bytes, const char *hex) {
    uint8_t expected[128];
    from_hex(hex, expected);
    return memcmp(bytes, expected, strlen(hex) / 2) == 0;
}

TEST(wpa_crypto, sha1_of_the_fips_examples) {
    uint8_t d[20];
    sha1("abc", 3, d);
    CHECK(equals_hex(d, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    sha1("", 0, d);
    CHECK(equals_hex(d, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1(two_blocks, strlen(two_blocks), d);
    CHECK(equals_hex(d, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    uint8_t *million = malloc(1000000);
    memset(million, 'a', 1000000);
    sha1_t c;
    sha1_begin(&c);
    for (int i = 0; i < 1000000; i += 999) {
        sha1_add(&c, million + i, (size_t)(1000000 - i < 999 ? 1000000 - i : 999));
    }
    sha1_end(&c, d);
    free(million);
    CHECK(equals_hex(d, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

TEST(wpa_crypto, hmac_sha1_rfc_2202) {
    uint8_t d[20];
    uint8_t key[80];
    memset(key, 0x0B, 20);
    hmac_sha1(key, 20, "Hi There", 8, d);
    CHECK(equals_hex(d, "b617318655057264e28bc0b6fb378c8ef146be00"));
    hmac_sha1((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, d);
    CHECK(equals_hex(d, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
    memset(key, 0xAA, 80);
    hmac_sha1(key, 80, "Test Using Larger Than Block-Size Key - Hash Key First", 54, d);
    CHECK(equals_hex(d, "aa4ae5e15272d00e95705637ce8a3b55ed402112"));
}

TEST(wpa_crypto, pbkdf2_rfc_6070_and_ieee_802_11) {
    uint8_t out[32];
    pbkdf2_sha1((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, out, 20);
    CHECK(equals_hex(out, "0c60c80f961f0e71f3a9b524af6012062fe037a6"));
    pbkdf2_sha1((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 2, out, 20);
    CHECK(equals_hex(out, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"));
    pbkdf2_sha1((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096, out, 20);
    CHECK(equals_hex(out, "4b007901b765489abead49d926f721d065a429c1"));
    uint8_t pmk[32];
    REQUIRE(wpa_passphrase_to_pmk("password", (const uint8_t *)"IEEE", 4, pmk));
    CHECK(equals_hex(pmk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
    REQUIRE(wpa_passphrase_to_pmk("ThisIsAPassword", (const uint8_t *)"ThisIsASSID", 11, pmk));
    CHECK(equals_hex(pmk, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"));
}

TEST(wpa_crypto, a_passphrase_must_be_8_to_63_printable_characters_or_64_hex_digits) {
    uint8_t pmk[32];
    CHECK(!wpa_passphrase_to_pmk("short", (const uint8_t *)"x", 1, pmk));
    char long_one[65];
    memset(long_one, 'a', 64);
    long_one[64] = 0;
    long_one[63] = 'z';
    CHECK(!wpa_passphrase_to_pmk(long_one, (const uint8_t *)"x", 1, pmk));
    CHECK(wpa_passphrase_to_pmk("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e",
                                (const uint8_t *)"x", 1, pmk));
    CHECK(equals_hex(pmk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
    CHECK(!wpa_passphrase_to_pmk("pass\x01word", (const uint8_t *)"x", 1, pmk));
    CHECK(wpa_passphrase_to_pmk("12345678", (const uint8_t *)"x", 1, pmk));
}

TEST(wpa_crypto, the_802_11_prf) {
    uint8_t key[20];
    memset(key, 0x0B, 20);
    uint8_t out[64];
    wpa_prf(key, 20, "prefix", (const uint8_t *)"Hi There", 8, out, 64);
    CHECK(equals_hex(out, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
                          "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"));
    wpa_prf((const uint8_t *)"Jefe", 4, "prefix-2", (const uint8_t *)"what do ya want for nothing?", 28, out, 48);
    CHECK(equals_hex(out, "47c4908e30c947521ad20be9053450ecbea23d3aa604b77326d8b3825ff7475c"
                          "06f51fb9c5313d1e9f90d897d134b72e"));
}

TEST(wpa_crypto, aes_128_fips_197_both_ways) {
    uint8_t key[16], plain[16], out[16], back[16];
    from_hex("000102030405060708090a0b0c0d0e0f", key);
    from_hex("00112233445566778899aabbccddeeff", plain);
    aes128_t c;
    aes128_set_key(&c, key);
    aes128_encrypt(&c, plain, out);
    CHECK(equals_hex(out, "69c4e0d86a7b0430d8cdb78070b4c55a"));
    aes128_decrypt(&c, out, back);
    CHECK_EQ(memcmp(back, plain, 16), 0);
    from_hex("2b7e151628aed2a6abf7158809cf4f3c", key);
    from_hex("3243f6a8885a308d313198a2e0370734", plain);
    aes128_set_key(&c, key);
    aes128_encrypt(&c, plain, out);
    CHECK(equals_hex(out, "3925841d02dc09fbdc118597196a0b32"));
}

TEST(wpa_crypto, key_wrap_rfc_3394) {
    uint8_t kek[16], plain[16], wrapped[24], back[16];
    from_hex("000102030405060708090a0b0c0d0e0f", kek);
    from_hex("00112233445566778899aabbccddeeff", plain);
    REQUIRE(aes_key_wrap(kek, plain, 16, wrapped));
    CHECK(equals_hex(wrapped, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"));
    CHECK(aes_key_unwrap(kek, wrapped, 24, back));
    CHECK_EQ(memcmp(back, plain, 16), 0);
    wrapped[5] ^= 1;
    CHECK(!aes_key_unwrap(kek, wrapped, 24, back));
    CHECK(!aes_key_unwrap(kek, wrapped, 20, back));
    uint8_t long_plain[32], long_wrapped[40], long_back[32];
    for (int i = 0; i < 32; i++) {
        long_plain[i] = (uint8_t)(i * 7 + 1);
    }
    REQUIRE(aes_key_wrap(kek, long_plain, 32, long_wrapped));
    CHECK(aes_key_unwrap(kek, long_wrapped, 40, long_back));
    CHECK_EQ(memcmp(long_back, long_plain, 32), 0);
}

TEST(wpa_crypto, cmac_rfc_4493) {
    uint8_t key[16], message[64], mac[16];
    from_hex("2b7e151628aed2a6abf7158809cf4f3c", key);
    from_hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
             "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
             message);
    aes_cmac(key, message, 0, mac);
    CHECK(equals_hex(mac, "bb1d6929e95937287fa37d129b756746"));
    aes_cmac(key, message, 16, mac);
    CHECK(equals_hex(mac, "070a16b46b4d4144f79bdd9dd04a287c"));
    aes_cmac(key, message, 40, mac);
    CHECK(equals_hex(mac, "dfa66747de9ae63030ca32611497c827"));
    aes_cmac(key, message, 64, mac);
    CHECK(equals_hex(mac, "51f0bebf7e3b9d92fc49741779363cfe"));
}
