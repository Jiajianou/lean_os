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

static int name_eq(const char *s, const char *want) {
    int i = 0;
    for (; want[i]; i++) {
        if (s[i] != want[i]) {
            return 0;
        }
    }
    return s[i] == '\0';
}

static int name_is_c(const char *s) {
    if (!s || s[0] == '\0') {
        return 1; /* "" means "ask the environment", which says nothing here */
    }
    /* M88: "C.UTF-8" joins the list, and it is the one addition this
     * machine can honestly accept. It names exactly what is true here -
     * the C locale's collation and formatting, with a UTF-8 codeset -
     * and every configure script that wants a UTF-8 locale asks for it
     * by that name first. "en_US.UTF-8" is still refused, because the
     * part of it that is not the codeset is a claim about collation and
     * month names that this libc does not implement. */
    return name_eq(s, "C") || name_eq(s, "POSIX") || name_eq(s, "C.UTF-8");
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
 * <langinfo.h> for why M88 changed it from ASCII to UTF-8.
 */
#include <langinfo.h>

char *nl_langinfo(nl_item item) {
    switch (item) {
    case CODESET:   return (char *)"UTF-8"; /* M88 - see <langinfo.h> for why this changed */
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
