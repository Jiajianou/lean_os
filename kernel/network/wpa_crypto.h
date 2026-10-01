#pragma once

#include <stddef.h>
#include <stdint.h>

/* The cryptography WPA2-Personal is made of, and nothing more: SHA-1 and
   HMAC-SHA1 (the key hierarchy and the EAPOL-Key MIC), PBKDF2 (a passphrase
   into the pairwise master key), the 802.11 pseudo-random function, AES-128
   (key unwrap for the group key, and CMAC for the descriptor version that
   uses it). Encrypting frames is the radio's job; this is the handshake. */

#define SHA1_DIGEST_LENGTH 20
#define SHA1_BLOCK_LENGTH  64
#define AES_BLOCK_LENGTH   16

typedef struct {
    uint32_t state[5];
    uint64_t length;
    uint8_t block[SHA1_BLOCK_LENGTH];
    uint32_t used;
} sha1_t;

void sha1_begin(sha1_t *context);
void sha1_add(sha1_t *context, const void *data, size_t length);
void sha1_end(sha1_t *context, uint8_t digest[SHA1_DIGEST_LENGTH]);
void sha1(const void *data, size_t length, uint8_t digest[SHA1_DIGEST_LENGTH]);

void hmac_sha1(const uint8_t *key, size_t key_length, const void *data, size_t length,
               uint8_t digest[SHA1_DIGEST_LENGTH]);

/* Several pieces of data hashed as one message, which is how the PRF and
   the MIC are both specified. */
void hmac_sha1_parts(const uint8_t *key, size_t key_length, const void *const *parts, const size_t *lengths,
                     size_t count, uint8_t digest[SHA1_DIGEST_LENGTH]);

void pbkdf2_sha1(const uint8_t *password, size_t password_length, const uint8_t *salt, size_t salt_length,
                 uint32_t iterations, uint8_t *out, size_t out_length);

/* IEEE 802.11's PRF-n: HMAC-SHA1(key, label || 0 || data || i) for i = 0,
   1, ..., concatenated and cut to out_length bytes. */
void wpa_prf(const uint8_t *key, size_t key_length, const char *label, const uint8_t *data, size_t data_length,
             uint8_t *out, size_t out_length);

/* The pairwise master key for a passphrase of 8 to 63 characters, or a
   64-digit hexadecimal key given directly. 0 when it is neither. */
int wpa_passphrase_to_pmk(const char *passphrase, const uint8_t *ssid, size_t ssid_length, uint8_t pmk[32]);

typedef struct {
    uint32_t round_keys[44];
} aes128_t;

void aes128_set_key(aes128_t *context, const uint8_t key[16]);
void aes128_encrypt(const aes128_t *context, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const aes128_t *context, const uint8_t in[16], uint8_t out[16]);

/* RFC 3394. `wrapped_length` is a multiple of 8 and at least 24; the plain
   key is 8 bytes shorter. 0 when the integrity check fails. */
int aes_key_unwrap(const uint8_t kek[16], const uint8_t *wrapped, size_t wrapped_length, uint8_t *out);
int aes_key_wrap(const uint8_t kek[16], const uint8_t *plain, size_t plain_length, uint8_t *out);

/* RFC 4493. */
void aes_cmac(const uint8_t key[16], const void *data, size_t length, uint8_t mac[16]);

/* Comparison whose time does not depend on where two buffers first differ. */
int wpa_equal(const uint8_t *a, const uint8_t *b, size_t length);
