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
    failures += render_range(face, 12, FT_LOAD_DEFAULT, "default");
    failures += render_range(face, 24, FT_LOAD_DEFAULT, "default");
    failures += render_range(face, 48, FT_LOAD_DEFAULT, "default");
    failures += render_range(face, 100, FT_LOAD_NO_HINTING, "nohint");
    failures += render_range(face, 16, FT_LOAD_TARGET_MONO, "mono");
    failures += render_range(face, 20, FT_LOAD_FORCE_AUTOHINT, "autohint");

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
