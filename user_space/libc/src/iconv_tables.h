/* user_space/libc/src/iconv_tables.h - M100.
 *
 * The shape of the generated table in iconv_tables.c. Internal to this
 * libc: nothing outside src/ includes it, and <iconv.h> says nothing
 * about which charsets exist, because the answer to that is a runtime
 * question (iconv_open fails) and not a compile-time one.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

struct iconv_sb_charset {
    const char *name;
    /* 0x80-0xFF to Unicode. 0xFFFF = unassigned in this charset. */
    const uint16_t *high;
};

extern const struct iconv_sb_charset __iconv_sb_charsets[];
extern const size_t __iconv_sb_charset_count;
