/* This libc's <uchar.h> against Python's own encoders, over every code
   point Unicode has.

   char32_t is a code point and char16_t is UTF-16, so the interesting part
   is the one the two encodings disagree about: everything above the basic
   plane is one char32_t and two char16_t, and the pair has to be produced
   and consumed across calls through state the caller keeps. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uchar.h>
#include <wchar.h>

static int failures = 0;
static long cases = 0;
static long above_the_basic_plane = 0;

static void report(unsigned long code_point, const char *what) {
    if (failures < 20) {
        fprintf(stderr, "uchar-test: U+%04lX %s\n", code_point, what);
    }
    failures++;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t unhex(const char *text, unsigned char *out, size_t max) {
    size_t n = 0;
    while (n < max) {
        int high = hex_digit(text[n * 2]);
        int low = hex_digit(text[n * 2 + 1]);
        if (high < 0 || low < 0) {
            break;
        }
        out[n++] = (unsigned char)((high << 4) | low);
    }
    return n;
}

static void one_case(unsigned long code_point, const unsigned char *utf8,
                     size_t utf8_length, const unsigned char *utf16,
                     size_t utf16_length) {
    cases++;
    size_t units = utf16_length / 2;
    if (units == 2) {
        above_the_basic_plane++;
    }

    /* UTF-8 in, char16_t out. */
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    char16_t produced[2];
    size_t produced_count = 0;
    size_t consumed = 0;
    for (;;) {
        char16_t unit = 0;
        size_t r = mbrtoc16(&unit, (const char *)utf8 + consumed,
                            utf8_length - consumed, &state);
        if (r == (size_t)-3) {
            if (produced_count >= 2) {
                report(code_point, "mbrtoc16 produced a third unit");
                return;
            }
            produced[produced_count++] = unit;
            continue;
        }
        if (r == (size_t)-1 || r == (size_t)-2) {
            report(code_point, "mbrtoc16 rejected its own UTF-8");
            return;
        }
        if (produced_count >= 2) {
            report(code_point, "mbrtoc16 produced a third unit");
            return;
        }
        produced[produced_count++] = unit;
        consumed += (r == 0) ? 1 : r;
        if (consumed >= utf8_length) {
            break;
        }
    }
    if (consumed != utf8_length) {
        report(code_point, "mbrtoc16 consumed the wrong number of bytes");
        return;
    }
    /* A surrogate pair leaves the low half waiting; ask for it. */
    if (produced_count < units) {
        char16_t unit = 0;
        size_t r = mbrtoc16(&unit, (const char *)utf8, 0, &state);
        if (r != (size_t)-3) {
            report(code_point, "mbrtoc16 did not hand back the low surrogate");
            return;
        }
        produced[produced_count++] = unit;
    }
    if (produced_count != units) {
        report(code_point, "mbrtoc16 produced the wrong number of units");
        return;
    }
    for (size_t i = 0; i < units; i++) {
        unsigned expected =
            (unsigned)utf16[i * 2] | ((unsigned)utf16[i * 2 + 1] << 8);
        if ((unsigned)produced[i] != expected) {
            report(code_point, "mbrtoc16 produced the wrong unit");
            return;
        }
    }

    /* char16_t in, UTF-8 out. */
    memset(&state, 0, sizeof(state));
    unsigned char bytes[8];
    size_t written = 0;
    for (size_t i = 0; i < units; i++) {
        char16_t unit =
            (char16_t)((unsigned)utf16[i * 2] |
                       ((unsigned)utf16[i * 2 + 1] << 8));
        char scratch[8];
        size_t r = c16rtomb(scratch, unit, &state);
        if (r == (size_t)-1) {
            report(code_point, "c16rtomb refused a unit it had just made");
            return;
        }
        if (written + r > sizeof(bytes)) {
            report(code_point, "c16rtomb wrote too much");
            return;
        }
        memcpy(bytes + written, scratch, r);
        written += r;
    }
    if (written != utf8_length || memcmp(bytes, utf8, utf8_length) != 0) {
        report(code_point, "c16rtomb produced the wrong UTF-8");
        return;
    }

    /* char32_t, both ways. */
    memset(&state, 0, sizeof(state));
    char32_t wide = 0;
    size_t r = mbrtoc32(&wide, (const char *)utf8, utf8_length, &state);
    if (r == (size_t)-1 || r == (size_t)-2) {
        report(code_point, "mbrtoc32 rejected its own UTF-8");
        return;
    }
    if ((unsigned long)wide != code_point) {
        report(code_point, "mbrtoc32 produced the wrong code point");
        return;
    }
    memset(&state, 0, sizeof(state));
    char scratch[8];
    size_t back = c32rtomb(scratch, (char32_t)code_point, &state);
    if (back != utf8_length || memcmp(scratch, utf8, utf8_length) != 0) {
        report(code_point, "c32rtomb produced the wrong UTF-8");
        return;
    }
}

/* The cases Python cannot express, because they are not characters. */
static void refusals(void) {
    mbstate_t state;
    char scratch[8];

    /* A low surrogate on its own is not a character and must be refused. */
    memset(&state, 0, sizeof(state));
    errno = 0;
    if (c16rtomb(scratch, (char16_t)0xDC00u, &state) != (size_t)-1 ||
        errno != EILSEQ) {
        fprintf(stderr, "uchar-test: c16rtomb accepted a lone low surrogate\n");
        failures++;
    }

    /* A high surrogate followed by something that is not a low one. */
    memset(&state, 0, sizeof(state));
    if (c16rtomb(scratch, (char16_t)0xD800u, &state) != 0) {
        fprintf(stderr, "uchar-test: c16rtomb wrote bytes for a high "
                        "surrogate on its own\n");
        failures++;
    }
    errno = 0;
    if (c16rtomb(scratch, (char16_t)0x0041u, &state) != (size_t)-1 ||
        errno != EILSEQ) {
        fprintf(stderr, "uchar-test: c16rtomb accepted an unpaired high "
                        "surrogate\n");
        failures++;
    }

    /* An incomplete UTF-8 sequence is "not yet", not "never". */
    memset(&state, 0, sizeof(state));
    char16_t unit = 0;
    if (mbrtoc16(&unit, "\xF0\x9F", 2, &state) != (size_t)-2) {
        fprintf(stderr, "uchar-test: mbrtoc16 did not report an incomplete "
                        "sequence as incomplete\n");
        failures++;
    }

    /* And a byte that can never start one is. */
    memset(&state, 0, sizeof(state));
    errno = 0;
    if (mbrtoc16(&unit, "\xFF", 1, &state) != (size_t)-1) {
        fprintf(stderr, "uchar-test: mbrtoc16 accepted 0xFF\n");
        failures++;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: uchar-test <cases file>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "uchar-test: cannot open %s\n", argv[1]);
        return 2;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *space = strchr(line, ' ');
        if (!space) {
            continue;
        }
        unsigned long code_point = strtoul(line, NULL, 16);
        char *utf8_text = space + 1;
        char *second = strchr(utf8_text, ' ');
        if (!second) {
            continue;
        }
        unsigned char utf8[8];
        unsigned char utf16[8];
        size_t utf8_length = unhex(utf8_text, utf8, sizeof(utf8));
        size_t utf16_length = unhex(second + 1, utf16, sizeof(utf16));
        if (utf8_length == 0 || utf16_length == 0) {
            continue;
        }
        one_case(code_point, utf8, utf8_length, utf16, utf16_length);
    }
    fclose(f);

    refusals();

    if (cases < 1000000) {
        fprintf(stderr, "uchar-test: only %ld cases - the generator did not "
                        "produce the whole of Unicode\n", cases);
        return 1;
    }
    if (above_the_basic_plane < 900000) {
        fprintf(stderr, "uchar-test: only %ld surrogate pairs - a run with "
                        "none would pass with the pairing broken\n",
                above_the_basic_plane);
        return 1;
    }
    printf("uchar-test: %ld code points, %ld of them a surrogate pair, "
           "against Python's own encoders\n", cases, above_the_basic_plane);
    return failures == 0 ? 0 : 1;
}
