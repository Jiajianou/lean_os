#include "fb.h"

#include "klog.h"
#include "lib/libk.h"
#include "mm/vmm.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL

static uint64_t fb_base;   /* virtual == physical: identity-mapped on demand below */
static uint32_t fb_pitch;
static uint32_t fb_w;
static uint32_t fb_h;

void fb_init(const fb_boot_info_t *info) {
    if (info->bpp != 32) {
        panic("fb_init: only 32bpp framebuffers are supported");
    }
    if (info->width == 0 || info->height == 0) {
        panic("fb_init: stage2 handed off an empty/zeroed fb_boot_info_t - VBE setup failed silently");
    }

    fb_base = info->phys_addr;
    fb_pitch = info->pitch;
    fb_w = info->width;
    fb_h = info->height;

    /* Map the whole framebuffer identity-style (virt == phys) page by
     * page. The physical base a VBE BIOS hands back lives in PCI MMIO
     * space, always well above the kernel's 1 GiB huge-page identity
     * range on every target this has been tested against - vmm_map_page
     * panics outright if that assumption ever breaks (an address that
     * falls inside the huge-mapped range), rather than silently
     * corrupting the mapping. */
    uint64_t size = (uint64_t)fb_pitch * fb_h;
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t addr = fb_base + i * PAGE_SIZE;
        vmm_map_page(addr, addr, VMM_FLAG_WRITABLE);
    }

    klog_puts("[fb] framebuffer at 0x");
    klog_put_hex64(fb_base);
    klog_puts(" ");
    klog_put_hex32(fb_w);
    klog_puts("x");
    klog_put_hex32(fb_h);
    klog_puts(" pitch=0x");
    klog_put_hex32(fb_pitch);
    klog_puts(" (");
    klog_put_hex64(pages);
    klog_puts(" pages mapped)\n");
}

uint32_t fb_width(void) {
    return fb_w;
}

uint32_t fb_height(void) {
    return fb_h;
}

uint32_t fb_pitch_bytes(void) {
    return fb_pitch;
}

uint64_t fb_phys_addr(void) {
    return fb_base;
}

static inline volatile uint32_t *pixel_addr(uint32_t x, uint32_t y) {
    return (volatile uint32_t *)(fb_base + (uint64_t)y * fb_pitch + (uint64_t)x * 4);
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
    if (x >= fb_w || y >= fb_h) {
        panic("fb_put_pixel: coordinates out of bounds");
    }
    *pixel_addr(x, y) = rgb;
}

uint32_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (x >= fb_w || y >= fb_h) {
        panic("fb_get_pixel: coordinates out of bounds");
    }
    return *pixel_addr(x, y);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb) {
    if (x + w > fb_w || y + h > fb_h) {
        panic("fb_fill_rect: rectangle out of bounds");
    }
    for (uint32_t row = 0; row < h; row++) {
        volatile uint32_t *p = pixel_addr(x, y + row);
        for (uint32_t col = 0; col < w; col++) {
            p[col] = rgb;
        }
    }
}

void fb_clear(uint32_t rgb) {
    fb_fill_rect(0, 0, fb_w, fb_h, rgb);
}

void fb_scroll_up(uint32_t rows, uint32_t bg_rgb) {
    if (rows == 0) {
        return;
    }
    if (rows >= fb_h) {
        fb_clear(bg_rgb);
        return;
    }
    for (uint32_t y = 0; y < fb_h - rows; y++) {
        volatile uint32_t *dst = pixel_addr(0, y);
        volatile uint32_t *src = pixel_addr(0, y + rows);
        k_memcpy((void *)dst, (const void *)src, (size_t)fb_w * sizeof(uint32_t));
    }
    fb_fill_rect(0, fb_h - rows, fb_w, rows, bg_rgb);
}

void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *pixels) {
    if (x + w > fb_w || y + h > fb_h) {
        panic("fb_blit: rectangle out of bounds");
    }
    for (uint32_t row = 0; row < h; row++) {
        volatile uint32_t *dst = pixel_addr(x, y + row);
        const uint32_t *src = pixels + (uint64_t)row * w;
        k_memcpy((void *)dst, src, (size_t)w * sizeof(uint32_t));
    }
}
