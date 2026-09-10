/* user_space/libc/include/iconv.h - M100.
 *
 * Character set conversion. NetSurf is what asked: utils/utf8.c includes
 * this unconditionally, because a browser's whole job includes reading a
 * document whose bytes are in a charset the document itself names.
 *
 * ---- what this implementation covers, stated rather than implied -----
 *
 * UTF-8, UTF-16 and UTF-32 (each in LE, BE and a BOM-bearing form),
 * UCS-2, US-ASCII, ISO-8859-1 through -16, windows-1250 through -1258,
 * KOI8-R, KOI8-U and Macintosh Roman. Every conversion goes through
 * Unicode, so any of them converts to any other.
 *
 * It does NOT cover the CJK multibyte encodings - Shift_JIS, EUC-JP,
 * GB18030, Big5. Those are megabyte-scale tables rather than a harder
 * algorithm, and nothing here has asked for one yet. iconv_open refuses
 * them by name, which is a failure a caller can act on; the alternative
 * - accepting the name and mangling the text - is the outcome this
 * whole interface exists to avoid.
 *
 * The names are matched case-insensitively and ignoring '-' and '_', so
 * "UTF-8", "utf8" and "UTF_8" are one charset. That is what every iconv
 * does and what the charset labels in real HTML require.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *iconv_t;

/* A descriptor converting FROM `fromcode` TO `tocode`, or (iconv_t)-1
 * with errno EINVAL if either name is one this libc does not have.
 *
 * The "//TRANSLIT" and "//IGNORE" suffixes GNU's iconv accepts are NOT
 * supported and are refused rather than silently dropped: dropping them
 * would turn "convert this, approximating what you cannot" into "fail
 * on the first character you cannot", which is a different answer to
 * the caller's question. */
iconv_t iconv_open(const char *tocode, const char *fromcode);

/* Converts up to *inbytesleft bytes at *inbuf into the buffer at *outbuf,
 * advancing both pointers and decrementing both counts by what it
 * consumed and produced.
 *
 * Returns the number of characters converted in a way that lost
 * information - always 0 here, because nothing in this implementation
 * approximates - or (size_t)-1 with errno:
 *
 *   EILSEQ  an input byte sequence is not valid in `fromcode`, or names
 *           a character `tocode` cannot represent
 *   EINVAL  the input ends in the middle of a valid multibyte sequence
 *   E2BIG   the output buffer filled first
 *
 * In every failure case both pointers and both counts are left pointing
 * at the first byte NOT converted, so a caller can grow its buffer and
 * call again. That is the clause that makes E2BIG recoverable and it is
 * the clause an implementation most often gets wrong.
 *
 * `inbuf` NULL (or *inbuf NULL) resets the descriptor's state.
 */
size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
             char **outbuf, size_t *outbytesleft);

int iconv_close(iconv_t cd);

#ifdef __cplusplus
}
#endif
