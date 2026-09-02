/* user_space/libc/include/byteswap.h - M89
 *
 * `bswap_16`, `bswap_32`, `bswap_64`.
 *
 * Not POSIX - this is the header Linux puts them in, and a ported
 * program reaches for it after deciding it is not on a BSD and not on a
 * Mac. That decision is made by `#ifdef`, so the only way to be the
 * third case is to have the header the third case includes.
 *
 * Implemented with the compiler's own builtins rather than by hand.
 * `__builtin_bswap32` is one `bswap` instruction on this target, and a
 * hand-written shift-and-mask version would be the same thing written
 * less clearly and optimised back into it anyway.
 */
#pragma once

#define bswap_16(x) __builtin_bswap16(x)
#define bswap_32(x) __builtin_bswap32(x)
#define bswap_64(x) __builtin_bswap64(x)
