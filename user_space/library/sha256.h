#pragma once

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_BYTES 32
#define SHA256_BLOCK_BYTES  64

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t  buffer[SHA256_BLOCK_BYTES];
    size_t   buffered;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t length);
void sha256_final(sha256_t *s, uint8_t out[SHA256_DIGEST_BYTES]);

void sha256(const void *data, size_t length, uint8_t out[SHA256_DIGEST_BYTES]);

void sha256_hex(const uint8_t digest[SHA256_DIGEST_BYTES], char *out);

int sha256_unhex(const char *hex, uint8_t out[SHA256_DIGEST_BYTES]);

int sha256_equal(const uint8_t a[SHA256_DIGEST_BYTES],
                 const uint8_t b[SHA256_DIGEST_BYTES]);
