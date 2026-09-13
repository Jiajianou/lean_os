#include <string.h>
#include <errno.h>
#include <stdlib.h>

void *memmove(void *destination, const void *source, size_t n) {
    unsigned char *d = (unsigned char *)destination;
    const unsigned char *s = (const unsigned char *)source;
    if (d == s || n == 0) {
        return destination;
    }
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return destination;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) {
            return (void *)(p + i);
        }
    }
    return (void *)0;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y) {
            return (int)x - (int)y;
        }
        if (x == 0) {
            return 0;
        }
    }
    return 0;
}

char *strcpy(char *destination, const char *source) {
    char *out = destination;
    while ((*destination++ = *source++)) {
    }
    return out;
}

char *strncpy(char *destination, const char *source, size_t n) {
    size_t i = 0;
    for (; i < n && source[i]; i++) {
        destination[i] = source[i];
    }
    for (; i < n; i++) {
        destination[i] = '\0';
    }
    return destination;
}

char *strcat(char *destination, const char *source) {
    char *out = destination;
    while (*destination) {
        destination++;
    }
    while ((*destination++ = *source++)) {
    }
    return out;
}

char *strncat(char *destination, const char *source, size_t n) {
    char *out = destination;
    while (*destination) {
        destination++;
    }
    size_t i = 0;
    for (; i < n && source[i]; i++) {
        destination[i] = source[i];
    }
    destination[i] = '\0';
    return out;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
        if (*s == '\0') {
            return (char *)0;
        }
    }
}

char *strrchr(const char *s, int c) {
    const char *found = (const char *)0;
    for (;; s++) {
        if (*s == (char)c) {
            found = s;
        }
        if (*s == '\0') {
            return (char *)found;
        }
    }
}

char *strstr(const char *haystack, const char *needle) {
    if (!needle[0]) {
        return (char *)haystack;
    }
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (needle[i] && h[i] == needle[i]) {
            i++;
        }
        if (!needle[i]) {
            return (char *)h;
        }
    }
    return (char *)0;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    for (; s[n]; n++) {
        if (!strchr(accept, s[n])) {
            break;
        }
    }
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    for (; s[n]; n++) {
        if (strchr(reject, s[n])) {
            break;
        }
    }
    return n;
}

char *strerror(int errnum) {
    static const struct {
        int code;
        const char *name;
    } NAMES[] = {
        {1, "EPERM"},   {2, "ENOENT"},  {3, "ESRCH"},   {4, "EINTR"},
        {5, "EIO"},     {9, "EBADF"},   {10, "ECHILD"}, {11, "EAGAIN"},
        {12, "ENOMEM"}, {13, "EACCES"}, {14, "EFAULT"}, {16, "EBUSY"},
        {17, "EEXIST"}, {20, "ENOTDIR"},{21, "EISDIR"}, {22, "EINVAL"},
        {23, "ENFILE"}, {24, "EMFILE"}, {28, "ENOSPC"}, {29, "ESPIPE"},
        {32, "EPIPE"},  {33, "EDOM"},   {34, "ERANGE"}, {36, "ENAMETOOLONG"},
        {38, "ENOSYS"}, {39, "ENOTEMPTY"},
    };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (NAMES[i].code == errnum) {
            return (char *)NAMES[i].name;
        }
    }
    return (char *)"error";
}

int strerror_r(int errnum, char *buffer, size_t buflen) {
    if (!buffer || buflen == 0) {
        return ERANGE;
    }
    const char *name = strerror(errnum);
    size_t length = strlen(name);
    if (length + 1 > buflen) {
        for (size_t i = 0; i + 1 < buflen; i++) {
            buffer[i] = name[i];
        }
        buffer[buflen - 1] = '\0';
        return ERANGE;
    }
    for (size_t i = 0; i <= length; i++) {
        buffer[i] = name[i];
    }
    return 0;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) {
                return (char *)s;
            }
        }
    }
    return (char *)0;
}

char *strdup(const char *s) {
    if (!s) {
        return (char *)0;
    }
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (!out) {
        return (char *)0;
    }
    memcpy(out, s, n);
    return out;
}

char *strndup(const char *s, size_t n) {
    if (!s) {
        return (char *)0;
    }
    size_t length = 0;
    while (length < n && s[length]) {
        length++;
    }
    char *p = (char *)malloc(length + 1);
    if (!p) {
        return (char *)0;
    }
    memcpy(p, s, length);
    p[length] = '\0';
    return p;
}

char *stpcpy(char *destination, const char *source) {
    while ((*destination = *source) != '\0') {
        destination++;
        source++;
    }
    return destination;
}

char *stpncpy(char *destination, const char *source, size_t n) {
    size_t i = 0;
    while (i < n && source[i]) {
        destination[i] = source[i];
        i++;
    }
    char *end = destination + i;
    while (i < n) {
        destination[i++] = '\0';
    }
    return end;
}

void *memmem(const void *haystack, size_t hlen, const void *needle,
             size_t nlen) {
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    if (nlen == 0) {
        return (void *)h;
    }
    if (nlen > hlen) {
        return (void *)0;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, nlen) == 0) {
            return (void *)(h + i);
        }
    }
    return (void *)0;
}

void *memccpy(void *destination, const void *source, int c, size_t n) {
    unsigned char *d = (unsigned char *)destination;
    const unsigned char *s = (const unsigned char *)source;
    unsigned char stop = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
        if (s[i] == stop) {
            return d + i + 1;
        }
    }
    return (void *)0;
}

size_t strnlen(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) {
        i++;
    }
    return i;
}

char *strtok_r(char *s, const char *delim, char **saveptr) {
    if (!s) {
        s = *saveptr;
    }
    if (!s) {
        return (char *)0;
    }
    while (*s && strchr(delim, *s)) {
        s++;
    }
    if (!*s) {
        *saveptr = (char *)0;
        return (char *)0;
    }
    char *token = s;
    while (*s && !strchr(delim, *s)) {
        s++;
    }
    if (*s) {
        *s = '\0';
        *saveptr = s + 1;
    } else {
        *saveptr = (char *)0;
    }
    return token;
}

char *strtok(char *s, const char *delim) {
    static char *saved;
    return strtok_r(s, delim, &saved);
}

int strcoll(const char *a, const char *b) {
    return strcmp(a, b);
}

size_t strxfrm(char *destination, const char *source, size_t n) {
    size_t length = strlen(source);
    if (n > 0) {
        size_t copy = length < n - 1 ? length : n - 1;
        for (size_t i = 0; i < copy; i++) {
            destination[i] = source[i];
        }
        destination[copy] = '\0';
    }
    return length;
}
