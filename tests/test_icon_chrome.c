#include "check.h"

#include "../system_api/include/icon.h"
#include "../user_space/library/graphics.h"
#include "../user_space/library/icons.h"

#define CANVAS 96
static uint32_t canvas[CANVAS * CANVAS];
static graphics_context_t context = { canvas, CANVAS, CANVAS };

#define GROUND 0x00202040u
#define INK    0x00FFFFFFu

static void clear(void) {
    for (int i = 0; i < CANVAS * CANVAS; i++) {
        canvas[i] = GROUND;
    }
}

static uint32_t at(int x, int y) {
    return canvas[y * CANVAS + x];
}

static int painted(int x, int y) {
    return at(x, y) != GROUND;
}

static int painted_count(void) {
    int n = 0;
    for (int i = 0; i < CANVAS * CANVAS; i++) {
        if (canvas[i] != GROUND) {
            n++;
        }
    }
    return n;
}

TEST(rounded, a_radius_of_zero_is_exactly_a_rectangle) {
    clear();
    graphics_fill_rounded(&context, 10, 10, 20, 16, 0, INK, 255);
    for (int y = 10; y < 26; y++) {
        for (int x = 10; x < 30; x++) {
            CHECK_EQ(at(x, y), INK);
        }
    }
    CHECK_EQ(painted_count(), 20 * 16);
}

TEST(rounded, the_corners_come_off_and_the_middle_stays) {
    clear();
    graphics_fill_rounded(&context, 10, 10, 40, 40, 12, INK, 255);
    CHECK_MSG(!painted(10, 10), "the top-left corner pixel survived a radius of 12");
    CHECK_MSG(!painted(49, 10), "the top-right corner pixel survived a radius of 12");
    CHECK_MSG(!painted(10, 49), "the bottom-left corner pixel survived a radius of 12");
    CHECK_MSG(!painted(49, 49), "the bottom-right corner pixel survived a radius of 12");
    CHECK_EQ(at(30, 30), INK);
    CHECK_EQ(at(30, 10), INK);
    CHECK_EQ(at(10, 30), INK);
}

TEST(rounded, a_bigger_radius_removes_strictly_more_ink) {
    int previous = -1;
    for (int radius = 0; radius <= 20; radius += 4) {
        clear();
        graphics_fill_rounded(&context, 10, 10, 40, 40, radius, INK, 255);
        int n = painted_count();
        if (previous >= 0) {
            CHECK_MSG(n < previous, "a larger corner radius did not remove more ink");
        }
        previous = n;
    }
}

TEST(rounded, the_corner_is_anti_aliased) {
    clear();
    graphics_fill_rounded(&context, 10, 10, 40, 40, 12, INK, 255);
    int partial = 0;
    for (int y = 10; y < 24; y++) {
        for (int x = 10; x < 24; x++) {
            uint32_t pixel = at(x, y);
            if (pixel != GROUND && pixel != INK) {
                partial++;
            }
        }
    }
    CHECK_MSG(partial >= 8, "a rounded corner produced no partly covered pixels");
}

TEST(rounded, the_shape_is_symmetric_in_both_axes) {
    clear();
    graphics_fill_rounded(&context, 10, 10, 40, 40, 13, INK, 255);
    for (int y = 0; y < 40; y++) {
        for (int x = 0; x < 40; x++) {
            CHECK_EQ(at(10 + x, 10 + y), at(10 + 39 - x, 10 + y));
            CHECK_EQ(at(10 + x, 10 + y), at(10 + x, 10 + 39 - y));
        }
    }
}

TEST(rounded, a_fill_never_leaves_its_box) {
    clear();
    graphics_fill_rounded(&context, 10, 10, 40, 40, 12, INK, 255);
    for (int y = 0; y < CANVAS; y++) {
        for (int x = 0; x < CANVAS; x++) {
            if (x >= 10 && x < 50 && y >= 10 && y < 50) {
                continue;
            }
            CHECK_MSG(!painted(x, y), "a rounded fill painted outside its rectangle");
        }
    }
}

TEST(rounded, a_stroke_leaves_the_interior_alone) {
    clear();
    graphics_stroke_rounded(&context, 10, 10, 40, 40, 12, INK, 255);
    for (int y = 14; y < 46; y++) {
        for (int x = 14; x < 46; x++) {
            CHECK_MSG(!painted(x, y), "a rounded stroke painted into its own interior");
        }
    }
    CHECK_MSG(painted(30, 10), "a rounded stroke left no ink on its top edge");
    CHECK_MSG(painted(10, 30), "a rounded stroke left no ink on its left edge");
    CHECK_MSG(painted(30, 49), "a rounded stroke left no ink on its bottom edge");
    CHECK_MSG(painted(49, 30), "a rounded stroke left no ink on its right edge");
}

TEST(rounded, a_transparent_fill_paints_nothing) {
    clear();
    graphics_fill_rounded(&context, 10, 10, 40, 40, 12, INK, 0);
    CHECK_EQ(painted_count(), 0);
}

TEST(icon_format, both_versions_are_recognised_and_sized) {
    static const uint8_t palette_icon[] = {
        'L', 'I', 'C', '1', 4, 4, 2, 0,
        0, 0, 0, 0xFF, 0xFF, 0xFF,
        0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
    };
    CHECK_MSG(icon_valid(palette_icon), "a version 1 icon was rejected");
    CHECK_MSG(!icon_is_truecolor(palette_icon), "a version 1 icon claimed to be truecolour");
    CHECK_EQ(icon_bytes(palette_icon), (int)sizeof(palette_icon));

    CHECK_MSG(icon_valid(ICON_TERMINAL), "the generated Terminal icon was rejected");
    CHECK_MSG(icon_is_truecolor(ICON_TERMINAL), "the generated icon is not truecolour");
    CHECK_EQ(icon_width(ICON_TERMINAL), ICON_LARGE_SIZE);
    CHECK_EQ(icon_height(ICON_TERMINAL), ICON_LARGE_SIZE);
    CHECK_EQ(icon_bytes(ICON_TERMINAL),
             ICON_HEADER_BYTES + 4 * ICON_LARGE_SIZE * ICON_LARGE_SIZE);
    CHECK_MSG(icon_bytes(ICON_TERMINAL) <= ICON_MAX_BYTES, "a shipped icon exceeds ICON_MAX_BYTES");

    static const uint8_t wrong_magic[] = { 'L', 'I', 'C', '3', 4, 4, 2, 0 };
    CHECK_MSG(!icon_valid(wrong_magic), "an unknown icon version was accepted");
}

TEST(icon_draw, transparent_pixels_leave_the_ground_and_opaque_ones_replace_it) {
    static const uint8_t blob[] = {
        'L', 'I', 'C', '2', 2, 2, 0, 0,
        0xFF, 0x00, 0x00, 0xFF,
        0x00, 0xFF, 0x00, 0x00,
        0x00, 0x00, 0xFF, 128,
        0xFF, 0xFF, 0xFF, 0xFF,
    };
    clear();
    icon_draw(&context, 4, 4, blob, 1);
    CHECK_EQ(at(4, 4), 0x00FF0000u);
    CHECK_EQ(at(5, 4), GROUND);
    CHECK_EQ(at(5, 5), 0x00FFFFFFu);
    uint32_t blended = at(4, 5);
    CHECK_MSG(blended != GROUND, "a half-transparent icon pixel painted nothing");
    CHECK_MSG(blended != 0x000000FFu, "a half-transparent icon pixel painted as if opaque");
    CHECK_MSG((blended & 0xFFu) > ((GROUND) & 0xFFu), "blending did not move towards the icon colour");
}

TEST(icon_draw, scale_replicates_every_pixel) {
    static const uint8_t blob[] = {
        'L', 'I', 'C', '2', 1, 1, 0, 0,
        0xFF, 0xFF, 0xFF, 0xFF,
    };
    clear();
    icon_draw(&context, 4, 4, blob, 3);
    for (int y = 4; y < 7; y++) {
        for (int x = 4; x < 7; x++) {
            CHECK_EQ(at(x, y), 0x00FFFFFFu);
        }
    }
    CHECK_EQ(painted_count(), 9);
}

TEST(icon_draw, a_full_tint_replaces_the_colour_but_keeps_the_coverage) {
    static const uint8_t blob[] = {
        'L', 'I', 'C', '2', 2, 1, 0, 0,
        0xFF, 0x00, 0x00, 0xFF,
        0x00, 0xFF, 0x00, 0x00,
    };
    clear();
    icon_draw_tinted(&context, 4, 4, blob, 1, 0x00123456u, 100);
    CHECK_EQ(at(4, 4), 0x00123456u);
    CHECK_EQ(at(5, 4), GROUND);
}

TEST(icon_draw, every_shipped_icon_is_anti_aliased_and_stays_in_its_box) {
    static const uint8_t *const ICONS[] = {
        ICON_TERMINAL, ICON_EDITOR, ICON_FILES, ICON_SETTINGS, ICON_CLOCK,
        ICON_PAINT, ICON_TASKS, ICON_BROWSER, ICON_APPLICATION,
    };
    for (unsigned i = 0; i < sizeof(ICONS) / sizeof(ICONS[0]); i++) {
        const uint8_t *pixels = ICONS[i] + ICON_HEADER_BYTES;
        int partial = 0;
        int opaque = 0;
        for (int p = 0; p < ICON_LARGE_SIZE * ICON_LARGE_SIZE; p++) {
            uint8_t alpha = pixels[p * 4 + 3];
            if (alpha > 8 && alpha < 247) {
                partial++;
            }
            if (alpha == 255) {
                opaque++;
            }
        }
        CHECK_MSG(partial >= ICON_LARGE_SIZE, "a shipped icon has no anti-aliased edge");
        CHECK_MSG(opaque > ICON_LARGE_SIZE * ICON_LARGE_SIZE / 4, "a shipped icon is mostly empty");

        clear();
        icon_draw(&context, 8, 8, ICONS[i], 1);
        for (int y = 0; y < CANVAS; y++) {
            for (int x = 0; x < CANVAS; x++) {
                if (x >= 8 && x < 8 + ICON_LARGE_SIZE && y >= 8 && y < 8 + ICON_LARGE_SIZE) {
                    continue;
                }
                CHECK_MSG(!painted(x, y), "a shipped icon painted outside its own box");
            }
        }
    }
}

TEST(rounded, a_fully_covered_stroke_pixel_is_exactly_a_blend_at_the_asked_alpha) {
    clear();
    graphics_stroke_rounded(&context, 10, 10, 40, 40, 12, INK, 128);
    uint32_t edge = at(30, 10);

    clear();
    graphics_blend_pixel(&context, 30, 10, INK, 128);
    uint32_t direct = at(30, 10);

    CHECK_EQ(edge, direct);
    CHECK_MSG(edge != GROUND, "a stroke at half alpha painted nothing on its straight edge");
    CHECK_MSG(edge != INK, "a stroke at half alpha painted as if it were opaque");
}

TEST(rounded, a_fully_covered_fill_pixel_is_exactly_a_blend_at_the_asked_alpha) {
    clear();
    graphics_fill_rounded(&context, 10, 10, 40, 40, 12, INK, 96);
    uint32_t middle = at(30, 30);

    clear();
    graphics_blend_pixel(&context, 30, 30, INK, 96);
    CHECK_EQ(middle, at(30, 30));
}

TEST(rounded, a_box_too_small_to_have_two_edges_is_refused_rather_than_drawn_inside_out) {
    clear();
    graphics_stroke_rounded(&context, 10, 10, 2, 40, 1, INK, 255);
    CHECK_EQ(painted_count(), 0);

    clear();
    graphics_stroke_rounded(&context, 10, 10, 40, 2, 1, INK, 255);
    CHECK_EQ(painted_count(), 0);

    clear();
    graphics_fill_rounded(&context, 10, 10, 0, 40, 4, INK, 255);
    CHECK_EQ(painted_count(), 0);

    clear();
    graphics_fill_rounded(&context, 10, 10, 40, 0, 4, INK, 255);
    CHECK_EQ(painted_count(), 0);
}

TEST(rounded, the_radius_is_clamped_to_what_the_box_can_hold) {
    CHECK_EQ(graphics_clamp_radius(40, 40, 12), 12);
    CHECK_EQ(graphics_clamp_radius(10, 40, 12), 5);
    CHECK_EQ(graphics_clamp_radius(40, 6, 12), 3);
    CHECK_EQ(graphics_clamp_radius(40, 40, 0), 0);
    CHECK_EQ(graphics_clamp_radius(40, 40, -3), 0);
}

TEST(rounded, only_the_rows_within_the_radius_are_inset) {
    for (int radius = 0; radius <= 20; radius++) {
        int h = 2 * radius + 6;
        for (int row = radius; row < h - radius; row++) {
            CHECK_MSG(graphics_rounded_edge_subpixels(row, h, radius) == 0,
                      "row %d of %d at radius %d is inset, so the straight-row fast path "
                      "would skip pixels it should paint", row, h, radius);
        }
        for (int row = 0; row < radius; row++) {
            CHECK_MSG(graphics_rounded_edge_subpixels(row, h, radius) > 0,
                      "row %d of %d at radius %d has no inset, so the corner is square there",
                      row, h, radius);
        }
    }
}

TEST(rounded, span_coverage_is_the_overlap_of_a_pixel_with_the_span) {
    CHECK_EQ(graphics_span_coverage(0, 0, GRAPHICS_SUBPIXEL), 255);
    CHECK_EQ(graphics_span_coverage(1, 0, GRAPHICS_SUBPIXEL), 0);
    CHECK_EQ(graphics_span_coverage(0, GRAPHICS_SUBPIXEL, 2 * GRAPHICS_SUBPIXEL), 0);
    CHECK_EQ(graphics_span_coverage(0, GRAPHICS_SUBPIXEL / 2, GRAPHICS_SUBPIXEL),
             (GRAPHICS_SUBPIXEL / 2) * 255 / GRAPHICS_SUBPIXEL);
    CHECK_EQ(graphics_span_coverage(5, 0, 100 * GRAPHICS_SUBPIXEL), 255);
}

static uint32_t reference[CANVAS * CANVAS];

static void reference_stroke(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                             uint32_t color, uint32_t alpha) {
    graphics_context_t target = { reference, CANVAS, CANVAS };
    for (int i = 0; i < CANVAS * CANVAS; i++) {
        reference[i] = GROUND;
    }
    if (w <= 2 || h <= 2 || alpha == 0) {
        return;
    }
    radius = graphics_clamp_radius(w, h, radius);
    int32_t inner_radius = radius > 0 ? radius - 1 : 0;
    for (int32_t row = 0; row < h; row++) {
        int32_t outer_left = graphics_rounded_edge_subpixels(row, h, radius);
        int32_t outer_right = w * GRAPHICS_SUBPIXEL - outer_left;
        int32_t inner_left;
        int32_t inner_right;
        if (row == 0 || row == h - 1) {
            inner_left = outer_right;
            inner_right = outer_right;
        } else {
            inner_left = GRAPHICS_SUBPIXEL +
                         graphics_rounded_edge_subpixels(row - 1, h - 2, inner_radius);
            inner_right = (w - 1) * GRAPHICS_SUBPIXEL - (inner_left - GRAPHICS_SUBPIXEL);
        }
        for (int32_t column = 0; column < w; column++) {
            int32_t outer = (int32_t)graphics_span_coverage(column, outer_left, outer_right);
            int32_t inner = (int32_t)graphics_span_coverage(column, inner_left, inner_right);
            int32_t coverage = outer - inner;
            if (coverage > 0) {
                graphics_blend_pixel(&target, x + column, y + row, color,
                                     (uint32_t)coverage * alpha / 255);
            }
        }
    }
}

TEST(rounded, the_banded_stroke_paints_exactly_what_a_full_scan_would) {
    static const int32_t SIZES[] = {3, 4, 12, 30, 64};
    static const int32_t RADII[] = {0, 1, 4, 9, 30};
    for (unsigned si = 0; si < sizeof(SIZES) / sizeof(SIZES[0]); si++) {
        for (unsigned sj = 0; sj < sizeof(SIZES) / sizeof(SIZES[0]); sj++) {
            for (unsigned ri = 0; ri < sizeof(RADII) / sizeof(RADII[0]); ri++) {
                int32_t w = SIZES[si];
                int32_t h = SIZES[sj];
                int32_t radius = RADII[ri];
                clear();
                graphics_stroke_rounded(&context, 8, 8, w, h, radius, INK, 200);
                reference_stroke(8, 8, w, h, radius, INK, 200);
                for (int i = 0; i < CANVAS * CANVAS; i++) {
                    CHECK_MSG(canvas[i] == reference[i],
                              "the banded stroke and a full scan disagree at pixel %d "
                              "for a %dx%d box at radius %d", i, w, h, radius);
                }
            }
        }
    }
}

static uint32_t outer_fill[CANVAS * CANVAS];
static uint32_t inner_fill[CANVAS * CANVAS];

TEST(rounded, a_stroke_is_exactly_the_outer_fill_minus_the_one_pixel_inset_fill) {
    static const int32_t SIZES[] = {6, 20, 41, 64};
    static const int32_t RADII[] = {0, 1, 5, 12};
    graphics_context_t outer_context = { outer_fill, CANVAS, CANVAS };
    graphics_context_t inner_context = { inner_fill, CANVAS, CANVAS };
    for (unsigned si = 0; si < sizeof(SIZES) / sizeof(SIZES[0]); si++) {
        for (unsigned ri = 0; ri < sizeof(RADII) / sizeof(RADII[0]); ri++) {
            int32_t w = SIZES[si];
            int32_t h = SIZES[(si + 1) % (sizeof(SIZES) / sizeof(SIZES[0]))];
            int32_t radius = RADII[ri];
            int32_t clamped = graphics_clamp_radius(w, h, radius);

            for (int i = 0; i < CANVAS * CANVAS; i++) {
                canvas[i] = 0;
                outer_fill[i] = 0;
                inner_fill[i] = 0;
            }
            graphics_stroke_rounded(&context, 8, 8, w, h, radius, 0x00FFFFFFu, 255);
            graphics_fill_rounded(&outer_context, 8, 8, w, h, clamped, 0x00FFFFFFu, 255);
            graphics_fill_rounded(&inner_context, 9, 9, w - 2, h - 2,
                                  clamped > 0 ? clamped - 1 : 0, 0x00FFFFFFu, 255);

            for (int i = 0; i < CANVAS * CANVAS; i++) {
                int32_t outer = (int32_t)(outer_fill[i] & 0xFFu);
                int32_t inner = (int32_t)(inner_fill[i] & 0xFFu);
                int32_t want = outer - inner;
                if (want < 0) {
                    want = 0;
                }
                CHECK_MSG((int32_t)(canvas[i] & 0xFFu) == want,
                          "a %dx%d stroke at radius %d is not its outer fill minus the "
                          "one-pixel-inset fill at pixel %d: stroke %d, outer %d, inner %d",
                          w, h, radius, i, (int)(canvas[i] & 0xFFu), outer, inner);
            }
        }
    }
}
