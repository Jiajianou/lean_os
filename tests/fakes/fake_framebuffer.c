#include "drivers/framebuffer.h"

#include <stdlib.h>

/* M200: a framebuffer that is memory and counts what is asked of it - in
   particular every READ, because on the machine a read of the framebuffer is
   an uncached read and the console's whole cost was reading it back.

   M225 (log-crash-path): and every pixel WRITTEN, because the log draws the
   console with interrupts off and what a section costs is the pixels it
   paints - a scroll used to repaint all of them inside one newline. */
static uint32_t *pixels;
static uint32_t width, height;
static unsigned long reads, scrolls;
static unsigned long long writes;
/* Calls, not pixels: what a repaint costs beyond its pixels is one call
   per fill, and per pixel of a glyph. */
static unsigned long long calls;

void fake_framebuffer_reset(uint32_t w, uint32_t h);
unsigned long fake_framebuffer_reads(void);
unsigned long fake_framebuffer_scrolls(void);
unsigned long long fake_framebuffer_writes(void);
unsigned long long fake_framebuffer_calls(void);
uint32_t fake_framebuffer_pixel(uint32_t x, uint32_t y);

void fake_framebuffer_reset(uint32_t w, uint32_t h) {
    free(pixels);
    pixels = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t));
    width = w;
    height = h;
    reads = 0;
    scrolls = 0;
    writes = 0;
    calls = 0;
}

unsigned long fake_framebuffer_reads(void) { return reads; }
unsigned long fake_framebuffer_scrolls(void) { return scrolls; }
unsigned long long fake_framebuffer_writes(void) { return writes; }
unsigned long long fake_framebuffer_calls(void) { return calls; }
uint32_t fake_framebuffer_pixel(uint32_t x, uint32_t y) { return pixels[y * width + x]; }

uint32_t framebuffer_width(void) { return width; }
uint32_t framebuffer_height(void) { return height; }

void framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
    calls++;
    if (x < width && y < height) {
        pixels[y * width + x] = rgb;
        writes++;
    }
}

uint32_t framebuffer_get_pixel(uint32_t x, uint32_t y) {
    reads++;
    return (x < width && y < height) ? pixels[y * width + x] : 0;
}

void framebuffer_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb) {
    calls++;
    for (uint32_t row = y; row < y + h && row < height; row++) {
        for (uint32_t col = x; col < x + w && col < width; col++) {
            pixels[row * width + col] = rgb;
            writes++;
        }
    }
}

void framebuffer_clear(uint32_t rgb) {
    framebuffer_fill_rect(0, 0, width, height, rgb);
}

void framebuffer_scroll_up(uint32_t rows, uint32_t bg_rgb) {
    scrolls++;
    reads += (unsigned long)width * (height - rows);
    writes += (unsigned long long)width * (height - rows);
    for (uint32_t y = 0; y + rows < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            pixels[y * width + x] = pixels[(y + rows) * width + x];
        }
    }
    framebuffer_fill_rect(0, height - rows, width, rows, bg_rgb);
}
