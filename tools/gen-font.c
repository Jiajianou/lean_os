#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

#define CAP_TOP    2
#define X_TOP      5
#define BASELINE   12
#define DESCRIPTOR_LAST  14

#define GLYPH_COLS 7

typedef struct {
    int         code;
    int         top;
    const char *rows[16];
} glyph_source_t;

static const glyph_source_t GLYPHS[] = {

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

{ ':', 6,       { "...##..","...##..",".......",".......","...##..","...##..", NULL } },
{ ';', 6,       { "...##..","...##..",".......",".......","..###..","..###..","...##..","..##...", NULL } },
{ '<', X_TOP,   { ".....##","...##..",".##....","##.....",".##....","...##..",".....##", NULL } },
{ '=', 6,       { "#######","#######",".......","#######","#######", NULL } },
{ '>', X_TOP,   { "##.....","..##...","....##.",".....##","....##.","..##...","##.....", NULL } },
{ '?', CAP_TOP, { ".#####.","##...##",".....##","....##.","...##..","...##..","...##..",".......","...##..","...##..", NULL } },
{ '@', CAP_TOP, { ".#####.","##...##","##.###.","##.#.##","##.#.##","##.#.##","##.####","##.....",".##...#","..#####", NULL } },

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

{ '[', CAP_TOP, { "..####.","..##...","..##...","..##...","..##...","..##...","..##...","..##...","..##...","..##...","..####.", NULL } },
{ '\\',CAP_TOP, { "##.....","##.....",".##....",".##....","..##...","..##...","...##..","...##..","....##.","....##.",".....##", NULL } },
{ ']', CAP_TOP, { ".####..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..",".####..", NULL } },
{ '^', CAP_TOP, { "...#...","..###..",".##.##.","##...##", NULL } },
{ '_', 13,      { "#######","#######", NULL } },
{ '`', CAP_TOP, { ".##....","..##...","...##..", NULL } },

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

{ '{', CAP_TOP, { "....##.","...##..","...##..","...##..","..##...","##.....","..##...","...##..","...##..","...##..","....##.", NULL } },
{ '|', CAP_TOP, { "...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..","...##..", NULL } },
{ '}', CAP_TOP, { ".##....","..##...","..##...","..##...","...##..",".....##","...##..","..##...","..##...","..##...",".##....", NULL } },
{ '~', 7,       { ".###.##","##.###.", NULL } },
};

#define NGLYPHS ((int)(sizeof(GLYPHS) / sizeof(GLYPHS[0])))

static unsigned char reg[128][FONT_HEIGHT];
static unsigned char bold[128][FONT_HEIGHT];

static int errors;

static void error(const char *fmt, int code, const char *detail) {
    fprintf(stderr, "gen-font: 0x%02X '%c': ", code,
            (code >= 0x20 && code < 0x7F) ? code : '?');
    fprintf(stderr, fmt, detail);
    fputc('\n', stderr);
    errors++;
}

static void build(void) {
    int seen[128] = { 0 };

    for (int g = 0; g < NGLYPHS; g++) {
        const glyph_source_t *source = &GLYPHS[g];
        int code = source->code;

        if (code < 0x20 || code > 0x7E) {
            error("outside the printable range this table covers%s", code, "");
            continue;
        }
        if (seen[code]) {
            error("defined twice%s", code, "");
            continue;
        }
        seen[code] = 1;

        for (int i = 0; source->rows[i]; i++) {
            const char *art = source->rows[i];
            int row = source->top + i;

            if (strlen(art) != GLYPH_COLS) {
                error("art row is not %s columns wide", code, "7");
                break;
            }
            if (row < 0 || row >= FONT_HEIGHT) {
                error("art runs past row 15%s", code, "");
                break;
            }
            for (int col = 0; col < GLYPH_COLS; col++) {
                if (art[col] == '#') {
                    reg[code][row] |= (unsigned char)(0x80u >> col);
                } else if (art[col] != '.') {
                    error("art uses a character other than '#' or '.'%s", code, "");
                    break;
                }
            }
        }
    }

    for (int code = 0x20; code <= 0x7E; code++) {
        if (!seen[code]) {
            error("no glyph defined%s", code, "");
        }
    }

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

static void check_metric(void) {
    static const char *UPPER  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char *DIGITS = "0123456789";
    static const char *XBAND  = "acemnorsuvwxz";
    static const char *DESCRIPTOR   = "gpqyj";
    static const char *ASC    = "bdfhiklt";

    for (int code = 0; code < 128; code++) {
        for (int row = 0; row < FONT_HEIGHT; row++) {
            if (reg[code][row] & 0x01u) {
                error("ink in column 7, which is the reserved advance gap%s", code, "");
                break;
            }
        }
    }

    for (int code = 0; code < 0x20; code++) {
        if (ink_top(code) != -1) error("control code is not blank%s", code, "");
    }
    if (ink_top(0x20) != -1) error("space is not blank%s", 0x20, "");
    if (ink_top(0x7F) != -1) error("0x7F is not blank%s", 0x7F, "");

    for (int code = 0x21; code <= 0x7E; code++) {
        if (ink_top(code) == -1) error("printable glyph is blank%s", code, "");
        if (ink_bottom(code) > DESCRIPTOR_LAST) error("ink below the descender row%s", code, "");
    }

    for (int code = 0x21; code <= 0x7E; code++) {
        int top = ink_top(code), bot = ink_bottom(code);
        if (top < 0) continue;

        if (in(UPPER, code) || in(DIGITS, code)) {
            if (top != CAP_TOP)      error("does not start on the shared cap line (row 2)%s", code, "");
            if (bot != BASELINE - 1) error("does not sit on the shared baseline (row 11)%s", code, "");
        } else if (in(XBAND, code)) {
            if (top != X_TOP)        error("does not start on the shared x-height line (row 5)%s", code, "");
            if (bot != BASELINE - 1) error("does not sit on the shared baseline (row 11)%s", code, "");
        } else if (in(DESCRIPTOR, code)) {
            if (bot != DESCRIPTOR_LAST)    error("descender does not reach the shared descender row (row 14)%s", code, "");
        } else if (in(ASC, code)) {
            if (top != CAP_TOP && code != 't') error("ascender does not start on the shared cap line (row 2)%s", code, "");
            if (bot != BASELINE - 1) error("does not sit on the shared baseline (row 11)%s", code, "");
        }
    }
}

static char out[1 << 22];
static size_t out_length;

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    out_length += (size_t)vsnprintf(out + out_length, sizeof(out) - out_length, fmt, ap);
    va_end(ap);
}

static void emit_table(const char *name, unsigned char t[128][FONT_HEIGHT]) {
    emit("const uint8_t %s[128][16] = {\n", name);
    for (int code = 0; code < 128; code++) {
        emit("    {");
        for (int row = 0; row < FONT_HEIGHT; row++) {
            emit(" 0x%02X%s", t[code][row], row == FONT_HEIGHT - 1 ? "" : ",");
        }
        emit(" },\n");
    }
    emit("};\n");
}


static void build_header(int kernel_side) {
    out_length = 0;
    (void)kernel_side;
    emit("#pragma once\n\n");
    emit("#include <stdint.h>\n\n");
    emit("#define FONT_WIDTH  8\n");
    emit("#define FONT_HEIGHT 16\n\n");
    emit("#define FONT_CAP_TOP   %d\n", CAP_TOP);
    emit("#define FONT_X_TOP     %d\n", X_TOP);
    emit("#define FONT_BASELINE  %d\n", BASELINE);
    emit("#define FONT_DESCRIPTOR_LAST %d\n\n", DESCRIPTOR_LAST);
    emit("#define FONT_GLYPH_COLS %d\n\n", GLYPH_COLS);
    emit("extern const uint8_t font8x16[128][16];\n\n");
    emit("extern const uint8_t font8x16_bold[128][16];\n");
}

static void build_source(int kernel_side) {
    out_length = 0;
    (void)kernel_side;
    emit("#include \"font8x16.h\"\n\n");
    emit_table("font8x16", reg);
    emit("\n");
    emit_table("font8x16_bold", bold);
}

#define UI_MAX_COLS   16
#define UI_MAX_ROWS   24

typedef struct {
    int         code;
    int         top;
    int         width;
    const char *rows[UI_MAX_ROWS + 1];
} ui_glyph_source_t;

typedef struct {
    const char    *ident;
    int            height;
    int            cap_top;
    int            x_top;
    int            baseline;
    int            descriptor_last;
    int            space_advance;
    int            has_bold;
    unsigned short rows[128][UI_MAX_ROWS];
    unsigned short bold[128][UI_MAX_ROWS];
    unsigned char  width[128];
    unsigned char  advance[128];
} user_interface_font_t;

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

static const ui_glyph_source_t UI_SPECIALS_16[] = {
{ UI_SPECIAL_ARROW_LEFT,  3, 5, { "....#","...##","..###",".####","#####",".####","..###","...##","....#", NULL } },
{ UI_SPECIAL_ARROW_RIGHT, 3, 5, { "#....","##...","###..","####.","#####","####.","###..","##...","#....", NULL } },
{ UI_SPECIAL_ARROW_UP,    5, 9, { "....#....","...###...","..#####..",".#######.","#########", NULL } },
{ UI_SPECIAL_ARROW_DOWN,  5, 9, { "#########",".#######.","..#####..","...###...","....#....", NULL } },
{ UI_SPECIAL_CHECK,       6, 8, { "......##",".....##.","....##..","#..##...","####....",".###....", NULL } },
{ UI_SPECIAL_BULLET,      7, 4, { ".##.","####","####",".##.", NULL } },
{ UI_SPECIAL_ELLIPSIS,   10,10, { "##..##..##","##..##..##", NULL } },
{ UI_SPECIAL_CLOSE,       4, 8, { "##....##","###..###",".######.","..####..","..####..",".######.","###..###","##....##", NULL } },
};

static const ui_glyph_source_t UI_SPECIALS_12[] = {
{ UI_SPECIAL_ARROW_LEFT,  2, 4, { "...#","..##",".###","####",".###","..##","...#", NULL } },
{ UI_SPECIAL_ARROW_RIGHT, 2, 4, { "#...","##..","###.","####","###.","##..","#...", NULL } },
{ UI_SPECIAL_ARROW_UP,    4, 7, { "...#...","..###..",".#####.","#######", NULL } },
{ UI_SPECIAL_ARROW_DOWN,  4, 7, { "#######",".#####.","..###..","...#...", NULL } },
{ UI_SPECIAL_CHECK,       4, 6, { ".....#","....#.","...#..","#.#...",".#....", NULL } },
{ UI_SPECIAL_BULLET,      5, 3, { ".#.","###",".#.", NULL } },
{ UI_SPECIAL_ELLIPSIS,    8, 7, { "#..#..#", NULL } },
{ UI_SPECIAL_CLOSE,       3, 6, { "#....#",".#..#.","..##..","..##..",".#..#.","#....#", NULL } },
};

static const ui_glyph_source_t UI_UI_OVERRIDE[] = {
{ 'M', CAP_TOP, 8, { "##....##","###..###","########","##.##.##","##.##.##","##....##","##....##","##....##","##....##","##....##", NULL } },
{ 'W', CAP_TOP, 8, { "##....##","##....##","##....##","##....##","##....##","##.##.##","##.##.##","##.##.##","########",".##..##.", NULL } },
{ 'm', X_TOP,   8, { "###..##.","########","##.##.##","##.##.##","##.##.##","##.##.##","##.##.##", NULL } },
{ 'w', X_TOP,   8, { "##....##","##....##","##.##.##","##.##.##","##.##.##","########",".##..##.", NULL } },
{ 'c', X_TOP,   6, { ".####.","##..##","##....","##....","##....","##..##",".####.", NULL } },
{ 'e', X_TOP,   6, { ".####.","##..##","##..##","######","##....","##..##",".####.", NULL } },
{ 's', X_TOP,   6, { ".####.","##..##","##....",".####.","....##","##..##",".####.", NULL } },
{ 'r', X_TOP,   6, { "##.###","###...","##....","##....","##....","##....","##....", NULL } },
};

#define SM_CAP_TOP   1
#define SM_X_TOP     4
#define SM_BASELINE  9
#define SM_DESCRIPTOR_LAST 11

static const ui_glyph_source_t UI_SM_GLYPHS[] = {
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

static user_interface_font_t ui_small, ui_ui, ui_large;

static void ui_error(const char *what, const char *fontname, int code) {
    fprintf(stderr, "gen-font: [%s] 0x%02X '%c': %s\n", fontname, code,
            (code >= 0x20 && code < 0x7F) ? code : '?', what);
    errors++;
}

static void ui_set_glyph(user_interface_font_t *f, const ui_glyph_source_t *g) {
    int code = g->code;

    if (code < 0 || code > 0x7E) {
        ui_error("outside the range this font covers", f->ident, code);
        return;
    }
    if (g->width < 0 || g->width > UI_MAX_COLS) {
        ui_error("width outside 0..UI_MAX_COLS", f->ident, code);
        return;
    }
    for (int r = 0; r < UI_MAX_ROWS; r++) {
        f->rows[code][r] = 0;
    }
    for (int i = 0; g->rows[i]; i++) {
        const char *art = g->rows[i];
        int row = g->top + i;

        if ((int)strlen(art) != g->width) {
            ui_error("art row is not the declared width", f->ident, code);
            return;
        }
        if (row < 0 || row >= f->height) {
            ui_error("art runs past the bottom of the cell", f->ident, code);
            return;
        }
        for (int c = 0; c < g->width; c++) {
            if (art[c] == '#') {
                f->rows[code][row] |= (unsigned short)(0x8000u >> c);
            } else if (art[c] != '.') {
                ui_error("art uses a character other than '#' or '.'", f->ident, code);
                return;
            }
        }
    }
    f->width[code]   = (unsigned char)g->width;
    f->advance[code] = (unsigned char)(g->width ? g->width + 1 : 0);
}

static void ui_build_from_mono(user_interface_font_t *f) {
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

static int ui_scale_first(int m) { return (3 * m + 1) / 2; }
static int ui_scale_last(int m)  { return (3 * (m + 1) - 1) / 2; }

static void ui_scale_3_2(const user_interface_font_t *source, user_interface_font_t *destination) {
    destination->height    = source->height * 3 / 2;
    destination->cap_top   = ui_scale_first(source->cap_top);
    destination->x_top     = ui_scale_first(source->x_top);
    destination->baseline  = ui_scale_first(source->baseline);
    destination->descriptor_last = ui_scale_last(source->descriptor_last);
    destination->space_advance = (3 * source->space_advance + 1) / 2;

    for (int code = 0; code < 128; code++) {
        int w = source->width[code];
        int dw = w ? (3 * w + 1) / 2 : 0;

        if (dw > UI_MAX_COLS) {
            ui_error("scaled glyph is wider than UI_MAX_COLS", destination->ident, code);
            dw = UI_MAX_COLS;
        }
        for (int r = 0; r < destination->height; r++) {
            int sr = (2 * r) / 3;
            unsigned short in_row = source->rows[code][sr];
            unsigned short out_row = 0;
            for (int c = 0; c < dw; c++) {
                int sc = (2 * c) / 3;
                if (in_row & (unsigned short)(0x8000u >> sc)) {
                    out_row |= (unsigned short)(0x8000u >> c);
                }
            }
            destination->rows[code][r] = out_row;
        }
        destination->width[code]   = (unsigned char)dw;
        destination->advance[code] = (unsigned char)(dw ? dw + 1 : 0);
    }
    destination->advance[' '] = (unsigned char)destination->space_advance;
}

static void ui_make_bold(user_interface_font_t *f) {
    for (int code = 0; code < 128; code++) {
        for (int r = 0; r < UI_MAX_ROWS; r++) {
            unsigned short bits = f->rows[code][r];
            f->bold[code][r] = (unsigned short)(bits | (bits >> 1));
        }
    }
    f->has_bold = 1;
}

static void ui_tabular_digits(user_interface_font_t *f) {
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

static int ui_ink_top(const user_interface_font_t *f, int code) {
    for (int r = 0; r < f->height; r++) if (f->rows[code][r]) return r;
    return -1;
}

static int ui_ink_bottom(const user_interface_font_t *f, int code) {
    for (int r = f->height - 1; r >= 0; r--) if (f->rows[code][r]) return r;
    return -1;
}

static void ui_check(const user_interface_font_t *f) {
    static const char *UPPER  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char *DIGITS = "0123456789";
    static const char *XBAND  = "acemnorsuvwxz";
    static const char *DESCRIPTOR   = "gpqyj";
    static const char *ASC    = "bdfhikl";

    if (f->advance[' '] == 0) {
        ui_error("the space glyph has no advance - text would have no word gaps", f->ident, ' ');
    }
    for (int code = 0x21; code <= 0x7E; code++) {
        if (f->advance[code] == 0) {
            ui_error("printable codepoint has no advance", f->ident, code);
        }
        if (ui_ink_top(f, code) < 0) {
            ui_error("printable codepoint is blank", f->ident, code);
        }
    }
    for (int code = UI_SPECIAL_FIRST; code <= UI_SPECIAL_LAST; code++) {
        if (f->advance[code] == 0 || ui_ink_top(f, code) < 0) {
            ui_error("special glyph is missing", f->ident, code);
        }
    }
    for (int code = 0; code < 128; code++) {
        if (f->advance[code] && f->advance[code] <= f->width[code]) {
            ui_error("advance does not clear the glyph box - letters would touch", f->ident, code);
        }
        for (int r = 0; r < UI_MAX_ROWS; r++) {
            unsigned short beyond = f->width[code] >= UI_MAX_COLS
                                  ? 0
                                  : (unsigned short)(f->rows[code][r] & (0xFFFFu >> f->width[code]));
            if (beyond) {
                ui_error("ink outside the declared glyph box", f->ident, code);
                break;
            }
        }
        if (code < 0x20 && (code < UI_SPECIAL_FIRST || code > UI_SPECIAL_LAST)) {
            if (ui_ink_top(f, code) >= 0) {
                ui_error("control code is not blank", f->ident, code);
            }
        }
    }
    if (ui_ink_top(f, 0x20) >= 0) ui_error("space is not blank", f->ident, 0x20);

    for (int code = '1'; code <= '9'; code++) {
        if (f->advance[code] != f->advance['0']) {
            ui_error("digit does not share the tabular advance", f->ident, code);
        }
    }
    if (ui_ink_top(f, 0x7F) >= 0) ui_error("0x7F is not blank", f->ident, 0x7F);

    for (int code = 0x21; code <= 0x7E; code++) {
        int top = ui_ink_top(f, code), bot = ui_ink_bottom(f, code);
        if (top < 0) continue;
        if (bot > f->descriptor_last) ui_error("ink below the descender row", f->ident, code);

        if (in(UPPER, code) || in(DIGITS, code)) {
            if (top != f->cap_top)      ui_error("does not start on the shared cap line", f->ident, code);
            if (bot != f->baseline - 1) ui_error("does not sit on the shared baseline", f->ident, code);
        } else if (in(XBAND, code)) {
            if (top != f->x_top)        ui_error("does not start on the shared x-height line", f->ident, code);
            if (bot != f->baseline - 1) ui_error("does not sit on the shared baseline", f->ident, code);
        } else if (in(DESCRIPTOR, code)) {
            if (bot != f->descriptor_last)    ui_error("descender does not reach the shared descender row", f->ident, code);
        } else if (in(ASC, code)) {
            if (top != f->cap_top)      ui_error("ascender does not start on the shared cap line", f->ident, code);
            if (bot != f->baseline - 1) ui_error("does not sit on the shared baseline", f->ident, code);
        }
    }
}

#define UI_COVERAGE_MAX_POINTS 4096
#define UI_COVERAGE_SUBSAMPLES 4
#define UI_INSET_LARGE 0.30
#define UI_COVERAGE_MAX_BYTES  (1 << 18)

typedef struct {
    double x, y;
    int    sharp;
} outline_point_t;

typedef struct {
    outline_point_t point[UI_COVERAGE_MAX_POINTS];
    int             count;
    int             start[64];
    int             length[64];
    int             contours;
} outline_t;

typedef struct {
    const char    *ident;
    int            height;
    int            width[128];
    int            bold_width[128];
    unsigned int   offset[128];
    unsigned int   offset_bold[128];
    unsigned char  data[UI_COVERAGE_MAX_BYTES];
    unsigned int   length;
    unsigned char  bold[UI_COVERAGE_MAX_BYTES];
    unsigned int   bold_length;
    int            has_bold;
} coverage_font_t;

static coverage_font_t cov_small, cov_ui, cov_large;

static int ui_cell(const user_interface_font_t *f, int code, int row, int col) {
    if (row < 0 || row >= f->height || col < 0 || col >= f->width[code]) {
        return 0;
    }
    return (f->rows[code][row] & (unsigned short)(0x8000u >> col)) ? 1 : 0;
}

typedef struct {
    int x0, y0, x1, y1;
    int used;
} crack_edge_t;

static crack_edge_t cracks[UI_COVERAGE_MAX_POINTS];
static int crack_count;

static void crack_add(int x0, int y0, int x1, int y1) {
    if (crack_count >= UI_COVERAGE_MAX_POINTS) {
        return;
    }
    cracks[crack_count].x0 = x0;
    cracks[crack_count].y0 = y0;
    cracks[crack_count].x1 = x1;
    cracks[crack_count].y1 = y1;
    cracks[crack_count].used = 0;
    crack_count++;
}

static user_interface_font_t ui_eroded;

static void ui_erode(const user_interface_font_t *source, user_interface_font_t *out) {
    *out = *source;
    for (int code = 0; code < 128; code++) {
        for (int row = 0; row < source->height; row++) {
            unsigned short v = source->rows[code][row];
            unsigned short thinned = 0;
            int col = 0;
            while (col < UI_MAX_COLS) {
                if (!(v & (unsigned short)(0x8000u >> col))) {
                    col++;
                    continue;
                }
                int run = 0;
                while (col + run < UI_MAX_COLS &&
                       (v & (unsigned short)(0x8000u >> (col + run)))) {
                    run++;
                }
                int keep = run > 1 ? run - 1 : run;
                for (int i = 0; i < keep; i++) {
                    thinned |= (unsigned short)(0x8000u >> (col + i));
                }
                col += run;
            }
            out->rows[code][row] = thinned;
        }
    }
}

static void ui_trace(const user_interface_font_t *f, int code, outline_t *out) {
    crack_count = 0;
    out->count = 0;
    out->contours = 0;

    for (int row = 0; row < f->height; row++) {
        for (int col = 0; col < f->width[code]; col++) {
            if (!ui_cell(f, code, row, col)) {
                continue;
            }
            if (!ui_cell(f, code, row - 1, col)) crack_add(col, row, col + 1, row);
            if (!ui_cell(f, code, row, col + 1)) crack_add(col + 1, row, col + 1, row + 1);
            if (!ui_cell(f, code, row + 1, col)) crack_add(col + 1, row + 1, col, row + 1);
            if (!ui_cell(f, code, row, col - 1)) crack_add(col, row + 1, col, row);
        }
    }

    for (int seed = 0; seed < crack_count; seed++) {
        if (cracks[seed].used) {
            continue;
        }
        int first = out->count;
        int at = seed;
        int guard = 0;
        while (at >= 0 && guard++ < UI_COVERAGE_MAX_POINTS) {
            cracks[at].used = 1;
            if (out->count < UI_COVERAGE_MAX_POINTS) {
                out->point[out->count].x = cracks[at].x0;
                out->point[out->count].y = cracks[at].y0;
                out->point[out->count].sharp = 1;
                out->count++;
            }
            int ex = cracks[at].x1, ey = cracks[at].y1;
            int dx = cracks[at].x1 - cracks[at].x0, dy = cracks[at].y1 - cracks[at].y0;
            int best = -1, best_rank = 9;
            for (int i = 0; i < crack_count; i++) {
                if (cracks[i].used || cracks[i].x0 != ex || cracks[i].y0 != ey) {
                    continue;
                }
                int nx = cracks[i].x1 - cracks[i].x0, ny = cracks[i].y1 - cracks[i].y0;
                int cross = dx * ny - dy * nx;
                int rank = cross < 0 ? 0 : (cross == 0 ? 1 : 2);
                if (rank < best_rank) {
                    best_rank = rank;
                    best = i;
                }
            }
            at = best;
        }
        if (out->contours < 64 && out->count > first) {
            out->start[out->contours] = first;
            out->length[out->contours] = out->count - first;
            out->contours++;
        }
    }
}

static double point_distance(const outline_point_t *a, const outline_point_t *b) {
    double dx = a->x - b->x, dy = a->y - b->y;
    return dx * dx + dy * dy;
}

static void ui_mark_staircase(outline_t *o) {
    for (int c = 0; c < o->contours; c++) {
        int s = o->start[c], n = o->length[c];
        for (int i = 0; i < n; i++) {
            const outline_point_t *previous = &o->point[s + (i + n - 1) % n];
            const outline_point_t *here     = &o->point[s + i];
            const outline_point_t *next     = &o->point[s + (i + 1) % n];
            double before = point_distance(previous, here);
            double after  = point_distance(here, next);
            double shorter = before < after ? before : after;
            o->point[s + i].sharp = shorter > 1.5 ? 1 : 0;
        }
    }
}

static void ui_cut_corners(outline_t *in, outline_t *out) {
    out->count = 0;
    out->contours = 0;
    for (int c = 0; c < in->contours; c++) {
        int s = in->start[c], n = in->length[c];
        int first = out->count;
        for (int i = 0; i < n; i++) {
            outline_point_t *previous = &in->point[s + (i + n - 1) % n];
            outline_point_t *here     = &in->point[s + i];
            outline_point_t *next     = &in->point[s + (i + 1) % n];
            if (here->sharp) {
                if (out->count < UI_COVERAGE_MAX_POINTS) {
                    out->point[out->count++] = *here;
                }
                continue;
            }
            if (out->count + 2 > UI_COVERAGE_MAX_POINTS) {
                break;
            }
            out->point[out->count].x = here->x + (previous->x - here->x) * 0.25;
            out->point[out->count].y = here->y + (previous->y - here->y) * 0.25;
            out->point[out->count].sharp = 0;
            out->count++;
            out->point[out->count].x = here->x + (next->x - here->x) * 0.25;
            out->point[out->count].y = here->y + (next->y - here->y) * 0.25;
            out->point[out->count].sharp = 0;
            out->count++;
        }
        if (out->contours < 64 && out->count > first) {
            out->start[out->contours] = first;
            out->length[out->contours] = out->count - first;
            out->contours++;
        }
    }
}

static double contour_area(const outline_t *o, int c) {
    int s = o->start[c], n = o->length[c];
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        const outline_point_t *a = &o->point[s + i];
        const outline_point_t *b = &o->point[s + (i + 1) % n];
        sum += a->x * b->y - b->x * a->y;
    }
    return sum * 0.5;
}

static void ui_inset(outline_t *o, double distance) {
    static outline_point_t moved[UI_COVERAGE_MAX_POINTS];
    if (distance <= 0.0) {
        return;
    }
    for (int c = 0; c < o->contours; c++) {
        int s = o->start[c], n = o->length[c];
        double sign = contour_area(o, c) > 0.0 ? 1.0 : -1.0;
        for (int i = 0; i < n; i++) {
            const outline_point_t *previous = &o->point[s + (i + n - 1) % n];
            const outline_point_t *here     = &o->point[s + i];
            const outline_point_t *next     = &o->point[s + (i + 1) % n];

            double ax = here->x - previous->x, ay = here->y - previous->y;
            double bx = next->x - here->x,     by = next->y - here->y;
            double al = sqrt(ax * ax + ay * ay), bl = sqrt(bx * bx + by * by);
            if (al < 1e-9 || bl < 1e-9) {
                moved[s + i] = *here;
                continue;
            }
            double nx = (ay / al + by / bl) * sign;
            double ny = (-ax / al - bx / bl) * sign;
            double nl = sqrt(nx * nx + ny * ny);
            if (nl < 1e-9) {
                moved[s + i] = *here;
                continue;
            }
            moved[s + i] = *here;
            moved[s + i].x = here->x + nx / nl * distance;
        }
    }
    for (int c = 0; c < o->contours; c++) {
        for (int i = 0; i < o->length[c]; i++) {
            o->point[o->start[c] + i] = moved[o->start[c] + i];
        }
    }
}

typedef struct {
    double x;
    int    winding;
} crossing_t;

static int crossing_order(const void *a, const void *b) {
    double da = ((const crossing_t *)a)->x, db = ((const crossing_t *)b)->x;
    return da < db ? -1 : (da > db ? 1 : 0);
}

static void ui_snap_y(outline_t *o, const user_interface_font_t *source,
                      const user_interface_font_t *destination) {
    double from[6], to[6];
    int n = 0;
    from[n] = 0.0;                                  to[n++] = 0.0;
    from[n] = source->cap_top;                      to[n++] = destination->cap_top;
    from[n] = source->x_top;                        to[n++] = destination->x_top;
    from[n] = source->baseline;                     to[n++] = destination->baseline;
    from[n] = source->descriptor_last + 1;          to[n++] = destination->descriptor_last + 1;
    from[n] = source->height;                       to[n++] = destination->height;

    for (int c = 0; c < o->contours; c++) {
        for (int i = 0; i < o->length[c]; i++) {
            double y = o->point[o->start[c] + i].y;
            double mapped = y * destination->height / source->height;
            for (int k = 0; k + 1 < n; k++) {
                if (y >= from[k] && y <= from[k + 1] && from[k + 1] > from[k]) {
                    double t = (y - from[k]) / (from[k + 1] - from[k]);
                    mapped = to[k] + (to[k + 1] - to[k]) * t;
                    break;
                }
            }
            o->point[o->start[c] + i].y = mapped;
        }
    }
}

static void ui_rasterise(const outline_t *o, double scale, int out_w, int out_h,
                         unsigned char *dst) {
    static double acc[UI_MAX_COLS * 4];
    static crossing_t hits[UI_COVERAGE_MAX_POINTS];

    for (int row = 0; row < out_h; row++) {
        for (int col = 0; col < out_w; col++) {
            acc[col] = 0.0;
        }
        for (int sub = 0; sub < UI_COVERAGE_SUBSAMPLES; sub++) {
            double sample_y = row + (sub + 0.5) / UI_COVERAGE_SUBSAMPLES;
            int count = 0;
            for (int c = 0; c < o->contours; c++) {
                int s = o->start[c], n = o->length[c];
                for (int i = 0; i < n; i++) {
                    const outline_point_t *a = &o->point[s + i];
                    const outline_point_t *b = &o->point[s + (i + 1) % n];
                    double ay = a->y, by = b->y;
                    if (ay == by) {
                        continue;
                    }
                    double lo = ay < by ? ay : by;
                    double hi = ay < by ? by : ay;
                    if (sample_y < lo || sample_y >= hi) {
                        continue;
                    }
                    double ax = a->x * scale, bx = b->x * scale;
                    double t = (sample_y - ay) / (by - ay);
                    if (count < UI_COVERAGE_MAX_POINTS) {
                        hits[count].x = ax + (bx - ax) * t;
                        hits[count].winding = by > ay ? 1 : -1;
                        count++;
                    }
                }
            }
            if (count < 2) {
                continue;
            }
            qsort(hits, (size_t)count, sizeof(hits[0]), crossing_order);
            int winding = 0;
            for (int i = 0; i + 1 < count; i++) {
                winding += hits[i].winding;
                if (winding == 0) {
                    continue;
                }
                double x0 = hits[i].x, x1 = hits[i + 1].x;
                if (x1 <= 0.0 || x0 >= out_w) {
                    continue;
                }
                if (x0 < 0.0) x0 = 0.0;
                if (x1 > out_w) x1 = out_w;
                int first = (int)x0, last = (int)(x1 - 1e-9);
                if (last >= out_w) last = out_w - 1;
                for (int col = first; col <= last; col++) {
                    double left = x0 > col ? x0 : col;
                    double right = x1 < col + 1 ? x1 : col + 1;
                    if (right > left) {
                        acc[col] += (right - left) / UI_COVERAGE_SUBSAMPLES;
                    }
                }
            }
        }
        for (int col = 0; col < out_w; col++) {
            double v = acc[col];
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
            dst[row * out_w + col] = (unsigned char)(v * 255.0 + 0.5);
        }
    }
}

static int coverage_ink_top(const coverage_font_t *c, int code) {
    int w = c->width[code];
    for (int row = 0; row < c->height; row++) {
        for (int col = 0; col < w; col++) {
            if (c->data[c->offset[code] + row * w + col]) {
                return row;
            }
        }
    }
    return -1;
}

static int coverage_ink_bottom(const coverage_font_t *c, int code) {
    int w = c->width[code];
    for (int row = c->height - 1; row >= 0; row--) {
        for (int col = 0; col < w; col++) {
            if (c->data[c->offset[code] + row * w + col]) {
                return row;
            }
        }
    }
    return -1;
}

static void ui_check_coverage(const coverage_font_t *c, const user_interface_font_t *f) {
    static const char *UPPER  = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char *DIGITS = "0123456789";
    static const char *XBAND  = "acemnorsuvwxz";
    static const char *DESCRIPTOR = "gpqyj";
    static const char *ASC    = "bdfhikl";
    int has_partial = 0;

    for (int code = 0x21; code <= 0x7E; code++) {
        if (f->width[code] == 0) {
            continue;
        }
        int top = coverage_ink_top(c, code);
        int bottom = coverage_ink_bottom(c, code);
        if (top < 0) {
            ui_error("rendered coverage is blank", c->ident, code);
            continue;
        }
        if (bottom > f->descriptor_last) {
            ui_error("rendered coverage runs below the descender row", c->ident, code);
        }
        if (in(UPPER, code) || in(DIGITS, code) || in(ASC, code)) {
            if (top != f->cap_top) {
                ui_error("rendered coverage does not start on the cap line", c->ident, code);
            }
            if (bottom != f->baseline - 1) {
                ui_error("rendered coverage does not sit on the baseline", c->ident, code);
            }
        } else if (in(XBAND, code)) {
            if (top != f->x_top) {
                ui_error("rendered coverage does not start on the x-height line", c->ident, code);
            }
            if (bottom != f->baseline - 1) {
                ui_error("rendered coverage does not sit on the baseline", c->ident, code);
            }
        } else if (in(DESCRIPTOR, code)) {
            if (bottom != f->descriptor_last) {
                ui_error("rendered descender does not reach the descender row", c->ident, code);
            }
        }
        for (int i = 0; i < f->width[code] * c->height; i++) {
            unsigned char v = c->data[c->offset[code] + i];
            if (v != 0 && v != 255) {
                has_partial = 1;
            }
        }
    }
    if (!has_partial) {
        ui_error("no glyph has a partly-covered pixel - this face is not anti-aliased",
                 c->ident, 0x41);
    }
}

static void ui_build_coverage(const user_interface_font_t *source,
                              const user_interface_font_t *metrics,
                              coverage_font_t *out, int scale_numerator, int scale_denominator,
                              double inset) {
    static outline_t traced, cut_once, cut_twice;
    double scale = (double)scale_numerator / scale_denominator;

    out->ident = metrics->ident;
    out->height = metrics->height;
    out->length = 0;
    out->bold_length = 0;
    out->has_bold = metrics->has_bold;

    for (int code = 0; code < 128; code++) {
        int w = metrics->width[code];
        out->width[code] = w;
        out->offset[code] = out->length;
        out->offset_bold[code] = out->bold_length;
        out->bold_width[code] = w ? w + 1 : 0;
        if (w == 0) {
            continue;
        }
        if (out->length + (unsigned)(w * out->height) > UI_COVERAGE_MAX_BYTES) {
            ui_error("coverage table overflowed", out->ident, code);
            return;
        }
        ui_trace(source, code, &traced);
        ui_mark_staircase(&traced);
        ui_cut_corners(&traced, &cut_once);
        ui_cut_corners(&cut_once, &cut_twice);
        ui_inset(&cut_twice, inset);
        ui_snap_y(&cut_twice, source, metrics);
        ui_rasterise(&cut_twice, scale, w, out->height, out->data + out->length);
        out->length += (unsigned)(w * out->height);

        if (out->has_bold) {
            int bw = w + 1;
            const unsigned char *src = out->data + out->offset[code];
            unsigned char *dst = out->bold + out->bold_length;
            for (int row = 0; row < out->height; row++) {
                for (int col = 0; col < bw; col++) {
                    int a = (col < w) ? src[row * w + col] : 0;
                    int b = (col > 0) ? src[row * w + col - 1] : 0;
                    dst[row * bw + col] = (unsigned char)(a > b ? a : b);
                }
            }
            out->bold_length += (unsigned)(bw * out->height);
        }
    }
}

static void ui_build_all(void) {
    ui_ui.ident    = "ui";
    ui_ui.height   = FONT_HEIGHT;
    ui_ui.cap_top  = CAP_TOP;
    ui_ui.x_top    = X_TOP;
    ui_ui.baseline = BASELINE;
    ui_ui.descriptor_last = DESCRIPTOR_LAST;
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
    ui_make_bold(&ui_ui);

    ui_small.ident    = "small";
    ui_small.height   = 12;
    ui_small.cap_top  = SM_CAP_TOP;
    ui_small.x_top    = SM_X_TOP;
    ui_small.baseline = SM_BASELINE;
    ui_small.descriptor_last = SM_DESCRIPTOR_LAST;
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

    ui_erode(&ui_ui, &ui_eroded);
    ui_eroded.ident = "ui-light";
    for (int code = 0x21; code <= 0x7E; code++) {
        if (ui_ink_top(&ui_ui, code) < 0) {
            continue;
        }
        if (ui_ink_top(&ui_eroded, code) < 0) {
            ui_error("thinning erased the glyph", ui_eroded.ident, code);
        } else if (ui_ink_top(&ui_eroded, code) != ui_ink_top(&ui_ui, code) ||
                   ui_ink_bottom(&ui_eroded, code) != ui_ink_bottom(&ui_ui, code)) {
            ui_error("thinning moved the glyph off its shared metric line",
                     ui_eroded.ident, code);
        }
    }
    for (int code = UI_SPECIAL_FIRST; code <= UI_SPECIAL_LAST; code++) {
        if (ui_ink_top(&ui_eroded, code) < 0) {
            ui_error("thinning erased a special glyph", ui_eroded.ident, code);
        }
    }
    ui_build_coverage(&ui_small, &ui_small, &cov_small, 1, 1, 0.0);
    ui_build_coverage(&ui_eroded, &ui_ui, &cov_ui, 1, 1, 0.0);
    ui_build_coverage(&ui_ui, &ui_large, &cov_large, 3, 2, UI_INSET_LARGE);

    ui_check_coverage(&cov_small, &ui_small);
    ui_check_coverage(&cov_ui, &ui_ui);
    ui_check_coverage(&cov_large, &ui_large);
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

static void emit_ui_coverage(const char *name, const unsigned char *data, unsigned int length) {
    emit("static const uint8_t %s[%u] = {\n", name, length);
    for (unsigned int i = 0; i < length; i += 24) {
        emit("   ");
        for (unsigned int j = i; j < i + 24 && j < length; j++) {
            emit("%4u,", data[j]);
        }
        emit("\n");
    }
    emit("};\n\n");
}

static void emit_ui_offsets(const char *name, const unsigned int *t) {
    emit("static const uint16_t %s[128] = {\n", name);
    for (int code = 0; code < 128; code += 8) {
        emit("   ");
        for (int i = 0; i < 8; i++) {
            emit("%7u,", t[code + i]);
        }
        emit("\n");
    }
    emit("};\n\n");
}

static void emit_user_interface_font(const user_interface_font_t *f, const coverage_font_t *c) {
    char buffer[64];

    snprintf(buffer, sizeof(buffer), "ui_coverage_%s", f->ident);
    emit_ui_coverage(buffer, c->data, c->length);
    snprintf(buffer, sizeof(buffer), "ui_offset_%s", f->ident);
    emit_ui_offsets(buffer, c->offset);
    if (f->has_bold) {
        snprintf(buffer, sizeof(buffer), "ui_coverage_bold_%s", f->ident);
        emit_ui_coverage(buffer, c->bold, c->bold_length);
        snprintf(buffer, sizeof(buffer), "ui_offset_bold_%s", f->ident);
        emit_ui_offsets(buffer, c->offset_bold);
    }
    snprintf(buffer, sizeof(buffer), "ui_adv_%s", f->ident);
    emit_ui_bytes(buffer, f->advance);
    snprintf(buffer, sizeof(buffer), "ui_wid_%s", f->ident);
    emit_ui_bytes(buffer, f->width);

    int max_adv = 0;
    for (int code = 0; code < 128; code++) {
        if (f->advance[code] > max_adv) max_adv = f->advance[code];
    }

    emit("const ui_font_t ui_font_%s = {\n", f->ident);
    emit("    .height = %d, .cap_top = %d, .x_top = %d, .baseline = %d,\n",
         f->height, f->cap_top, f->x_top, f->baseline);
    emit("    .descriptor_last = %d, .max_advance = %d,\n", f->descriptor_last, max_adv);
    emit("    .coverage = ui_coverage_%s,\n", f->ident);
    emit("    .offset = ui_offset_%s,\n", f->ident);
    if (f->has_bold) {
        emit("    .coverage_bold = ui_coverage_bold_%s,\n", f->ident);
        emit("    .offset_bold = ui_offset_bold_%s,\n", f->ident);
    } else {
        emit("    .coverage_bold = 0,\n");
        emit("    .offset_bold = 0,\n");
    }
    emit("    .advance = ui_adv_%s,\n", f->ident);
    emit("    .width = ui_wid_%s,\n", f->ident);
    emit("};\n\n");
}


static void build_user_interface_font_header(void) {
    out_length = 0;
    emit("#pragma once\n\n");
    emit("#include <stdint.h>\n\n");
    emit("#define UI_FONT_MAX_COLS %d\n", UI_MAX_COLS);
    emit("#define UI_FONT_MAX_ROWS %d\n\n", UI_MAX_ROWS);
    emit("typedef struct {\n"
         "    uint8_t height;\n"
         "    uint8_t cap_top;\n"
         "    uint8_t x_top;\n"
         "    uint8_t baseline;\n"
         "    uint8_t descriptor_last;\n"
         "    uint8_t max_advance;\n"
         "    const uint8_t  *coverage;\n"
         "    const uint8_t  *coverage_bold;\n"
         "    const uint16_t *offset;\n"
         "    const uint16_t *offset_bold;\n"
         "    const uint8_t  *advance;\n"
         "    const uint8_t  *width;\n"
         "} ui_font_t;\n\n");
    emit("extern const ui_font_t ui_font_small;\n");
    emit("extern const ui_font_t ui_font_ui;\n");
    emit("extern const ui_font_t ui_font_large;\n\n");
    emit("#define UI_FONT_SMALL_HEIGHT %d\n", ui_small.height);
    emit("#define UI_FONT_UI_HEIGHT    %d\n", ui_ui.height);
    emit("#define UI_FONT_LARGE_HEIGHT %d\n\n", ui_large.height);
    struct { const char *name; int code; } specials[] = {
        { "ARROW_LEFT",  UI_SPECIAL_ARROW_LEFT  },
        { "ARROW_RIGHT", UI_SPECIAL_ARROW_RIGHT },
        { "ARROW_UP",    UI_SPECIAL_ARROW_UP    },
        { "ARROW_DOWN",  UI_SPECIAL_ARROW_DOWN  },
        { "CHECK",       UI_SPECIAL_CHECK       },
        { "BULLET",      UI_SPECIAL_BULLET      },
        { "ELLIPSIS",    UI_SPECIAL_ELLIPSIS    },
        { "CLOSE",       UI_SPECIAL_CLOSE       },
    };
    for (size_t i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
        emit("#define UI_G_%-11s '\\x%02X'\n", specials[i].name, specials[i].code);
    }
    emit("\n");
    for (size_t i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
        emit("#define UI_S_%-11s \"\\x%02X\"\n", specials[i].name, specials[i].code);
    }
    emit("\n#define UI_GLYPH_SPECIAL_FIRST 0x%02X\n", UI_SPECIAL_FIRST);
    emit("#define UI_GLYPH_SPECIAL_LAST  0x%02X\n", UI_SPECIAL_LAST);
}

static void build_user_interface_font_source(void) {
    out_length = 0;
    emit("#include \"user_interface_font.h\"\n\n");
    emit_user_interface_font(&ui_small, &cov_small);
    emit_user_interface_font(&ui_ui, &cov_ui);
    emit_user_interface_font(&ui_large, &cov_large);
}

static int write_if(const char *path, int check_only) {
    FILE *f = fopen(path, check_only ? "rb" : "wb");
    if (!f) {
        fprintf(stderr, "gen-font: cannot open %s\n", path);
        return 1;
    }
    if (!check_only) {
        fwrite(out, 1, out_length, f);
        fclose(f);
        printf("gen-font: wrote %s (%zu bytes)\n", path, out_length);
        return 0;
    }

    static char have[1 << 22];
    size_t have_length = fread(have, 1, sizeof(have), f);
    fclose(f);

    if (have_length != out_length || memcmp(have, out, out_length) != 0) {
        size_t i = 0, line = 1;
        while (i < have_length && i < out_length && have[i] == out[i]) {
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

static void write_preview(const char *path) {
    static const char *SAMPLE = "Handgloves 0123 Settings Tasks Files";
    const coverage_font_t *faces[3] = { &cov_small, &cov_ui, &cov_large };
    const user_interface_font_t *metrics[3] = { &ui_small, &ui_ui, &ui_large };
    int pad = 8;
    int width = 960;
    int height = pad;
    for (int f = 0; f < 3; f++) {
        height += metrics[f]->height + pad;
    }
    height += metrics[1]->height + pad;

    static unsigned char image[960 * 400 * 3];
    for (int i = 0; i < width * height * 3; i++) {
        image[i] = 0x14;
    }

    int y = pad;
    for (int f = 0; f < 4; f++) {
        int face = f < 3 ? f : 1;
        int bold = f == 3;
        const coverage_font_t *c = faces[face];
        const user_interface_font_t *m = metrics[face];
        int x = pad;
        for (const char *p = SAMPLE; *p; p++) {
            int code = (unsigned char)*p;
            int w = bold ? c->bold_width[code] : c->width[code];
            const unsigned char *g = (bold ? c->bold : c->data) +
                                     (bold ? c->offset_bold[code] : c->offset[code]);
            if (bold && !c->has_bold) {
                break;
            }
            for (int row = 0; row < c->height; row++) {
                for (int col = 0; col < w; col++) {
                    int a = g[row * w + col];
                    int px = x + col, py = y + row;
                    if (px < 0 || px >= width || py < 0 || py >= height) {
                        continue;
                    }
                    for (int ch = 0; ch < 3; ch++) {
                        unsigned char *d = &image[(py * width + px) * 3 + ch];
                        *d = (unsigned char)((*d * (255 - a) + 0xF0 * a) / 255);
                    }
                }
            }
            x += m->advance[code];
        }
        y += c->height + pad;
    }

    FILE *out_file = fopen(path, "wb");
    if (!out_file) {
        fprintf(stderr, "gen-font: cannot write %s\n", path);
        return;
    }
    fprintf(out_file, "P6\n%d %d\n255\n", width, height);
    fwrite(image, 1, (size_t)(width * height * 3), out_file);
    fclose(out_file);
    printf("gen-font: preview written to %s (%dx%d)\n", path, width, height);
}

int main(int argc, char **argv) {
    int check_only;

    if (argc == 2 && strcmp(argv[1], "--check") == 0) {
        check_only = 1;
    } else if (argc == 2 && strcmp(argv[1], "--write") == 0) {
        check_only = 0;
    } else if (argc == 3 && strcmp(argv[1], "--preview") == 0) {
        build();
        ui_build_all();
        write_preview(argv[2]);
        return errors ? 1 : 0;
    } else {
        fprintf(stderr, "usage: %s --write | --check | --preview <file.ppm>\n", argv[0]);
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
    build_header(0); bad |= write_if("user_space/library/font8x16.h", check_only);
    build_source(0); bad |= write_if("user_space/library/font8x16.c", check_only);
    build_user_interface_font_header(); bad |= write_if("user_space/library/user_interface_font.h", check_only);
    build_user_interface_font_source(); bad |= write_if("user_space/library/user_interface_font.c", check_only);

    if (bad) return 1;
    if (check_only) printf("gen-font: all six font files match the generator.\n");
    return 0;
}
