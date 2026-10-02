#include "bitmap_file.h"

#include "malloc.h"
#include "syscall_wrappers.h"

/* M211. The one picture format simple enough to write in a page: a 24-bit
   BMP, rows stored bottom first and each padded to four bytes. Every
   decoder there is reads it, which is the point - Paint's pictures open in
   Files, in Quick Look and as the desktop. */

static size_t row_bytes(int32_t width) {
    return ((size_t)width * 3u + 3u) & ~(size_t)3u;
}

size_t bitmap_file_size(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    return BITMAP_FILE_HEADER_BYTES + row_bytes(width) * (size_t)height;
}

static void put16(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *at, uint32_t value) {
    put16(at, value);
    put16(at + 2, value >> 16);
}

size_t bitmap_file_encode(const uint32_t *pixels, int32_t width, int32_t height, int32_t stride, uint8_t *out,
                          size_t capacity) {
    size_t total = bitmap_file_size(width, height);
    if (total == 0 || total > capacity || stride < width) {
        return 0;
    }
    for (size_t i = 0; i < BITMAP_FILE_HEADER_BYTES; i++) {
        out[i] = 0;
    }
    out[0] = 'B';
    out[1] = 'M';
    put32(out + 2, (uint32_t)total);
    put32(out + 10, BITMAP_FILE_HEADER_BYTES);
    put32(out + 14, 40);
    put32(out + 18, (uint32_t)width);
    put32(out + 22, (uint32_t)height);
    put16(out + 26, 1);
    put16(out + 28, 24);
    put32(out + 34, (uint32_t)(total - BITMAP_FILE_HEADER_BYTES));
    put32(out + 38, 2835);
    put32(out + 42, 2835);
    size_t pitch = row_bytes(width);
    for (int32_t row = 0; row < height; row++) {
        const uint32_t *source = pixels + (size_t)(height - 1 - row) * (size_t)stride;
        uint8_t *line = out + BITMAP_FILE_HEADER_BYTES + (size_t)row * pitch;
        for (int32_t column = 0; column < width; column++) {
            line[3 * column] = (uint8_t)source[column];
            line[3 * column + 1] = (uint8_t)(source[column] >> 8);
            line[3 * column + 2] = (uint8_t)(source[column] >> 16);
        }
        for (size_t pad = (size_t)width * 3u; pad < pitch; pad++) {
            line[pad] = 0;
        }
    }
    return total;
}

int bitmap_file_write(const char *path, const uint32_t *pixels, int32_t width, int32_t height, int32_t stride) {
    size_t total = bitmap_file_size(width, height);
    uint8_t *bytes = total ? (uint8_t *)malloc(total) : 0;
    if (!bytes) {
        return -1;
    }
    int result = -1;
    if (bitmap_file_encode(pixels, width, height, stride, bytes, total) == total) {
        result = sys_writefile(path, bytes, total) == 0 ? 0 : -1;
    }
    free(bytes);
    return result;
}
