#include "fb.h"

#include "console.h"
#include "klog.h"
#include "lib/libk.h"
#include "mm/vmm.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL

static uint64_t fb_base;   /* virtual == physical: identity-mapped on demand below */
static uint32_t fb_pitch;
static uint32_t fb_w;
static uint32_t fb_h;
/* M58: the high-water mark of what has actually been mapped, which is not
 * the same as what the current mode uses once a mode change has happened.
 * See fb_remap. */
static uint64_t fb_mapped;

/* Maps [fb_base, fb_base + bytes) identity-style, skipping whatever is
 * already covered. Split out of fb_init at M58 because fb_remap needs
 * exactly the same loop for a mode that got bigger. */
static uint64_t map_through(uint64_t bytes) {
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t have = fb_mapped / PAGE_SIZE;
    for (uint64_t i = have; i < pages; i++) {
        uint64_t addr = fb_base + i * PAGE_SIZE;
        vmm_map_page(addr, addr, VMM_FLAG_WRITABLE);
    }
    if (pages * PAGE_SIZE > fb_mapped) {
        fb_mapped = pages * PAGE_SIZE;
    }
    return pages;
}

void fb_init(const fb_boot_info_t *info) {
    if (info->bpp != 32) {
        panic("fb_init: only 32bpp framebuffers are supported");
    }
    if (info->width == 0 || info->height == 0) {
        panic("fb_init: boot loader handed off an empty/zeroed fb_boot_info_t - GOP setup failed silently");
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
    uint64_t pages = map_through((uint64_t)fb_pitch * fb_h);

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

void fb_remap(uint32_t pitch, uint32_t width, uint32_t height) {
    if (pitch == 0 || width == 0 || height == 0) {
        panic("fb_remap: refusing an empty geometry");
    }
    /* Grow the mapping *before* the accessors start answering with the
     * new geometry. The other order leaves a window - however short - in
     * which fb_put_pixel's own bounds check says a row is legal and the
     * page behind it is not mapped, which on this kernel is a page fault
     * in ring 0. */
    uint64_t pages = map_through((uint64_t)pitch * height);
    fb_pitch = pitch;
    fb_w = width;
    fb_h = height;

    /* Re-derive the text console *before* anything logs, and that
     * ordering is not a nicety - it is the first thing a mode change
     * breaks. console.c caches how many cells fit across and down; on a
     * mode that got smaller its cursor is immediately outside the new
     * screen, and the very next klog line draws a glyph through
     * fb_put_pixel, which panics on an out-of-bounds coordinate - from
     * inside the logging path, so the panic's own message tries to draw
     * too. Found exactly that way: the first mode change this project
     * ever performed died mid-sentence in this function's own log line.
     *
     * A driver calling a driver above it is unusual here and worth the
     * exception: the console is *defined* in terms of the framebuffer, so
     * a framebuffer whose geometry changed has to hand it the new one,
     * and there must be no instruction in between where a log would
     * fault. */
    console_init();

    klog_puts("[fb] re-mapped for a new mode: ");
    klog_put_hex32(fb_w);
    klog_puts("x");
    klog_put_hex32(fb_h);
    klog_puts(" pitch=0x");
    klog_put_hex32(fb_pitch);
    klog_puts(" (");
    klog_put_hex64(pages);
    klog_puts(" pages mapped in total)\n");
}

uint64_t fb_mapped_bytes(void) {
    return fb_mapped;
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
