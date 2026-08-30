/* user_space/libc/src/locale.c - M80 groundwork. See <locale.h>. */
#include <locale.h>

static char c_locale[] = "C";
static char empty[] = "";
static char point[] = ".";
static char minus[] = "-";

/* CHAR_MAX for the fields the C locale leaves unspecified. Spelled out
 * rather than included from <limits.h> so this file has one dependency
 * fewer, and 127 is what CHAR_MAX is on this target. */
#define UNSPECIFIED ((char)127)

static struct lconv c_lconv = {
    point, empty, empty,
    empty, empty,
    point, empty, empty,
    empty, minus,
    UNSPECIFIED, UNSPECIFIED,
    UNSPECIFIED, UNSPECIFIED, UNSPECIFIED, UNSPECIFIED,
    UNSPECIFIED, UNSPECIFIED,
};

static int name_is_c(const char *s) {
    if (!s || s[0] == '\0') {
        return 1; /* "" means "ask the environment", which says nothing here */
    }
    if (s[0] == 'C' && s[1] == '\0') {
        return 1;
    }
    const char *p = "POSIX";
    int i = 0;
    for (; p[i]; i++) {
        if (s[i] != p[i]) {
            return 0;
        }
    }
    return s[i] == '\0';
}

char *setlocale(int category, const char *locale) {
    (void)category; /* one locale, so every category has the same answer */
    if (!locale) {
        return c_locale; /* a query */
    }
    /* Refused rather than accepted-and-ignored. A program that asked for
     * en_US.UTF-8 and was told yes would then believe this system does
     * UTF-8 collation, which it does not. */
    return name_is_c(locale) ? c_locale : (char *)0;
}

struct lconv *localeconv(void) {
    return &c_lconv;
}

/* ---- <langinfo.h> ------------------------------------------------------
 *
 * The C locale's answers, and CODESET is the one that matters - see
 * <langinfo.h> for why it says ASCII rather than UTF-8.
 */
#include <langinfo.h>

char *nl_langinfo(nl_item item) {
    switch (item) {
    case CODESET:   return (char *)"ANSI_X3.4-1968";
    case D_T_FMT:   return (char *)"%a %b %e %H:%M:%S %Y";
    case D_FMT:     return (char *)"%m/%d/%y";
    case T_FMT:     return (char *)"%H:%M:%S";
    case AM_STR:    return (char *)"AM";
    case PM_STR:    return (char *)"PM";
    case RADIXCHAR: return (char *)".";
    case THOUSEP:   return (char *)"";
    case YESEXPR:   return (char *)"^[yY]";
    case NOEXPR:    return (char *)"^[nN]";
    default:        return (char *)"";
    }
}
