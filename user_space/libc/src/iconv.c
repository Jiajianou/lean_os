/* user_space/libc/src/iconv.c - M100: charset conversion.
 *
 * NetSurf named this: utils/utf8.c includes <iconv.h> unconditionally,
 * and a browser that cannot read a windows-1252 page is a browser that
 * cannot read most of the pages that predate 2010.
 *
 * ---- the shape, and why it is this shape -----------------------------
 *
 * Every conversion goes through Unicode scalar values, which turns N
 * charsets into N decoders and N encoders rather than N^2 conversions.
 * That is how every iconv is built and it is worth saying why it is
 * right *here*: the alternative pays off only for pairs that share a
 * structure, and the pairs a browser actually asks for (anything to
 * UTF-8) share none.
 *
 * The single-byte charsets are a generated table - see iconv_tables.c
 * and tools/gen-iconv-tables.py - and the multibyte ones are code,
 * because UTF-8 and UTF-16 are algorithms rather than tables.
 *
 * ---- the part that is easy to get wrong ------------------------------
 *
 * On any failure, `iconv` must leave *inbuf and *inbytesleft pointing at
 * the first byte it did NOT convert. A loop that decoded a character,
 * then found no room for it, and had already advanced the input, would
 * lose that character silently on the caller's retry. So the input
 * cursor advances only after the output has been written, and the
 * decoder reports how many bytes a character occupied rather than
 * consuming them itself.
 */
#include <errno.h>
#include <iconv.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "iconv_tables.h"

/* The multibyte encodings, plus the two that need no table. */
enum enc {
    ENC_UTF8,
    ENC_UTF16LE,
    ENC_UTF16BE,
    ENC_UTF32LE,
    ENC_UTF32BE,
    ENC_ASCII,
    ENC_LATIN1,
    ENC_SB /* one of the generated tables; `tbl` says which */
};

struct iconv_cd {
    enum enc from, to;
    const uint16_t *from_tbl, *to_tbl;
    /* ---- byte order marks -------------------------------------------
     *
     * UTF-16 and UTF-32 spelled WITHOUT an endianness carry one.
     * Decoding such a stream means reading a leading BOM, letting it
     * choose the endianness, and then not treating it as a character;
     * encoding one means emitting a BOM before the first character and
     * never again.
     *
     * `from_initial` exists because decoding can CHANGE `from` - a
     * little-endian BOM on a stream opened as "UTF-16" flips it - and
     * iconv(cd, NULL, ...) has to put the descriptor back the way it
     * was. Without it a reset would leave the endianness the last
     * stream happened to have. */
    int from_has_bom, to_has_bom;
    int from_bom_pending, to_bom_pending;
    enum enc from_initial;
};

#define REPLACEMENT_UNASSIGNED 0xFFFFu

/* ---- name matching --------------------------------------------------
 *
 * Case-insensitive and ignoring '-' and '_', because the charset labels
 * in real documents are written every way a person can write them and
 * "ISO-8859-1", "iso8859-1" and "ISO_8859_1" are one charset in every
 * standard that mentions the question.
 */
static int name_eq(const char *a, const char *b) {
    while (*a && *b) {
        while (*a == '-' || *a == '_') {
            a++;
        }
        while (*b == '-' || *b == '_') {
            b++;
        }
        if (!*a || !*b) {
            break;
        }
        char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    while (*a == '-' || *a == '_') {
        a++;
    }
    while (*b == '-' || *b == '_') {
        b++;
    }
    return *a == 0 && *b == 0;
}

struct alias {
    const char *name;
    enum enc enc;
    int bom; /* 1: no endianness in the name, so a BOM decides */
    enum enc dflt; /* what to assume when there is no BOM */
};

/* The aliases that appear in real HTML and in real HTTP headers. The
 * list is short on purpose: an alias nobody has seen in the wild is a
 * name this libc would have to keep working forever. */
static const struct alias ALIASES[] = {
    {"utf-8", ENC_UTF8, 0, ENC_UTF8},
    {"utf8", ENC_UTF8, 0, ENC_UTF8},
    {"csutf8", ENC_UTF8, 0, ENC_UTF8},
    /* Unicode without an endianness: big-endian unless a BOM says
     * otherwise, which is what the Unicode standard specifies and what
     * catches people out, since the common file on disk is little. */
    {"utf-16", ENC_UTF16BE, 1, ENC_UTF16BE},
    {"utf-16le", ENC_UTF16LE, 0, ENC_UTF16LE},
    {"utf-16be", ENC_UTF16BE, 0, ENC_UTF16BE},
    {"ucs-2", ENC_UTF16BE, 1, ENC_UTF16BE},
    {"ucs-2le", ENC_UTF16LE, 0, ENC_UTF16LE},
    {"ucs-2be", ENC_UTF16BE, 0, ENC_UTF16BE},
    {"utf-32", ENC_UTF32BE, 1, ENC_UTF32BE},
    {"utf-32le", ENC_UTF32LE, 0, ENC_UTF32LE},
    {"utf-32be", ENC_UTF32BE, 0, ENC_UTF32BE},
    {"ucs-4", ENC_UTF32BE, 1, ENC_UTF32BE},
    {"ucs-4le", ENC_UTF32LE, 0, ENC_UTF32LE},
    {"ucs-4be", ENC_UTF32BE, 0, ENC_UTF32BE},
    {"us-ascii", ENC_ASCII, 0, ENC_ASCII},
    {"ascii", ENC_ASCII, 0, ENC_ASCII},
    {"ansi_x3.4-1968", ENC_ASCII, 0, ENC_ASCII},
    {"iso-8859-1", ENC_LATIN1, 0, ENC_LATIN1},
    {"latin1", ENC_LATIN1, 0, ENC_LATIN1},
    {"iso-latin-1", ENC_LATIN1, 0, ENC_LATIN1},
    {"cp819", ENC_LATIN1, 0, ENC_LATIN1},
    {"iso-8859-15", ENC_SB, 0, ENC_SB}, /* also in the table; see below */
};

/* Resolves a charset name. Returns 0 if this libc does not have it. */
static int lookup(const char *name, enum enc *enc, const uint16_t **tbl,
                  int *bom, enum enc *dflt) {
    *tbl = 0;
    *bom = 0;
    /* The aliases first, so that "iso-8859-1" is the identity path
     * rather than a table lookup - it is much the most common charset
     * after UTF-8 and it needs no table at all. */
    for (size_t i = 0; i < sizeof(ALIASES) / sizeof(ALIASES[0]); i++) {
        if (ALIASES[i].enc != ENC_SB && name_eq(name, ALIASES[i].name)) {
            *enc = ALIASES[i].enc;
            *bom = ALIASES[i].bom;
            *dflt = ALIASES[i].dflt;
            return 1;
        }
    }
    for (size_t i = 0; i < __iconv_sb_charset_count; i++) {
        if (name_eq(name, __iconv_sb_charsets[i].name)) {
            *enc = ENC_SB;
            *tbl = __iconv_sb_charsets[i].high;
            *dflt = ENC_SB;
            return 1;
        }
    }
    /* A few single-byte charsets go by a second name often enough to be
     * worth carrying. Kept here rather than in the generated table so
     * that regenerating the table never has to preserve them. */
    static const struct { const char *alias, *real; } SB_ALIASES[] = {
        {"latin2", "iso-8859-2"},
        {"latin9", "iso-8859-15"},
        {"cp1250", "windows-1250"},
        {"cp1251", "windows-1251"},
        {"cp1252", "windows-1252"},
        {"cp1253", "windows-1253"},
        {"cp1254", "windows-1254"},
        {"cp1255", "windows-1255"},
        {"cp1256", "windows-1256"},
        {"cp1257", "windows-1257"},
        {"cp1258", "windows-1258"},
        {"mac", "macintosh"},
        {"x-mac-roman", "macintosh"},
    };
    for (size_t i = 0; i < sizeof(SB_ALIASES) / sizeof(SB_ALIASES[0]); i++) {
        if (name_eq(name, SB_ALIASES[i].alias)) {
            return lookup(SB_ALIASES[i].real, enc, tbl, bom, dflt);
        }
    }
    return 0;
}

iconv_t iconv_open(const char *tocode, const char *fromcode) {
    if (!tocode || !fromcode) {
        errno = EINVAL;
        return (iconv_t)-1;
    }
    /* GNU's //TRANSLIT and //IGNORE change what the call MEANS, so
     * quietly ignoring the suffix would answer a different question
     * from the one asked. See <iconv.h>. */
    if (strstr(tocode, "//") || strstr(fromcode, "//")) {
        errno = EINVAL;
        return (iconv_t)-1;
    }
    struct iconv_cd cd;
    memset(&cd, 0, sizeof(cd));
    int fbom = 0, tbom = 0;
    enum enc fdflt = ENC_UTF8, tdflt = ENC_UTF8;
    if (!lookup(fromcode, &cd.from, &cd.from_tbl, &fbom, &fdflt) ||
        !lookup(tocode, &cd.to, &cd.to_tbl, &tbom, &tdflt)) {
        errno = EINVAL;
        return (iconv_t)-1;
    }
    cd.from_has_bom = fbom;
    cd.from_bom_pending = fbom;
    cd.from_initial = cd.from;
    cd.to_has_bom = tbom;
    cd.to_bom_pending = tbom;
    (void)fdflt;
    (void)tdflt;

    struct iconv_cd *out = malloc(sizeof(*out));
    if (!out) {
        errno = ENOMEM;
        return (iconv_t)-1;
    }
    *out = cd;
    return (iconv_t)out;
}

int iconv_close(iconv_t cd) {
    if (cd == (iconv_t)-1 || !cd) {
        errno = EBADF;
        return -1;
    }
    free(cd);
    return 0;
}

/* ---- decoding -------------------------------------------------------
 *
 * Reports the scalar value in *cp and the number of bytes it occupied as
 * the return value; 0 means "the input ends mid-character" (EINVAL) and
 * -1 means "this is not valid in this charset" (EILSEQ). It never
 * advances anything itself, for the reason in the file header.
 */
static int decode(struct iconv_cd *c, const unsigned char *in, size_t len,
                  uint32_t *cp) {
    switch (c->from) {
    case ENC_ASCII:
        if (in[0] > 0x7F) {
            return -1;
        }
        *cp = in[0];
        return 1;
    case ENC_LATIN1:
        /* ISO-8859-1 is the identity on the first 256 scalars, which is
         * the whole reason it needs no table. */
        *cp = in[0];
        return 1;
    case ENC_SB: {
        if (in[0] < 0x80) {
            *cp = in[0];
            return 1;
        }
        uint16_t v = c->from_tbl[in[0] - 0x80];
        if (v == REPLACEMENT_UNASSIGNED) {
            return -1; /* a real hole in the charset, not a stray byte */
        }
        *cp = v;
        return 1;
    }
    case ENC_UTF8: {
        unsigned char b = in[0];
        int n;
        uint32_t v;
        if (b < 0x80) {
            *cp = b;
            return 1;
        } else if ((b & 0xE0) == 0xC0) {
            n = 2; v = b & 0x1Fu;
        } else if ((b & 0xF0) == 0xE0) {
            n = 3; v = b & 0x0Fu;
        } else if ((b & 0xF8) == 0xF0) {
            n = 4; v = b & 0x07u;
        } else {
            return -1; /* a continuation byte or an 0xF8+ lead */
        }
        if (len < (size_t)n) {
            return 0;
        }
        for (int i = 1; i < n; i++) {
            if ((in[i] & 0xC0) != 0x80) {
                return -1;
            }
            v = (v << 6) | (in[i] & 0x3Fu);
        }
        /* Overlong forms are rejected. This is not pedantry: an overlong
         * encoding of '/' or '.' that a decoder accepted is the classic
         * way a path check is walked past, and a browser is exactly the
         * program that gets handed one on purpose. */
        static const uint32_t MIN[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (v < MIN[n] || v > 0x10FFFF ||
            (v >= 0xD800 && v <= 0xDFFF)) {
            return -1;
        }
        *cp = v;
        return n;
    }
    case ENC_UTF16LE:
    case ENC_UTF16BE: {
        int be = c->from == ENC_UTF16BE;
        if (len < 2) {
            return 0;
        }
        uint32_t u = be ? (uint32_t)((in[0] << 8) | in[1])
                        : (uint32_t)((in[1] << 8) | in[0]);
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (len < 4) {
                return 0;
            }
            uint32_t lo = be ? (uint32_t)((in[2] << 8) | in[3])
                             : (uint32_t)((in[3] << 8) | in[2]);
            if (lo < 0xDC00 || lo > 0xDFFF) {
                return -1; /* a high surrogate with no low one after it */
            }
            *cp = 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
            return 4;
        }
        if (u >= 0xDC00 && u <= 0xDFFF) {
            return -1; /* a low surrogate on its own */
        }
        *cp = u;
        return 2;
    }
    default: { /* ENC_UTF32LE / ENC_UTF32BE */
        int be = c->from == ENC_UTF32BE;
        if (len < 4) {
            return 0;
        }
        uint32_t v = be ? ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                              ((uint32_t)in[2] << 8) | in[3]
                        : ((uint32_t)in[3] << 24) | ((uint32_t)in[2] << 16) |
                              ((uint32_t)in[1] << 8) | in[0];
        if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
            return -1;
        }
        *cp = v;
        return 4;
    }
    }
}

/* Writes `cp` in the target charset. Returns the number of bytes
 * written, 0 if the output buffer is too small (E2BIG), or -1 if this
 * charset cannot represent the character (EILSEQ). Nothing is written
 * unless the whole character fits. */
static int encode(struct iconv_cd *c, uint32_t cp, unsigned char *out,
                  size_t room) {
    switch (c->to) {
    case ENC_ASCII:
        if (cp > 0x7F) {
            return -1;
        }
        if (room < 1) {
            return 0;
        }
        out[0] = (unsigned char)cp;
        return 1;
    case ENC_LATIN1:
        if (cp > 0xFF) {
            return -1;
        }
        if (room < 1) {
            return 0;
        }
        out[0] = (unsigned char)cp;
        return 1;
    case ENC_SB: {
        if (cp < 0x80) {
            if (room < 1) {
                return 0;
            }
            out[0] = (unsigned char)cp;
            return 1;
        }
        if (cp > 0xFFFF) {
            return -1;
        }
        /* U+FFFF is this table's sentinel for "this byte is unassigned
         * in this charset", and it is also a real (if noncharacter)
         * code point. Without this line, encoding U+FFFF matched the
         * first hole in the table and produced that hole's byte - so
         * `iconv -t ISO-8859-3` turned U+FFFF into 0xA5, which decodes
         * back to nothing at all. tools/iconv-test.sh found it in
         * thirteen charsets at once, which is what a sentinel sharing a
         * value with real data looks like from the outside. */
        if (cp == REPLACEMENT_UNASSIGNED) {
            return -1;
        }
        /* A linear scan of 128 entries. The alternative is a reverse
         * table per charset - 26 more tables, to save 64 comparisons on
         * a path that is already one branch per character. Measured
         * nowhere, so not optimised; see CLAUDE.md on that. */
        for (int i = 0; i < 128; i++) {
            if (c->to_tbl[i] == (uint16_t)cp) {
                if (room < 1) {
                    return 0;
                }
                out[0] = (unsigned char)(0x80 + i);
                return 1;
            }
        }
        return -1;
    }
    case ENC_UTF8: {
        int n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (room < (size_t)n) {
            return 0;
        }
        switch (n) {
        case 1:
            out[0] = (unsigned char)cp;
            break;
        case 2:
            out[0] = (unsigned char)(0xC0 | (cp >> 6));
            out[1] = (unsigned char)(0x80 | (cp & 0x3F));
            break;
        case 3:
            out[0] = (unsigned char)(0xE0 | (cp >> 12));
            out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            out[2] = (unsigned char)(0x80 | (cp & 0x3F));
            break;
        default:
            out[0] = (unsigned char)(0xF0 | (cp >> 18));
            out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
            out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            out[3] = (unsigned char)(0x80 | (cp & 0x3F));
            break;
        }
        return n;
    }
    case ENC_UTF16LE:
    case ENC_UTF16BE: {
        int be = c->to == ENC_UTF16BE;
        if (cp >= 0x10000) {
            if (room < 4) {
                return 0;
            }
            uint32_t v = cp - 0x10000;
            uint32_t hi = 0xD800 + (v >> 10), lo = 0xDC00 + (v & 0x3FF);
            out[be ? 0 : 1] = (unsigned char)(hi >> 8);
            out[be ? 1 : 0] = (unsigned char)(hi & 0xFF);
            out[be ? 2 : 3] = (unsigned char)(lo >> 8);
            out[be ? 3 : 2] = (unsigned char)(lo & 0xFF);
            return 4;
        }
        if (room < 2) {
            return 0;
        }
        out[be ? 0 : 1] = (unsigned char)(cp >> 8);
        out[be ? 1 : 0] = (unsigned char)(cp & 0xFF);
        return 2;
    }
    default: { /* UTF-32 */
        int be = c->to == ENC_UTF32BE;
        if (room < 4) {
            return 0;
        }
        out[be ? 0 : 3] = (unsigned char)(cp >> 24);
        out[be ? 1 : 2] = (unsigned char)((cp >> 16) & 0xFF);
        out[be ? 2 : 1] = (unsigned char)((cp >> 8) & 0xFF);
        out[be ? 3 : 0] = (unsigned char)(cp & 0xFF);
        return 4;
    }
    }
}

size_t iconv(iconv_t cdp, char **inbuf, size_t *inbytesleft,
             char **outbuf, size_t *outbytesleft) {
    struct iconv_cd *c = (struct iconv_cd *)cdp;
    if (!c || cdp == (iconv_t)-1) {
        errno = EBADF;
        return (size_t)-1;
    }
    if (!inbuf || !*inbuf) {
        /* "Reset to the initial state". No charset here has a shift
         * state, so the whole of that state is the BOM bookkeeping - and
         * the endianness, which a BOM on the previous stream may have
         * flipped. */
        c->from = c->from_initial;
        c->from_bom_pending = c->from_has_bom;
        c->to_bom_pending = c->to_has_bom;
        return 0;
    }

    unsigned char *in = (unsigned char *)*inbuf;
    unsigned char *out = outbuf ? (unsigned char *)*outbuf : 0;
    size_t inleft = *inbytesleft;
    size_t outleft = outbytesleft ? *outbytesleft : 0;

    /* A BOM on the way out goes first, and only once. It is written
     * before the loop so that a full output buffer reports E2BIG with
     * nothing consumed, rather than after a character has gone. */
    if (c->to_bom_pending) {
        int n = encode(c, 0xFEFF, out, outleft);
        if (n == 0) {
            errno = E2BIG;
            return (size_t)-1;
        }
        out += n;
        outleft -= (size_t)n;
        c->to_bom_pending = 0;
        *outbuf = (char *)out;
        *outbytesleft = outleft;
    }

    /* ---- the leading BOM, read as BYTES rather than as a character ---
     *
     * This has to happen before decode() sees them, and the reason is
     * UTF-32: a big-endian stream opened little-endian starts with the
     * bytes 00 00 FE FF, which reads as 0xFFFE0000 - larger than
     * U+10FFFF, so the decoder correctly refuses it as an invalid
     * scalar value and the stream fails with EILSEQ instead of simply
     * being the other endianness. Deciding the byte order is not a
     * question a Unicode decoder can answer, because the bytes are not
     * Unicode yet. */
    if (c->from_bom_pending) {
        int is16 = c->from == ENC_UTF16LE || c->from == ENC_UTF16BE;
        int is32 = c->from == ENC_UTF32LE || c->from == ENC_UTF32BE;
        size_t need = is32 ? 4 : 2;
        if ((is16 || is32) && inleft >= need) {
            int be = 0, found = 0;
            if (is16) {
                if (in[0] == 0xFE && in[1] == 0xFF) { be = 1; found = 1; }
                else if (in[0] == 0xFF && in[1] == 0xFE) { be = 0; found = 1; }
            } else {
                if (in[0] == 0 && in[1] == 0 && in[2] == 0xFE && in[3] == 0xFF) {
                    be = 1; found = 1;
                } else if (in[0] == 0xFF && in[1] == 0xFE && in[2] == 0 &&
                           in[3] == 0) {
                    be = 0; found = 1;
                }
            }
            if (found) {
                c->from = is16 ? (be ? ENC_UTF16BE : ENC_UTF16LE)
                               : (be ? ENC_UTF32BE : ENC_UTF32LE);
                in += need;
                inleft -= need;
                *inbuf = (char *)in;
                *inbytesleft = inleft;
            }
            /* Either way the question has been asked once. A stream with
             * no BOM keeps the endianness its name implied, which for
             * "UTF-16" is big - what the Unicode standard specifies, and
             * what catches people out, since the common file on disk is
             * little. */
            c->from_bom_pending = 0;
        } else if (!is16 && !is32) {
            c->from_bom_pending = 0; /* nothing else carries one */
        }
        /* Too few bytes to tell yet: leave it pending. The loop below
         * will report EINVAL, the caller will come back with more, and
         * this runs again. */
    }

    while (inleft > 0) {
        uint32_t cp;
        int used = decode(c, in, inleft, &cp);
        if (used == 0) {
            errno = EINVAL; /* ends mid-character */
            return (size_t)-1;
        }
        if (used < 0) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        int wrote = encode(c, cp, out, outleft);
        if (wrote == 0) {
            errno = E2BIG;
            return (size_t)-1;
        }
        if (wrote < 0) {
            errno = EILSEQ; /* the target charset has no such character */
            return (size_t)-1;
        }
        /* Both cursors move only now, together - see the file header. */
        in += used;
        inleft -= (size_t)used;
        out += wrote;
        outleft -= (size_t)wrote;
        *inbuf = (char *)in;
        *inbytesleft = inleft;
        *outbuf = (char *)out;
        *outbytesleft = outleft;
    }
    return 0; /* nothing here converts approximately, so never non-zero */
}
