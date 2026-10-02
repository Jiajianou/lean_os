#pragma once

#include <stddef.h>
#include <stdint.h>

#define BITMAP_FILE_HEADER_BYTES 54

size_t bitmap_file_size(int32_t width, int32_t height);

size_t bitmap_file_encode(const uint32_t *pixels, int32_t width, int32_t height, int32_t stride, uint8_t *out,
                          size_t capacity);

int bitmap_file_write(const char *path, const uint32_t *pixels, int32_t width, int32_t height, int32_t stride);
