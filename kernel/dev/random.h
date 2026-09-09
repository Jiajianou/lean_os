/* kernel/dev/random.h - M100: random bytes that are not a counter.
 *
 * ---- who asked ----------------------------------------------------------
 *
 * Nothing, until the eighth library of M100's stack. /dev/urandom was a
 * xorshift over the TSC, its own header said so, and for the hash salt
 * expat wanted and the temporary-file names Python wanted that was
 * enough. A TLS library wants a KEY, and a key drawn from a counter an
 * attacker can also read is not one - so this is the first thing on
 * this machine whose job is to be unpredictable, and it is written so a
 * host test can say whether it is.
 *
 * ---- what it is ---------------------------------------------------------
 *
 * Three parts, and the split is what makes it testable:
 *
 *   1. ChaCha20 (RFC 7539), the one cryptographic primitive here, used
 *      as a stream generator. Sixty lines, and graded against the RFC's
 *      own block-function test vector rather than against anything
 *      written here.
 *
 *   2. A pool: 32 bytes of key. Every byte of entropy that arrives is
 *      XORed into it and the key is then run through ChaCha20 to mix -
 *      so the pool is never "the last thing that came in", it is every
 *      thing that ever came in, permuted. Feeding the pool is O(1) and
 *      takes no lock that an interrupt handler cannot take, because the
 *      interrupt handler is where most of the feeding happens.
 *
 *   3. Extraction with fast key erasure: to produce N bytes, generate
 *      N + 32 bytes of keystream from the current key, hand out N, and
 *      REPLACE the key with the other 32. So the key that produced any
 *      output is gone the instant the output exists, and a machine
 *      whose memory is read afterwards gives up nothing that was
 *      already handed out. (This is the construction BSD's arc4random
 *      and Linux's random.c both settled on.)
 *
 * ---- where the entropy comes from --------------------------------------
 *
 * The TSC at every interrupt - keyboard, mouse, disk, NIC, timer - which
 * is the timing of the world outside the CPU, and RDSEED/RDRAND where
 * CPUID says the part has them, which under QEMU's default CPU it does
 * not (the machine every graded boot here runs on has no hardware
 * generator, which is why the interrupt path is the primary source and
 * not the fallback). The boot mixes in the RTC and the MAC address too:
 * not entropy, but distinct per machine, and a seed that is at least
 * different everywhere is better than one that is the same.
 *
 * ---- what is NOT claimed ------------------------------------------------
 *
 * No entropy ESTIMATE. Linux's credit-counting has been wrong in both
 * directions for twenty years and this project is not going to do it
 * better in an afternoon. What is claimed is narrower and checkable:
 * the generator is a proper stream cipher, its key has had every
 * interrupt of the boot mixed into it, and the output is never a
 * function of a state that still exists. random_events() says how many
 * inputs the pool has taken, so a self-test can require that the
 * number is large by the time anything asks for a key.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* The RFC 7539 block function, exposed for the host test and nothing
 * else: 64 bytes of keystream from a 32-byte key, a 12-byte nonce and a
 * 32-bit block counter. */
void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
                    uint32_t counter, uint8_t out[64]);

/* One-time setup: seeds the pool from whatever the boot has - the TSC,
 * the hardware generator if there is one, and `seed` (the RTC, the MAC,
 * anything distinct). Safe to call before interrupts are enabled. */
void random_init(const void *seed, size_t len);

/* Feed the pool. Called from interrupt handlers with the TSC, and from
 * anywhere with anything. Never blocks, never allocates. */
void random_feed(const void *data, size_t len);

/* `len` bytes of output into `out`. Never fails and never blocks: this
 * device does not pretend to know when it is "ready" (see the header),
 * so a caller that wants to know how much has been fed asks the count. */
void random_bytes(void *out, size_t len);

/* How many random_feed calls the pool has absorbed since init. */
uint64_t random_events(void);

/* Whether CPUID reported RDRAND / RDSEED - reported, for the boot log. */
int random_has_rdrand(void);
int random_has_rdseed(void);
