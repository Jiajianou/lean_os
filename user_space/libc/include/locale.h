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

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

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
    /* ---- M121: the six C99 added, and why their value is CHAR_MAX ----
     *
     * C89's `lconv` ended at n_sign_posn; C99 added these six, which
     * describe how an INTERNATIONAL monetary amount is punctuated as
     * distinct from a local one. This struct stopped at the C89 set, and
     * libc++'s moneypunct facet reads all six - sixteen errors from
     * locale.cpp, each one "no member named 'int_p_cs_precedes' in
     * 'lconv'".
     *
     * CHAR_MAX is not a placeholder: C specifies it as the value meaning
     * "this quantity is not available in the current locale", and it is
     * what the C locale's own entry must be. localeconv() sets them (see
     * locale.c), so a program reading them gets the standard's answer
     * for "C" rather than a zero that would read as "the symbol follows
     * the amount with no space". */
    char int_p_cs_precedes;
    char int_n_cs_precedes;
    char int_p_sep_by_space;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

/* Returns "C" for LC_* with a NULL, an empty, or a "C"/"POSIX" locale
 * name, and NULL for anything else. */
char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

/* ---- M89: the per-thread locale interface ----------------------------
 *
 * POSIX.1-2008's `locale_t`: a locale object a thread can adopt without
 * changing the process's. There is one locale on this machine, so the
 * only object these can ever produce is the C locale and there is
 * nothing for two threads to disagree about.
 *
 * `newlocale` accepts the same names `setlocale` does - "C", "POSIX",
 * "C.UTF-8" and the empty string - and returns NULL for anything else,
 * which is the same refusal in the same place and is what a program
 * probing for a locale it wants is asking. It does NOT allocate: there
 * is one locale object and it is static, so `freelocale` is a no-op and
 * a program that leaks one loses nothing.
 *
 * The masks are a bitfield rather than the category numbers above,
 * because that is what newlocale takes. LC_GLOBAL_LOCALE is the sentinel
 * `uselocale` returns for a thread that has not adopted one.
 */
typedef struct __locale *locale_t;

#define LC_COLLATE_MASK  (1 << LC_COLLATE)
#define LC_CTYPE_MASK    (1 << LC_CTYPE)
#define LC_MONETARY_MASK (1 << LC_MONETARY)
#define LC_NUMERIC_MASK  (1 << LC_NUMERIC)
#define LC_TIME_MASK     (1 << LC_TIME)
#define LC_MESSAGES_MASK (1 << LC_MESSAGES)
#define LC_ALL_MASK      (LC_COLLATE_MASK | LC_CTYPE_MASK | LC_MONETARY_MASK | \
                          LC_NUMERIC_MASK | LC_TIME_MASK | LC_MESSAGES_MASK)

#define LC_GLOBAL_LOCALE ((locale_t)-1)

locale_t newlocale(int category_mask, const char *locale, locale_t base);
locale_t uselocale(locale_t loc);
locale_t duplocale(locale_t loc);
void     freelocale(locale_t loc);

#ifdef __cplusplus
}
#endif
