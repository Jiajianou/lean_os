/* user_space/lib/sha256.h
 *
 * M111: SHA-256, written here, because a package manager whose integrity
 * check is a checksum is a package manager with no integrity check.
 *
 * This project already has a hash - leanfs's FNV-1a, over a directory
 * record - and it is the right one for what it does: catch a bit that
 * rotted on a disk this machine owns. It is the wrong one here, and the
 * difference is the whole reason this file exists. FNV is not a claim
 * about an adversary; it is trivially invertible, and two inputs with the
 * same FNV value take milliseconds to find. An installed package is
 * something that arrived from somewhere else, and the only useful
 * question about it is whether it is the *same bytes* somebody meant to
 * send - which is a question about a second preimage, not about disk rot.
 *
 * mbedtls has a SHA-256 and it is on this machine (M100), and it cannot
 * be used: CLAUDE.md's first non-negotiable is that no third-party code
 * ships in the OS, and `os` is part of the OS rather than something
 * ported onto it. So this is FIPS 180-4 section 6.2, written from the
 * specification, and graded three ways - the standard's own vectors, a
 * long-message vector, and tools/sha256-test.sh, which hashes several
 * hundred real files from this tree and requires every answer to match
 * the host's own `shasum -a 256`. The last of those is the one that
 * matters: nothing in it says what the right answer is.
 *
 * Compiled for the target (into libc.a, for /bin/os) and for the host
 * (tools/os-pkg.c and the unit tests) from this one source, which is
 * what makes a package built on the host and verified on the machine a
 * meaningful comparison rather than two implementations agreeing to
 * differ.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_BYTES 32
#define SHA256_BLOCK_BYTES  64

typedef struct {
    uint32_t h[8];
    uint64_t bits;               /* message length so far, in bits */
    uint8_t  buf[SHA256_BLOCK_BYTES];
    size_t   buffered;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t len);
void sha256_final(sha256_t *s, uint8_t out[SHA256_DIGEST_BYTES]);

/* The one-shot form, which is what nearly every caller wants. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_BYTES]);

/* Lowercase hex, 64 characters plus a NUL. `out` must have room for 65. */
void sha256_hex(const uint8_t digest[SHA256_DIGEST_BYTES], char *out);

/* Parse 64 hex characters into a digest. Returns 0, or -1 if `hex` is not
 * exactly 64 hex digits - a partial parse is refused rather than
 * zero-filled, because a truncated hash that compares equal against a
 * zeroed field is the failure this whole file exists to prevent. */
int sha256_unhex(const char *hex, uint8_t out[SHA256_DIGEST_BYTES]);

/* Constant-time equality. Not because a timing attack on a package
 * digest is a threat this machine faces, but because the alternative is
 * a memcmp whose early exit is a habit worth not having in the one file
 * on this system whose whole job is to compare secrets-shaped things. */
int sha256_equal(const uint8_t a[SHA256_DIGEST_BYTES],
                 const uint8_t b[SHA256_DIGEST_BYTES]);
