#pragma once

#include <stddef.h>
#include <stdint.h>

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C++ makes these keywords; C11 puts them here. */
#ifndef __cplusplus
typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;
#endif

/* UTF-8 is this machine's only multibyte encoding, so a char32_t is a code
   point and these two are mbrtowc and wcrtomb under another name. char16_t
   is UTF-16, where a code point above the basic plane is a pair - which is
   why this pair of functions needs state the caller keeps, and why
   mbrtoc16 can return (size_t)-3 for a unit it produced without consuming
   anything. */
size_t mbrtoc16(char16_t *destination, const char *source, size_t n,
                mbstate_t *ps);
size_t c16rtomb(char *destination, char16_t c16, mbstate_t *ps);
size_t mbrtoc32(char32_t *destination, const char *source, size_t n,
                mbstate_t *ps);
size_t c32rtomb(char *destination, char32_t c32, mbstate_t *ps);

#ifdef __cplusplus
}
#endif
