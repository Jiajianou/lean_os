/* user_space/libc/include/locale.h - M80 groundwork
 *
 * There is one locale on this machine and it is "C".
 *
 * That is a fact about the system, not a gap in this header: the font
 * has one glyph per byte (M39/M57), every comparison in this libc is
 * byte-wise, and there is no message catalogue, no collation table and
 * no character-set conversion anywhere in the tree. `setlocale` therefore
 * succeeds for "C" and for the empty string (which means "whatever the
 * environment says", and the environment says nothing) and fails for
 * anything else - which is the answer a program can act on, rather than
 * a success that quietly did nothing.
 *
 * `localeconv` returns the C locale's own values, which are real rather
 * than invented: a "." decimal point and no grouping is what the C
 * locale IS, and it is what this system's printf actually does.
 */
#pragma once

#include <stddef.h>

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5
#define LC_MESSAGES 6

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
};

/* Returns "C" for LC_* with a NULL, an empty, or a "C"/"POSIX" locale
 * name, and NULL for anything else. */
char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);
