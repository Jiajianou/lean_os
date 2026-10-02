#pragma once

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint64_t phys_address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
} framebuffer_boot_info_t;

void framebuffer_init(const framebuffer_boot_info_t *info);

void framebuffer_remap(uint32_t pitch, uint32_t width, uint32_t height);

uint64_t framebuffer_mapped_bytes(void);

uint32_t framebuffer_width(void);
uint32_t framebuffer_scale_percent(void);
uint32_t framebuffer_scale_requested(void);
int framebuffer_set_scale_percent(uint32_t percent);
void framebuffer_desktop_size(uint32_t *width, uint32_t *height);
uint32_t framebuffer_height(void);
uint32_t framebuffer_pitch_bytes(void);
uint64_t framebuffer_phys_address(void);

void framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t rgb);
uint32_t framebuffer_get_pixel(uint32_t x, uint32_t y);
void framebuffer_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void framebuffer_clear(uint32_t rgb);


void framebuffer_scroll_up(uint32_t rows, uint32_t bg_rgb);
