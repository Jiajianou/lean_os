#include "framebuffer.h"

#include "console.h"
#include "kernel_log.h"
#include "library/kernel_library.h"
#include "memory_management/virtual_memory.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL

static uint64_t framebuffer_base;
static uint32_t framebuffer_pitch;
static uint32_t framebuffer_w;
static uint32_t framebuffer_h;
static uint64_t framebuffer_mapped;

static uint64_t map_through(uint64_t bytes) {
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t have = framebuffer_mapped / PAGE_SIZE;
    for (uint64_t i = have; i < pages; i++) {
        uint64_t addr = framebuffer_base + i * PAGE_SIZE;
        virtual_memory_map_page(addr, addr, VMM_FLAG_WRITABLE);
    }
    if (pages * PAGE_SIZE > framebuffer_mapped) {
        framebuffer_mapped = pages * PAGE_SIZE;
    }
    return pages;
}

void framebuffer_init(const framebuffer_boot_info_t *info) {
    if (info->bpp != 32) {
        panic("fb_init: only 32bpp framebuffers are supported");
    }
    if (info->width == 0 || info->height == 0) {
        panic("fb_init: boot loader handed off an empty/zeroed fb_boot_info_t - GOP setup failed silently");
    }

    framebuffer_base = info->phys_address;
    framebuffer_pitch = info->pitch;
    framebuffer_w = info->width;
    framebuffer_h = info->height;

    uint64_t pages = map_through((uint64_t)framebuffer_pitch * framebuffer_h);

    kernel_log_puts("[fb] framebuffer at 0x");
    kernel_log_put_hex64(framebuffer_base);
    kernel_log_puts(" ");
    kernel_log_put_hex32(framebuffer_w);
    kernel_log_puts("x");
    kernel_log_put_hex32(framebuffer_h);
    kernel_log_puts(" pitch=0x");
    kernel_log_put_hex32(framebuffer_pitch);
    kernel_log_puts(" (");
    kernel_log_put_hex64(pages);
    kernel_log_puts(" pages mapped)\n");
}

void framebuffer_remap(uint32_t pitch, uint32_t width, uint32_t height) {
    if (pitch == 0 || width == 0 || height == 0) {
        panic("fb_remap: refusing an empty geometry");
    }
    uint64_t pages = map_through((uint64_t)pitch * height);
    framebuffer_pitch = pitch;
    framebuffer_w = width;
    framebuffer_h = height;

    console_init();

    kernel_log_puts("[fb] re-mapped for a new mode: ");
    kernel_log_put_hex32(framebuffer_w);
    kernel_log_puts("x");
    kernel_log_put_hex32(framebuffer_h);
    kernel_log_puts(" pitch=0x");
    kernel_log_put_hex32(framebuffer_pitch);
    kernel_log_puts(" (");
    kernel_log_put_hex64(pages);
    kernel_log_puts(" pages mapped in total)\n");
}

uint64_t framebuffer_mapped_bytes(void) {
    return framebuffer_mapped;
}

uint32_t framebuffer_width(void) {
    return framebuffer_w;
}

uint32_t framebuffer_height(void) {
    return framebuffer_h;
}

uint32_t framebuffer_pitch_bytes(void) {
    return framebuffer_pitch;
}

uint64_t framebuffer_phys_address(void) {
    return framebuffer_base;
}

static inline volatile uint32_t *pixel_address(uint32_t x, uint32_t y) {
    return (volatile uint32_t *)(framebuffer_base + (uint64_t)y * framebuffer_pitch + (uint64_t)x * 4);
}

void framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
    if (x >= framebuffer_w || y >= framebuffer_h) {
        panic("fb_put_pixel: coordinates out of bounds");
    }
    *pixel_address(x, y) = rgb;
}

uint32_t framebuffer_get_pixel(uint32_t x, uint32_t y) {
    if (x >= framebuffer_w || y >= framebuffer_h) {
        panic("fb_get_pixel: coordinates out of bounds");
    }
    return *pixel_address(x, y);
}

void framebuffer_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb) {
    if (x + w > framebuffer_w || y + h > framebuffer_h) {
        panic("fb_fill_rect: rectangle out of bounds");
    }
    for (uint32_t row = 0; row < h; row++) {
        volatile uint32_t *p = pixel_address(x, y + row);
        for (uint32_t col = 0; col < w; col++) {
            p[col] = rgb;
        }
    }
}

void framebuffer_clear(uint32_t rgb) {
    framebuffer_fill_rect(0, 0, framebuffer_w, framebuffer_h, rgb);
}

void framebuffer_scroll_up(uint32_t rows, uint32_t bg_rgb) {
    if (rows == 0) {
        return;
    }
    if (rows >= framebuffer_h) {
        framebuffer_clear(bg_rgb);
        return;
    }
    for (uint32_t y = 0; y < framebuffer_h - rows; y++) {
        volatile uint32_t *dst = pixel_address(0, y);
        volatile uint32_t *src = pixel_address(0, y + rows);
        k_memcpy((void *)dst, (const void *)src, (size_t)framebuffer_w * sizeof(uint32_t));
    }
    framebuffer_fill_rect(0, framebuffer_h - rows, framebuffer_w, rows, bg_rgb);
}
