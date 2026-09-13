#include <wctype.h>

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
        return 1;
    }
    if (name[0] == 't' && name[1] == 'o' && name[2] == 'l') {
        return 2;
    }
    return 0;
}

wint_t towctrans(wint_t c, wctrans_t descriptor) {
    if (descriptor == 1) {
        return towupper(c);
    }
    if (descriptor == 2) {
        return towlower(c);
    }
    return c;
}

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
