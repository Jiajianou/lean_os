/* tools/gen-font.c — M39 font generator (host program, dev-time only)
 *
 * A *host* program (ordinary libc, built with the host's own cc, not the
 * x86_64-elf cross-compiler — the same "never runs as part of the OS"
 * arrangement tools/leanfs-put.c already uses). It emits all four font
 * files from the single glyph source below:
 *
 *     kernel/drivers/font8x16.{h,c}
 *     user_space/lib/font8x16.{h,c}
 *
 * Why this exists at all: user_space/lib/font8x16.h has carried "keep the
 * two tables in sync by hand if either ever changes" since M21, because a
 * user program can't link against the kernel image and the plain-data
 * glyph table has to be duplicated rather than shared. M39 re-authored
 * all 95 printable glyphs, which is exactly the change that warning was
 * about. The duplication is still there (that constraint hasn't changed);
 * what's gone is the hand-sync — both copies now come out of one array,
 * and `make font-check` fails the build if either checked-in file stops
 * matching what this program produces.
 *
 * Usage:
 *   gen-font --write   regenerate all four files in place
 *   gen-font --check   verify the checked-in files match (exit 1 if not)
 *
 * ---------------------------------------------------------------------
 * The metric (M39's actual point — the old table held none of this
 * consistently, which is why a line of text read as visually noisy even
 * though every glyph was blitted pixel-exactly):
 *
 *   row  0 ..  1   leading, always blank
 *   row  2         CAP_TOP    — top of uppercase, digits, ascenders
 *   row  5         X_TOP      — top of lowercase x-height
 *   row 11         last row of the glyph body
 *   row 12         BASELINE   — first row *below* the body; blank unless
 *                              the glyph descends
 *   row 14         DESC_LAST  — bottom of descenders (g j p q y)
 *   row 15         trailing, always blank
 *
 *   column 0 .. 6  the glyph itself
 *   column 7       ALWAYS BLANK — the advance gap
 *
 * That reserved column 7 is load-bearing twice over. It gives every
 * character uniform 1px letter spacing (gfx_draw_text advances exactly
 * FONT_WIDTH with no tracking of its own, so the gap has to live inside
 * the cell). And it makes the bold weight below lossless: M38's runtime
 * `bits | (bits >> 1)` thickened rightward *within the byte*, so ink
 * already sitting in column 7 shifted off the end and was silently
 * dropped — the old 'A', whose bottom rows were 0xE7, lost its
 * thickening on exactly the stem that most needed it. With column 7
 * reserved, nothing can fall off, and the dilation is done once here at
 * generation time instead of per-pixel on every redraw.
 *
 * check_metric() below enforces all of this, so a future edit that
 * breaks the shared baseline fails this program rather than quietly
 * shipping ragged text.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

#define CAP_TOP    2
#define X_TOP      5
#define BASELINE   12  /* first row below the glyph body */
#define DESC_LAST  14

#define GLYPH_COLS 7   /* column 7 is the reserved advance gap */

typedef struct {
    int         code;
    int         top;          /* row the first art string lands on */
    const char *rows[16];     /* NULL-terminated; '#' = ink, '.' = blank */
} glyph_src_t;

/* Uppercase, digits and ascenders: 10 rows, CAP_TOP..11.
 * Plain lowercase: 7 rows, X_TOP..11.
 * Descenders: 10 rows, X_TOP..DESC_LAST.
 * Punctuation carries its own explicit `top` — it has no single shared
 * rule, but check_metric() still holds it inside rows 0..14 / cols 0..6. */
static const glyph_src_t GLYPHS[] = {

/* ---- 0x20-0x2F ---- */
{ ' ', 0, { NULL } },
{ '!', CAP_TOP, { "...##..","...##..","...##..","...##..","...##..","...##..","...##..",".......","...##..","...##..", NULL } },
{ '"', CAP_TOP, { ".##.##.",".##.##.",".##.##.", NULL } },
{ '#', 4,       { ".##.##.",".##.##.","#######",".##.##.",".##.##.","#######",".##.##.",".##.##.", NULL } },
{ '$', 3,       { "...#...",".#####.","##.#.##","##.#...",".#####.","...#.##","##.#.##",".#####.","...#...", NULL } },
{ '%', CAP_TOP, { "##...##","##..##.","....##.","...##..","...##..","..##...","..##...",".##....",".##..##","##...##", NULL } },
{ '&', CAP_TOP, { "..###..",".##.##.",".##.##.","..###..",".####..","##..##.","##..##.","##...##","##..###",".###.##", NULL } },
{ '\'',CAP_TOP, { "...##..","...##..","...##..", NULL } },
{ '(', CAP_TOP, { "....##.","...##..","..##...","..##...","..##...","..##...","..##...","..##...","..##...","...##..","....##.", NULL } },
{ ')', CAP_TOP, { ".##....","..##...","...##..","...##..","...##..","...##..","...##..","...##..","...##..","..##...",".##....", NULL } },
{ '*', 4,       { "...#...","#..#..#",".#####.","..###..",".#####.","#..#..#","...#...", NULL } },
{ '+', X_TOP,   { "...##..","...##..","#######","#######","...##..","...##..","...##..", NULL } },
{ ',', 10,      { "..###..","..###..","...##..","..##...", NULL } },
{ '-', 7,       { ".#####.",".#####.", NULL } },
{ '.', 10,      { "...##..","...##..", NULL } },
{ '/', CAP_TOP, { ".....##",".....##","....##.","....##.","...##..","...##..","..##...","..##...",".##....",".##....","##.....", NULL } },

/* ---- digits ---- */
{ '0', CAP_TOP, { ".#####.","##...##","##...##","##..###","##.#.##","###..##","##...##","##...##","##...##",".#####.", NULL } },
{ '1', CAP_TOP, { "...##..","..###..",".####..","...##..","...##..","...##..","...##..","...##..","...##..",".#####.", NULL } },
{ '2', CAP_TOP, { ".#####.","##...##",".....##",".....##","....##.","...##..","..##...",".##....","##.....","#######", NULL } },
{ '3', CAP_TOP, { ".#####.","##...##",".....##",".....##","..####.",".....##",".....##",".....##","##...##",".#####.", NULL } },
{ '4', CAP_TOP, { "....##.","...###.","..####.",".##.##.","##..##.","#######","....##.","....##.","....##.","....##.", NULL } },
{ '5', CAP_TOP, { "#######","##.....","##.....","##.....","######.",".....##",".....##",".....##","##...##",".#####.", NULL } },
{ '6', CAP_TOP, { "..####.",".##..##","##.....","##.....","######.","##...##","##...##","##...##","##...##",".#####.", NULL } },
{ '7', CAP_TOP, { "#######","##...##",".....##","....##.","....##.","...##..","...##..","...##..","...##..","...##..", NULL } },
{ '8', CAP_TOP, { ".#####.","##...##","##...##","##...##",".#####.","##...##","##...##","##...##","##...##",".#####.", NULL } },
{ '9', CAP_TOP, { ".#####.","##...##","##...##","##...##","##...##",".######",".....##",".....##",".##..##","..####.", NULL } },

/* ---- 0x3A-0x40 ---- */
{ ':', 6,       { "...##..","...##..",".......",".......","...##..","...##..", NULL } },
{ ';', 6,       { "...##..","...##..",".......",".......","..###..","..###..","...##..","..##...", NULL } },
{ '<', X_TOP,   { ".....##","...##..",".##....","##.....",".##....","...##..",".....##", NULL } },
{ '=', 6,       { "#######","#######",".......","#######","#######", NULL } },
{ '>', X_TOP,   { "##.....","..##...","....##.",".....##","....##.","..##...","##.....", NULL } },
{ '?', CAP_TOP, { ".#####.","##...##",".....##","....##.","...##..","...##..","...##..",".......","...##..","...##..", NULL } },
{ '@', CAP_TOP, { ".#####.","##...##","##.###.","##.#.##","##.#.##","##.#.##","##.####","##.....",".##...#","..#####", NULL } },

/* ---- uppercase ---- */
{ 'A', CAP_TOP, { "...#...","..###..","..###..",".##.##.",".##.##.","##...##","#######","##...##","##...##","##...##", NULL } },
{ 'B', CAP_TOP, { "######.","##...##","##...##","##...##","######.","##...##","##...##","##...##","##...##","######.", NULL } },
{ 'C', CAP_TOP, { "..####.",".##..##","##...##","##.....","##.....","##.....","##.....","##...##",".##..##","..####.", NULL } },
{ 'D', CAP_TOP, { "#####..","##..##.","##...##","##...##","##...##","##...##","##...##","##...##","##..##.","#####..", NULL } },
{ 'E', CAP_TOP, { "#######","##.....","##.....","##.....","#####..","##.....","##.....","##.....","##.....","#######", NULL } },
{ 'F', CAP_TOP, { "#######","##.....","##.....","##.....","#####..","##.....","##.....","##.....","##.....","##.....", NULL } },
{ 'G', CAP_TOP, { "..####.",".##..##","##...##","##.....","##.....","##.####","##...##","##...##",".##..##","..####.", NULL } },
{ 'H', CAP_TOP, { "##...##","##...##","##...##","##...##","#######","##...##","##...##","##...##","##...##","##...##", NULL } },
{ 'I', CAP_TOP, { "..####.","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","..####.", NULL } },
{ 'J', CAP_TOP, { ".....##",".....##",".....##",".....##",".....##",".....##",".....##","##...##","##...##",".#####.", NULL } },
{ 'K', CAP_TOP, { "##...##","##..##.","##.##..","##.##..","####...","####...","##.##..","##..##.","##...##","##...##", NULL } },
{ 'L', CAP_TOP, { "##.....","##.....","##.....","##.....","##.....","##.....","##.....","##.....","##.....","#######", NULL } },
{ 'M', CAP_TOP, { "##...##","###.###","#######","##.#.##","##.#.##","##...##","##...##","##...##","##...##","##...##", NULL } },
{ 'N', CAP_TOP, { "##...##","###..##","###..##","##.#.##","##.#.##","##.#.##","##..###","##..###","##...##","##...##", NULL } },
{ 'O', CAP_TOP, { ".#####.","##...##","##...##","##...##","##...##","##...##","##...##","##...##","##...##",".#####.", NULL } },
{ 'P', CAP_TOP, { "######.","##...##","##...##","##...##","##...##","######.","##.....","##.....","##.....","##.....", NULL } },
{ 'Q', CAP_TOP, { ".#####.","##...##","##...##","##...##","##...##","##...##","##.#.##","##..###",".#####.",".....##", NULL } },
{ 'R', CAP_TOP, { "######.","##...##","##...##","##...##","##...##","######.","##.##..","##..##.","##...##","##...##", NULL } },
{ 'S', CAP_TOP, { ".#####.","##...##","##.....","##.....",".#####.",".....##",".....##",".....##","##...##",".#####.", NULL } },
{ 'T', CAP_TOP, { "#######","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..", NULL } },
{ 'U', CAP_TOP, { "##...##","##...##","##...##","##...##","##...##","##...##","##...##","##...##","##...##",".#####.", NULL } },
{ 'V', CAP_TOP, { "##...##","##...##","##...##","##...##","##...##",".##.##.",".##.##.",".##.##.","..###..","...#...", NULL } },
{ 'W', CAP_TOP, { "##...##","##...##","##...##","##...##","##...##","##.#.##","##.#.##","##.#.##","#######",".##.##.", NULL } },
{ 'X', CAP_TOP, { "##...##","##...##",".##.##.",".##.##.","..###..","..###..",".##.##.",".##.##.","##...##","##...##", NULL } },
{ 'Y', CAP_TOP, { "##...##","##...##",".##.##.",".##.##.","..###..","...##..","...##..","...##..","...##..","...##..", NULL } },
{ 'Z', CAP_TOP, { "#######",".....##","....##.","...##..","...##..","..##...",".##....","##.....","##.....","#######", NULL } },

/* ---- 0x5B-0x60 ---- */
{ '[', CAP_TOP, { "..####.","..##...","..##...","..##...","..##...","..##...","..##...","..##...","..##...","..##...","..####.", NULL } },
{ '\\',CAP_TOP, { "##.....","##.....",".##....",".##....","..##...","..##...","...##..","...##..","....##.","....##.",".....##", NULL } },
{ ']', CAP_TOP, { ".####..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..",".####..", NULL } },
{ '^', CAP_TOP, { "...#...","..###..",".##.##.","##...##", NULL } },
{ '_', 13,      { "#######","#######", NULL } },
{ '`', CAP_TOP, { ".##....","..##...","...##..", NULL } },

/* ---- lowercase ---- */
{ 'a', X_TOP,   { ".#####.",".....##",".#####.","##...##","##...##","##..###",".###.##", NULL } },
{ 'b', CAP_TOP, { "##.....","##.....","##.....","######.","##...##","##...##","##...##","##...##","##...##","######.", NULL } },
{ 'c', X_TOP,   { ".#####.","##...##","##.....","##.....","##.....","##...##",".#####.", NULL } },
{ 'd', CAP_TOP, { ".....##",".....##",".....##",".######","##...##","##...##","##...##","##...##","##...##",".######", NULL } },
{ 'e', X_TOP,   { ".#####.","##...##","##...##","#######","##.....","##...##",".#####.", NULL } },
{ 'f', CAP_TOP, { "...###.","..##...","..##...",".#####.","..##...","..##...","..##...","..##...","..##...","..##...", NULL } },
{ 'g', X_TOP,   { ".######","##...##","##...##","##...##","##...##",".######",".....##",".....##","##...##",".#####.", NULL } },
{ 'h', CAP_TOP, { "##.....","##.....","##.....","######.","##...##","##...##","##...##","##...##","##...##","##...##", NULL } },
{ 'i', CAP_TOP, { "...##..","...##..",".......","..###..","...##..","...##..","...##..","...##..","...##..",".#####.", NULL } },
{ 'j', CAP_TOP, { "...##..","...##..",".......","..###..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","##.##..",".###...", NULL } },
{ 'k', CAP_TOP, { "##.....","##.....","##.....","##..##.","##.##..","####...","####...","##.##..","##..##.","##...##", NULL } },
{ 'l', CAP_TOP, { "..###..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..",".#####.", NULL } },
{ 'm', X_TOP,   { "######.","#######","##.#.##","##.#.##","##.#.##","##.#.##","##.#.##", NULL } },
{ 'n', X_TOP,   { "######.","##...##","##...##","##...##","##...##","##...##","##...##", NULL } },
{ 'o', X_TOP,   { ".#####.","##...##","##...##","##...##","##...##","##...##",".#####.", NULL } },
{ 'p', X_TOP,   { "######.","##...##","##...##","##...##","##...##","##...##","######.","##.....","##.....","##.....", NULL } },
{ 'q', X_TOP,   { ".######","##...##","##...##","##...##","##...##","##...##",".######",".....##",".....##",".....##", NULL } },
{ 'r', X_TOP,   { "##.####","###....","##.....","##.....","##.....","##.....","##.....", NULL } },
{ 's', X_TOP,   { ".#####.","##...##","##.....",".#####.",".....##","##...##",".#####.", NULL } },
{ 't', 3,       { "..##...","..##...",".#####.","..##...","..##...","..##...","..##...","..##.##","...###.", NULL } },
{ 'u', X_TOP,   { "##...##","##...##","##...##","##...##","##...##","##..###",".###.##", NULL } },
{ 'v', X_TOP,   { "##...##","##...##","##...##",".##.##.",".##.##.","..###..","...#...", NULL } },
{ 'w', X_TOP,   { "##...##","##...##","##.#.##","##.#.##","##.#.##","#######",".##.##.", NULL } },
{ 'x', X_TOP,   { "##...##",".##.##.","..###..","...#...","..###..",".##.##.","##...##", NULL } },
{ 'y', X_TOP,   { "##...##","##...##","##...##","##...##","##...##","##..###",".###.##",".....##","##...##",".#####.", NULL } },
{ 'z', X_TOP,   { "#######","....##.","...##..","..##...",".##....","##.....","#######", NULL } },

/* ---- 0x7B-0x7E ---- */
{ '{', CAP_TOP, { "....##.","...##..","...##..","...##..","..##...","##.....","..##...","...##..","...##..","...##..","....##.", NULL } },
{ '|', CAP_TOP, { "...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..", NULL } },
{ '}', CAP_TOP, { ".##....","..##...","..##...","..##...","...##..",".....##","...##..","..##...","..##...","..##...",".##....", NULL } },
{ '~', 7,       { ".###.##","##.###.", NULL } },
};

#define NGLYPHS ((int)(sizeof(GLYPHS) / sizeof(GLYPHS[0])))

static unsigned char reg[128][FONT_HEIGHT];
static unsigned char bold[128][FONT_HEIGHT];

static int errors;

static void err(const char *fmt, int code, const char *detail) {
    fprintf(stderr, "gen-font: 0x%02X '%c': ", code,
            (code >= 0x20 && code < 0x7F) ? code : '?');
    fprintf(stderr, fmt, detail);
    fputc('\n', stderr);
    errors++;
}

static void build(void) {
    int seen[128] = { 0 };

    for (int g = 0; g < NGLYPHS; g++) {
        const glyph_src_t *src = &GLYPHS[g];
        int code = src->code;

        if (code < 0x20 || code > 0x7E) {
            err("outside the printable range this table covers%s", code, "");
            continue;
        }
        if (seen[code]) {
            err("defined twice%s", code, "");
            continue;
        }
        seen[code] = 1;

        for (int i = 0; src->rows[i]; i++) {
            const char *art = src->rows[i];
            int row = src->top + i;

            if (strlen(art) != GLYPH_COLS) {
                err("art row is not %s columns wide", code, "7");
                break;
            }
            if (row < 0 || row >= FONT_HEIGHT) {
                err("art runs past row 15%s", code, "");
                break;
            }
            for (int col = 0; col < GLYPH_COLS; col++) {
                if (art[col] == '#') {
                    reg[code][row] |= (unsigned char)(0x80u >> col);
                } else if (art[col] != '.') {
                    err("art uses a character other than '#' or '.'%s", code, "");
                    break;
                }
            }
        }
    }

    for (int code = 0x20; code <= 0x7E; code++) {
        if (!seen[code]) {
            err("no glyph defined%s", code, "");
        }
    }

    /* The bold weight: a one-column horizontal dilation, done once here
     * rather than per-pixel at draw time. Lossless because column 7 (bit
     * 0) is reserved blank in every glyph — check_metric() proves that,
     * and check_metric() proving it is the whole reason this is safe. */
    for (int code = 0; code < 128; code++) {
        for (int row = 0; row < FONT_HEIGHT; row++) {
            unsigned char bits = reg[code][row];
            bold[code][row] = (unsigned char)(bits | (bits >> 1));
        }
    }
}

static int ink_top(int code) {
    for (int r = 0; r < FONT_HEIGHT; r++) {
        if (reg[code][r]) return r;
    }
    return -1;
}

static int ink_bottom(int code) {
    for (int r = FONT_HEIGHT - 1; r >= 0; r--) {
        if (reg[code][r]) return r;
    }
    return -1;
}

static int in(const char *set, int code) { return strchr(set, code) != NULL; }

/* The metric this milestone exists to establish, enforced rather than
 * trusted. A future edit that puts a letter half a pixel off the shared
 * baseline fails the build here instead of quietly shipping. */
static void check_metric(void) {
    static const char *UPPER  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char *DIGITS = "0123456789";
    /* lowercase whose body is exactly the x-height band */
    static const char *XBAND  = "acemnorsuvwxz";
    /* lowercase that descends below the baseline */
    static const char *DESC   = "gpqyj";
    /* lowercase with an ascender or a dot reaching into the cap band */
    static const char *ASC    = "bdfhiklt";

    for (int code = 0; code < 128; code++) {
        for (int row = 0; row < FONT_HEIGHT; row++) {
            if (reg[code][row] & 0x01u) {
                err("ink in column 7, which is the reserved advance gap%s", code, "");
                break;
            }
        }
    }

    for (int code = 0; code < 0x20; code++) {
        if (ink_top(code) != -1) err("control code is not blank%s", code, "");
    }
    if (ink_top(0x20) != -1) err("space is not blank%s", 0x20, "");
    if (ink_top(0x7F) != -1) err("0x7F is not blank%s", 0x7F, "");

    for (int code = 0x21; code <= 0x7E; code++) {
        if (ink_top(code) == -1) err("printable glyph is blank%s", code, "");
        if (ink_bottom(code) > DESC_LAST) err("ink below the descender row%s", code, "");
    }

    for (int code = 0x21; code <= 0x7E; code++) {
        int top = ink_top(code), bot = ink_bottom(code);
        if (top < 0) continue;

        if (in(UPPER, code) || in(DIGITS, code)) {
            if (top != CAP_TOP)      err("does not start on the shared cap line (row 2)%s", code, "");
            if (bot != BASELINE - 1) err("does not sit on the shared baseline (row 11)%s", code, "");
        } else if (in(XBAND, code)) {
            if (top != X_TOP)        err("does not start on the shared x-height line (row 5)%s", code, "");
            if (bot != BASELINE - 1) err("does not sit on the shared baseline (row 11)%s", code, "");
        } else if (in(DESC, code)) {
            if (bot != DESC_LAST)    err("descender does not reach the shared descender row (row 14)%s", code, "");
        } else if (in(ASC, code)) {
            if (top != CAP_TOP && code != 't') err("ascender does not start on the shared cap line (row 2)%s", code, "");
            if (bot != BASELINE - 1) err("does not sit on the shared baseline (row 11)%s", code, "");
        }
    }
}

/* ------------------------------------------------------------------ */

static char out[1 << 20];
static size_t out_len;

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    out_len += (size_t)vsnprintf(out + out_len, sizeof(out) - out_len, fmt, ap);
    va_end(ap);
}

static void emit_table(const char *name, unsigned char t[128][FONT_HEIGHT]) {
    emit("const uint8_t %s[128][16] = {\n", name);
    for (int code = 0; code < 128; code++) {
        emit("    {");
        for (int row = 0; row < FONT_HEIGHT; row++) {
            emit(" 0x%02X%s", t[code][row], row == FONT_HEIGHT - 1 ? "" : ",");
        }
        if (code >= 0x20 && code < 0x7F) {
            emit(" }, /* 0x%02X '%c' */\n", code, code);
        } else {
            emit(" }, /* 0x%02X */\n", code);
        }
    }
    emit("};\n");
}

static const char *GENERATED_BANNER =
    " * GENERATED FILE - do not edit by hand.\n"
    " *\n"
    " * Produced by tools/gen-font.c (M39), which holds the one glyph\n"
    " * source both copies of this table come from and the metric checks\n"
    " * that keep them honest. Regenerate with `make font`; `make\n"
    " * font-check` (a prerequisite of the build) fails if this file\n"
    " * stops matching what that program produces.\n";

static void build_header(int kernel_side) {
    out_len = 0;
    emit("/* %s/font8x16.h\n *\n", kernel_side ? "kernel/drivers" : "user_space/lib");
    emit("%s", GENERATED_BANNER);
    emit(" *\n");
    if (kernel_side) {
        emit(" * Plain data, no kernel dependency at all - which is what lets\n"
             " * user_space/lib/font8x16.{h,c} carry a byte-identical copy of the\n"
             " * same table (a user program can't link against the kernel image).\n"
             " * Since M39 that copy is generated alongside this one rather than\n"
             " * kept in step by hand.\n");
    } else {
        emit(" * A byte-identical copy of kernel/drivers/font8x16.{h,c}'s glyph\n"
             " * tables - a user program can't link against the kernel image, so\n"
             " * this plain-data table (no kernel dependency at all) is duplicated\n"
             " * rather than shared, the same reasoning compositor.c's hand-authored\n"
             " * cursor sprite already used for the same kernel/user split.\n"
             " *\n"
             " * M39: the \"keep the two tables in sync by hand\" warning this header\n"
             " * used to carry is gone - both files now come out of tools/gen-font.c,\n"
             " * and the build refuses to proceed if they diverge. See gfx.h for the\n"
             " * user-space text-drawing API built on this.\n");
    }
    emit(" */\n");
    emit("#pragma once\n\n");
    emit("#include <stdint.h>\n\n");
    emit("#define FONT_WIDTH  8\n");
    emit("#define FONT_HEIGHT 16\n\n");
    emit("/* M39's shared metric, in rows of the 16-row cell. Exposed so callers\n"
         " * that need to reason about text geometry (and the boot-time self-test\n"
         " * that verifies it) use the same numbers gen-font.c enforced. */\n");
    emit("#define FONT_CAP_TOP   %d  /* top of uppercase, digits, ascenders */\n", CAP_TOP);
    emit("#define FONT_X_TOP     %d  /* top of the lowercase x-height band */\n", X_TOP);
    emit("#define FONT_BASELINE  %d  /* first row *below* the glyph body */\n", BASELINE);
    emit("#define FONT_DESC_LAST %d  /* bottom row of descenders (g j p q y) */\n\n", DESC_LAST);
    emit("/* Every glyph lives in columns 0-6; column 7 is always blank - the\n"
         " * advance gap that gives text uniform letter spacing (text drawing\n"
         " * advances exactly FONT_WIDTH with no tracking of its own) and that\n"
         " * makes font8x16_bold below lossless. */\n");
    emit("#define FONT_GLYPH_COLS %d\n\n", GLYPH_COLS);
    emit("/* One glyph per ASCII codepoint 0x00-0x7F, 16 rows of 8 pixels each (one\n"
         " * byte per row, MSB = leftmost pixel). Codepoints outside 0x20-0x7E\n"
         " * (control codes, 0x7F) are blank. */\n");
    emit("extern const uint8_t font8x16[128][16];\n\n");
    emit("/* The bold weight: font8x16 dilated one column to the right. Computed\n"
         " * once at generation time, not per-pixel at draw time - and lossless,\n"
         " * because column 7 is reserved blank so nothing shifts off the end.\n"
         " * (M38 did this smear at draw time inside the byte, which silently\n"
         " * dropped the thickening of any glyph that already had ink in column\n"
         " * 7 - exactly the stems that most needed it.) */\n");
    emit("extern const uint8_t font8x16_bold[128][16];\n");
}

static void build_source(int kernel_side) {
    out_len = 0;
    emit("/* %s/font8x16.c\n *\n", kernel_side ? "kernel/drivers" : "user_space/lib");
    emit("%s", GENERATED_BANNER);
    emit(" *\n");
    emit(" * Each row is one byte, MSB = leftmost pixel. See font8x16.h for the\n"
         " * shared metric every glyph below is authored against.\n");
    emit(" */\n");
    emit("#include \"font8x16.h\"\n\n");
    emit_table("font8x16", reg);
    emit("\n");
    emit_table("font8x16_bold", bold);
}

static int write_if(const char *path, int check_only) {
    FILE *f = fopen(path, check_only ? "rb" : "wb");
    if (!f) {
        fprintf(stderr, "gen-font: cannot open %s\n", path);
        return 1;
    }
    if (!check_only) {
        fwrite(out, 1, out_len, f);
        fclose(f);
        printf("gen-font: wrote %s (%zu bytes)\n", path, out_len);
        return 0;
    }

    static char have[1 << 20];
    size_t have_len = fread(have, 1, sizeof(have), f);
    fclose(f);

    if (have_len != out_len || memcmp(have, out, out_len) != 0) {
        size_t i = 0, line = 1;
        while (i < have_len && i < out_len && have[i] == out[i]) {
            if (have[i] == '\n') line++;
            i++;
        }
        fprintf(stderr,
                "gen-font: %s is out of date (first difference at line %zu).\n"
                "          This file is generated - edit tools/gen-font.c and run\n"
                "          `make font`, don't edit it by hand.\n", path, line);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int check_only;

    if (argc == 2 && strcmp(argv[1], "--check") == 0) {
        check_only = 1;
    } else if (argc == 2 && strcmp(argv[1], "--write") == 0) {
        check_only = 0;
    } else {
        fprintf(stderr, "usage: %s --write | --check\n", argv[0]);
        return 2;
    }

    build();
    check_metric();
    if (errors) {
        fprintf(stderr, "gen-font: %d glyph/metric error(s) - refusing to emit.\n", errors);
        return 1;
    }

    int bad = 0;
    build_header(1); bad |= write_if("kernel/drivers/font8x16.h", check_only);
    build_source(1); bad |= write_if("kernel/drivers/font8x16.c", check_only);
    build_header(0); bad |= write_if("user_space/lib/font8x16.h", check_only);
    build_source(0); bad |= write_if("user_space/lib/font8x16.c", check_only);

    if (bad) return 1;
    if (check_only) printf("gen-font: all four font files match the generator.\n");
    return 0;
}
