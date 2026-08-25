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

/* M58: fb_init used to run exactly once, from the fb_boot_info_t the
 * bootloader filled in, and every accessor answered out of statics that
 * nothing could ever change. A resolution setting makes that false: after
 * kernel/drivers/dispi.h reprograms the adapter, this driver is pointed
 * at the same physical base with a different pitch and a different size,
 * and a *larger* mode needs a larger mapping than the boot mode's covered
 * - so this is a re-map, not a struct update.
 *
 * Only ever grows the mapping (it tracks the high-water mark), because a
 * smaller mode leaves pages mapped that simply stop being read; unmapping
 * them would buy nothing and could race a draw already in flight.
 *
 * The framebuffer's *physical* base does not move on a mode change with
 * this adapter - it is the device's BAR, not something the mode selects -
 * so this deliberately keeps the base fb_init was given rather than
 * re-deriving it, which would be a second source of truth for a value
 * that has one. */
void fb_remap(uint32_t pitch, uint32_t width, uint32_t height);

/* How many bytes of framebuffer are currently mapped - the high-water
 * mark fb_remap maintains. SYS_fb_map needs it: a client mapping the
 * framebuffer into its own address space has to cover the same range the
 * kernel does, or a mode change would leave it drawing off the end of its
 * own mapping. */
uint64_t fb_mapped_bytes(void);

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
