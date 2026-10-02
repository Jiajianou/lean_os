#pragma once

#include <stddef.h>
#include <stdint.h>

#include "graphics.h"

#define WALLPAPER_PICTURE_MAGIC "LEANWPIC"
#define WALLPAPER_PICTURE_MAGIC_LENGTH 8
#define WALLPAPER_PICTURE_NAME_MAX 128
#define WALLPAPER_PICTURE_HEADER (WALLPAPER_PICTURE_MAGIC_LENGTH + 8 + WALLPAPER_PICTURE_NAME_MAX)

#define WALLPAPER_PICTURE_COVER_WIDTH  1920
#define WALLPAPER_PICTURE_COVER_HEIGHT 1200
#define WALLPAPER_PICTURE_LARGEST_SIDE 4096

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
    char name[WALLPAPER_PICTURE_NAME_MAX];
} wallpaper_picture_t;

void wallpaper_picture_stored_size(uint32_t width, uint32_t height, uint32_t *out_width, uint32_t *out_height);

void wallpaper_picture_downscale(const uint32_t *source, uint32_t width, uint32_t height, uint32_t stride,
                                 uint32_t *out, uint32_t out_width, uint32_t out_height);

int wallpaper_picture_save(const char *path, const char *name, const uint32_t *pixels, uint32_t width,
                           uint32_t height, uint32_t stride);

int wallpaper_picture_load(const char *path, wallpaper_picture_t *out);

int wallpaper_picture_read_name(const char *path, char *out, size_t capacity);

void wallpaper_picture_free(wallpaper_picture_t *picture);

void wallpaper_picture_cover(const wallpaper_picture_t *picture, graphics_context_t *target, int32_t x, int32_t y,
                             int32_t w, int32_t h);
