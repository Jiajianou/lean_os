#include "wpa_crypto.h"

static uint32_t rotate_left(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

static void copy(void *to, const void *from, size_t length) {
    uint8_t *t = (uint8_t *)to;
    const uint8_t *f = (const uint8_t *)from;
    for (size_t i = 0; i < length; i++) {
        t[i] = f[i];
    }
}

static void wipe(void *pointer, size_t length) {
    volatile uint8_t *bytes = (volatile uint8_t *)pointer;
    for (size_t i = 0; i < length; i++) {
        bytes[i] = 0;
    }
}

static void sha1_block(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) | ((uint32_t)block[i * 4 + 2] << 8) |
               block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rotate_left(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t t = rotate_left(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = t;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1_begin(sha1_t *context) {
    context->state[0] = 0x67452301u;
    context->state[1] = 0xEFCDAB89u;
    context->state[2] = 0x98BADCFEu;
    context->state[3] = 0x10325476u;
    context->state[4] = 0xC3D2E1F0u;
    context->length = 0;
    context->used = 0;
}

void sha1_add(sha1_t *context, const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    context->length += length;
    while (length > 0) {
        uint32_t take = SHA1_BLOCK_LENGTH - context->used;
        if (take > length) {
            take = (uint32_t)length;
        }
        copy(context->block + context->used, bytes, take);
        context->used += take;
        bytes += take;
        length -= take;
        if (context->used == SHA1_BLOCK_LENGTH) {
            sha1_block(context->state, context->block);
            context->used = 0;
        }
    }
}

void sha1_end(sha1_t *context, uint8_t digest[SHA1_DIGEST_LENGTH]) {
    uint64_t bits = context->length * 8;
    uint8_t pad = 0x80;
    sha1_add(context, &pad, 1);
    uint8_t zero = 0;
    while (context->used != 56) {
        sha1_add(context, &zero, 1);
    }
    uint8_t length[8];
    for (int i = 0; i < 8; i++) {
        length[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha1_add(context, length, 8);
    for (int i = 0; i < 5; i++) {
        digest[i * 4] = (uint8_t)(context->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(context->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(context->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)context->state[i];
    }
    wipe(context, sizeof(*context));
}

void sha1(const void *data, size_t length, uint8_t digest[SHA1_DIGEST_LENGTH]) {
    sha1_t context;
    sha1_begin(&context);
    sha1_add(&context, data, length);
    sha1_end(&context, digest);
}

void hmac_sha1_parts(const uint8_t *key, size_t key_length, const void *const *parts, const size_t *lengths,
                     size_t count, uint8_t digest[SHA1_DIGEST_LENGTH]) {
    uint8_t block_key[SHA1_BLOCK_LENGTH];
    wipe(block_key, sizeof(block_key));
    if (key_length > SHA1_BLOCK_LENGTH) {
        sha1(key, key_length, block_key);
    } else {
        copy(block_key, key, key_length);
    }
    uint8_t pad[SHA1_BLOCK_LENGTH];
    sha1_t context;

    for (int i = 0; i < SHA1_BLOCK_LENGTH; i++) {
        pad[i] = block_key[i] ^ 0x36;
    }
    sha1_begin(&context);
    sha1_add(&context, pad, sizeof(pad));
    for (size_t i = 0; i < count; i++) {
        sha1_add(&context, parts[i], lengths[i]);
    }
    uint8_t inner[SHA1_DIGEST_LENGTH];
    sha1_end(&context, inner);

    for (int i = 0; i < SHA1_BLOCK_LENGTH; i++) {
        pad[i] = block_key[i] ^ 0x5C;
    }
    sha1_begin(&context);
    sha1_add(&context, pad, sizeof(pad));
    sha1_add(&context, inner, sizeof(inner));
    sha1_end(&context, digest);
    wipe(block_key, sizeof(block_key));
    wipe(pad, sizeof(pad));
    wipe(inner, sizeof(inner));
}

void hmac_sha1(const uint8_t *key, size_t key_length, const void *data, size_t length,
               uint8_t digest[SHA1_DIGEST_LENGTH]) {
    const void *parts[1] = {data};
    size_t lengths[1] = {length};
    hmac_sha1_parts(key, key_length, parts, lengths, 1, digest);
}

void pbkdf2_sha1(const uint8_t *password, size_t password_length, const uint8_t *salt, size_t salt_length,
                 uint32_t iterations, uint8_t *out, size_t out_length) {
    uint32_t block_index = 1;
    while (out_length > 0) {
        uint8_t counter[4] = {(uint8_t)(block_index >> 24), (uint8_t)(block_index >> 16), (uint8_t)(block_index >> 8),
                              (uint8_t)block_index};
        const void *parts[2] = {salt, counter};
        size_t lengths[2] = {salt_length, 4};
        uint8_t u[SHA1_DIGEST_LENGTH];
        uint8_t t[SHA1_DIGEST_LENGTH];
        hmac_sha1_parts(password, password_length, parts, lengths, 2, u);
        copy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_sha1(password, password_length, u, sizeof(u), u);
            for (int b = 0; b < SHA1_DIGEST_LENGTH; b++) {
                t[b] ^= u[b];
            }
        }
        size_t take = out_length < SHA1_DIGEST_LENGTH ? out_length : SHA1_DIGEST_LENGTH;
        copy(out, t, take);
        out += take;
        out_length -= take;
        block_index++;
        wipe(u, sizeof(u));
        wipe(t, sizeof(t));
    }
}

void wpa_prf(const uint8_t *key, size_t key_length, const char *label, const uint8_t *data, size_t data_length,
             uint8_t *out, size_t out_length) {
    size_t label_length = 0;
    while (label[label_length]) {
        label_length++;
    }
    uint8_t zero = 0;
    uint8_t counter = 0;
    while (out_length > 0) {
        const void *parts[4] = {label, &zero, data, &counter};
        size_t lengths[4] = {label_length, 1, data_length, 1};
        uint8_t digest[SHA1_DIGEST_LENGTH];
        hmac_sha1_parts(key, key_length, parts, lengths, 4, digest);
        size_t take = out_length < SHA1_DIGEST_LENGTH ? out_length : SHA1_DIGEST_LENGTH;
        copy(out, digest, take);
        out += take;
        out_length -= take;
        counter++;
        wipe(digest, sizeof(digest));
    }
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int wpa_passphrase_to_pmk(const char *passphrase, const uint8_t *ssid, size_t ssid_length, uint8_t pmk[32]) {
    size_t length = 0;
    while (passphrase[length]) {
        length++;
    }
    if (length == 64) {
        for (int i = 0; i < 32; i++) {
            int high = hex_digit(passphrase[i * 2]);
            int low = hex_digit(passphrase[i * 2 + 1]);
            if (high < 0 || low < 0) {
                return 0;
            }
            pmk[i] = (uint8_t)(high << 4 | low);
        }
        return 1;
    }
    if (length < 8 || length > 63 || ssid_length > 32) {
        return 0;
    }
    for (size_t i = 0; i < length; i++) {
        if ((uint8_t)passphrase[i] < 32 || (uint8_t)passphrase[i] > 126) {
            return 0;
        }
    }
    pbkdf2_sha1((const uint8_t *)passphrase, length, ssid, ssid_length, 4096, pmk, 32);
    return 1;
}

static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static uint8_t inverse_sbox[256];
static int inverse_ready;

static void build_inverse(void) {
    if (inverse_ready) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        inverse_sbox[sbox[i]] = (uint8_t)i;
    }
    inverse_ready = 1;
}

static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

static uint8_t multiply(uint8_t a, uint8_t b) {
    uint8_t result = 0;
    while (b) {
        if (b & 1) {
            result ^= a;
        }
        a = xtime(a);
        b >>= 1;
    }
    return result;
}

void aes128_set_key(aes128_t *context, const uint8_t key[16]) {
    build_inverse();
    uint8_t round_constant = 1;
    for (int i = 0; i < 4; i++) {
        context->round_keys[i] = ((uint32_t)key[i * 4] << 24) | ((uint32_t)key[i * 4 + 1] << 16) |
                                 ((uint32_t)key[i * 4 + 2] << 8) | key[i * 4 + 3];
    }
    for (int i = 4; i < 44; i++) {
        uint32_t t = context->round_keys[i - 1];
        if (i % 4 == 0) {
            t = (t << 8) | (t >> 24);
            t = ((uint32_t)sbox[t >> 24] << 24) | ((uint32_t)sbox[(t >> 16) & 0xFF] << 16) |
                ((uint32_t)sbox[(t >> 8) & 0xFF] << 8) | sbox[t & 0xFF];
            t ^= (uint32_t)round_constant << 24;
            round_constant = xtime(round_constant);
        }
        context->round_keys[i] = context->round_keys[i - 4] ^ t;
    }
}

static void add_round_key(uint8_t state[16], const uint32_t *round_key) {
    for (int c = 0; c < 4; c++) {
        state[c * 4] ^= (uint8_t)(round_key[c] >> 24);
        state[c * 4 + 1] ^= (uint8_t)(round_key[c] >> 16);
        state[c * 4 + 2] ^= (uint8_t)(round_key[c] >> 8);
        state[c * 4 + 3] ^= (uint8_t)round_key[c];
    }
}

void aes128_encrypt(const aes128_t *context, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    copy(s, in, 16);
    add_round_key(s, context->round_keys);
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) {
            s[i] = sbox[s[i]];
        }
        uint8_t t[16];
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
            }
        }
        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3];
                s[c * 4] = (uint8_t)(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
                s[c * 4 + 1] = (uint8_t)(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
                s[c * 4 + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
                s[c * 4 + 3] = (uint8_t)(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
            }
        } else {
            copy(s, t, 16);
        }
        add_round_key(s, context->round_keys + round * 4);
    }
    copy(out, s, 16);
}

void aes128_decrypt(const aes128_t *context, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    copy(s, in, 16);
    add_round_key(s, context->round_keys + 40);
    for (int round = 9; round >= 0; round--) {
        uint8_t t[16];
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                t[((c + r) % 4) * 4 + r] = s[c * 4 + r];
            }
        }
        for (int i = 0; i < 16; i++) {
            s[i] = inverse_sbox[t[i]];
        }
        add_round_key(s, context->round_keys + round * 4);
        if (round != 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = s[c * 4], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
                s[c * 4] = (uint8_t)(multiply(a0, 14) ^ multiply(a1, 11) ^ multiply(a2, 13) ^ multiply(a3, 9));
                s[c * 4 + 1] = (uint8_t)(multiply(a0, 9) ^ multiply(a1, 14) ^ multiply(a2, 11) ^ multiply(a3, 13));
                s[c * 4 + 2] = (uint8_t)(multiply(a0, 13) ^ multiply(a1, 9) ^ multiply(a2, 14) ^ multiply(a3, 11));
                s[c * 4 + 3] = (uint8_t)(multiply(a0, 11) ^ multiply(a1, 13) ^ multiply(a2, 9) ^ multiply(a3, 14));
            }
        }
    }
    copy(out, s, 16);
}

static const uint8_t wrap_iv[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};

int aes_key_unwrap(const uint8_t kek[16], const uint8_t *wrapped, size_t wrapped_length, uint8_t *out) {
    if (wrapped_length < 24 || wrapped_length % 8 != 0) {
        return 0;
    }
    size_t n = wrapped_length / 8 - 1;
    aes128_t context;
    aes128_set_key(&context, kek);
    uint8_t a[8];
    copy(a, wrapped, 8);
    copy(out, wrapped + 8, n * 8);
    for (int j = 5; j >= 0; j--) {
        for (size_t i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * (uint64_t)j + i;
            uint8_t block[16];
            copy(block, a, 8);
            for (int b = 0; b < 8; b++) {
                block[7 - b] ^= (uint8_t)(t >> (8 * b));
            }
            copy(block + 8, out + (i - 1) * 8, 8);
            aes128_decrypt(&context, block, block);
            copy(a, block, 8);
            copy(out + (i - 1) * 8, block + 8, 8);
            wipe(block, sizeof(block));
        }
    }
    wipe(&context, sizeof(context));
    return wpa_equal(a, wrap_iv, 8);
}

int aes_key_wrap(const uint8_t kek[16], const uint8_t *plain, size_t plain_length, uint8_t *out) {
    if (plain_length < 16 || plain_length % 8 != 0) {
        return 0;
    }
    size_t n = plain_length / 8;
    aes128_t context;
    aes128_set_key(&context, kek);
    uint8_t a[8];
    copy(a, wrap_iv, 8);
    copy(out + 8, plain, plain_length);
    for (int j = 0; j <= 5; j++) {
        for (size_t i = 1; i <= n; i++) {
            uint8_t block[16];
            copy(block, a, 8);
            copy(block + 8, out + i * 8, 8);
            aes128_encrypt(&context, block, block);
            uint64_t t = (uint64_t)n * (uint64_t)j + i;
            copy(a, block, 8);
            for (int b = 0; b < 8; b++) {
                a[7 - b] ^= (uint8_t)(t >> (8 * b));
            }
            copy(out + i * 8, block + 8, 8);
        }
    }
    copy(out, a, 8);
    wipe(&context, sizeof(context));
    return 1;
}

static void shift_left(const uint8_t in[16], uint8_t out[16]) {
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--) {
        uint8_t next = in[i] >> 7;
        out[i] = (uint8_t)((in[i] << 1) | carry);
        carry = next;
    }
}

void aes_cmac(const uint8_t key[16], const void *data, size_t length, uint8_t mac[16]) {
    aes128_t context;
    aes128_set_key(&context, key);
    uint8_t zero[16] = {0};
    uint8_t l[16], k1[16], k2[16];
    aes128_encrypt(&context, zero, l);
    shift_left(l, k1);
    if (l[0] & 0x80) {
        k1[15] ^= 0x87;
    }
    shift_left(k1, k2);
    if (k1[0] & 0x80) {
        k2[15] ^= 0x87;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    size_t blocks = length == 0 ? 1 : (length + 15) / 16;
    int complete = length != 0 && length % 16 == 0;
    uint8_t x[16] = {0};
    for (size_t b = 0; b < blocks; b++) {
        uint8_t block[16];
        if (b + 1 < blocks) {
            copy(block, bytes + b * 16, 16);
        } else {
            size_t remaining = length - b * 16;
            for (int i = 0; i < 16; i++) {
                if ((size_t)i < remaining) {
                    block[i] = bytes[b * 16 + (size_t)i];
                } else {
                    block[i] = (size_t)i == remaining ? 0x80 : 0;
                }
            }
            for (int i = 0; i < 16; i++) {
                block[i] ^= complete ? k1[i] : k2[i];
            }
        }
        for (int i = 0; i < 16; i++) {
            x[i] ^= block[i];
        }
        aes128_encrypt(&context, x, x);
    }
    copy(mac, x, 16);
    wipe(&context, sizeof(context));
    wipe(l, sizeof(l));
    wipe(k1, sizeof(k1));
    wipe(k2, sizeof(k2));
}

int wpa_equal(const uint8_t *a, const uint8_t *b, size_t length) {
    uint8_t difference = 0;
    for (size_t i = 0; i < length; i++) {
        difference |= a[i] ^ b[i];
    }
    return difference == 0;
}
