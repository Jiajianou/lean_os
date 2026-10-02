#include "check.h"
#include "fakes.h"

#include <stdlib.h>
#include <string.h>

#include "bitmap_file.h"
#include "wallpaper.h"
#include "wallpaper_picture.h"

#define SCREEN_W 96
#define SCREEN_H 60

static uint32_t screen[SCREEN_W * SCREEN_H];

static graphics_context_t screen_context(void) {
    graphics_context_t context = {screen, SCREEN_W, SCREEN_H};
    return context;
}

TEST(wallpaper, the_gradient_is_still_the_gradient_the_boot_battery_measures) {
    graphics_context_t context = screen_context();
    wallpaper_fill(&context, 0, 0, SCREEN_W, SCREEN_H, WALLPAPER_GRADIENT, 0x001A1A2Eu);
    CHECK_EQ(screen[0], 0x00282847u);
    CHECK_EQ(screen[(SCREEN_H - 1) * SCREEN_W], 0x000F0F1Bu);
    wallpaper_fill(&context, 0, 0, SCREEN_W, SCREEN_H, WALLPAPER_FLAT, 0x001A1A2Eu);
    CHECK_EQ(screen[0], 0x001A1A2Eu);
    CHECK_EQ(screen[SCREEN_W * SCREEN_H - 1], 0x001A1A2Eu);
}

TEST(wallpaper, every_painted_style_covers_every_pixel_and_differs_from_the_others) {
    graphics_context_t context = screen_context();
    uint32_t first[WALLPAPER_COUNT];
    uint32_t sums[WALLPAPER_COUNT];
    for (int style = WALLPAPER_AURORA; style < WALLPAPER_COUNT; style++) {
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
            screen[i] = 0xDEADBEEFu;
        }
        wallpaper_fill(&context, 0, 0, SCREEN_W, SCREEN_H, style, 0x001A1A2Eu);
        uint32_t sum = 0;
        int distinct_rows = 0;
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
            CHECK(screen[i] != 0xDEADBEEFu);
            CHECK_EQ(screen[i] >> 24, 0);
            sum = sum * 31u + screen[i];
        }
        for (int row = 1; row < SCREEN_H; row++) {
            distinct_rows += screen[row * SCREEN_W + SCREEN_W / 2] != screen[(row - 1) * SCREEN_W + SCREEN_W / 2];
        }
        CHECK(distinct_rows > SCREEN_H / 3);
        first[style] = screen[0];
        sums[style] = sum;
        CHECK_EQ(wallpaper_uses_desktop_colour(style), 0);
    }
    CHECK(sums[WALLPAPER_AURORA] != sums[WALLPAPER_DUSK]);
    CHECK(sums[WALLPAPER_DUSK] != sums[WALLPAPER_OCEAN]);
    CHECK(first[WALLPAPER_AURORA] != first[WALLPAPER_OCEAN]);
    CHECK_EQ(wallpaper_uses_desktop_colour(WALLPAPER_GRID), 1);
}

TEST(wallpaper, a_painted_style_is_the_same_pixels_every_time_and_ignores_the_desktop_colour) {
    graphics_context_t context = screen_context();
    static uint32_t again[SCREEN_W * SCREEN_H];
    wallpaper_fill(&context, 0, 0, SCREEN_W, SCREEN_H, WALLPAPER_DUSK, 0x001A1A2Eu);
    memcpy(again, screen, sizeof(screen));
    wallpaper_fill(&context, 0, 0, SCREEN_W, SCREEN_H, WALLPAPER_DUSK, 0x00FF0000u);
    CHECK_MEMEQ(screen, again, sizeof(screen));
}

TEST(wallpaper, a_painted_style_drawn_into_part_of_a_screen_stays_inside_it) {
    graphics_context_t context = screen_context();
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        screen[i] = 0x00123456u;
    }
    wallpaper_fill(&context, 10, 5, 40, 30, WALLPAPER_AURORA, 0);
    CHECK_EQ(screen[4 * SCREEN_W + 20], 0x00123456u);
    CHECK_EQ(screen[20 * SCREEN_W + 9], 0x00123456u);
    CHECK_EQ(screen[20 * SCREEN_W + 50], 0x00123456u);
    CHECK_EQ(screen[35 * SCREEN_W + 20], 0x00123456u);
    CHECK(screen[20 * SCREEN_W + 20] != 0x00123456u);
    wallpaper_fill(&context, -30, -30, SCREEN_W + 60, SCREEN_H + 60, WALLPAPER_OCEAN, 0);
}

TEST(wallpaper, the_picture_without_a_picture_is_the_gradient) {
    graphics_context_t context = screen_context();
    static uint32_t gradient[SCREEN_W * SCREEN_H];
    wallpaper_fill(&context, 0, 0, SCREEN_W, SCREEN_H, WALLPAPER_GRADIENT, 0x00203040u);
    memcpy(gradient, screen, sizeof(screen));
    wallpaper_fill(&context, 0, 0, SCREEN_W, SCREEN_H, WALLPAPER_PICTURE, 0x00203040u);
    CHECK_MEMEQ(screen, gradient, sizeof(screen));
    CHECK_STREQ(wallpaper_name(WALLPAPER_PICTURE), "Picture");
    CHECK_STREQ(wallpaper_name(WALLPAPER_OCEAN), "Ocean");
}

TEST(wallpaper_picture, a_picture_is_kept_only_as_large_as_the_largest_desktop_needs) {
    uint32_t w;
    uint32_t h;
    wallpaper_picture_stored_size(4000, 3000, &w, &h);
    CHECK_EQ(w, 1920);
    CHECK_EQ(h, 1440);
    wallpaper_picture_stored_size(2400, 12000, &w, &h);
    CHECK_EQ(w, 1920);
    CHECK_EQ(h, 9600);
    wallpaper_picture_stored_size(1200, 6000, &w, &h);
    CHECK_EQ(w, 1200);
    CHECK_EQ(h, 6000);
    wallpaper_picture_stored_size(800, 600, &w, &h);
    CHECK_EQ(w, 800);
    CHECK_EQ(h, 600);
    wallpaper_picture_stored_size(3840, 2400, &w, &h);
    CHECK_EQ(w, 1920);
    CHECK_EQ(h, 1200);
}

TEST(wallpaper_picture, shrinking_averages_the_pixels_under_each_one) {
    const uint32_t source[4] = {0x00000000u, 0x00FF0000u, 0x0000FF00u, 0x000000FFu};
    uint32_t out = 0;
    wallpaper_picture_downscale(source, 2, 2, 2, &out, 1, 1);
    CHECK_EQ(out, 0x00404040u);
    const uint32_t row[6] = {0x00101010u, 0x00303030u, 0x00505050u, 0x00707070u, 0, 0};
    uint32_t two[2];
    wallpaper_picture_downscale(row, 4, 1, 6, two, 2, 1);
    CHECK_EQ(two[0], 0x00202020u);
    CHECK_EQ(two[1], 0x00606060u);
}

static uint32_t quadrant_colour(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (y < h / 2) {
        return x < w / 2 ? 0x00E74C3Cu : 0x002ECC71u;
    }
    return x < w / 2 ? 0x003498DBu : 0x00F1C40Fu;
}

TEST(wallpaper_picture, a_saved_picture_loads_back_with_its_name_and_pixels) {
    fake_user_fs_reset();
    REQUIRE(fake_user_fs_mkdir("/etc") == 0);
    static uint32_t source[64 * 40];
    for (uint32_t y = 0; y < 40; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            source[y * 64 + x] = quadrant_colour(x, y, 64, 40);
        }
    }
    CHECK_EQ(wallpaper_picture_save("/etc/wallpaper.picture", "quadrants.bmp", source, 64, 40, 64), 0);
    CHECK_EQ(fake_user_fs_exists("/etc/wallpaper.picture.new"), 0);
    wallpaper_picture_t picture;
    REQUIRE(wallpaper_picture_load("/etc/wallpaper.picture", &picture) == 0);
    CHECK_EQ(picture.width, 64);
    CHECK_EQ(picture.height, 40);
    CHECK_STREQ(picture.name, "quadrants.bmp");
    CHECK_MEMEQ(picture.pixels, source, sizeof(source));
    char name[32];
    CHECK_EQ(wallpaper_picture_read_name("/etc/wallpaper.picture", name, sizeof(name)), 0);
    CHECK_STREQ(name, "quadrants.bmp");
    wallpaper_picture_free(&picture);
    CHECK(picture.pixels == 0);
}

TEST(wallpaper_picture, a_file_that_is_not_a_picture_or_is_cut_short_does_not_load) {
    fake_user_fs_reset();
    REQUIRE(fake_user_fs_mkdir("/etc") == 0);
    wallpaper_picture_t picture;
    CHECK_EQ(wallpaper_picture_load("/etc/missing", &picture), -1);
    REQUIRE(fake_user_fs_write("/etc/junk", 400) == 0);
    CHECK_EQ(wallpaper_picture_load("/etc/junk", &picture), -1);
    uint32_t pixels[4] = {1, 2, 3, 4};
    REQUIRE(wallpaper_picture_save("/etc/small", "x", pixels, 2, 2, 2) == 0);
    REQUIRE(wallpaper_picture_save("/etc/small", "y", pixels, 2, 2, 2) == 0);
    CHECK_EQ(wallpaper_picture_load("/etc/small", &picture), 0);
    CHECK_STREQ(picture.name, "y");
    wallpaper_picture_free(&picture);
    CHECK_EQ(wallpaper_picture_save("/etc/none", "z", pixels, 0, 2, 2), -1);
    CHECK_EQ(wallpaper_picture_save("/etc/none", "z", pixels, 20000, 1, 20000), -1);
}

TEST(wallpaper_picture, cover_fills_the_screen_and_crops_the_sides_evenly) {
    static uint32_t source[200 * 60];
    for (uint32_t y = 0; y < 60; y++) {
        for (uint32_t x = 0; x < 200; x++) {
            source[y * 200 + x] = x < 50 || x >= 150 ? 0x00FF00FFu : quadrant_colour(x - 50, y, 100, 60);
        }
    }
    wallpaper_picture_t picture = {200, 60, source, "wide"};
    graphics_context_t context = screen_context();
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        screen[i] = 0xDEADBEEFu;
    }
    wallpaper_picture_cover(&picture, &context, 0, 0, SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        CHECK(screen[i] != 0xDEADBEEFu);
        CHECK(screen[i] != 0x00FF00FFu);
    }
    CHECK_EQ(screen[10 * SCREEN_W + 10], 0x00E74C3Cu);
    CHECK_EQ(screen[10 * SCREEN_W + SCREEN_W - 10], 0x002ECC71u);
    CHECK_EQ(screen[(SCREEN_H - 10) * SCREEN_W + 10], 0x003498DBu);
    CHECK_EQ(screen[(SCREEN_H - 10) * SCREEN_W + SCREEN_W - 10], 0x00F1C40Fu);
}

TEST(wallpaper_picture, cover_enlarges_a_small_picture_smoothly) {
    uint32_t source[2] = {0x00000000u, 0x00FFFFFFu};
    wallpaper_picture_t picture = {2, 1, source, "tiny"};
    graphics_context_t context = screen_context();
    wallpaper_picture_cover(&picture, &context, 0, 0, SCREEN_W, SCREEN_H);
    CHECK_EQ(screen[0], 0x00000000u);
    CHECK_EQ(screen[SCREEN_W - 1], 0x00FFFFFFu);
    uint32_t previous = 0;
    int levels = 0;
    for (int x = 0; x < SCREEN_W; x++) {
        uint32_t v = screen[30 * SCREEN_W + x] & 0xFFu;
        CHECK(v >= previous);
        levels += v != previous;
        previous = v;
    }
    CHECK(levels > 20);
}

TEST(bitmap_file, a_painting_is_written_as_a_bottom_up_twenty_four_bit_bmp) {
    const uint32_t pixels[2 * 3] = {0x00112233u, 0x00445566u, 0, 0x00778899u, 0x00AABBCCu, 0};
    uint8_t bytes[128];
    size_t n = bitmap_file_encode(pixels, 2, 2, 3, bytes, sizeof(bytes));
    CHECK_EQ(n, 54 + 2 * 8);
    CHECK_EQ(bitmap_file_size(2, 2), n);
    CHECK_EQ(bytes[0], 'B');
    CHECK_EQ(bytes[1], 'M');
    CHECK_EQ(bytes[2], 70);
    CHECK_EQ(bytes[10], 54);
    CHECK_EQ(bytes[14], 40);
    CHECK_EQ(bytes[18], 2);
    CHECK_EQ(bytes[22], 2);
    CHECK_EQ(bytes[26], 1);
    CHECK_EQ(bytes[28], 24);
    const uint8_t rows[] = {0x99, 0x88, 0x77, 0xCC, 0xBB, 0xAA, 0, 0, 0x33, 0x22, 0x11, 0x66, 0x55, 0x44, 0, 0};
    CHECK_MEMEQ(bytes + 54, rows, sizeof(rows));
    CHECK_EQ(bitmap_file_encode(pixels, 2, 2, 3, bytes, 69), 0);
    CHECK_EQ(bitmap_file_encode(pixels, 0, 2, 3, bytes, sizeof(bytes)), 0);
    CHECK_EQ(bitmap_file_encode(pixels, 4, 2, 3, bytes, sizeof(bytes)), 0);
}

TEST(bitmap_file, the_file_on_disk_is_the_encoded_bytes) {
    fake_user_fs_reset();
    REQUIRE(fake_user_fs_mkdir("/home") == 0);
    uint32_t pixels[3] = {0x00FF0000u, 0x0000FF00u, 0x000000FFu};
    CHECK_EQ(bitmap_file_write("/home/p.bmp", pixels, 3, 1, 3), 0);
    char text[80];
    int got = fake_user_fs_read_text("/home/p.bmp", text, sizeof(text));
    CHECK_EQ(got, 54 + 12);
}

TEST(wallpaper_picture, a_long_name_is_cut_to_fit_and_a_picture_too_tall_to_keep_is_refused) {
    fake_user_fs_reset();
    REQUIRE(fake_user_fs_mkdir("/etc") == 0);
    char name[300];
    memset(name, 'n', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    uint32_t pixels[4] = {1, 2, 3, 4};
    REQUIRE(wallpaper_picture_save("/etc/p", name, pixels, 2, 2, 2) == 0);
    char back[300];
    REQUIRE(wallpaper_picture_read_name("/etc/p", back, sizeof(back)) == 0);
    CHECK_EQ(strlen(back), WALLPAPER_PICTURE_NAME_MAX - 1);
    static uint32_t column[5000];
    CHECK_EQ(wallpaper_picture_save("/etc/tall", "tall", column, 1, 5000, 1), -1);
    CHECK_EQ(fake_user_fs_exists("/etc/tall"), 0);
    CHECK_EQ(wallpaper_picture_save("/etc/tall", "tall", column, 1, WALLPAPER_PICTURE_LARGEST_SIDE, 1), 0);
    CHECK_EQ(wallpaper_picture_save("/nowhere/p", "x", pixels, 2, 2, 2), -1);
    CHECK_EQ(wallpaper_picture_read_name("/nowhere/p", back, sizeof(back)), -1);
}

TEST(wallpaper_picture, a_tall_picture_is_cropped_top_and_bottom_evenly) {
    static uint32_t source[60 * 200];
    for (uint32_t y = 0; y < 200; y++) {
        for (uint32_t x = 0; x < 60; x++) {
            source[y * 60 + x] = y < 90 ? 0x00FF00FFu : y < 110 ? 0x0000FF00u : 0x000000FFu;
        }
    }
    wallpaper_picture_t picture = {60, 200, source, "tall"};
    graphics_context_t context = screen_context();
    wallpaper_picture_cover(&picture, &context, 0, 0, SCREEN_W, SCREEN_H);
    CHECK_EQ(screen[SCREEN_H / 2 * SCREEN_W + SCREEN_W / 2], 0x0000FF00u);
    CHECK_EQ(screen[0], 0x00FF00FFu);
    CHECK_EQ(screen[SCREEN_W * SCREEN_H - 1], 0x000000FFu);
    CHECK_EQ(screen[(SCREEN_H / 2 - 10) * SCREEN_W], 0x0000FF00u);
    CHECK_EQ(screen[(SCREEN_H / 2 + 10) * SCREEN_W], 0x0000FF00u);
}
