/* user_space/libc/src/strings.c - M89. See <strings.h>, including why
 * the case folding is ASCII-only on a system whose encoding is UTF-8. */
#include <strings.h>

#include <ctype.h>
#include <string.h>

int strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb || !ca) {
            return ca - cb;
        }
    }
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb || !ca) {
            return ca - cb;
        }
    }
    return 0;
}

char *strcasestr(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) {
        return (char *)haystack;
    }
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, n) == 0) {
            return (char *)haystack;
        }
    }
    return (char *)0;
}

void bzero(void *dst, size_t n) {
    memset(dst, 0, n);
}

void bcopy(const void *src, void *dst, size_t n) {
    /* The argument order is reversed from memcpy's, which is the entire
     * reason this deprecated function still catches people out. It also
     * has memmove's overlap guarantee rather than memcpy's. */
    memmove(dst, src, n);
}

int bcmp(const void *a, const void *b, size_t n) {
    return memcmp(a, b, n);
}

char *index(const char *s, int c) {
    return strchr(s, c);
}

char *rindex(const char *s, int c) {
    return strrchr(s, c);
}

int ffs(int v) {
    if (v == 0) {
        return 0;
    }
    int n = 1;
    while ((v & 1) == 0) {
        v = (int)((unsigned int)v >> 1);
        n++;
    }
    return n;
}
