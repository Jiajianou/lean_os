#include "cursor.h"

#include "framebuffer.h"

#define CURSOR_SIZE  8
#define CURSOR_COLOR 0x00FFFFFFu

static const uint8_t shape[CURSOR_SIZE] = {
    0b10000000,
    0b11000000,
    0b10100000,
    0b10010000,
    0b10001000,
    0b10111000,
    0b11000100,
    0b10000100,
};

static int32_t cx;
static int32_t cy;
static uint32_t saved[CURSOR_SIZE][CURSOR_SIZE];
static int drawn;

static void erase(void) {
    if (!drawn) {
        return;
    }
    for (uint32_t y = 0; y < CURSOR_SIZE; y++) {
        for (uint32_t x = 0; x < CURSOR_SIZE; x++) {
            framebuffer_put_pixel((uint32_t)cx + x, (uint32_t)cy + y, saved[y][x]);
        }
    }
    drawn = 0;
}

static void draw(void) {
    for (uint32_t y = 0; y < CURSOR_SIZE; y++) {
        uint8_t row = shape[y];
        for (uint32_t x = 0; x < CURSOR_SIZE; x++) {
            uint32_t px = (uint32_t)cx + x;
            uint32_t py = (uint32_t)cy + y;
            saved[y][x] = framebuffer_get_pixel(px, py);
            if (row & (0x80 >> x)) {
                framebuffer_put_pixel(px, py, CURSOR_COLOR);
            }
        }
    }
    drawn = 1;
}

static void clamp_to_screen(void) {
    int32_t maxx = (int32_t)framebuffer_width() - CURSOR_SIZE;
    int32_t maxy = (int32_t)framebuffer_height() - CURSOR_SIZE;
    if (cx < 0) {
        cx = 0;
    }
    if (cx > maxx) {
        cx = maxx;
    }
    if (cy < 0) {
        cy = 0;
    }
    if (cy > maxy) {
        cy = maxy;
    }
}

void cursor_init(int32_t x, int32_t y) {
    cx = x;
    cy = y;
    drawn = 0;
    clamp_to_screen();
    draw();
}

void cursor_move(int32_t dx, int32_t dy) {
    erase();
    cx += dx;
    cy += dy;
    clamp_to_screen();
    draw();
}

int32_t cursor_x(void) {
    return cx;
}

int32_t cursor_y(void) {
    return cy;
}
