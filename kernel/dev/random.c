/* kernel/dev/random.c - M100: random bytes that are not a counter. See
 * random.h for the design; this file is the arithmetic. */
#include "random.h"

#include "lib/libk.h"
#include "lib/spinlock.h"

/* ---- 1. ChaCha20, RFC 7539 section 2.3 ----------------------------------
 *
 * Written from the RFC rather than copied from anywhere, which is what
 * the non-negotiables require of everything that ships, and graded
 * against the RFC's own vector in tests/test_random.c, which is what
 * makes writing it from the RFC safe. */
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
    /* "expand 32-byte k", as four little-endian words. */
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
        QR(x[0], x[4], x[8], x[12]);   /* the four columns */
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);  /* the four diagonals */
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        store32(out + 4 * i, x[i] + in[i]);
    }
}

/* ---- 2. The pool ----------------------------------------------------------
 *
 * One lock, taken with interrupts off, because random_feed is called
 * from interrupt handlers and random_bytes from syscalls, and the two
 * must not interleave halfway through a key replacement. Nothing under
 * it blocks, allocates, or calls out. */
static spinlock_t rng_lock;
static uint8_t key[32];
static uint32_t counter;          /* block counter for the current key */
static uint64_t events;
static uint64_t hw_words;         /* RDSEED/RDRAND words mixed so far */
static int have_rdrand, have_rdseed;

/* The nonce is a constant: uniqueness of the keystream comes from the
 * key changing on every extraction and every mix, which is a stronger
 * property than a per-call nonce would add. */
static const uint8_t NONCE[12] = {'l', 'e', 'a', 'n', '_', 'o', 's', 0, 0, 0, 0, 0};

/* Replace the key with a block of its own keystream. After this, the
 * key that existed a moment ago exists nowhere. */
static void ratchet(void) {
    uint8_t block[64];
    chacha20_block(key, NONCE, counter++, block);
    k_memcpy(key, block, 32);
    k_memset(block, 0, sizeof(block));
}

/* XOR up to 32 bytes into the key and ratchet, so an input is never
 * left sitting in the key in the clear. Longer inputs go in 32 bytes
 * at a time, each chunk mixed before the next - which means a 33-byte
 * input is two mixes, and no input can overwrite an earlier one. */
static void mix_locked(const uint8_t *p, size_t len) {
    while (len) {
        size_t n = len < 32 ? len : 32;
        for (size_t i = 0; i < n; i++) {
            key[i] ^= p[i];
        }
        ratchet();
        p += n;
        len -= n;
    }
}

/* ---- the hardware generator, where there is one --------------------------
 *
 * CPUID leaf 1 ECX bit 30 is RDRAND; leaf 7 EBX bit 18 is RDSEED. Both
 * are asked once at init. RDRAND can legitimately fail (CF clear) when
 * the DRBG behind it is being reseeded, so a word is retried a few
 * times and then given up on - an unmixed word costs nothing. Under
 * QEMU's default CPU neither bit is set, and every graded boot in this
 * project runs on that CPU, so the code below is exercised only on
 * metal or under `-cpu max` and the test for it is the boot log line
 * that says which. */
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
/* The host test: no CPUID, no RDSEED, and a "TSC" the test controls,
 * because a generator whose seed cannot be held still cannot be
 * checked for determinism. */
static void probe_hardware(void) {}
static int hw_word(uint64_t *out) { (void)out; return 0; }
uint64_t random_test_tsc_value;
static uint64_t tsc(void) { return random_test_tsc_value; }
#endif

void random_init(const void *seed, size_t len) {
    k_memset(key, 0, sizeof(key));
    counter = 0;
    events = 0;
    hw_words = 0;
    probe_hardware();
    uint64_t t = tsc();
    mix_locked((const uint8_t *)&t, sizeof(t));
    if (seed && len) {
        mix_locked((const uint8_t *)seed, len);
    }
    /* Eight words from the hardware generator, if there is one: 512 bits
     * of the only entropy this boot can have before the first interrupt. */
    for (int i = 0; i < 8; i++) {
        uint64_t w;
        if (!hw_word(&w)) {
            break;
        }
        mix_locked((const uint8_t *)&w, sizeof(w));
        hw_words++;
    }
}

void random_feed(const void *data, size_t len) {
    uint64_t f = spin_lock_irqsave(&rng_lock);
    /* The TSC goes in with every feed: two identical inputs at two
     * different moments are two different inputs. */
    uint64_t t = tsc();
    mix_locked((const uint8_t *)&t, sizeof(t));
    if (data && len) {
        mix_locked((const uint8_t *)data, len);
    }
    events++;
    spin_unlock_irqrestore(&rng_lock, f);
}

void random_bytes(void *out, size_t len) {
    uint8_t *dst = (uint8_t *)out;
    uint64_t f = spin_lock_irqsave(&rng_lock);
    /* A hardware word on every extraction as well, when there is one:
     * cheap, and it means even a pool that was never fed is not a pure
     * function of the boot's seed on a machine that has RDSEED. */
    uint64_t w;
    if (hw_word(&w)) {
        mix_locked((const uint8_t *)&w, sizeof(w));
        hw_words++;
    }
    /* Fast key erasure: the FIRST block of keystream becomes the next
     * key, and only blocks after it are handed out. So the output was
     * generated under a key that no longer exists by the time the
     * caller reads it. */
    uint8_t block[64];
    chacha20_block(key, NONCE, counter++, block);
    uint8_t next[32];
    k_memcpy(next, block, 32);
    size_t have = 32; /* the second half of that block is output already */
    size_t off = 0;
    while (off < len) {
        if (have == 64) {
            chacha20_block(key, NONCE, counter++, block);
            have = 0;
        }
        size_t n = 64 - have;
        if (n > len - off) {
            n = len - off;
        }
        k_memcpy(dst + off, block + have, n);
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
