#include <ctype.h>
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

/* POSIX 2008's locale-argument functions. Every one of them does what its
   neighbour without the _l does, and on this machine that is the whole truth
   rather than a shortcut: newlocale returns exactly one object, for "C",
   "POSIX", "C.UTF-8" or the empty name, and refuses every other name. There
   is one locale here, so a function told which locale to use can only have
   been told that one.

   What they do NOT do is ignore the argument. A null handle is EINVAL, and
   a handle that is neither the one locale nor LC_GLOBAL_LOCALE is EINVAL
   too - so a program that passes a locale this machine never made finds out,
   instead of getting the C locale's answer and believing it asked for
   something else. */

static int locale_is_known(locale_t locale) {
    return locale == LC_GLOBAL_LOCALE || locale == duplocale(LC_GLOBAL_LOCALE);
}

#define CTYPE_QUERY(name)                                                     \
    int name##_l(int c, locale_t locale) {                                    \
        if (!locale_is_known(locale)) {                                       \
            errno = EINVAL;                                                   \
            return 0;                                                         \
        }                                                                     \
        return name(c);                                                       \
    }

CTYPE_QUERY(isalnum)
CTYPE_QUERY(isalpha)
CTYPE_QUERY(isblank)
CTYPE_QUERY(iscntrl)
CTYPE_QUERY(isdigit)
CTYPE_QUERY(isgraph)
CTYPE_QUERY(islower)
CTYPE_QUERY(isprint)
CTYPE_QUERY(ispunct)
CTYPE_QUERY(isspace)
CTYPE_QUERY(isupper)
CTYPE_QUERY(isxdigit)

int tolower_l(int c, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return c;
    }
    return tolower(c);
}

int toupper_l(int c, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return c;
    }
    return toupper(c);
}

#define WCTYPE_QUERY(name)                                                    \
    int name##_l(wint_t c, locale_t locale) {                                 \
        if (!locale_is_known(locale)) {                                       \
            errno = EINVAL;                                                   \
            return 0;                                                         \
        }                                                                     \
        return name(c);                                                       \
    }

WCTYPE_QUERY(iswalnum)
WCTYPE_QUERY(iswalpha)
WCTYPE_QUERY(iswblank)
WCTYPE_QUERY(iswcntrl)
WCTYPE_QUERY(iswdigit)
WCTYPE_QUERY(iswgraph)
WCTYPE_QUERY(iswlower)
WCTYPE_QUERY(iswprint)
WCTYPE_QUERY(iswpunct)
WCTYPE_QUERY(iswspace)
WCTYPE_QUERY(iswupper)
WCTYPE_QUERY(iswxdigit)

int iswctype_l(wint_t c, wctype_t type, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0;
    }
    return iswctype(c, type);
}

wctype_t wctype_l(const char *name, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return (wctype_t)0;
    }
    return wctype(name);
}

wint_t towlower_l(wint_t c, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return c;
    }
    return towlower(c);
}

wint_t towupper_l(wint_t c, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return c;
    }
    return towupper(c);
}

wctrans_t wctrans_l(const char *name, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return (wctrans_t)0;
    }
    return wctrans(name);
}

wint_t towctrans_l(wint_t c, wctrans_t transform, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return c;
    }
    return towctrans(c, transform);
}

/* Collation in the C locale is comparison of the encoded bytes, which is what
   strcmp does - so strcoll IS strcmp here, and strxfrm is a copy. That is the
   C locale's definition rather than an approximation of a real collation. */
int strcoll_l(const char *a, const char *b, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0;
    }
    return strcoll(a, b);
}

size_t strxfrm_l(char *out, const char *in, size_t length, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0;
    }
    return strxfrm(out, in, length);
}

int wcscoll_l(const wchar_t *a, const wchar_t *b, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0;
    }
    return wcscoll(a, b);
}

size_t wcsxfrm_l(wchar_t *out, const wchar_t *in, size_t length,
                 locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0;
    }
    return wcsxfrm(out, in, length);
}

double strtod_l(const char *text, char **end, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0.0;
    }
    return strtod(text, end);
}

float strtof_l(const char *text, char **end, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0.0f;
    }
    return strtof(text, end);
}

long double strtold_l(const char *text, char **end, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0.0L;
    }
    return strtold(text, end);
}

long strtol_l(const char *text, char **end, int base, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0;
    }
    return strtol(text, end, base);
}

long long strtoll_l(const char *text, char **end, int base, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0;
    }
    return strtoll(text, end, base);
}

unsigned long strtoul_l(const char *text, char **end, int base,
                        locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0;
    }
    return strtoul(text, end, base);
}

unsigned long long strtoull_l(const char *text, char **end, int base,
                              locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0;
    }
    return strtoull(text, end, base);
}

size_t strftime_l(char *out, size_t max, const char *format,
                  const struct tm *when, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return 0;
    }
    return strftime(out, max, format, when);
}

char *nl_langinfo_l(nl_item item, locale_t locale) {
    if (!locale_is_known(locale)) {
        errno = EINVAL;
        return (char *)"";
    }
    return nl_langinfo(item);
}
