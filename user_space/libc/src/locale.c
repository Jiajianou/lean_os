#include <locale.h>
#include <string.h>

static char c_locale[] = "C";
static char empty[] = "";
static char point[] = ".";
static char minus[] = "-";

#define UNSPECIFIED ((char)127)

static struct lconv c_lconv = {
    point, empty, empty,
    empty, empty,
    point, empty, empty,
    empty, minus,
    UNSPECIFIED, UNSPECIFIED,
    UNSPECIFIED, UNSPECIFIED, UNSPECIFIED, UNSPECIFIED,
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
        return 1;
    }
    return name_eq(s, "C") || name_eq(s, "POSIX") || name_eq(s, "C.UTF-8");
}

char *setlocale(int category, const char *locale) {
    (void)category;
    if (!locale) {
        return c_locale;
    }
    return name_is_c(locale) ? c_locale : (char *)0;
}

struct lconv *localeconv(void) {
    return &c_lconv;
}

#include <langinfo.h>

static const char *const DAY_NAMES[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"};
static const char *const ABDAY_NAMES[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const MON_NAMES[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"};
static const char *const ABMON_NAMES[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
    "Nov", "Dec"};

char *nl_langinfo(nl_item item) {
    if (item >= DAY_1 && item <= DAY_7) {
        return (char *)DAY_NAMES[item - DAY_1];
    }
    if (item >= ABDAY_1 && item <= ABDAY_7) {
        return (char *)ABDAY_NAMES[item - ABDAY_1];
    }
    if (item >= MON_1 && item <= MON_12) {
        return (char *)MON_NAMES[item - MON_1];
    }
    if (item >= ABMON_1 && item <= ABMON_12) {
        return (char *)ABMON_NAMES[item - ABMON_1];
    }
    switch (item) {
    case CODESET:    return (char *)"UTF-8";
    case D_T_FMT:    return (char *)"%a %b %e %H:%M:%S %Y";
    case D_FMT:      return (char *)"%m/%d/%y";
    case T_FMT:      return (char *)"%H:%M:%S";
    case T_FMT_AMPM: return (char *)"%I:%M:%S %p";
    case AM_STR:     return (char *)"AM";
    case PM_STR:     return (char *)"PM";
    case RADIXCHAR:  return (char *)".";
    case THOUSEP:    return (char *)"";
    case YESEXPR:    return (char *)"^[yY]";
    case NOEXPR:     return (char *)"^[nN]";
    case CRNCYSTR:
    case ERA:
    case ERA_D_FMT:
    case ERA_D_T_FMT:
    case ERA_T_FMT:
    case ALT_DIGITS: return (char *)"";
    default:         return (char *)"";
    }
}

static struct __locale {
    int mask;
} the_c_locale = {LC_ALL_MASK};

static locale_t current_locale;

locale_t newlocale(int category_mask, const char *locale, locale_t base) {
    (void)base;
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
    return (locale_t)0;
}

locale_t uselocale(locale_t loc) {
    locale_t prev = current_locale ? current_locale : LC_GLOBAL_LOCALE;
    if (loc) {
        current_locale = (loc == LC_GLOBAL_LOCALE) ? (locale_t)0 : loc;
    }
    return prev;
}

locale_t duplocale(locale_t loc) {
    return (loc == LC_GLOBAL_LOCALE || !loc) ? &the_c_locale : loc;
}

void freelocale(locale_t loc) {
    (void)loc;
}
