/* user_space/libc/src/fnmatch.c - M89
 *
 * Shell wildcard matching. See <fnmatch.h> for why this is a different
 * and smaller language than <regex.h>'s.
 *
 * The algorithm is backtracking, and the shape of it is the one that
 * matters: a naive recursive `*` case is exponential on a pattern like
 * `a*a*a*a*b` against a string of `a`s, which is a real denial of
 * service for a program matching untrusted names. The loop below records
 * the last `*` and the position after the character it matched, and
 * resumes from there on a mismatch - the standard linear-space
 * backtracking that keeps `*` from nesting.
 */
#include <fnmatch.h>

#include <ctype.h>
#include <string.h>

/* Matches one bracket expression against `c`, starting at *pp which
 * points just past the '['. Advances *pp past the ']' and returns 1 on a
 * match, 0 on no match, -1 if the expression is unterminated (in which
 * case the '[' is a literal, which is what every shell does). */
static int match_bracket(const char **pp, char c, int fold) {
    const char *p = *pp;
    int negate = 0;
    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }
    int matched = 0;
    /* A ']' immediately after the (possibly negated) '[' is a literal
     * ']' rather than the end of the expression - POSIX, and every shell
     * agrees. */
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
        return -1; /* unterminated - the caller treats '[' as literal */
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

    /* FNM_PERIOD: a leading '.' is matched only literally. Checked here
     * for the start of the string, and after each '/' below when
     * FNM_PATHNAME is on - which together are the rule that keeps `*`
     * from matching a dotfile. */
    if ((flags & FNM_PERIOD) && *s == '.' && *p != '.') {
        return FNM_NOMATCH;
    }

    while (*s) {
        char pc = *p;
        if (pc == '*') {
            /* Collapse a run of stars; they mean the same as one. */
            while (*p == '*') {
                p++;
            }
            if (!*p) {
                /* Trailing star. With FNM_PATHNAME it still may not
                 * cross a '/', so the rest of the string must have
                 * none. */
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
                goto literal; /* unterminated '[' is an ordinary character */
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
                /* A '/' just matched: re-apply FNM_PERIOD to the next
                 * component, which is what keeps `*/ /*` from finding
                 * `a/.hidden`. */
                if (pathname && (flags & FNM_PERIOD) && *s == '/' && s[1] == '.' && p[1] != '.') {
                    return FNM_NOMATCH;
                }
                p++;
                s++;
                continue;
            }
        }
backtrack:
        /* No match here. If a '*' is open, let it swallow one more
         * character and resume - unless that character is a '/' and
         * FNM_PATHNAME forbids crossing one. */
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
    /* The string is exhausted; the pattern must be too, modulo stars. */
    while (*p == '*') {
        p++;
    }
    if (!*p) {
        return 0;
    }
    if (flags & FNM_LEADING_DIR) {
        return 0; /* the pattern matched a leading directory of the string */
    }
    return FNM_NOMATCH;
}
