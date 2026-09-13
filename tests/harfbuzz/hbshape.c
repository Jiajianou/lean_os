#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <hb.h>
#include <hb-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

static uint64_t total = 1469598103934665603ULL;

static void mix(uint64_t v) {
    for (int i = 0; i < 8; i++) {
        total ^= (v >> (8 * i)) & 0xff;
        total *= 1099511628211ULL;
    }
}

static const struct {
    const char *name;
    const char *text;
    hb_script_t script;
    hb_direction_t dir;
    const char *lang;
} CASES[] = {
    {"latin-ligatures", "office waffle fjord AVAST To.", HB_SCRIPT_LATIN, HB_DIRECTION_LTR, "en"},
    {"greek", "Ἀθῆναι καὶ Σπάρτη", HB_SCRIPT_GREEK, HB_DIRECTION_LTR, "el"},
    {"cyrillic", "Съешь же ещё этих мягких булок", HB_SCRIPT_CYRILLIC, HB_DIRECTION_LTR, "ru"},
    {"arabic", "السلام عليكم ورحمة الله", HB_SCRIPT_ARABIC, HB_DIRECTION_RTL, "ar"},
    {"hebrew-marks", "שָׁלוֹם עֲלֵיכֶם", HB_SCRIPT_HEBREW, HB_DIRECTION_RTL, "he"},
    {"mixed-digits", "x² + y² = r², 3.14159", HB_SCRIPT_LATIN, HB_DIRECTION_LTR, "en"},
};

static void shape_and_print(hb_font_t *font, const char *how, unsigned c) {
    hb_buffer_t *buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, CASES[c].text, -1, 0, -1);
    hb_buffer_set_script(buf, CASES[c].script);
    hb_buffer_set_direction(buf, CASES[c].dir);
    hb_buffer_set_language(buf, hb_language_from_string(CASES[c].lang, -1));
    hb_shape(font, buf, NULL, 0);
    unsigned n = hb_buffer_get_length(buf);
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, NULL);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, NULL);
    printf("%s %s: %u glyphs\n", how, CASES[c].name, n);
    long pen = 0;
    for (unsigned i = 0; i < n; i++) {
        printf("  gid %u cluster %u adv %d,%d off %d,%d\n",
               (unsigned)info[i].codepoint, (unsigned)info[i].cluster,
               (int)pos[i].x_advance, (int)pos[i].y_advance,
               (int)pos[i].x_offset, (int)pos[i].y_offset);
        mix(info[i].codepoint);
        mix(info[i].cluster);
        mix((uint64_t)(int64_t)pos[i].x_advance);
        mix((uint64_t)(int64_t)pos[i].x_offset);
        mix((uint64_t)(int64_t)pos[i].y_offset);
        pen += pos[i].x_advance;
    }
    printf("  width %ld\n", pen);
    hb_buffer_destroy(buf);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: hbshape <font.ttf>\n");
        return 2;
    }
    printf("harfbuzz %s\n", hb_version_string());

    hb_blob_t *blob = hb_blob_create_from_file_or_fail(argv[1]);
    if (!blob) {
        printf("could not read %s\n", argv[1]);
        return 1;
    }
    hb_face_t *face = hb_face_create(blob, 0);
    hb_font_t *font = hb_font_create(face);
    printf("face upem %u glyphs %u\n", (unsigned)hb_face_get_upem(face),
           (unsigned)hb_face_get_glyph_count(face));
    for (unsigned c = 0; c < sizeof(CASES) / sizeof(CASES[0]); c++) {
        shape_and_print(font, "ot", c);
    }
    hb_font_destroy(font);
    hb_face_destroy(face);
    hb_blob_destroy(blob);

    FT_Library lib;
    FT_Face ftface;
    if (FT_Init_FreeType(&lib) != 0 || FT_New_Face(lib, argv[1], 0, &ftface) != 0) {
        printf("freetype could not open %s\n", argv[1]);
        return 1;
    }
    FT_Set_Pixel_Sizes(ftface, 0, 24);
    hb_font_t *ftfont = hb_ft_font_create_referenced(ftface);
    hb_ft_font_set_load_flags(ftfont, FT_LOAD_DEFAULT);
    for (unsigned c = 0; c < sizeof(CASES) / sizeof(CASES[0]); c++) {
        shape_and_print(ftfont, "ft", c);
    }
    hb_font_destroy(ftfont);
    FT_Done_Face(ftface);
    FT_Done_FreeType(lib);

    printf("total %016llx\n", (unsigned long long)total);
    return 0;
}
