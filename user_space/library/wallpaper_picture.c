#include "wallpaper_picture.h"

#include "malloc.h"
#include "string_utilities.h"
#include "syscall.h"
#include "syscall_wrappers.h"

/* M211. A desktop picture is decoded once, by the program that chose it -
   Files or Settings, which carry an image decoder in the toolkit - and kept
   here already decoded, at a size that still covers the largest desktop this
   machine draws. The desktop itself links no decoder: a PNG decoder in every
   program the kernel carries would cost more than the kernel has room for,
   and a picture the desktop could not open would leave it blank. */

void wallpaper_picture_stored_size(uint32_t width, uint32_t height, uint32_t *out_width, uint32_t *out_height) {
    *out_width = width;
    *out_height = height;
    if (width == 0 || height == 0) {
        return;
    }
    uint64_t by_width = (uint64_t)WALLPAPER_PICTURE_COVER_WIDTH * 65536u / width;
    uint64_t by_height = (uint64_t)WALLPAPER_PICTURE_COVER_HEIGHT * 65536u / height;
    uint64_t scale = by_width > by_height ? by_width : by_height;
    if (scale >= 65536u) {
        return;
    }
    uint64_t w = ((uint64_t)width * scale + 32768u) / 65536u;
    uint64_t h = ((uint64_t)height * scale + 32768u) / 65536u;
    *out_width = w == 0 ? 1 : (uint32_t)w;
    *out_height = h == 0 ? 1 : (uint32_t)h;
}

/* Every output pixel is the average of the source pixels under it, so a
   photograph shrunk to a third keeps its colours rather than one pixel in
   nine of them. */
void wallpaper_picture_downscale(const uint32_t *source, uint32_t width, uint32_t height, uint32_t stride,
                                 uint32_t *out, uint32_t out_width, uint32_t out_height) {
    for (uint32_t oy = 0; oy < out_height; oy++) {
        uint32_t y0 = (uint32_t)((uint64_t)oy * height / out_height);
        uint32_t y1 = (uint32_t)((uint64_t)(oy + 1) * height / out_height);
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        for (uint32_t ox = 0; ox < out_width; ox++) {
            uint32_t x0 = (uint32_t)((uint64_t)ox * width / out_width);
            uint32_t x1 = (uint32_t)((uint64_t)(ox + 1) * width / out_width);
            if (x1 <= x0) {
                x1 = x0 + 1;
            }
            uint64_t red = 0;
            uint64_t green = 0;
            uint64_t blue = 0;
            for (uint32_t sy = y0; sy < y1; sy++) {
                const uint32_t *row = source + (uint64_t)sy * stride;
                for (uint32_t sx = x0; sx < x1; sx++) {
                    red += (row[sx] >> 16) & 0xFFu;
                    green += (row[sx] >> 8) & 0xFFu;
                    blue += row[sx] & 0xFFu;
                }
            }
            uint64_t count = (uint64_t)(x1 - x0) * (y1 - y0);
            out[(uint64_t)oy * out_width + ox] = (uint32_t)(((red + count / 2) / count) << 16 |
                                                            ((green + count / 2) / count) << 8 |
                                                            ((blue + count / 2) / count));
        }
    }
}

static void put32(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static uint32_t get32(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static int write_all(int fd, const void *buffer, size_t length) {
    const uint8_t *at = (const uint8_t *)buffer;
    while (length > 0) {
        long n = sys_write(fd, at, length);
        if (n <= 0) {
            return -1;
        }
        at += n;
        length -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buffer, size_t length) {
    uint8_t *at = (uint8_t *)buffer;
    while (length > 0) {
        long n = sys_read(fd, at, length);
        if (n <= 0) {
            return -1;
        }
        at += n;
        length -= (size_t)n;
    }
    return 0;
}

/* Written beside the old one and renamed over it, so the desktop - which
   looks for a new picture twice a second - can never read half of one. */
int wallpaper_picture_save(const char *path, const char *name, const uint32_t *pixels, uint32_t width,
                           uint32_t height, uint32_t stride) {
    if (!path || !pixels || width == 0 || height == 0 || width > WALLPAPER_PICTURE_LARGEST_SIDE * 4 ||
        height > WALLPAPER_PICTURE_LARGEST_SIDE * 4) {
        return -1;
    }
    uint32_t stored_width;
    uint32_t stored_height;
    wallpaper_picture_stored_size(width, height, &stored_width, &stored_height);
    if (stored_width > WALLPAPER_PICTURE_LARGEST_SIDE || stored_height > WALLPAPER_PICTURE_LARGEST_SIDE) {
        return -1;
    }
    uint32_t *stored = (uint32_t *)malloc((size_t)stored_width * stored_height * sizeof(uint32_t));
    if (!stored) {
        return -1;
    }
    wallpaper_picture_downscale(pixels, width, height, stride, stored, stored_width, stored_height);

    uint8_t header[WALLPAPER_PICTURE_HEADER];
    memset(header, 0, sizeof(header));
    memcpy(header, WALLPAPER_PICTURE_MAGIC, WALLPAPER_PICTURE_MAGIC_LENGTH);
    put32(header + WALLPAPER_PICTURE_MAGIC_LENGTH, stored_width);
    put32(header + WALLPAPER_PICTURE_MAGIC_LENGTH + 4, stored_height);
    if (name) {
        size_t length = strlen(name);
        if (length >= WALLPAPER_PICTURE_NAME_MAX) {
            length = WALLPAPER_PICTURE_NAME_MAX - 1;
        }
        memcpy(header + WALLPAPER_PICTURE_MAGIC_LENGTH + 8, name, length);
    }

    char temporary[512];
    size_t path_length = strlen(path);
    if (path_length + 5 > sizeof(temporary)) {
        free(stored);
        return -1;
    }
    memcpy(temporary, path, path_length);
    memcpy(temporary + path_length, ".new", 5);
    sys_unlink(temporary);
    long fd = sys_open(temporary, OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE);
    if (fd < 0) {
        free(stored);
        return -1;
    }
    int failed = write_all((int)fd, header, sizeof(header)) != 0 ||
                 write_all((int)fd, stored, (size_t)stored_width * stored_height * sizeof(uint32_t)) != 0;
    sys_close((int)fd);
    free(stored);
    if (failed) {
        sys_unlink(temporary);
        return -1;
    }
    sys_unlink(path);
    if (sys_rename(temporary, path) != 0) {
        sys_unlink(temporary);
        return -1;
    }
    return 0;
}

static int read_header(int fd, uint32_t *width, uint32_t *height, char *name, size_t capacity) {
    uint8_t header[WALLPAPER_PICTURE_HEADER];
    if (read_all(fd, header, sizeof(header)) != 0 ||
        memcmp(header, WALLPAPER_PICTURE_MAGIC, WALLPAPER_PICTURE_MAGIC_LENGTH) != 0) {
        return -1;
    }
    *width = get32(header + WALLPAPER_PICTURE_MAGIC_LENGTH);
    *height = get32(header + WALLPAPER_PICTURE_MAGIC_LENGTH + 4);
    if (*width == 0 || *height == 0 || *width > WALLPAPER_PICTURE_LARGEST_SIDE ||
        *height > WALLPAPER_PICTURE_LARGEST_SIDE) {
        return -1;
    }
    if (name && capacity > 0) {
        size_t i = 0;
        const uint8_t *stored = header + WALLPAPER_PICTURE_MAGIC_LENGTH + 8;
        for (; i + 1 < capacity && i < WALLPAPER_PICTURE_NAME_MAX - 1 && stored[i]; i++) {
            name[i] = (char)stored[i];
        }
        name[i] = '\0';
    }
    return 0;
}

int wallpaper_picture_load(const char *path, wallpaper_picture_t *out) {
    memset(out, 0, sizeof(*out));
    long fd = sys_open(path, OPEN_READ);
    if (fd < 0) {
        return -1;
    }
    if (read_header((int)fd, &out->width, &out->height, out->name, sizeof(out->name)) != 0) {
        sys_close((int)fd);
        return -1;
    }
    size_t bytes = (size_t)out->width * out->height * sizeof(uint32_t);
    out->pixels = (uint32_t *)malloc(bytes);
    if (!out->pixels || read_all((int)fd, out->pixels, bytes) != 0) {
        sys_close((int)fd);
        wallpaper_picture_free(out);
        return -1;
    }
    sys_close((int)fd);
    return 0;
}

int wallpaper_picture_read_name(const char *path, char *out, size_t capacity) {
    long fd = sys_open(path, OPEN_READ);
    if (fd < 0) {
        return -1;
    }
    uint32_t width;
    uint32_t height;
    int result = read_header((int)fd, &width, &height, out, capacity);
    sys_close((int)fd);
    return result;
}

void wallpaper_picture_free(wallpaper_picture_t *picture) {
    free(picture->pixels);
    picture->pixels = 0;
    picture->width = 0;
    picture->height = 0;
}

static uint32_t lerp_pixel(uint32_t a, uint32_t b, uint32_t fraction) {
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        uint32_t from = (a >> shift) & 0xFFu;
        uint32_t to = (b >> shift) & 0xFFu;
        out |= ((from * (256u - fraction) + to * fraction + 128u) >> 8) << shift;
    }
    return out;
}

/* Scaled until it covers the whole rectangle and cropped evenly from both
   sides of the direction that is left over - what every desktop calls Fill -
   with bilinear sampling, so an enlarged picture is soft rather than
   blocky. */
void wallpaper_picture_cover(const wallpaper_picture_t *picture, graphics_context_t *target, int32_t x, int32_t y,
                             int32_t w, int32_t h) {
    if (!picture->pixels || w <= 0 || h <= 0) {
        return;
    }
    uint64_t pw = picture->width;
    uint64_t ph = picture->height;
    uint64_t visible_w = pw << 16;
    uint64_t visible_h = ph << 16;
    if (pw * (uint64_t)h > ph * (uint64_t)w) {
        visible_w = (ph << 16) * (uint64_t)w / (uint64_t)h;
    } else {
        visible_h = (pw << 16) * (uint64_t)h / (uint64_t)w;
    }
    uint64_t step_x = visible_w / (uint64_t)w;
    uint64_t step_y = visible_h / (uint64_t)h;
    int64_t start_x = (int64_t)(((pw << 16) - visible_w) / 2) + (int64_t)step_x / 2 - 32768;
    int64_t start_y = (int64_t)(((ph << 16) - visible_h) / 2) + (int64_t)step_y / 2 - 32768;

    for (int32_t row = 0; row < h; row++) {
        int32_t ty = y + row;
        if (ty < 0 || ty >= target->height) {
            continue;
        }
        int64_t sy = start_y + (int64_t)step_y * row;
        if (sy < 0) {
            sy = 0;
        }
        uint32_t y0 = (uint32_t)(sy >> 16);
        uint32_t fy = (uint32_t)((sy >> 8) & 0xFF);
        if (y0 >= ph - 1) {
            y0 = (uint32_t)ph - 1;
            fy = 0;
        }
        uint32_t y1 = y0 + 1 < ph ? y0 + 1 : y0;
        const uint32_t *top = picture->pixels + (uint64_t)y0 * pw;
        const uint32_t *bottom = picture->pixels + (uint64_t)y1 * pw;
        uint32_t *line = target->pixels + (int64_t)ty * target->width;
        for (int32_t column = 0; column < w; column++) {
            int32_t tx = x + column;
            if (tx < 0 || tx >= target->width) {
                continue;
            }
            int64_t sx = start_x + (int64_t)step_x * column;
            if (sx < 0) {
                sx = 0;
            }
            uint32_t x0 = (uint32_t)(sx >> 16);
            uint32_t fx = (uint32_t)((sx >> 8) & 0xFF);
            if (x0 >= pw - 1) {
                x0 = (uint32_t)pw - 1;
                fx = 0;
            }
            uint32_t x1 = x0 + 1 < pw ? x0 + 1 : x0;
            uint32_t upper = lerp_pixel(top[x0], top[x1], fx);
            uint32_t lower = lerp_pixel(bottom[x0], bottom[x1], fx);
            line[tx] = lerp_pixel(upper, lower, fy);
        }
    }
}
