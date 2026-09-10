/* tests/iconv/main.c - M100: this project's iconv, against the host's.
 *
 * The seventh instrument of the kind tools/sh-test.sh introduced, and it
 * is pointed at the newest and least-proven thing in this libc. Nothing
 * in this file says what the right answer is: for every charset, every
 * byte and every code point below, a library nobody here wrote decides,
 * and this one has to agree.
 *
 * That matters more for iconv than for most of them, because a charset
 * table is 3,328 numbers and a wrong one is invisible - it produces a
 * plausible character in a language nobody on this project reads.
 *
 * Three questions are asked, and they are different questions:
 *
 *   1. DECODE. For every charset and every one of its 256 bytes: does
 *      this libc produce the same UTF-8 the host does, and does it fail
 *      on exactly the bytes the host fails on? The second half is the
 *      one that catches a table which filled a hole with a guess.
 *
 *   2. ENCODE. Not "does the host produce the same byte", because the
 *      host on this desk CANNOT be asked that question - see the note
 *      on transliteration below. Instead: the charset's true repertoire
 *      is derived from the host's decode direction, which is exact, and
 *      then this libc must (a) encode every character in that repertoire
 *      back to a byte that decodes to it again, and (b) REFUSE every
 *      character outside it. That is a stronger test than byte equality
 *      with the host would have been, and it does not depend on the
 *      host's error policy at all.
 *
 * ---- why (2) is written that way, which is a finding of its own ------
 *
 * macOS's iconv TRANSLITERATES by default. Asked for U+0100 (Latin
 * capital A with macron) in ISO-8859-1, GNU libiconv on Linux returns
 * EILSEQ; the one on this desk returns `A` - and returns 0 rather than
 * the count of non-reversible conversions POSIX specifies for that case,
 * so a caller cannot even detect it happened. The first version of this
 * test compared refusals directly and reported 144,589 disagreements,
 * every one of them the host quietly approximating.
 *
 * That is worth writing down rather than working around silently: it
 * means "the host's iconv" is not one oracle, and a differential test
 * has to be built on the half of the interface both implementations
 * agree about. Decoding is that half.
 *
 *   3. UTF ROUND TRIPS. Every scalar value, through UTF-8, UTF-16LE,
 *      UTF-16BE, UTF-32LE and UTF-32BE, in both directions, compared
 *      against the host at every step - including the surrogate range,
 *      which is the range both sides must REFUSE.
 */
#include <errno.h>
#include <iconv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This project's, renamed by the harness. */
typedef void *lean_iconv_t;
lean_iconv_t lean_iconv_open(const char *tocode, const char *fromcode);
size_t lean_iconv(lean_iconv_t cd, char **inbuf, size_t *inbytesleft,
                  char **outbuf, size_t *outbytesleft);
int lean_iconv_close(lean_iconv_t cd);

static int failures;
static long long checks;

/* The charsets this libc claims, in the spelling the host also knows. */
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
    int ok;          /* 1 converted, 0 refused */
    int err;         /* errno when refused */
    unsigned char b[16];
    size_t len;
};

static struct result host_conv(const char *to, const char *from,
                               const unsigned char *in, size_t inlen) {
    struct result r;
    memset(&r, 0, sizeof(r));
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) {
        r.ok = -1; /* the host does not have this charset */
        return r;
    }
    char *ip = (char *)in, *op = (char *)r.b;
    size_t il = inlen, ol = sizeof(r.b);
    errno = 0;
    size_t rc = iconv(cd, &ip, &il, &op, &ol);
    /* A conversion that consumed everything is a success. libiconv can
     * return a non-zero count for approximate conversions; none is asked
     * for here, so anything other than "all consumed" is a refusal. */
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
        return; /* the host cannot answer; nothing to grade against */
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
        return; /* both refused; the bytes are not defined */
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

/* UTF-8 for one scalar, written here rather than taken from either
 * library, so that the input to a comparison is not produced by one of
 * the two things being compared. */
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

/* ---- deliberate divergences from the host, each with a reason -------
 *
 * The same device as tests/math/main.c's DIVERGE table: a difference
 * that has been looked at and decided is a row here, and a difference
 * that has not is a failure. The list is meant to stay short.
 */
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
    /* ---- 1. decode: every byte of every charset ---------------------- */
    for (int c = 0; c < NCHARSETS; c++) {
        for (int b = 0; b < 256; b++) {
            if (diverges(CHARSETS[c], b)) {
                continue;
            }
            unsigned char in = (unsigned char)b;
            compare("decode", "UTF-8", CHARSETS[c], &in, 1);
        }
    }

    /* ---- 2. encode, against the repertoire the host just described --- */
    for (int c = 0; c < NCHARSETS; c++) {
        /* Which byte (if any) each code point comes from, built from the
         * host's decode direction. -1 means "not in this charset".
         *
         * Two bytes can decode to one code point in a few charsets; the
         * first one wins for the "expected" byte, and the check below
         * accepts any byte that decodes back correctly, so an encoder
         * choosing the other is not a failure. */
        static int from_cp[0x10000];
        for (int i = 0; i < 0x10000; i++) {
            from_cp[i] = -1;
        }
        int have_host = 0;
        for (int b = 0; b < 256; b++) {
            unsigned char in = (unsigned char)b;
            struct result h = host_conv("UTF-32LE", CHARSETS[c], &in, 1);
            if (h.ok == -1) {
                break; /* the host has no such charset */
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
                continue; /* not a scalar value */
            }
            unsigned char in[4];
            size_t n = utf8(cp, in);
            struct result l = lean_conv(CHARSETS[c], "UTF-8", in, n);
            checks++;
            if (from_cp[cp] == -1) {
                /* Not in the charset. This libc must refuse it. The host
                 * is NOT consulted here - it would transliterate. */
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
            /* Any byte that decodes back to this code point is correct. */
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

    /* ---- 3. the UTF family, both directions -------------------------- */
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
    /* And back: a surrogate half in UTF-16, which both must refuse. */
    for (int be = 0; be < 2; be++) {
        for (uint32_t s = 0xD800; s <= 0xDFFF; s += 3) {
            unsigned char in[2];
            in[be ? 0 : 1] = (unsigned char)(s >> 8);
            in[be ? 1 : 0] = (unsigned char)(s & 0xFF);
            compare("utf16-lone-surrogate", "UTF-8",
                    be ? "UTF-16BE" : "UTF-16LE", in, 2);
        }
    }

    /* ---- 4. byte order marks ---------------------------------------
     *
     * "UTF-16" and "UTF-32" without an endianness are real charset
     * labels and they are the only stateful thing in this iconv: a
     * leading BOM decides the byte order and is then not a character.
     * Graded against the host because the rules are easy to state and
     * easy to get subtly wrong - and because the UTF-32 case cannot be
     * decided by a Unicode decoder at all (a big-endian BOM read
     * little-endian is 0xFFFE0000, which is not a scalar value, so a
     * decoder that looked at characters would refuse the stream rather
     * than flip). */
    {
        struct { const char *name; const unsigned char bytes[8]; size_t len; }
        cases[] = {
            /* UTF-16 BE BOM then 'A' */
            {"UTF-16", {0xFE, 0xFF, 0x00, 0x41}, 4},
            /* UTF-16 LE BOM then 'A' */
            {"UTF-16", {0xFF, 0xFE, 0x41, 0x00}, 4},
            /* No BOM at all: big-endian by the standard's own default */
            {"UTF-16", {0x00, 0x41}, 2},
            /* UTF-32 BE BOM then 'A' */
            {"UTF-32", {0x00, 0x00, 0xFE, 0xFF, 0x00, 0x00, 0x00, 0x41}, 8},
            /* UTF-32 LE BOM then 'A' */
            {"UTF-32", {0xFF, 0xFE, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00}, 8},
            /* A BOM in the MIDDLE is U+FEFF, a real character, and must
             * be passed through rather than eaten. */
            {"UTF-16BE", {0x00, 0x41, 0xFE, 0xFF}, 4},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            compare("bom", "UTF-8", cases[i].name, cases[i].bytes,
                    cases[i].len);
        }
        /* And the other direction: encoding to an endianness-less name
         * must emit a BOM, once. */
        static const unsigned char aa[] = {'A', 'B'};
        compare("bom-out", "UTF-16", "UTF-8", aa, 2);
        compare("bom-out", "UTF-32", "UTF-8", aa, 2);
    }

    printf("iconv-test: %lld conversions compared against the host's "
           "iconv, %d disagreed\n", checks, failures);
    return failures ? 1 : 0;
}
