/* system_api/include/icon.h
 *
 * M56: an icon image format, finally.
 *
 * Every icon this project has ever drawn was hand-placed rectangles in
 * whichever client needed one - noted as such, apologetically, in three
 * separate files. That is the last thing making this desktop look
 * hand-drawn rather than designed, and it is not fixable by drawing
 * *better* rectangles: what was missing was a way to say "here is a
 * picture" at all.
 *
 * The format is deliberately the smallest thing that is genuinely one:
 *
 *   0..3   magic "LIC1"
 *   4      width, 1..255
 *   5      height, 1..255
 *   6      palette entry count, 1..16
 *   7      reserved, 0
 *   8..    palette: 3 bytes (R, G, B) per entry
 *   then   pixels: 4-bit palette indices, two per byte, high nibble
 *          first, row-major
 *
 * Four bits per pixel because sixteen colors is enough for a 24x24 icon
 * and it halves the size; index 0 is always transparent, which is what
 * lets an icon be drawn over a wallpaper or a taskbar without carrying an
 * alpha channel this compositor could not blend anyway. A 24x24 icon is
 * 8 + 48 + 288 = 344 bytes.
 *
 * Deliberately *not* a file on disk yet. The loader (user_space/lib/
 * icons.h) takes a pointer to these bytes and does not care where they
 * came from; today they are compiled in, which costs a client no file
 * I/O on every redraw and no failure path for "the icon is missing". The
 * day icons are user-replaceable, the format does not change - only where
 * the bytes are read from.
 */
#pragma once

#include <stdint.h>

#define ICON_MAGIC_0 'L'
#define ICON_MAGIC_1 'I'
#define ICON_MAGIC_2 'C'
#define ICON_MAGIC_3 '1'

#define ICON_HEADER_BYTES 8
#define ICON_MAX_PALETTE  16

static inline int icon_width(const uint8_t *blob) { return blob[4]; }
static inline int icon_height(const uint8_t *blob) { return blob[5]; }
static inline int icon_palette_count(const uint8_t *blob) { return blob[6]; }

static inline int icon_valid(const uint8_t *blob) {
    return blob && blob[0] == ICON_MAGIC_0 && blob[1] == ICON_MAGIC_1 &&
           blob[2] == ICON_MAGIC_2 && blob[3] == ICON_MAGIC_3 &&
           blob[4] > 0 && blob[5] > 0 &&
           blob[6] > 0 && blob[6] <= ICON_MAX_PALETTE;
}
