#include <fnmatch.h>

#include <ctype.h>
#include <string.h>

static int match_bracket(const char **pp, char c, int fold) {
    const char *p = *pp;
    int negate = 0;
    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }
    int matched = 0;
    int first = 1;
    for (; *p && (*p != ']' || first); p++, first = 0) {
        char lo = *p;
        if (lo == '\\' && p[1]) {
            lo = *++p;
        }
        if (p[1] == '-' && p[2] && p[2] != ']') {
            char hi = p[2];
            p += 2;
            if (hi == '\\' && p[1]) {
                hi = *++p;
            }
            char t = c, l = lo, h = hi;
            if (fold) {
                t = (char)tolower((unsigned char)t);
                l = (char)tolower((unsigned char)l);
                h = (char)tolower((unsigned char)h);
            }
            if ((unsigned char)t >= (unsigned char)l && (unsigned char)t <= (unsigned char)h) {
                matched = 1;
            }
        } else {
            char t = c, l = lo;
            if (fold) {
                t = (char)tolower((unsigned char)t);
                l = (char)tolower((unsigned char)l);
            }
            if (t == l) {
                matched = 1;
            }
        }
    }
    if (*p != ']') {
        return -1;
    }
    *pp = p + 1;
    return negate ? !matched : matched;
}

int fnmatch(const char *pattern, const char *string, int flags) {
    const char *p = pattern, *s = string;
    const char *star_p = 0, *star_s = 0;
    int fold = (flags & FNM_CASEFOLD) != 0;
    int noescape = (flags & FNM_NOESCAPE) != 0;
    int pathname = (flags & FNM_PATHNAME) != 0;

    if ((flags & FNM_PERIOD) && *s == '.' && *p != '.') {
        return FNM_NOMATCH;
    }

    while (*s) {
        char pc = *p;
        if (pc == '*') {
            while (*p == '*') {
                p++;
            }
            if (!*p) {
                if (pathname) {
                    return strchr(s, '/') ? FNM_NOMATCH : 0;
                }
                return 0;
            }
            star_p = p;
            star_s = s;
            continue;
        }
        if (pc == '?' ) {
            if (pathname && *s == '/') {
                goto backtrack;
            }
            p++;
            s++;
            continue;
        }
        if (pc == '[') {
            const char *after = p + 1;
            int r = match_bracket(&after, *s, fold);
            if (r < 0) {
                goto literal;
            }
            if (r == 0 || (pathname && *s == '/')) {
                goto backtrack;
            }
            p = after;
            s++;
            continue;
        }
        if (pc == '\\' && !noescape && p[1]) {
            pc = p[1];
            if (pc != *s) {
                goto backtrack;
            }
            p += 2;
            s++;
            continue;
        }
literal:
        pc = *p;
        {
            char a = pc, b = *s;
            if (fold) {
                a = (char)tolower((unsigned char)a);
                b = (char)tolower((unsigned char)b);
            }
            if (pc && a == b) {
                if (pathname && (flags & FNM_PERIOD) && *s == '/' && s[1] == '.' && p[1] != '.') {
                    return FNM_NOMATCH;
                }
                p++;
                s++;
                continue;
            }
        }
backtrack:
        if (!star_p) {
            return FNM_NOMATCH;
        }
        if (pathname && *star_s == '/') {
            return FNM_NOMATCH;
        }
        star_s++;
        s = star_s;
        p = star_p;
        if (!*s) {
            break;
        }
    }
    while (*p == '*') {
        p++;
    }
    if (!*p) {
        return 0;
    }
    if (flags & FNM_LEADING_DIR) {
        return 0;
    }
    return FNM_NOMATCH;
}
