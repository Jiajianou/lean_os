#include "sha256.h"

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(sha256_t *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sha256_init(sha256_t *s) {
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u;
    s->h[2] = 0x3c6ef372u; s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;
    s->bits = 0;
    s->buffered = 0;
}

void sha256_update(sha256_t *s, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    s->bits += (uint64_t)len * 8u;

    if (s->buffered) {
        size_t want = SHA256_BLOCK_BYTES - s->buffered;
        size_t take = len < want ? len : want;
        for (size_t i = 0; i < take; i++) {
            s->buf[s->buffered + i] = p[i];
        }
        s->buffered += take;
        p += take;
        len -= take;
        if (s->buffered < SHA256_BLOCK_BYTES) {
            return;
        }
        sha256_block(s, s->buf);
        s->buffered = 0;
    }

    while (len >= SHA256_BLOCK_BYTES) {
        sha256_block(s, p);
        p += SHA256_BLOCK_BYTES;
        len -= SHA256_BLOCK_BYTES;
    }

    for (size_t i = 0; i < len; i++) {
        s->buf[i] = p[i];
    }
    s->buffered = len;
}

void sha256_final(sha256_t *s, uint8_t out[SHA256_DIGEST_BYTES]) {
    uint64_t bits = s->bits;

    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    s->bits = bits;

    uint8_t zero = 0;
    while (s->buffered != 56) {
        sha256_update(s, &zero, 1);
        s->bits = bits;
    }

    uint8_t tail[8];
    for (int i = 0; i < 8; i++) {
        tail[i] = (uint8_t)(bits >> (56 - i * 8));
    }
    sha256_update(s, tail, 8);

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_BYTES]) {
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final(&s, out);
}

void sha256_hex(const uint8_t digest[SHA256_DIGEST_BYTES], char *out) {
    static const char hexdigits[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_DIGEST_BYTES; i++) {
        out[i * 2]     = hexdigits[digest[i] >> 4];
        out[i * 2 + 1] = hexdigits[digest[i] & 0xF];
    }
    out[SHA256_DIGEST_BYTES * 2] = '\0';
}

int sha256_unhex(const char *hex, uint8_t out[SHA256_DIGEST_BYTES]) {
    for (int i = 0; i < SHA256_DIGEST_BYTES * 2; i++) {
        int c = (unsigned char)hex[i];
        int v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            return -1;
        }
        if (i & 1) {
            out[i / 2] = (uint8_t)(out[i / 2] | v);
        } else {
            out[i / 2] = (uint8_t)(v << 4);
        }
    }
    return hex[SHA256_DIGEST_BYTES * 2] == '\0' ? 0 : -1;
}

int sha256_equal(const uint8_t a[SHA256_DIGEST_BYTES],
                 const uint8_t b[SHA256_DIGEST_BYTES]) {
    uint8_t diff = 0;
    for (int i = 0; i < SHA256_DIGEST_BYTES; i++) {
        diff = (uint8_t)(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}
