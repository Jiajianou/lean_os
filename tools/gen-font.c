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


/* ==================================================================
 * M57: the proportional UI font family.
 *
 * Everything above this line is M39's 8x16 monospace cell, and it is
 * staying exactly as it is: the kernel console draws with it during a
 * panic, and gui_terminal.c and text_editor.c both do genuine column
 * arithmetic over a fixed cell. Two fonts with two jobs.
 *
 * What follows is the other job - chrome, labels, menus, buttons,
 * filenames - where a fixed eight-pixel advance is simply wrong. Three
 * sizes, all on M39's shared cap/x-height/baseline discipline so they
 * read as one family:
 *
 *   ui_font_small   12-row cell, 1px stems   - dense lists
 *   ui_font_ui      16-row cell, 2px stems   - chrome and labels (default)
 *   ui_font_large   24-row cell              - headings and dialog titles
 *
 * How each one is produced, and why:
 *
 *   small   Hand-authored below (UI_SM_GLYPHS). A 12-row cell has no
 *           room for a 2px stem at a readable x-height, so this is a
 *           genuinely different drawing rather than a scaled one - and
 *           it is where the proportional metric shows most: '.' and '!'
 *           are one column wide, 'i' and 'l' three, 'M' and 'W' seven.
 *
 *   ui      Derived from M39's own glyphs, trimmed to their ink and
 *           given a per-glyph advance of width + 1. M39 authored those
 *           glyphs inside columns 0..6 with column 7 reserved blank as
 *           the advance gap; a variable advance is exactly what replaces
 *           that reservation, so the trim *is* the conversion. A short
 *           override table (UI_UI_OVERRIDE) re-authors the handful of
 *           glyphs the 7-column box distorted most - 'M', 'W', 'm' and
 *           'w' get the eighth column back, 'c', 'e', 's' and 'r' give
 *           one up.
 *
 *   large   A 3/2 nearest-neighbour scale of `ui`, which is exact for
 *           this design rather than approximate: every feature in it is
 *           a 2-pixel run, and under a 3/2 map every 2-run becomes a
 *           3-run no matter where it starts. Scaling a bitmap font is
 *           usually how you get mush; here the ratio was picked so that
 *           it cannot be.
 *
 * The specials (UI_SPECIAL_*, codepoints 0x01-0x08) are the glyphs the
 * UI has been faking with hand-drawn rectangles and spelled-out words
 * since M35: an arrow each way, a checkmark, a bullet, an ellipsis and
 * a close X. They live in the control-code range, which is blank in
 * every text this OS draws, so no real string can collide with them.
 * ================================================================== */

#define UI_MAX_COLS   16   /* one row is a uint16_t, MSB = leftmost column */
#define UI_MAX_ROWS   24

typedef struct {
    int         code;
    int         top;              /* row the first art string lands on */
    int         width;            /* columns; every art row must be this wide */
    const char *rows[UI_MAX_ROWS + 1];
} ui_glyph_src_t;

typedef struct {
    const char    *ident;         /* C identifier suffix */
    int            height;
    int            cap_top;
    int            x_top;
    int            baseline;      /* first row below the glyph body */
    int            desc_last;
    int            space_advance;
    int            has_bold;
    unsigned short rows[128][UI_MAX_ROWS];
    unsigned short bold[128][UI_MAX_ROWS];
    unsigned char  width[128];
    unsigned char  advance[128];
} uifont_t;

/* ---- the specials, authored once per authored size ---------------- */

#define UI_SPECIAL_ARROW_LEFT  0x01
#define UI_SPECIAL_ARROW_RIGHT 0x02
#define UI_SPECIAL_ARROW_UP    0x03
#define UI_SPECIAL_ARROW_DOWN  0x04
#define UI_SPECIAL_CHECK       0x05
#define UI_SPECIAL_BULLET      0x06
#define UI_SPECIAL_ELLIPSIS    0x07
#define UI_SPECIAL_CLOSE       0x08
#define UI_SPECIAL_FIRST       UI_SPECIAL_ARROW_LEFT
#define UI_SPECIAL_LAST        UI_SPECIAL_CLOSE

/* 16-row cell: body rows 2..11, so a 9-row triangle at top 3 sits on the
 * same baseline the letters do - which is the whole point of putting
 * these in the font instead of drawing them by hand next to it. */
static const ui_glyph_src_t UI_SPECIALS_16[] = {
{ UI_SPECIAL_ARROW_LEFT,  3, 5, { "....#","...##","..###",".####","#####",".####","..###","...##","....#", NULL } },
{ UI_SPECIAL_ARROW_RIGHT, 3, 5, { "#....","##...","###..","####.","#####","####.","###..","##...","#....", NULL } },
{ UI_SPECIAL_ARROW_UP,    5, 9, { "....#....","...###...","..#####..",".#######.","#########", NULL } },
{ UI_SPECIAL_ARROW_DOWN,  5, 9, { "#########",".#######.","..#####..","...###...","....#....", NULL } },
{ UI_SPECIAL_CHECK,       6, 8, { "......##",".....##.","....##..","#..##...","####....",".###....", NULL } },
{ UI_SPECIAL_BULLET,      7, 4, { ".##.","####","####",".##.", NULL } },
{ UI_SPECIAL_ELLIPSIS,   10,10, { "##..##..##","##..##..##", NULL } },
{ UI_SPECIAL_CLOSE,       4, 8, { "##....##","###..###",".######.","..####..","..####..",".######.","###..###","##....##", NULL } },
};

/* 12-row cell: body rows 1..8, 1px strokes to match the letters. */
static const ui_glyph_src_t UI_SPECIALS_12[] = {
{ UI_SPECIAL_ARROW_LEFT,  2, 4, { "...#","..##",".###","####",".###","..##","...#", NULL } },
{ UI_SPECIAL_ARROW_RIGHT, 2, 4, { "#...","##..","###.","####","###.","##..","#...", NULL } },
{ UI_SPECIAL_ARROW_UP,    4, 7, { "...#...","..###..",".#####.","#######", NULL } },
{ UI_SPECIAL_ARROW_DOWN,  4, 7, { "#######",".#####.","..###..","...#...", NULL } },
{ UI_SPECIAL_CHECK,       4, 6, { ".....#","....#.","...#..","#.#...",".#....", NULL } },
{ UI_SPECIAL_BULLET,      5, 3, { ".#.","###",".#.", NULL } },
{ UI_SPECIAL_ELLIPSIS,    8, 7, { "#..#..#", NULL } },
{ UI_SPECIAL_CLOSE,       3, 6, { "#....#",".#..#.","..##..","..##..",".#..#.","#....#", NULL } },
};

/* ---- the 16-row overrides ----------------------------------------- */

static const ui_glyph_src_t UI_UI_OVERRIDE[] = {
/* the four M39 squeezed into seven columns */
{ 'M', CAP_TOP, 8, { "##....##","###..###","########","##.##.##","##.##.##","##....##","##....##","##....##","##....##","##....##", NULL } },
{ 'W', CAP_TOP, 8, { "##....##","##....##","##....##","##....##","##....##","##.##.##","##.##.##","##.##.##","########",".##..##.", NULL } },
{ 'm', X_TOP,   8, { "###..##.","########","##.##.##","##.##.##","##.##.##","##.##.##","##.##.##", NULL } },
{ 'w', X_TOP,   8, { "##....##","##....##","##.##.##","##.##.##","##.##.##","########",".##..##.", NULL } },
/* and four that seven columns made too wide */
{ 'c', X_TOP,   6, { ".####.","##..##","##....","##....","##....","##..##",".####.", NULL } },
{ 'e', X_TOP,   6, { ".####.","##..##","##..##","######","##....","##..##",".####.", NULL } },
{ 's', X_TOP,   6, { ".####.","##..##","##....",".####.","....##","##..##",".####.", NULL } },
{ 'r', X_TOP,   6, { "##.###","###...","##....","##....","##....","##....","##....", NULL } },
};

/* ---- the 12-row face ---------------------------------------------- */

#define SM_CAP_TOP   1
#define SM_X_TOP     4
#define SM_BASELINE  9   /* first row below the body */
#define SM_DESC_LAST 11

static const ui_glyph_src_t UI_SM_GLYPHS[] = {
{ ' ', 0, 0, { NULL } },
{ '!', SM_CAP_TOP, 1, { "#","#","#","#","#","#",".","#", NULL } },
{ '"', SM_CAP_TOP, 3, { "#.#","#.#", NULL } },
{ '#', 2, 5, { ".#.#.",".#.#.","#####",".#.#.","#####",".#.#.",".#.#.", NULL } },
{ '$', SM_CAP_TOP, 5, { "..#..",".####","#.#..","#.#..",".###.","..#.#","####.","..#..", NULL } },
{ '%', 2, 5, { "##..#","##.#.","...#.","..#..",".#...","#.###","..###", NULL } },
{ '&', SM_CAP_TOP, 5, { ".##..","#..#.","#..#.",".##..","#..#.","#...#","#..#.",".##.#", NULL } },
{ '\'',SM_CAP_TOP, 1, { "#","#", NULL } },
{ '(', SM_CAP_TOP, 2, { ".#","#.","#.","#.","#.","#.","#.","#.",".#", NULL } },
{ ')', SM_CAP_TOP, 2, { "#.",".#",".#",".#",".#",".#",".#",".#","#.", NULL } },
{ '*', 2, 5, { "..#..","#.#.#",".###.","#.#.#","..#..", NULL } },
{ '+', SM_X_TOP,   5, { "..#..","..#..","#####","..#..","..#..", NULL } },
{ ',', 8, 2, { ".#",".#","#.", NULL } },
{ '-', 6, 4, { "####", NULL } },
{ '.', 8, 1, { "#", NULL } },
{ '/', SM_CAP_TOP, 5, { "....#","....#","...#.","..#..","..#..",".#...","#....","#....", NULL } },
{ '0', SM_CAP_TOP, 5, { ".###.","#...#","#..##","#.#.#","##..#","#...#","#...#",".###.", NULL } },
{ '1', SM_CAP_TOP, 4, { "..#.",".##.","..#.","..#.","..#.","..#.","..#.",".###", NULL } },
{ '2', SM_CAP_TOP, 5, { ".###.","#...#","....#","...#.","..#..",".#...","#....","#####", NULL } },
{ '3', SM_CAP_TOP, 5, { ".###.","#...#","....#","..##.","....#","....#","#...#",".###.", NULL } },
{ '4', SM_CAP_TOP, 5, { "...#.","..##.",".#.#.","#..#.","#####","...#.","...#.","...#.", NULL } },
{ '5', SM_CAP_TOP, 5, { "#####","#....","#....","####.","....#","....#","#...#",".###.", NULL } },
{ '6', SM_CAP_TOP, 5, { "..##.",".#...","#....","####.","#...#","#...#","#...#",".###.", NULL } },
{ '7', SM_CAP_TOP, 5, { "#####","....#","...#.","...#.","..#..","..#..",".#...",".#...", NULL } },
{ '8', SM_CAP_TOP, 5, { ".###.","#...#","#...#",".###.","#...#","#...#","#...#",".###.", NULL } },
{ '9', SM_CAP_TOP, 5, { ".###.","#...#","#...#","#...#",".####","....#","...#.",".##..", NULL } },
{ ':', SM_X_TOP,   1, { "#",".",".",".","#", NULL } },
{ ';', SM_X_TOP,   2, { ".#","..","..","..",".#",".#","#.", NULL } },
{ '<', SM_X_TOP,   4, { "..##",".##.","##..",".##.","..##", NULL } },
{ '=', 5, 4, { "####","....","####", NULL } },
{ '>', SM_X_TOP,   4, { "##..",".##.","..##",".##.","##..", NULL } },
{ '?', SM_CAP_TOP, 5, { ".###.","#...#","....#","...#.","..#..","..#..",".....","..#..", NULL } },
{ '@', SM_CAP_TOP, 6, { ".####.","#....#","#.##.#","#.#.##","#.#.##","#.####","#.....",".####.", NULL } },
{ 'A', SM_CAP_TOP, 5, { ".###.","#...#","#...#","#...#","#####","#...#","#...#","#...#", NULL } },
{ 'B', SM_CAP_TOP, 5, { "####.","#...#","#...#","####.","#...#","#...#","#...#","####.", NULL } },
{ 'C', SM_CAP_TOP, 5, { ".###.","#...#","#....","#....","#....","#....","#...#",".###.", NULL } },
{ 'D', SM_CAP_TOP, 5, { "####.","#...#","#...#","#...#","#...#","#...#","#...#","####.", NULL } },
{ 'E', SM_CAP_TOP, 5, { "#####","#....","#....","####.","#....","#....","#....","#####", NULL } },
{ 'F', SM_CAP_TOP, 5, { "#####","#....","#....","####.","#....","#....","#....","#....", NULL } },
{ 'G', SM_CAP_TOP, 5, { ".###.","#...#","#....","#....","#..##","#...#","#...#",".###.", NULL } },
{ 'H', SM_CAP_TOP, 5, { "#...#","#...#","#...#","#####","#...#","#...#","#...#","#...#", NULL } },
{ 'I', SM_CAP_TOP, 3, { "###",".#.",".#.",".#.",".#.",".#.",".#.","###", NULL } },
{ 'J', SM_CAP_TOP, 4, { "...#","...#","...#","...#","...#","...#","#..#",".##.", NULL } },
{ 'K', SM_CAP_TOP, 5, { "#...#","#..#.","#.#..","##...","##...","#.#..","#..#.","#...#", NULL } },
{ 'L', SM_CAP_TOP, 5, { "#....","#....","#....","#....","#....","#....","#....","#####", NULL } },
{ 'M', SM_CAP_TOP, 7, { "#.....#","##...##","#.#.#.#","#..#..#","#.....#","#.....#","#.....#","#.....#", NULL } },
{ 'N', SM_CAP_TOP, 5, { "#...#","##..#","##..#","#.#.#","#.#.#","#..##","#..##","#...#", NULL } },
{ 'O', SM_CAP_TOP, 5, { ".###.","#...#","#...#","#...#","#...#","#...#","#...#",".###.", NULL } },
{ 'P', SM_CAP_TOP, 5, { "####.","#...#","#...#","#...#","####.","#....","#....","#....", NULL } },
{ 'Q', SM_CAP_TOP, 5, { ".###.","#...#","#...#","#...#","#...#","#.#.#","#..#.",".##.#", NULL } },
{ 'R', SM_CAP_TOP, 5, { "####.","#...#","#...#","#...#","####.","#.#..","#..#.","#...#", NULL } },
{ 'S', SM_CAP_TOP, 5, { ".###.","#...#","#....",".###.","....#","....#","#...#",".###.", NULL } },
{ 'T', SM_CAP_TOP, 5, { "#####","..#..","..#..","..#..","..#..","..#..","..#..","..#..", NULL } },
{ 'U', SM_CAP_TOP, 5, { "#...#","#...#","#...#","#...#","#...#","#...#","#...#",".###.", NULL } },
{ 'V', SM_CAP_TOP, 5, { "#...#","#...#","#...#","#...#","#...#",".#.#.",".#.#.","..#..", NULL } },
{ 'W', SM_CAP_TOP, 7, { "#.....#","#.....#","#.....#","#..#..#","#.#.#.#","#.#.#.#","##...##","#.....#", NULL } },
{ 'X', SM_CAP_TOP, 5, { "#...#","#...#",".#.#.","..#..","..#..",".#.#.","#...#","#...#", NULL } },
{ 'Y', SM_CAP_TOP, 5, { "#...#","#...#",".#.#.","..#..","..#..","..#..","..#..","..#..", NULL } },
{ 'Z', SM_CAP_TOP, 5, { "#####","....#","...#.","..#..",".#...","#....","#....","#####", NULL } },
{ '[', SM_CAP_TOP, 2, { "##","#.","#.","#.","#.","#.","#.","#.","##", NULL } },
{ '\\',SM_CAP_TOP, 5, { "#....","#....",".#...","..#..","..#..","...#.","....#","....#", NULL } },
{ ']', SM_CAP_TOP, 2, { "##",".#",".#",".#",".#",".#",".#",".#","##", NULL } },
{ '^', SM_CAP_TOP, 5, { "..#..",".#.#.","#...#", NULL } },
{ '_', 10, 5, { "#####", NULL } },
{ '`', SM_CAP_TOP, 2, { "#.",".#", NULL } },
{ 'a', SM_X_TOP,   5, { ".###.","....#",".####","#...#",".####", NULL } },
{ 'b', SM_CAP_TOP, 5, { "#....","#....","#....","####.","#...#","#...#","#...#","####.", NULL } },
{ 'c', SM_X_TOP,   5, { ".###.","#....","#....","#....",".###.", NULL } },
{ 'd', SM_CAP_TOP, 5, { "....#","....#","....#",".####","#...#","#...#","#...#",".####", NULL } },
{ 'e', SM_X_TOP,   5, { ".###.","#...#","#####","#....",".###.", NULL } },
{ 'f', SM_CAP_TOP, 4, { ".###",".#..",".#..","####",".#..",".#..",".#..",".#..", NULL } },
{ 'g', SM_X_TOP,   5, { ".####","#...#","#...#","#...#",".####","....#","#...#",".###.", NULL } },
{ 'h', SM_CAP_TOP, 5, { "#....","#....","#....","####.","#...#","#...#","#...#","#...#", NULL } },
{ 'i', SM_CAP_TOP, 3, { ".#.","...","##.",".#.",".#.",".#.",".#.",".##", NULL } },
{ 'j', SM_CAP_TOP, 3, { "..#","...","..#","..#","..#","..#","..#","..#","..#","#.#",".#.", NULL } },
{ 'k', SM_CAP_TOP, 5, { "#....","#....","#....","#..#.","#.#..","##...","#.#..","#..#.", NULL } },
{ 'l', SM_CAP_TOP, 3, { "##.",".#.",".#.",".#.",".#.",".#.",".#.",".##", NULL } },
{ 'm', SM_X_TOP,   5, { "#####","#.#.#","#.#.#","#.#.#","#.#.#", NULL } },
{ 'n', SM_X_TOP,   5, { "####.","#...#","#...#","#...#","#...#", NULL } },
{ 'o', SM_X_TOP,   5, { ".###.","#...#","#...#","#...#",".###.", NULL } },
{ 'p', SM_X_TOP,   5, { "####.","#...#","#...#","#...#","####.","#....","#....","#....", NULL } },
{ 'q', SM_X_TOP,   5, { ".####","#...#","#...#","#...#",".####","....#","....#","....#", NULL } },
{ 'r', SM_X_TOP,   5, { "#.###","##...","#....","#....","#....", NULL } },
{ 's', SM_X_TOP,   5, { ".####","#....",".###.","....#","####.", NULL } },
{ 't', 2, 4, { ".#..",".#..","####",".#..",".#..",".#..",".##.", NULL } },
{ 'u', SM_X_TOP,   5, { "#...#","#...#","#...#","#...#",".####", NULL } },
{ 'v', SM_X_TOP,   5, { "#...#","#...#","#...#",".#.#.","..#..", NULL } },
{ 'w', SM_X_TOP,   5, { "#...#","#...#","#.#.#","#####",".#.#.", NULL } },
{ 'x', SM_X_TOP,   5, { "#...#",".#.#.","..#..",".#.#.","#...#", NULL } },
{ 'y', SM_X_TOP,   5, { "#...#","#...#","#...#","#...#",".####","....#","#...#",".###.", NULL } },
{ 'z', SM_X_TOP,   5, { "#####","...#.","..#..",".#...","#####", NULL } },
{ '{', SM_CAP_TOP, 3, { "..#",".#.",".#.",".#.","#..",".#.",".#.",".#.","..#", NULL } },
{ '|', SM_CAP_TOP, 1, { "#","#","#","#","#","#","#","#","#","#", NULL } },
{ '}', SM_CAP_TOP, 3, { "#..",".#.",".#.",".#.","..#",".#.",".#.",".#.","#..", NULL } },
{ '~', 6, 5, { ".#..#","#..#.", NULL } },
};

static uifont_t ui_small, ui_ui, ui_large;

static void ui_err(const char *what, const char *fontname, int code) {
    fprintf(stderr, "gen-font: [%s] 0x%02X '%c': %s\n", fontname, code,
            (code >= 0x20 && code < 0x7F) ? code : '?', what);
    errors++;
}

/* Rasterizes one art block into `f`, replacing whatever was there. The
 * declared width is the glyph's box - art may leave columns blank at
 * either end (that is a side bearing, and 'j' wants one), and the
 * advance is the box plus a single column of letter spacing. */
static void ui_set_glyph(uifont_t *f, const ui_glyph_src_t *g) {
    int code = g->code;

    if (code < 0 || code > 0x7E) {
        ui_err("outside the range this font covers", f->ident, code);
        return;
    }
    if (g->width < 0 || g->width > UI_MAX_COLS) {
        ui_err("width outside 0..UI_MAX_COLS", f->ident, code);
        return;
    }
    for (int r = 0; r < UI_MAX_ROWS; r++) {
        f->rows[code][r] = 0;
    }
    for (int i = 0; g->rows[i]; i++) {
        const char *art = g->rows[i];
        int row = g->top + i;

        if ((int)strlen(art) != g->width) {
            ui_err("art row is not the declared width", f->ident, code);
            return;
        }
        if (row < 0 || row >= f->height) {
            ui_err("art runs past the bottom of the cell", f->ident, code);
            return;
        }
        for (int c = 0; c < g->width; c++) {
            if (art[c] == '#') {
                f->rows[code][row] |= (unsigned short)(0x8000u >> c);
            } else if (art[c] != '.') {
                ui_err("art uses a character other than '#' or '.'", f->ident, code);
                return;
            }
        }
    }
    f->width[code]   = (unsigned char)g->width;
    f->advance[code] = (unsigned char)(g->width ? g->width + 1 : 0);
}

/* The 16-row face: M39's own glyphs, trimmed to their ink. Column 7 was
 * reserved blank there precisely because the advance was a compile-time
 * constant; here the advance is data, so the reservation becomes one
 * column of letter spacing and everything narrower than seven columns
 * stops paying for width it never used. */
static void ui_build_from_mono(uifont_t *f) {
    for (int code = 0; code < 128; code++) {
        int lo = FONT_WIDTH, hi = -1;

        for (int row = 0; row < FONT_HEIGHT; row++) {
            for (int col = 0; col < FONT_WIDTH; col++) {
                if (reg[code][row] & (0x80u >> col)) {
                    if (col < lo) lo = col;
                    if (col > hi) hi = col;
                }
            }
        }
        if (hi < 0) {
            f->width[code]   = 0;
            f->advance[code] = 0;
            continue;
        }
        for (int row = 0; row < FONT_HEIGHT; row++) {
            unsigned short out_row = 0;
            for (int col = lo; col <= hi; col++) {
                if (reg[code][row] & (0x80u >> col)) {
                    out_row |= (unsigned short)(0x8000u >> (col - lo));
                }
            }
            f->rows[code][row] = out_row;
        }
        f->width[code]   = (unsigned char)(hi - lo + 1);
        f->advance[code] = (unsigned char)(hi - lo + 2);
    }
}

/* 3/2 nearest-neighbour, exact for this design: every stroke in the
 * 16-row face is a 2-pixel run, and floor(2n/3) maps any 2-run onto a
 * 3-run wherever it starts. A "first row of" metric scales as
 * ceil(3m/2); a "last row of" metric as the last destination row that
 * still maps back to m. */
static int ui_scale_first(int m) { return (3 * m + 1) / 2; }
static int ui_scale_last(int m)  { return (3 * (m + 1) - 1) / 2; }

static void ui_scale_3_2(const uifont_t *src, uifont_t *dst) {
    dst->height    = src->height * 3 / 2;
    dst->cap_top   = ui_scale_first(src->cap_top);
    dst->x_top     = ui_scale_first(src->x_top);
    dst->baseline  = ui_scale_first(src->baseline);
    dst->desc_last = ui_scale_last(src->desc_last);
    dst->space_advance = (3 * src->space_advance + 1) / 2;

    for (int code = 0; code < 128; code++) {
        int w = src->width[code];
        int dw = w ? (3 * w + 1) / 2 : 0;

        if (dw > UI_MAX_COLS) {
            ui_err("scaled glyph is wider than UI_MAX_COLS", dst->ident, code);
            dw = UI_MAX_COLS;
        }
        for (int r = 0; r < dst->height; r++) {
            int sr = (2 * r) / 3;
            unsigned short in_row = src->rows[code][sr];
            unsigned short out_row = 0;
            for (int c = 0; c < dw; c++) {
                int sc = (2 * c) / 3;
                if (in_row & (unsigned short)(0x8000u >> sc)) {
                    out_row |= (unsigned short)(0x8000u >> c);
                }
            }
            dst->rows[code][r] = out_row;
        }
        dst->width[code]   = (unsigned char)dw;
        dst->advance[code] = (unsigned char)(dw ? dw + 1 : 0);
    }
    dst->advance[' '] = (unsigned char)dst->space_advance;
}

/* Same one-column dilation M39 settled on, for the same reason: done
 * here once rather than per pixel on every redraw. It spends the glyph's
 * single column of letter spacing, which is exactly what bold is
 * supposed to look like and is why the advance is left alone. */
static void ui_make_bold(uifont_t *f) {
    for (int code = 0; code < 128; code++) {
        for (int r = 0; r < UI_MAX_ROWS; r++) {
            unsigned short bits = f->rows[code][r];
            f->bold[code][r] = (unsigned short)(bits | (bits >> 1));
        }
    }
    f->has_bold = 1;
}

/* Tabular figures. A proportional advance is right for letters and
 * wrong for anything that has to line up in a column or tick over in
 * place: with '1' narrower than '0', a taskbar clock would shuffle
 * sideways once a minute and a file size column would not align. Every
 * digit is given the widest digit's box and centred inside it - the same
 * thing a real font's tabular-figure set does, and the reason it belongs
 * here rather than in a caller is that no caller can see it. */
static void ui_tabular_digits(uifont_t *f) {
    int box = 0;
    for (int c = '0'; c <= '9'; c++) {
        if (f->width[c] > box) box = f->width[c];
    }
    for (int c = '0'; c <= '9'; c++) {
        int shift = (box - f->width[c]) / 2;
        if (shift > 0) {
            for (int r = 0; r < UI_MAX_ROWS; r++) {
                f->rows[c][r] = (unsigned short)(f->rows[c][r] >> shift);
            }
        }
        f->width[c]   = (unsigned char)box;
        f->advance[c] = (unsigned char)(box + 1);
    }
}

static int ui_ink_top(const uifont_t *f, int code) {
    for (int r = 0; r < f->height; r++) if (f->rows[code][r]) return r;
    return -1;
}

static int ui_ink_bottom(const uifont_t *f, int code) {
    for (int r = f->height - 1; r >= 0; r--) if (f->rows[code][r]) return r;
    return -1;
}

/* The family check. Three faces authored three different ways is
 * exactly the situation where "they read as one family" stops being
 * true without anybody noticing, so it is asserted rather than
 * intended: every uppercase letter starts on its face's own cap line,
 * every x-band lowercase on its x-height line, everything sits on one
 * baseline, and no glyph's ink escapes the box its advance promises. */
static void ui_check(const uifont_t *f) {
    static const char *UPPER  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char *DIGITS = "0123456789";
    static const char *XBAND  = "acemnorsuvwxz";
    static const char *DESC   = "gpqyj";
    static const char *ASC    = "bdfhikl";

    if (f->advance[' '] == 0) {
        ui_err("the space glyph has no advance - text would have no word gaps", f->ident, ' ');
    }
    for (int code = 0x21; code <= 0x7E; code++) {
        if (f->advance[code] == 0) {
            ui_err("printable codepoint has no advance", f->ident, code);
        }
        if (ui_ink_top(f, code) < 0) {
            ui_err("printable codepoint is blank", f->ident, code);
        }
    }
    for (int code = UI_SPECIAL_FIRST; code <= UI_SPECIAL_LAST; code++) {
        if (f->advance[code] == 0 || ui_ink_top(f, code) < 0) {
            ui_err("special glyph is missing", f->ident, code);
        }
    }
    for (int code = 0; code < 128; code++) {
        if (f->advance[code] && f->advance[code] <= f->width[code]) {
            ui_err("advance does not clear the glyph box - letters would touch", f->ident, code);
        }
        /* Ink outside the declared box is the one error a width table
         * cannot survive: gfx_text_width would under-measure and every
         * centered label in the OS would sit off by that much. */
        for (int r = 0; r < UI_MAX_ROWS; r++) {
            unsigned short beyond = f->width[code] >= UI_MAX_COLS
                                  ? 0
                                  : (unsigned short)(f->rows[code][r] & (0xFFFFu >> f->width[code]));
            if (beyond) {
                ui_err("ink outside the declared glyph box", f->ident, code);
                break;
            }
        }
        if (code < 0x20 && (code < UI_SPECIAL_FIRST || code > UI_SPECIAL_LAST)) {
            if (ui_ink_top(f, code) >= 0) {
                ui_err("control code is not blank", f->ident, code);
            }
        }
    }
    if (ui_ink_top(f, 0x20) >= 0) ui_err("space is not blank", f->ident, 0x20);

    /* Tabular figures, asserted rather than assumed - a clock that
     * shuffles sideways once a minute is the kind of thing that is
     * obvious in motion and invisible in a screenshot. */
    for (int code = '1'; code <= '9'; code++) {
        if (f->advance[code] != f->advance['0']) {
            ui_err("digit does not share the tabular advance", f->ident, code);
        }
    }
    if (ui_ink_top(f, 0x7F) >= 0) ui_err("0x7F is not blank", f->ident, 0x7F);

    for (int code = 0x21; code <= 0x7E; code++) {
        int top = ui_ink_top(f, code), bot = ui_ink_bottom(f, code);
        if (top < 0) continue;
        if (bot > f->desc_last) ui_err("ink below the descender row", f->ident, code);

        if (in(UPPER, code) || in(DIGITS, code)) {
            if (top != f->cap_top)      ui_err("does not start on the shared cap line", f->ident, code);
            if (bot != f->baseline - 1) ui_err("does not sit on the shared baseline", f->ident, code);
        } else if (in(XBAND, code)) {
            if (top != f->x_top)        ui_err("does not start on the shared x-height line", f->ident, code);
            if (bot != f->baseline - 1) ui_err("does not sit on the shared baseline", f->ident, code);
        } else if (in(DESC, code)) {
            if (bot != f->desc_last)    ui_err("descender does not reach the shared descender row", f->ident, code);
        } else if (in(ASC, code)) {
            if (top != f->cap_top)      ui_err("ascender does not start on the shared cap line", f->ident, code);
            if (bot != f->baseline - 1) ui_err("does not sit on the shared baseline", f->ident, code);
        }
    }
}

static void ui_build_all(void) {
    ui_ui.ident    = "ui";
    ui_ui.height   = FONT_HEIGHT;
    ui_ui.cap_top  = CAP_TOP;
    ui_ui.x_top    = X_TOP;
    ui_ui.baseline = BASELINE;
    ui_ui.desc_last = DESC_LAST;
    ui_ui.space_advance = 5;
    ui_build_from_mono(&ui_ui);
    for (size_t i = 0; i < sizeof(UI_UI_OVERRIDE) / sizeof(UI_UI_OVERRIDE[0]); i++) {
        ui_set_glyph(&ui_ui, &UI_UI_OVERRIDE[i]);
    }
    for (size_t i = 0; i < sizeof(UI_SPECIALS_16) / sizeof(UI_SPECIALS_16[0]); i++) {
        ui_set_glyph(&ui_ui, &UI_SPECIALS_16[i]);
    }
    ui_ui.advance[' '] = (unsigned char)ui_ui.space_advance;
    ui_tabular_digits(&ui_ui);
    /* Bold exists for this size only. It is what a focused window title
     * is drawn in (compositor.c), and nothing has ever wanted a bold
     * list row or a bold heading - two more tables no caller references
     * is 9 KiB in every program that links gfx.c. */
    ui_make_bold(&ui_ui);

    ui_small.ident    = "small";
    ui_small.height   = 12;
    ui_small.cap_top  = SM_CAP_TOP;
    ui_small.x_top    = SM_X_TOP;
    ui_small.baseline = SM_BASELINE;
    ui_small.desc_last = SM_DESC_LAST;
    ui_small.space_advance = 4;
    for (size_t i = 0; i < sizeof(UI_SM_GLYPHS) / sizeof(UI_SM_GLYPHS[0]); i++) {
        ui_set_glyph(&ui_small, &UI_SM_GLYPHS[i]);
    }
    for (size_t i = 0; i < sizeof(UI_SPECIALS_12) / sizeof(UI_SPECIALS_12[0]); i++) {
        ui_set_glyph(&ui_small, &UI_SPECIALS_12[i]);
    }
    ui_small.advance[' '] = (unsigned char)ui_small.space_advance;
    ui_tabular_digits(&ui_small);

    ui_large.ident = "large";
    ui_scale_3_2(&ui_ui, &ui_large);

    ui_check(&ui_small);
    ui_check(&ui_ui);
    ui_check(&ui_large);
}

/* ------------------------------------------------------------------ */

static void emit_ui_rows(const char *name, const uifont_t *f, const unsigned short t[128][UI_MAX_ROWS]) {
    emit("static const uint16_t %s[128 * %d] = {\n", name, f->height);
    for (int code = 0; code < 128; code++) {
        emit("   ");
        for (int r = 0; r < f->height; r++) {
            emit(" 0x%04X,", t[code][r]);
        }
        if (code >= 0x20 && code < 0x7F) {
            emit(" /* 0x%02X '%c' */\n", code, code);
        } else {
            emit(" /* 0x%02X */\n", code);
        }
    }
    emit("};\n\n");
}

static void emit_ui_bytes(const char *name, const unsigned char t[128]) {
    emit("static const uint8_t %s[128] = {\n", name);
    for (int code = 0; code < 128; code += 16) {
        emit("   ");
        for (int i = 0; i < 16; i++) {
            emit(" %3u,", t[code + i]);
        }
        emit("\n");
    }
    emit("};\n\n");
}

static void emit_uifont(const uifont_t *f) {
    char buf[64];

    snprintf(buf, sizeof(buf), "ui_rows_%s", f->ident);
    emit_ui_rows(buf, f, f->rows);
    if (f->has_bold) {
        snprintf(buf, sizeof(buf), "ui_bold_%s", f->ident);
        emit_ui_rows(buf, f, f->bold);
    }
    snprintf(buf, sizeof(buf), "ui_adv_%s", f->ident);
    emit_ui_bytes(buf, f->advance);
    snprintf(buf, sizeof(buf), "ui_wid_%s", f->ident);
    emit_ui_bytes(buf, f->width);

    int max_adv = 0;
    for (int code = 0; code < 128; code++) {
        if (f->advance[code] > max_adv) max_adv = f->advance[code];
    }

    emit("const ui_font_t ui_font_%s = {\n", f->ident);
    emit("    .height = %d, .cap_top = %d, .x_top = %d, .baseline = %d,\n",
         f->height, f->cap_top, f->x_top, f->baseline);
    emit("    .desc_last = %d, .max_advance = %d,\n", f->desc_last, max_adv);
    emit("    .rows = ui_rows_%s,\n", f->ident);
    if (f->has_bold) {
        emit("    .rows_bold = ui_bold_%s,\n", f->ident);
    } else {
        emit("    .rows_bold = 0,\n");
    }
    emit("    .advance = ui_adv_%s,\n", f->ident);
    emit("    .width = ui_wid_%s,\n", f->ident);
    emit("};\n\n");
}

static const char *UIFONT_BANNER =
    " * GENERATED FILE - do not edit by hand.\n"
    " *\n"
    " * Produced by tools/gen-font.c (M57), which holds the glyph source\n"
    " * for all three sizes and the family checks that keep them on one\n"
    " * baseline. Regenerate with `make font`; `make font-check` (a\n"
    " * prerequisite of the build) fails if this file stops matching what\n"
    " * that program produces.\n";

static void build_uifont_header(void) {
    out_len = 0;
    emit("/* user_space/lib/uifont.h\n *\n");
    emit("%s", UIFONT_BANNER);
    emit(" *\n"
         " * M57: the proportional UI font family - per-glyph advance widths\n"
         " * beside the bitmap, in three sizes on one shared metric. This is what\n"
         " * chrome, labels, menus, buttons and filenames draw with; font8x16.h's\n"
         " * fixed cell stays exactly where it belongs, under gui_terminal.c's\n"
         " * character grid, text_editor.c's column arithmetic and the kernel\n"
         " * console.\n"
         " *\n"
         " * A row is a uint16_t, MSB = leftmost column, so a glyph may be up to\n"
         " * 16 columns wide. `width[c]` is the glyph's box and `advance[c]` is\n"
         " * how far the pen moves after drawing it - always at least one column\n"
         " * more, which is where letter spacing now comes from. Measure text with\n"
         " * gfx_text_width() (gfx.h); nothing may assume a constant advance.\n"
         " */\n");
    emit("#pragma once\n\n");
    emit("#include <stdint.h>\n\n");
    emit("#define UI_FONT_MAX_COLS %d\n", UI_MAX_COLS);
    emit("#define UI_FONT_MAX_ROWS %d\n\n", UI_MAX_ROWS);
    emit("typedef struct {\n"
         "    uint8_t height;      /* rows in the cell - the line height */\n"
         "    uint8_t cap_top;     /* first row of uppercase, digits, ascenders */\n"
         "    uint8_t x_top;       /* first row of the lowercase x-height band */\n"
         "    uint8_t baseline;    /* first row *below* the glyph body */\n"
         "    uint8_t desc_last;   /* last row of descenders (g j p q y) */\n"
         "    uint8_t max_advance; /* widest advance in the face */\n"
         "    const uint16_t *rows;      /* 128 * height, MSB = leftmost column */\n"
         "    const uint16_t *rows_bold; /* same shape, or NULL if this size has no bold */\n"
         "    const uint8_t  *advance;   /* 128 */\n"
         "    const uint8_t  *width;     /* 128 - the glyph box, always < advance */\n"
         "} ui_font_t;\n\n");
    emit("/* 12-row cell, 1px strokes - dense lists (file manager, task manager). */\n");
    emit("extern const ui_font_t ui_font_small;\n");
    emit("/* 16-row cell - chrome and labels. gfx.c's default. */\n");
    emit("extern const ui_font_t ui_font_ui;\n");
    emit("/* 24-row cell - headings and dialog titles. */\n");
    emit("extern const ui_font_t ui_font_large;\n\n");
    emit("/* Each face's line height as a constant, because window layout in this\n"
         " * project is compile-time arithmetic (#define ROW_H, WIN_H, ...) and a\n"
         " * struct field cannot appear in a #define. These are the same numbers\n"
         " * ui_font_*.height carries at runtime - emitted together so they cannot\n"
         " * drift. Widths have no equivalent and never will: that is the whole\n"
         " * point of this milestone. */\n");
    emit("#define UI_FONT_SMALL_HEIGHT %d\n", ui_small.height);
    emit("#define UI_FONT_UI_HEIGHT    %d\n", ui_ui.height);
    emit("#define UI_FONT_LARGE_HEIGHT %d\n\n", ui_large.height);
    emit("/* The glyphs the UI used to fake with hand-drawn rectangles and\n"
         " * spelled-out words. They live in the control-code range, which is\n"
         " * blank in every string this OS draws, so nothing real collides with\n"
         " * them - write them straight into a literal: \"Delete\" UI_S_ELLIPSIS. */\n");
    struct { const char *name; int code; const char *what; } specials[] = {
        { "ARROW_LEFT",  UI_SPECIAL_ARROW_LEFT,  "left-pointing triangle" },
        { "ARROW_RIGHT", UI_SPECIAL_ARROW_RIGHT, "right-pointing triangle (submenu, disclosure)" },
        { "ARROW_UP",    UI_SPECIAL_ARROW_UP,    "up-pointing triangle (sort ascending)" },
        { "ARROW_DOWN",  UI_SPECIAL_ARROW_DOWN,  "down-pointing triangle (sort descending)" },
        { "CHECK",       UI_SPECIAL_CHECK,       "checkmark" },
        { "BULLET",      UI_SPECIAL_BULLET,      "bullet / radio dot" },
        { "ELLIPSIS",    UI_SPECIAL_ELLIPSIS,    "ellipsis" },
        { "CLOSE",       UI_SPECIAL_CLOSE,       "close X" },
    };
    for (size_t i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
        emit("#define UI_G_%-11s '\\x%02X' /* %s */\n", specials[i].name, specials[i].code, specials[i].what);
    }
    emit("\n");
    for (size_t i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
        emit("#define UI_S_%-11s \"\\x%02X\"\n", specials[i].name, specials[i].code);
    }
    emit("\n#define UI_GLYPH_SPECIAL_FIRST 0x%02X\n", UI_SPECIAL_FIRST);
    emit("#define UI_GLYPH_SPECIAL_LAST  0x%02X\n", UI_SPECIAL_LAST);
}

static void build_uifont_source(void) {
    out_len = 0;
    emit("/* user_space/lib/uifont.c\n *\n");
    emit("%s", UIFONT_BANNER);
    emit(" *\n"
         " * One uint16_t per glyph row, MSB = leftmost column. See uifont.h for\n"
         " * the metric every face below is authored (or scaled) against.\n"
         " */\n");
    emit("#include \"uifont.h\"\n\n");
    emit_uifont(&ui_small);
    emit_uifont(&ui_ui);
    emit_uifont(&ui_large);
}

/* ------------------------------------------------------------------ */

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
    ui_build_all();
    if (errors) {
        fprintf(stderr, "gen-font: %d glyph/metric error(s) - refusing to emit.\n", errors);
        return 1;
    }

    int bad = 0;
    build_header(1); bad |= write_if("kernel/drivers/font8x16.h", check_only);
    build_source(1); bad |= write_if("kernel/drivers/font8x16.c", check_only);
    build_header(0); bad |= write_if("user_space/lib/font8x16.h", check_only);
    build_source(0); bad |= write_if("user_space/lib/font8x16.c", check_only);
    build_uifont_header(); bad |= write_if("user_space/lib/uifont.h", check_only);
    build_uifont_source(); bad |= write_if("user_space/lib/uifont.c", check_only);

    if (bad) return 1;
    if (check_only) printf("gen-font: all six font files match the generator.\n");
    return 0;
}
