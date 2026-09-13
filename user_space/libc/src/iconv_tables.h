#pragma once
#include <stddef.h>
#include <stdint.h>

struct iconv_sb_charset {
    const char *name;
    const uint16_t *high;
};

extern const struct iconv_sb_charset __iconv_sb_charsets[];
extern const size_t __iconv_sb_charset_count;
