#include "check.h"

#include "../user_space/library/desktop_palette.c"

static const uint32_t BACKGROUNDS[] = {
    0x001A1A2Eu, 0x00203040u, 0x00301A1Au, 0x001A3020u, 0x00302A1Au, 0x00101018u,
};
#define BACKGROUND_COUNT ((int)(sizeof(BACKGROUNDS) / sizeof(BACKGROUNDS[0])))

static const uint32_t ACCENTS[] = {
    0x004C99E6u, 0x00E67E22u, 0x0027AE60u, 0x009B59B6u, 0x00E74C3Cu, 0x00F1C40Fu,
};
#define ACCENT_COUNT ((int)(sizeof(ACCENTS) / sizeof(ACCENTS[0])))

static uint32_t luminance_order(uint32_t color) {
    return desktop_palette_contrast_x100(color, 0x00000000u);
}

TEST(desktop_palette, mixing_nothing_and_everything_are_the_endpoints) {
    CHECK_EQ(desktop_palette_mix(0x00112233u, 0x00FFFFFFu, 0), 0x00112233u);
    CHECK_EQ(desktop_palette_mix(0x00112233u, 0x00AABBCCu, 100), 0x00AABBCCu);
    CHECK_EQ(desktop_palette_mix(0x00112233u, 0x00AABBCCu, 255), 0x00AABBCCu);
    CHECK_EQ(desktop_palette_mix(0x00000000u, 0x00FFFFFFu, 50), 0x00808080u);
}

TEST(desktop_palette, contrast_is_symmetric_and_black_on_white_is_twenty_one) {
    CHECK_EQ(desktop_palette_contrast_x100(0x00000000u, 0x00FFFFFFu), 2100u);
    CHECK_EQ(desktop_palette_contrast_x100(0x00FFFFFFu, 0x00000000u), 2100u);
    CHECK_EQ(desktop_palette_contrast_x100(0x00445566u, 0x00445566u), 100u);
}

TEST(desktop_palette, every_desktop_and_accent_the_settings_offers_stays_readable) {
    for (int b = 0; b < BACKGROUND_COUNT; b++) {
        for (int a = 0; a < ACCENT_COUNT; a++) {
            desktop_palette_t palette;
            desktop_palette_derive(BACKGROUNDS[b], ACCENTS[a], &palette);

            uint32_t body = desktop_palette_contrast_x100(palette.text, palette.surface);
            CHECK_MSG(body >= 700u,
                  "body text on a card is only %u.%02u:1 for background %06x accent %06x",
                  body / 100u, body % 100u, BACKGROUNDS[b], ACCENTS[a]);

            uint32_t dim = desktop_palette_contrast_x100(palette.text_dim, palette.surface);
            CHECK_MSG(dim >= 450u,
                  "dim text on a card is only %u.%02u:1 for background %06x accent %06x",
                  dim / 100u, dim % 100u, BACKGROUNDS[b], ACCENTS[a]);

            uint32_t on_accent = desktop_palette_contrast_x100(palette.accent_text, palette.accent);
            CHECK_MSG(on_accent >= 300u,
                  "a label on an accent button is only %u.%02u:1 for accent %06x",
                  on_accent / 100u, on_accent % 100u, ACCENTS[a]);
        }
    }
}

TEST(desktop_palette, the_surfaces_step_upward_out_of_the_window) {
    for (int b = 0; b < BACKGROUND_COUNT; b++) {
        desktop_palette_t palette;
        desktop_palette_derive(BACKGROUNDS[b], 0x004C99E6u, &palette);
        CHECK_MSG(luminance_order(palette.surface) > luminance_order(palette.window),
              "a card is not lighter than the window it sits in (background %06x)",
              BACKGROUNDS[b]);
        CHECK_MSG(luminance_order(palette.surface_raised) > luminance_order(palette.surface),
              "a raised control is not lighter than the card (background %06x)", BACKGROUNDS[b]);
        CHECK_MSG(luminance_order(palette.outline) > luminance_order(palette.surface_raised),
              "a border is not lighter than the control it draws around (background %06x)",
              BACKGROUNDS[b]);
    }
}

TEST(desktop_palette, the_window_is_dark_whatever_desktop_color_was_chosen) {
    for (int b = 0; b < BACKGROUND_COUNT; b++) {
        desktop_palette_t palette;
        desktop_palette_derive(BACKGROUNDS[b], 0x00F1C40Fu, &palette);
        CHECK_MSG(desktop_palette_contrast_x100(palette.window, 0x00FFFFFFu) >= 1200u,
              "the window ground %06x is too light for a dark theme (background %06x)",
              palette.window, BACKGROUNDS[b]);
    }
}

TEST(desktop_palette, a_label_takes_whichever_ink_reads_better_on_its_button) {
    desktop_palette_t yellow;
    desktop_palette_derive(0x001A1A2Eu, 0x00F1C40Fu, &yellow);
    CHECK_EQ(yellow.accent_text, PALETTE_INK);

    desktop_palette_t purple;
    desktop_palette_derive(0x001A1A2Eu, 0x009B59B6u, &purple);
    CHECK_EQ(purple.accent_text, PALETTE_TEXT);

    for (int a = 0; a < ACCENT_COUNT; a++) {
        desktop_palette_t palette;
        desktop_palette_derive(0x001A1A2Eu, ACCENTS[a], &palette);
        uint32_t chosen = desktop_palette_contrast_x100(palette.accent_text, ACCENTS[a]);
        uint32_t other = desktop_palette_contrast_x100(
            palette.accent_text == PALETTE_TEXT ? PALETTE_INK : PALETTE_TEXT, ACCENTS[a]);
        CHECK_MSG(chosen >= other,
                  "accent %06x took the worse of the two inks: %u.%02u:1 over %u.%02u:1",
                  ACCENTS[a], chosen / 100u, chosen % 100u, other / 100u, other % 100u);
    }
}

TEST(desktop_palette, the_accent_reaches_the_neutral_surfaces) {
    desktop_palette_t blue;
    desktop_palette_t red;
    desktop_palette_derive(0x001A1A2Eu, 0x004C99E6u, &blue);
    desktop_palette_derive(0x001A1A2Eu, 0x00E74C3Cu, &red);
    CHECK_MSG(blue.window != red.window,
          "two different accents produced the same window ground - the accent is not a token");
    CHECK_MSG(blue.surface != red.surface,
          "two different accents produced the same card - the accent is not a token");
}

TEST(desktop_palette, deriving_into_nothing_does_not_write_anywhere) {
    desktop_palette_derive(0x001A1A2Eu, 0x004C99E6u, NULL);
}
