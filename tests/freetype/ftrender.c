/* tests/freetype/ftrender.c - M100: freetype, graded against itself.
 *
 * ---- what this is for --------------------------------------------------
 *
 * freetype's tarball ships no test suite that runs without meson and a
 * network, and "the glyph looks right" is not an assertion. So this is a
 * differential test of the kind tools/sh-test.sh and tools/math-test.sh
 * are: one program, compiled twice from the same source - once for the
 * host against a host build of freetype 2.13.3, once for the machine
 * against the freetype the cross compiler built - and the two outputs
 * must be byte-identical. Nothing here says what a glyph should look
 * like. freetype on another machine decides.
 *
 * What it grades is not small. The TrueType bytecode interpreter, the
 * smooth rasterizer and the monochrome one are tens of thousands of
 * lines of fixed-point integer arithmetic, and every glyph's bitmap is
 * hashed - a compiler that gets one shift or one signed division wrong
 * anywhere in that changes the hash. It also grades this libc's FILE
 * layer on a 750 KB font: freetype's stdio stream seeks and reads its
 * tables piecemeal, and a wrong ftell is a wrong glyph.
 *
 * ---- what it prints ----------------------------------------------------
 *
 * One line per face with its metrics, then one line per (size, mode,
 * glyph) with the bitmap's geometry, the advance, and an FNV-1a hash of
 * the bitmap's bytes, then kerning for a few pairs, then a hash over
 * everything. Deterministic, text, no floats - so `cmp` decides.
 *
 * Usage: ftrender <font.ttf>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <ft2build.h>
#include FT_FREETYPE_H

static uint64_t fnv1a(uint64_t h, const unsigned char *p, size_t n) {
    while (n--) {
        h ^= *p++;
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t total;

static int render_range(FT_Face face, int px, int32_t flags, const char *mode) {
    if (FT_Set_Pixel_Sizes(face, 0, px) != 0) {
        printf("FT_Set_Pixel_Sizes(%d) failed\n", px);
        return 1;
    }
    int failures = 0;
    for (int c = 0x20; c < 0x7f; c++) {
        FT_Error err = FT_Load_Char(face, (FT_ULong)c, flags | FT_LOAD_RENDER);
        if (err != 0) {
            printf("%dpx %s U+%04X load error %d\n", px, mode, c, (int)err);
            failures++;
            continue;
        }
        FT_GlyphSlot g = face->glyph;
        FT_Bitmap *b = &g->bitmap;
        /* The bitmap is hashed row by row for exactly `width` bytes (or
         * width/8 rounded up for mono), never the pitch: pitch padding is
         * allocator territory and may legitimately differ. */
        unsigned rowbytes = b->pixel_mode == FT_PIXEL_MODE_MONO
                                ? (b->width + 7) / 8 : b->width;
        uint64_t h = 1469598103934665603ULL;
        for (unsigned r = 0; r < b->rows; r++) {
            h = fnv1a(h, b->buffer + (long)r * b->pitch, rowbytes);
        }
        printf("%dpx %s U+%04X gid %u %ux%u left %d top %d adv %ld mode %d hash %016llx\n",
               px, mode, c, (unsigned)FT_Get_Char_Index(face, (FT_ULong)c),
               (unsigned)b->width, (unsigned)b->rows, (int)g->bitmap_left,
               (int)g->bitmap_top, (long)g->advance.x, (int)b->pixel_mode,
               (unsigned long long)h);
        total = fnv1a(total, (const unsigned char *)&h, sizeof h);
    }
    return failures;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: ftrender <font.ttf>\n");
        return 2;
    }
    FT_Library lib;
    FT_Error err = FT_Init_FreeType(&lib);
    if (err != 0) {
        printf("FT_Init_FreeType failed: %d\n", (int)err);
        return 1;
    }
    FT_Int vmaj, vmin, vpatch;
    FT_Library_Version(lib, &vmaj, &vmin, &vpatch);
    printf("freetype %d.%d.%d\n", (int)vmaj, (int)vmin, (int)vpatch);

    FT_Face face;
    err = FT_New_Face(lib, argv[1], 0, &face);
    if (err != 0) {
        printf("FT_New_Face(%s) failed: %d\n", argv[1], (int)err);
        return 1;
    }
    printf("face %s / %s glyphs %ld upem %u ascender %d descender %d height %d "
           "bbox %ld %ld %ld %ld scalable %d kerning %d\n",
           face->family_name, face->style_name, (long)face->num_glyphs,
           (unsigned)face->units_per_EM, (int)face->ascender,
           (int)face->descender, (int)face->height,
           (long)face->bbox.xMin, (long)face->bbox.yMin,
           (long)face->bbox.xMax, (long)face->bbox.yMax,
           FT_IS_SCALABLE(face) ? 1 : 0, FT_HAS_KERNING(face) ? 1 : 0);

    total = 1469598103934665603ULL;
    int failures = 0;
    /* Three renderers and two hinting regimes:
     *   default   the TrueType interpreter (v40) into the smooth rasterizer
     *   nohint    outlines scaled and rasterized with no bytecode run
     *   mono      the monochrome rasterizer, which is a separate module
     *   autohint  freetype's own hinter rather than the font's bytecode */
    failures += render_range(face, 12, FT_LOAD_DEFAULT, "default");
    failures += render_range(face, 24, FT_LOAD_DEFAULT, "default");
    failures += render_range(face, 48, FT_LOAD_DEFAULT, "default");
    failures += render_range(face, 100, FT_LOAD_NO_HINTING, "nohint");
    failures += render_range(face, 16, FT_LOAD_TARGET_MONO, "mono");
    failures += render_range(face, 20, FT_LOAD_FORCE_AUTOHINT, "autohint");

    /* Kerning is a table lookup plus a scale, and the pairs below are
     * ones a Latin font actually kerns. At 48px so the deltas are not
     * all rounded to zero. */
    FT_Set_Pixel_Sizes(face, 0, 48);
    static const char PAIRS[][2] = {{'A','V'},{'T','o'},{'W','a'},{'L','T'},{'f','f'},{'r','.'}};
    for (unsigned i = 0; i < sizeof(PAIRS) / sizeof(PAIRS[0]); i++) {
        FT_Vector k = {0, 0};
        FT_UInt l = FT_Get_Char_Index(face, (FT_ULong)PAIRS[i][0]);
        FT_UInt r = FT_Get_Char_Index(face, (FT_ULong)PAIRS[i][1]);
        err = FT_Get_Kerning(face, l, r, FT_KERNING_DEFAULT, &k);
        printf("kern %c%c err %d x %ld y %ld\n", PAIRS[i][0], PAIRS[i][1],
               (int)err, (long)k.x, (long)k.y);
        total = fnv1a(total, (const unsigned char *)&k, sizeof k);
    }

    printf("glyph load failures %d\n", failures);
    printf("total %016llx\n", (unsigned long long)total);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return failures ? 1 : 0;
}
