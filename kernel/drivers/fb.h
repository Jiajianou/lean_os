/* kernel/drivers/fb.h
 *
 * Linear framebuffer driver (M16). The boot loader (kernel/boot/uefi/boot.c's
 * init_framebuffer, via the Graphics Output Protocol) sets a linear
 * framebuffer graphics mode and hands off a small fb_boot_info_t describing
 * it in RSI (alongside the e820 memory map in RDI - see kernel_main). This
 * driver maps that physical region into the kernel's address space and
 * gives the rest of the kernel pixel-level primitives to build on -
 * everything from M17's text console onward draws through this, nothing
 * touches the framebuffer memory directly.
 *
 * Pixel format: always treated as packed 0x00RRGGBB (XRGB8888) - the boot
 * loader only ever selects GOP's PixelBlueGreenRedReserved8BitPerColor
 * mode, which is the same 32-bit value read little-endian, and the only
 * format this driver supports.
 */
#pragma once

#include <stdint.h>

/* Matches exactly what boot.c's init_framebuffer fills in before jumping
 * into the kernel. */
typedef struct __attribute__((packed)) {
    uint64_t phys_addr;
    uint32_t pitch;  /* bytes per scanline - not necessarily width * 4 */
    uint32_t width;  /* pixels */
    uint32_t height; /* pixels */
    uint32_t bpp;    /* bits per pixel - fb_init panics if this isn't 32 */
} fb_boot_info_t;

void fb_init(const fb_boot_info_t *info);

uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_pitch_bytes(void);
uint64_t fb_phys_addr(void);

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb);
uint32_t fb_get_pixel(uint32_t x, uint32_t y);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void fb_clear(uint32_t rgb);

/* Copies a w*h XRGB8888 buffer into the framebuffer at (x, y), row by row
 * (pixels itself is tightly packed - w * sizeof(uint32_t) per row - the
 * framebuffer's own pitch may differ, which is exactly why this can't be
 * a single flat memcpy). */
void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *pixels);

/* Shifts the whole framebuffer up by `rows` pixel rows (scanline by
 * scanline, not cell by cell - the text console (M17) is this driver's
 * only caller so far, but this has no notion of glyph cells itself), then
 * fills the newly-exposed rows at the bottom with bg_rgb. rows >= height
 * degenerates to a plain fb_clear. */
void fb_scroll_up(uint32_t rows, uint32_t bg_rgb);
