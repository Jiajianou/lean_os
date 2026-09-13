#include "random.h"

#include "library/kernel_library.h"
#include "library/spinlock.h"

static inline uint32_t rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

#define QR(a, b, c, d)                                          \
    do {                                                        \
        a += b; d ^= a; d = rotl32(d, 16);                      \
        c += d; b ^= c; b = rotl32(b, 12);                      \
        a += b; d ^= a; d = rotl32(d, 8);                       \
        c += d; b ^= c; b = rotl32(b, 7);                       \
    } while (0)

static inline uint32_t load32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void store32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
                    uint32_t counter, uint8_t out[64]) {
    uint32_t in[16] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};
    for (int i = 0; i < 8; i++) {
        in[4 + i] = load32(key + 4 * i);
    }
    in[12] = counter;
    for (int i = 0; i < 3; i++) {
        in[13 + i] = load32(nonce + 4 * i);
    }
    uint32_t x[16];
    for (int i = 0; i < 16; i++) {
        x[i] = in[i];
    }
    for (int round = 0; round < 10; round++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        store32(out + 4 * i, x[i] + in[i]);
    }
}

static spinlock_t rng_lock;
static uint8_t key[32];
static uint32_t counter;
static uint64_t events;
static uint64_t hw_words;
static int have_rdrand, have_rdseed;

static const uint8_t NONCE[12] = {'l', 'e', 'a', 'n', '_', 'o', 's', 0, 0, 0, 0, 0};

static void ratchet(void) {
    uint8_t block[64];
    chacha20_block(key, NONCE, counter++, block);
    k_memcpy(key, block, 32);
    k_memset(block, 0, sizeof(block));
}

static void mix_locked(const uint8_t *p, size_t length) {
    while (length) {
        size_t n = length < 32 ? length : 32;
        for (size_t i = 0; i < n; i++) {
            key[i] ^= p[i];
        }
        ratchet();
        p += n;
        length -= n;
    }
}

#if defined(__x86_64__) && !defined(LEANOS_HOST_TEST)
static void probe_hardware(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    have_rdrand = (c >> 30) & 1;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    have_rdseed = (b >> 18) & 1;
}

static int hw_word(uint64_t *out) {
    for (int tries = 0; tries < 8; tries++) {
        uint64_t v;
        unsigned char ok;
        if (have_rdseed) {
            __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
        } else if (have_rdrand) {
            __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        } else {
            return 0;
        }
        if (ok) {
            *out = v;
            return 1;
        }
    }
    return 0;
}

static uint64_t tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#else
static void probe_hardware(void) {}
static int hw_word(uint64_t *out) { (void)out; return 0; }
uint64_t random_test_tsc_value;
static uint64_t tsc(void) { return random_test_tsc_value; }
#endif

void random_init(const void *seed, size_t length) {
    k_memset(key, 0, sizeof(key));
    counter = 0;
    events = 0;
    hw_words = 0;
    probe_hardware();
    uint64_t t = tsc();
    mix_locked((const uint8_t *)&t, sizeof(t));
    if (seed && length) {
        mix_locked((const uint8_t *)seed, length);
    }
    for (int i = 0; i < 8; i++) {
        uint64_t w;
        if (!hw_word(&w)) {
            break;
        }
        mix_locked((const uint8_t *)&w, sizeof(w));
        hw_words++;
    }
}

void random_feed(const void *data, size_t length) {
    uint64_t f = spin_lock_irqsave(&rng_lock);
    uint64_t t = tsc();
    mix_locked((const uint8_t *)&t, sizeof(t));
    if (data && length) {
        mix_locked((const uint8_t *)data, length);
    }
    events++;
    spin_unlock_irqrestore(&rng_lock, f);
}

void random_bytes(void *out, size_t length) {
    uint8_t *destination = (uint8_t *)out;
    uint64_t f = spin_lock_irqsave(&rng_lock);
    uint64_t w;
    if (hw_word(&w)) {
        mix_locked((const uint8_t *)&w, sizeof(w));
        hw_words++;
    }
    uint8_t block[64];
    chacha20_block(key, NONCE, counter++, block);
    uint8_t next[32];
    k_memcpy(next, block, 32);
    size_t have = 32;
    size_t off = 0;
    while (off < length) {
        if (have == 64) {
            chacha20_block(key, NONCE, counter++, block);
            have = 0;
        }
        size_t n = 64 - have;
        if (n > length - off) {
            n = length - off;
        }
        k_memcpy(destination + off, block + have, n);
        off += n;
        have += n;
    }
    k_memcpy(key, next, 32);
    k_memset(next, 0, sizeof(next));
    k_memset(block, 0, sizeof(block));
    spin_unlock_irqrestore(&rng_lock, f);
}

uint64_t random_events(void) {
    return events;
}

int random_has_rdrand(void) {
    return have_rdrand;
}

int random_has_rdseed(void) {
    return have_rdseed;
}
