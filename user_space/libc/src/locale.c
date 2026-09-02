/* user_space/libc/src/locale.c - M80 groundwork. See <locale.h>. */
#include <locale.h>
#include <string.h> /* M89: newlocale compares the locale name */

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

/* ---- M89: locale_t - see <locale.h> for why none of this allocates --- */

/* The one locale object. Its contents are never read: everything in this
 * library that would consult a locale consults the C one unconditionally,
 * so what matters about this pointer is only that it is not NULL and is
 * stable across calls. */
static struct __locale {
    int mask;
} the_c_locale = {LC_ALL_MASK};

/* What this thread has adopted. Not thread-local, because this libc has
 * no thread-local storage yet (M96's) - which is honest rather than a
 * bug here: there is one locale, so a per-thread copy of a value that
 * can only ever be one thing would differ from a shared one in no
 * observable way. The day there are two locales this needs M96. */
static locale_t current_locale;

locale_t newlocale(int category_mask, const char *locale, locale_t base) {
    (void)base; /* nothing to inherit from: there is one locale */
    if (category_mask & ~LC_ALL_MASK) {
        return (locale_t)0;
    }
    if (!locale) {
        return (locale_t)0;
    }
    if (locale[0] == '\0' || strcmp(locale, "C") == 0 ||
        strcmp(locale, "POSIX") == 0 || strcmp(locale, "C.UTF-8") == 0) {
        return &the_c_locale;
    }
    return (locale_t)0; /* the same refusal setlocale gives, in the same place */
}

locale_t uselocale(locale_t loc) {
    locale_t prev = current_locale ? current_locale : LC_GLOBAL_LOCALE;
    if (loc) {
        current_locale = (loc == LC_GLOBAL_LOCALE) ? (locale_t)0 : loc;
    }
    return prev; /* a NULL argument is a query, which is POSIX's rule */
}

locale_t duplocale(locale_t loc) {
    /* One object, so a duplicate is the same object. Correct rather than
     * lazy: freelocale is a no-op, so two callers holding one pointer
     * cannot free it out from under each other. */
    return (loc == LC_GLOBAL_LOCALE || !loc) ? &the_c_locale : loc;
}

void freelocale(locale_t loc) {
    (void)loc; /* nothing was allocated - see the note above */
}
