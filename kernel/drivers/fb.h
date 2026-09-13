#pragma once

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint64_t phys_addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
} fb_boot_info_t;

void fb_init(const fb_boot_info_t *info);

void fb_remap(uint32_t pitch, uint32_t width, uint32_t height);

uint64_t fb_mapped_bytes(void);

uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_pitch_bytes(void);
uint64_t fb_phys_addr(void);

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb);
uint32_t fb_get_pixel(uint32_t x, uint32_t y);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void fb_clear(uint32_t rgb);

void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *pixels);

void fb_scroll_up(uint32_t rows, uint32_t bg_rgb);
