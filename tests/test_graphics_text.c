#include "check.h"

#include "../user_space/library/graphics.h"
#include "../user_space/library/user_interface_font.h"

#define CANVAS_W 256
#define CANVAS_H 40

static uint32_t canvas[CANVAS_W * CANVAS_H];
static graphics_context_t context = { canvas, CANVAS_W, CANVAS_H };

#define INK 0x00FFFFFFu
#define BG  0x00000000u

static void clear(void) {
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) {
        canvas[i] = BG;
    }
}

static const ui_font_t *FACES[3];
static const char *FACE_NAMES[3] = { "small", "ui", "large" };

static void faces(void) {
    FACES[0] = &ui_font_small;
    FACES[1] = &ui_font_ui;
    FACES[2] = &ui_font_large;
}

TEST(graphics_text, blending_nothing_and_everything_are_the_endpoints) {
    clear();
    graphics_blend_pixel(&context, 1, 1, INK, 255);
    CHECK_EQ(canvas[1 * CANVAS_W + 1], INK);

    clear();
    graphics_blend_pixel(&context, 1, 1, INK, 0);
    CHECK_EQ(canvas[1 * CANVAS_W + 1], BG);

    clear();
    graphics_blend_pixel(&context, 1, 1, 0x00FFFFFFu, 128);
    uint32_t half = canvas[1 * CANVAS_W + 1];
    CHECK_EQ(half & 0xFFu, 128u);
    CHECK_EQ((half >> 8) & 0xFFu, 128u);
    CHECK_EQ((half >> 16) & 0xFFu, 128u);
}

TEST(graphics_text, blending_leaves_the_canvas_alone_outside_it) {
    clear();
    graphics_blend_pixel(&context, -1, 0, INK, 255);
    graphics_blend_pixel(&context, 0, -1, INK, 255);
    graphics_blend_pixel(&context, CANVAS_W, 0, INK, 255);
    graphics_blend_pixel(&context, 0, CANVAS_H, INK, 255);
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) {
        if (canvas[i] != BG) {
            test_fail(__FILE__, __LINE__, "a blend outside the canvas wrote at index %d", i);
            break;
        }
    }
}

static uint32_t expected_channel(uint32_t behind, uint32_t ink, uint32_t alpha) {
    return (ink * alpha + behind * (255u - alpha) + 127u) / 255u;
}

TEST(graphics_text, the_divisionless_blend_agrees_with_dividing_by_255) {
    for (uint32_t ink = 0; ink <= 255; ink++) {
        for (uint32_t alpha = 0; alpha <= 255; alpha++) {
            uint32_t behind = (ink * 7u + alpha * 13u) & 0xFFu;
            uint32_t v = ink * alpha + behind * (255u - alpha);
            uint32_t want = (v + 127u) / 255u;
            uint32_t got = GRAPHICS_OVER_255(v);
            CHECK_MSG(got == want,
                      "ink %u over %u at alpha %u: shift gave %u, division gives %u",
                      ink, behind, alpha, got, want);
        }
    }
}

TEST(graphics_text, a_partial_alpha_is_an_exact_interpolation) {
    static const uint32_t BEHIND[] = { 0x00000000u, 0x00202020u, 0x00808080u, 0x00FFFFFFu };
    static const uint32_t INKS[]   = { 0x00000000u, 0x00A0A0A0u, 0x00FFFFFFu };
    static const uint32_t ALPHAS[] = { 1, 32, 64, 127, 128, 200, 254 };

    for (unsigned b = 0; b < sizeof(BEHIND) / sizeof(BEHIND[0]); b++) {
        for (unsigned i = 0; i < sizeof(INKS) / sizeof(INKS[0]); i++) {
            for (unsigned a = 0; a < sizeof(ALPHAS) / sizeof(ALPHAS[0]); a++) {
                clear();
                canvas[0] = BEHIND[b];
                graphics_blend_pixel(&context, 0, 0, INKS[i], ALPHAS[a]);
                uint32_t want = expected_channel(BEHIND[b] & 0xFFu, INKS[i] & 0xFFu, ALPHAS[a]);
                uint32_t got = canvas[0] & 0xFFu;
                CHECK_MSG(got == want,
                          "blending ink %06x over %06x at alpha %u gave %u, wanted %u",
                          INKS[i], BEHIND[b], ALPHAS[a], got, want);
            }
        }
    }
}

TEST(graphics_text, a_low_alpha_stays_close_to_what_was_behind_it) {
    clear();
    canvas[0] = 0x00808080u;
    graphics_blend_pixel(&context, 0, 0, 0x00FFFFFFu, 64);
    uint32_t quarter = canvas[0] & 0xFFu;
    clear();
    canvas[0] = 0x00808080u;
    graphics_blend_pixel(&context, 0, 0, 0x00FFFFFFu, 192);
    uint32_t three_quarters = canvas[0] & 0xFFu;
    CHECK_MSG(quarter < three_quarters,
              "a quarter-covered pixel (%u) is not closer to the background than a "
              "three-quarter-covered one (%u)", quarter, three_quarters);
    CHECK_MSG(quarter - 0x80u < 0x80u - 0x00u,
              "a quarter-covered pixel moved more than a quarter of the way");
}

TEST(graphics_text, every_face_keeps_its_ink_inside_the_declared_glyph_box) {
    faces();
    for (int f = 0; f < 3; f++) {
        const ui_font_t *face = FACES[f];
        for (int code = 0x21; code <= 0x7E; code++) {
            clear();
            graphics_draw_char_font(&context, 0, 0, (char)code, INK, face, 0);
            for (int y = 0; y < CANVAS_H; y++) {
                for (int x = 0; x < CANVAS_W; x++) {
                    if (canvas[y * CANVAS_W + x] == BG) {
                        continue;
                    }
                    CHECK_MSG(x < face->width[code],
                              "%s 0x%02X drew ink at column %d, outside its %u-wide box",
                              FACE_NAMES[f], code, x, face->width[code]);
                    CHECK_MSG(y < face->height,
                              "%s 0x%02X drew ink at row %d, below its %u-row cell",
                              FACE_NAMES[f], code, y, face->height);
                }
            }
        }
    }
}

TEST(graphics_text, every_face_is_actually_anti_aliased) {
    faces();
    for (int f = 0; f < 3; f++) {
        int partial = 0;
        for (int code = 0x21; code <= 0x7E && !partial; code++) {
            clear();
            graphics_draw_char_font(&context, 0, 0, (char)code, INK, FACES[f], 0);
            for (int i = 0; i < CANVAS_W * CANVAS_H; i++) {
                if (canvas[i] != BG && canvas[i] != INK) {
                    partial = 1;
                    break;
                }
            }
        }
        CHECK_MSG(partial, "the %s face drew no partly-covered pixel - it is not anti-aliased",
                  FACE_NAMES[f]);
    }
}

TEST(graphics_text, a_bold_glyph_covers_everything_its_regular_weight_covers) {
    faces();
    const ui_font_t *face = &ui_font_ui;
    REQUIRE(face->coverage_bold != 0);
    for (int code = 0x21; code <= 0x7E; code++) {
        static uint32_t regular[CANVAS_W * CANVAS_H];
        clear();
        graphics_draw_char_font(&context, 0, 0, (char)code, INK, face, 0);
        for (int i = 0; i < CANVAS_W * CANVAS_H; i++) {
            regular[i] = canvas[i];
        }
        clear();
        graphics_draw_char_font(&context, 0, 0, (char)code, INK, face, 1);
        for (int i = 0; i < CANVAS_W * CANVAS_H; i++) {
            if (regular[i] != BG && canvas[i] == BG) {
                test_fail(__FILE__, __LINE__,
                          "bold 0x%02X is missing ink the regular weight has at index %d",
                          code, i);
                return;
            }
        }
    }
}

TEST(graphics_text, measured_width_covers_the_ink_that_gets_drawn) {
    faces();
    static const char *SAMPLES[] = {
        "Settings", "Tasks", "Handgloves", "00:00", "iiiii", "MMMMM", "0123456789",
    };
    for (int f = 0; f < 3; f++) {
        for (unsigned s = 0; s < sizeof(SAMPLES) / sizeof(SAMPLES[0]); s++) {
            int32_t want = graphics_text_width(FACES[f], SAMPLES[s]);
            clear();
            graphics_draw_text_font(&context, 0, 0, SAMPLES[s], INK, FACES[f], 0);
            int32_t rightmost = -1;
            for (int y = 0; y < CANVAS_H; y++) {
                for (int x = 0; x < CANVAS_W; x++) {
                    if (canvas[y * CANVAS_W + x] != BG && x > rightmost) {
                        rightmost = x;
                    }
                }
            }
            CHECK_MSG(rightmost >= 0, "%s drew nothing for %s", FACE_NAMES[f], SAMPLES[s]);
            CHECK_MSG(rightmost < want,
                      "%s drew %s out to column %d, past its measured width %d",
                      FACE_NAMES[f], SAMPLES[s], rightmost, want);
        }
    }
}

TEST(graphics_text, the_three_faces_are_ordered_by_height) {
    faces();
    CHECK(ui_font_small.height < ui_font_ui.height);
    CHECK(ui_font_ui.height < ui_font_large.height);
}
