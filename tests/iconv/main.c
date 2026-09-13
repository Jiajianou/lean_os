#include <errno.h>
#include <iconv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *lean_iconv_t;
lean_iconv_t lean_iconv_open(const char *tocode, const char *fromcode);
size_t lean_iconv(lean_iconv_t cd, char **inbuf, size_t *inbytesleft,
                  char **outbuf, size_t *outbytesleft);
int lean_iconv_close(lean_iconv_t cd);

static int failures;
static long long checks;

static const char *CHARSETS[] = {
    "US-ASCII", "ISO-8859-1",
    "ISO-8859-2", "ISO-8859-3", "ISO-8859-4", "ISO-8859-5",
    "ISO-8859-6", "ISO-8859-7", "ISO-8859-8", "ISO-8859-9",
    "ISO-8859-10", "ISO-8859-11", "ISO-8859-13", "ISO-8859-14",
    "ISO-8859-15", "ISO-8859-16",
    "WINDOWS-1250", "WINDOWS-1251", "WINDOWS-1252", "WINDOWS-1253",
    "WINDOWS-1254", "WINDOWS-1255", "WINDOWS-1256", "WINDOWS-1257",
    "WINDOWS-1258",
    "KOI8-R", "KOI8-U", "MACINTOSH",
};
#define NCHARSETS ((int)(sizeof(CHARSETS) / sizeof(CHARSETS[0])))

struct result {
    int ok;
    int err;
    unsigned char b[16];
    size_t len;
};

static struct result host_conv(const char *to, const char *from,
                               const unsigned char *in, size_t inlen) {
    struct result r;
    memset(&r, 0, sizeof(r));
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) {
        r.ok = -1;
        return r;
    }
    char *ip = (char *)in, *op = (char *)r.b;
    size_t il = inlen, ol = sizeof(r.b);
    errno = 0;
    size_t rc = iconv(cd, &ip, &il, &op, &ol);
    r.ok = (rc != (size_t)-1 && il == 0);
    r.err = errno;
    r.len = sizeof(r.b) - ol;
    iconv_close(cd);
    return r;
}

static struct result lean_conv(const char *to, const char *from,
                               const unsigned char *in, size_t inlen) {
    struct result r;
    memset(&r, 0, sizeof(r));
    lean_iconv_t cd = lean_iconv_open(to, from);
    if (cd == (lean_iconv_t)-1) {
        r.ok = -1;
        return r;
    }
    char *ip = (char *)in, *op = (char *)r.b;
    size_t il = inlen, ol = sizeof(r.b);
    errno = 0;
    size_t rc = lean_iconv(cd, &ip, &il, &op, &ol);
    r.ok = (rc != (size_t)-1 && il == 0);
    r.err = errno;
    r.len = sizeof(r.b) - ol;
    lean_iconv_close(cd);
    return r;
}

static void compare(const char *what, const char *to, const char *from,
                    const unsigned char *in, size_t inlen) {
    struct result h = host_conv(to, from, in, inlen);
    if (h.ok == -1) {
        return;
    }
    struct result l = lean_conv(to, from, in, inlen);
    checks++;
    if (l.ok == -1) {
        if (failures++ < 20) {
            printf("FAIL %s %s->%s: this libc has no such charset pair\n",
                   what, from, to);
        }
        return;
    }
    if (l.ok != h.ok) {
        if (failures++ < 20) {
            printf("FAIL %s %s->%s in=", what, from, to);
            for (size_t i = 0; i < inlen; i++) {
                printf("%02X", in[i]);
            }
            printf(": ours %s, the host's %s\n",
                   l.ok ? "converted" : "refused",
                   h.ok ? "converted" : "refused");
        }
        return;
    }
    if (!h.ok) {
        return;
    }
    if (l.len != h.len || memcmp(l.b, h.b, h.len) != 0) {
        if (failures++ < 20) {
            printf("FAIL %s %s->%s in=", what, from, to);
            for (size_t i = 0; i < inlen; i++) {
                printf("%02X", in[i]);
            }
            printf(" ours=");
            for (size_t i = 0; i < l.len; i++) {
                printf("%02X", l.b[i]);
            }
            printf(" host=");
            for (size_t i = 0; i < h.len; i++) {
                printf("%02X", h.b[i]);
            }
            printf("\n");
        }
    }
}

static size_t utf8(uint32_t cp, unsigned char *out) {
    if (cp < 0x80) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xC0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xE0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

static const struct {
    const char *charset;
    int byte;
    const char *why;
} DIVERGE[] = {
    {"MACINTOSH", 0x7F,
     "Apple's own table leaves 0x7F (DEL) unmapped; Python's mac_roman, "
     "and every ISO-8859 charset, map it to U+007F. Both readings are "
     "defensible and this one is the one that does not lose a control "
     "character on a round trip."},
};

static int diverges(const char *charset, int byte) {
    for (size_t i = 0; i < sizeof(DIVERGE) / sizeof(DIVERGE[0]); i++) {
        if (strcmp(DIVERGE[i].charset, charset) == 0 &&
            DIVERGE[i].byte == byte) {
            return 1;
        }
    }
    return 0;
}

int main(void) {
    for (int c = 0; c < NCHARSETS; c++) {
        for (int b = 0; b < 256; b++) {
            if (diverges(CHARSETS[c], b)) {
                continue;
            }
            unsigned char in = (unsigned char)b;
            compare("decode", "UTF-8", CHARSETS[c], &in, 1);
        }
    }

    for (int c = 0; c < NCHARSETS; c++) {
        static int from_cp[0x10000];
        for (int i = 0; i < 0x10000; i++) {
            from_cp[i] = -1;
        }
        int have_host = 0;
        for (int b = 0; b < 256; b++) {
            unsigned char in = (unsigned char)b;
            struct result h = host_conv("UTF-32LE", CHARSETS[c], &in, 1);
            if (h.ok == -1) {
                break;
            }
            have_host = 1;
            if (!h.ok || h.len != 4) {
                continue;
            }
            uint32_t cp = (uint32_t)h.b[0] | ((uint32_t)h.b[1] << 8) |
                          ((uint32_t)h.b[2] << 16) | ((uint32_t)h.b[3] << 24);
            if (cp < 0x10000 && from_cp[cp] == -1) {
                from_cp[cp] = b;
            }
        }
        if (!have_host) {
            continue;
        }

        for (uint32_t cp = 0; cp < 0x10000; cp++) {
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                continue;
            }
            unsigned char in[4];
            size_t n = utf8(cp, in);
            struct result l = lean_conv(CHARSETS[c], "UTF-8", in, n);
            checks++;
            if (from_cp[cp] == -1) {
                if (l.ok) {
                    if (diverges(CHARSETS[c], l.len == 1 ? l.b[0] : -1)) {
                        continue;
                    }
                    if (failures++ < 20) {
                        printf("FAIL encode %s U+%04X: this libc produced "
                               "%02X for a character the charset does not "
                               "have\n", CHARSETS[c], cp, l.b[0]);
                    }
                }
                continue;
            }
            if (!l.ok) {
                if (diverges(CHARSETS[c], from_cp[cp])) {
                    continue;
                }
                if (failures++ < 20) {
                    printf("FAIL encode %s U+%04X: this libc refused a "
                           "character the host decodes from byte %02X\n",
                           CHARSETS[c], cp, from_cp[cp]);
                }
                continue;
            }
            if (l.len != 1) {
                if (failures++ < 20) {
                    printf("FAIL encode %s U+%04X: %zu bytes from a "
                           "single-byte charset\n", CHARSETS[c], cp, l.len);
                }
                continue;
            }
            unsigned char got = l.b[0];
            struct result back = host_conv("UTF-32LE", CHARSETS[c], &got, 1);
            uint32_t rt = back.ok && back.len == 4
                              ? ((uint32_t)back.b[0] |
                                 ((uint32_t)back.b[1] << 8) |
                                 ((uint32_t)back.b[2] << 16) |
                                 ((uint32_t)back.b[3] << 24))
                              : 0xFFFFFFFFu;
            if (rt != cp) {
                if (failures++ < 20) {
                    printf("FAIL encode %s U+%04X: this libc wrote %02X, "
                           "which the host decodes as U+%04X\n",
                           CHARSETS[c], cp, got, rt);
                }
            }
        }
    }

    static const char *UTFS[] = {"UTF-8", "UTF-16LE", "UTF-16BE",
                                 "UTF-32LE", "UTF-32BE"};
    for (int u = 0; u < 5; u++) {
        for (uint32_t cp = 0; cp <= 0x10FFFF; cp += 7) {
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                continue;
            }
            unsigned char in[4];
            size_t n = utf8(cp, in);
            compare("utf-out", UTFS[u], "UTF-8", in, n);
        }
    }
    for (int be = 0; be < 2; be++) {
        for (uint32_t s = 0xD800; s <= 0xDFFF; s += 3) {
            unsigned char in[2];
            in[be ? 0 : 1] = (unsigned char)(s >> 8);
            in[be ? 1 : 0] = (unsigned char)(s & 0xFF);
            compare("utf16-lone-surrogate", "UTF-8",
                    be ? "UTF-16BE" : "UTF-16LE", in, 2);
        }
    }

    {
        struct { const char *name; const unsigned char bytes[8]; size_t len; }
        cases[] = {
            {"UTF-16", {0xFE, 0xFF, 0x00, 0x41}, 4},
            {"UTF-16", {0xFF, 0xFE, 0x41, 0x00}, 4},
            {"UTF-16", {0x00, 0x41}, 2},
            {"UTF-32", {0x00, 0x00, 0xFE, 0xFF, 0x00, 0x00, 0x00, 0x41}, 8},
            {"UTF-32", {0xFF, 0xFE, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00}, 8},
            {"UTF-16BE", {0x00, 0x41, 0xFE, 0xFF}, 4},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            compare("bom", "UTF-8", cases[i].name, cases[i].bytes,
                    cases[i].len);
        }
        static const unsigned char aa[] = {'A', 'B'};
        compare("bom-out", "UTF-16", "UTF-8", aa, 2);
        compare("bom-out", "UTF-32", "UTF-8", aa, 2);
    }

    printf("iconv-test: %lld conversions compared against the host's "
           "iconv, %d disagreed\n", checks, failures);
    return failures ? 1 : 0;
}
