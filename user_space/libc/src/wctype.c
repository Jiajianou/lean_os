/* user_space/libc/src/wctype.c - M89
 *
 * <wctype.h>'s out-of-line half: the four calls that take a character
 * class by name, and wcwidth. Everything else in that header is inline.
 *
 * A separate file from wchar.c rather than appended to it, and the
 * reason is the host test tier: tests/ compiles wchar.c for the machine
 * you are sitting at, and macOS's own <ctype.h> declares `iswctype`, so
 * a translation unit that includes both this project's <wctype.h> and
 * the host's ctype.h cannot compile. Splitting the file is a smaller
 * change than shadowing a third host header - see tests/fakes/wchar.h
 * for the two that already are, and why that list should stay short.
 *
 * See <wctype.h> for why every answer here is an ASCII answer on a
 * system whose encoding is UTF-8.
 */
#include <wctype.h>

/* The class names, in the order iswctype's opaque value indexes them. A
 * value of 0 means "no such class", which is what wctype() returns for a
 * name it does not know and what iswctype() must then report false for. */
static int class_matches(int index, wint_t c) {
    switch (index) {
    case 1:  return iswalpha(c);
    case 2:  return iswdigit(c);
    case 3:  return iswalnum(c);
    case 4:  return iswspace(c);
    case 5:  return iswupper(c);
    case 6:  return iswlower(c);
    case 7:  return iswpunct(c);
    case 8:  return iswprint(c);
    case 9:  return iswgraph(c);
    case 10: return iswcntrl(c);
    case 11: return iswblank(c);
    case 12: return iswxdigit(c);
    default: return 0;
    }
}

wctype_t wctype(const char *name) {
    static const char *names[] = {
        "alpha", "digit", "alnum", "space", "upper", "lower",
        "punct", "print", "graph", "cntrl", "blank", "xdigit",
    };
    if (!name) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *a = names[i];
        const char *b = name;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (!*a && !*b) {
            return (wctype_t)(i + 1);
        }
    }
    return 0;
}

int iswctype(wint_t c, wctype_t type) {
    return class_matches((int)type, c);
}

wctrans_t wctrans(const char *name) {
    if (!name) {
        return 0;
    }
    if (name[0] == 't' && name[1] == 'o' && name[2] == 'u') {
        return 1; /* toupper */
    }
    if (name[0] == 't' && name[1] == 'o' && name[2] == 'l') {
        return 2; /* tolower */
    }
    return 0;
}

wint_t towctrans(wint_t c, wctrans_t desc) {
    if (desc == 1) {
        return towupper(c);
    }
    if (desc == 2) {
        return towlower(c);
    }
    return c;
}

/* One column per character, and the header says why that is true of what
 * this machine actually draws rather than a simplification. A control
 * character occupies no columns and is reported as -1, which is the
 * signal a terminal layout uses to skip it. */
int wcwidth(wchar_t c) {
    if (c == 0) {
        return 0;
    }
    if ((unsigned int)c < 32 || ((unsigned int)c >= 0x7F && (unsigned int)c < 0xA0)) {
        return -1;
    }
    return 1;
}

int wcswidth(const wchar_t *s, size_t n) {
    int total = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        int w = wcwidth(s[i]);
        if (w < 0) {
            return -1;
        }
        total += w;
    }
    return total;
}
