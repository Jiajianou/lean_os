#include <stdio.h>
#include <string.h>

#include "bitmap_file.h"
#include "desktop_applications.h"
#include "lvgl.h"
#include "lvgl_leanos.h"
#include "paths.h"
#include "settings_file.h"
#include "syscall.h"
#include "syscall_wrappers.h"
#include "wallpaper.h"
#include "wallpaper_picture.h"
#include "window_manager_client.h"

#define DESKTOP_PICTURE_LARGEST_PIXELS (16u * 1024u * 1024u)

static const char *base_name(const char *path) {
    const char *name = path;
    for (const char *at = path; *at; at++) {
        if (*at == '/' && at[1]) {
            name = at + 1;
        }
    }
    return name;
}

/* M211. The toolkit already opens every picture Files can show - PNG through
   lodepng and BMP in all its depths - so the picture is drawn by it, once,
   onto a canvas of its own size, and what lands there is what the desktop
   will show. Drawing rather than reading the decoder's buffer is the point:
   the decoders hand back four different pixel formats, and the canvas is
   always the one this compositor uses. */
int desktop_application_make_desktop_picture(const char *image_path, const char *output_path, char *message,
                                             size_t capacity) {
    char source[PATH_MAX_LENGTH + 4];
    snprintf(source, sizeof(source), "A:%s", image_path);
    lv_image_header_t header;
    if (lv_image_decoder_get_info(source, &header) != LV_RESULT_OK || header.w == 0 || header.h == 0) {
        snprintf(message, capacity, "%s is not a picture this machine can open.", base_name(image_path));
        return -1;
    }
    if ((uint32_t)header.w * header.h > DESKTOP_PICTURE_LARGEST_PIXELS) {
        snprintf(message, capacity, "%s is %ux%u - too large for a desktop picture here.", base_name(image_path),
                 (unsigned)header.w, (unsigned)header.h);
        return -1;
    }
    lv_draw_buf_t *buffer = lv_draw_buf_create(header.w, header.h, LV_COLOR_FORMAT_XRGB8888, LV_STRIDE_AUTO);
    if (!buffer) {
        snprintf(message, capacity, "There is not enough memory to open %s.", base_name(image_path));
        return -1;
    }
    lv_obj_t *canvas = lv_canvas_create(lv_screen_active());
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(canvas, buffer);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_image_dsc_t image;
    lv_draw_image_dsc_init(&image);
    image.src = source;
    lv_area_t area = {0, 0, (int32_t)header.w - 1, (int32_t)header.h - 1};
    lv_draw_image(&layer, &image, &area);
    lv_canvas_finish_layer(canvas, &layer);

    int result = wallpaper_picture_save(output_path, base_name(image_path), (const uint32_t *)buffer->data,
                                        header.w, header.h, buffer->header.stride / 4u);
    lv_obj_delete(canvas);
    lv_draw_buf_destroy(buffer);
    lv_image_cache_drop(source);
    if (result != 0) {
        snprintf(message, capacity, "The picture could not be saved for the desktop.");
        return -1;
    }
    snprintf(message, capacity, "%s is the desktop picture.", base_name(image_path));
    return 0;
}

int desktop_application_use_wallpaper(uint32_t wallpaper) {
    window_manager_settings_request_t settings;
    settings_file_defaults(&settings);
    settings_file_load(&settings);
    window_manager_settings_request_t live;
    if (window_manager_query_settings(&live) == 0) {
        settings = live;
    }
    settings.wallpaper = wallpaper;
    int sent = window_manager_set_settings(&settings);
    int saved = settings_file_save(&settings);
    return sent == 0 && saved == 0 ? 0 : -1;
}

#define SELFTEST_WIDTH  64
#define SELFTEST_HEIGHT 40

static uint32_t selftest_quadrant(uint32_t x, uint32_t y) {
    if (y < SELFTEST_HEIGHT / 2) {
        return x < SELFTEST_WIDTH / 2 ? 0x00E74C3Cu : 0x002ECC71u;
    }
    return x < SELFTEST_WIDTH / 2 ? 0x003498DBu : 0x00F1C40Fu;
}

static int selftest_input_devices(void) {
    char devices[256];
    long fd = sys_open(PATH_PROCESS_DIRECTORY "input", OPEN_READ);
    if (fd < 0) {
        return 0;
    }
    long n = sys_read((int)fd, devices, sizeof(devices) - 1);
    sys_close((int)fd);
    if (n <= 0) {
        return 0;
    }
    devices[n] = '\0';
    printf("[m211] /proc/input: %s", devices);
    return strstr(devices, "mouse\t") != 0 || strstr(devices, "trackpad\t") != 0;
}

/* M211's boot self-test, run under the name wallpapertest: a picture this
   program writes is turned into the desktop's picture by the same path
   Files' menu takes, read back, and checked pixel for pixel - so the
   decoder, the canvas, the averaging and the file format are graded on
   leanfs, on the machine. Each failure has its own exit code. */
int desktop_application_wallpaper_selftest(int selftest) {
    (void)selftest;
    lvgl_window_t window;
    if (lvgl_window_open(120, 80, "Wallpaper test", &window) != 0) {
        return 2;
    }
    static uint32_t source[SELFTEST_WIDTH * SELFTEST_HEIGHT];
    for (uint32_t y = 0; y < SELFTEST_HEIGHT; y++) {
        for (uint32_t x = 0; x < SELFTEST_WIDTH; x++) {
            source[y * SELFTEST_WIDTH + x] = selftest_quadrant(x, y);
        }
    }
    int result = 0;
    char message[160];
    wallpaper_picture_t picture = {0};
    if (bitmap_file_write(PATH_TEMPORARY_DIRECTORY "m211.bmp", source, SELFTEST_WIDTH, SELFTEST_HEIGHT,
                          SELFTEST_WIDTH) != 0) {
        result = 3;
    } else if (desktop_application_make_desktop_picture(PATH_TEMPORARY_DIRECTORY "m211.bmp",
                                                        PATH_TEMPORARY_DIRECTORY "m211.picture", message,
                                                        sizeof(message)) != 0) {
        printf("[m211] %s\n", message);
        result = 4;
    } else if (wallpaper_picture_load(PATH_TEMPORARY_DIRECTORY "m211.picture", &picture) != 0) {
        result = 5;
    } else if (picture.width != SELFTEST_WIDTH || picture.height != SELFTEST_HEIGHT ||
               strcmp(picture.name, "m211.bmp") != 0) {
        result = 6;
    } else {
        for (uint32_t i = 0; i < SELFTEST_WIDTH * SELFTEST_HEIGHT && result == 0; i++) {
            if ((picture.pixels[i] & 0x00FFFFFFu) != source[i]) {
                printf("[m211] pixel %u is 0x%06x, wanted 0x%06x\n", (unsigned)i,
                       (unsigned)(picture.pixels[i] & 0x00FFFFFFu), (unsigned)source[i]);
                result = 7;
            }
        }
    }
    if (result == 0) {
        static uint32_t screen[160 * 100];
        graphics_context_t context = {screen, 160, 100};
        wallpaper_picture_cover(&picture, &context, 0, 0, 160, 100);
        if ((screen[10 * 160 + 10] & 0x00FFFFFFu) != 0x00E74C3Cu ||
            (screen[90 * 160 + 150] & 0x00FFFFFFu) != 0x00F1C40Fu) {
            result = 8;
        }
    }
    wallpaper_picture_free(&picture);
    if (result == 0) {
        sys_writefile(PATH_TEMPORARY_DIRECTORY "m211.txt", "not a picture\n", 14);
        if (desktop_application_make_desktop_picture(PATH_TEMPORARY_DIRECTORY "m211.txt",
                                                     PATH_TEMPORARY_DIRECTORY "m211.refused", message,
                                                     sizeof(message)) == 0) {
            result = 9;
        } else {
            os_stat_t status;
            if (sys_stat(PATH_TEMPORARY_DIRECTORY "m211.refused", &status) == 0) {
                result = 10;
            }
        }
    }
    if (result == 0 && !selftest_input_devices()) {
        result = 11;
    }
    sys_unlink(PATH_TEMPORARY_DIRECTORY "m211.bmp");
    sys_unlink(PATH_TEMPORARY_DIRECTORY "m211.picture");
    sys_unlink(PATH_TEMPORARY_DIRECTORY "m211.txt");
    lvgl_window_close(&window);
    return result;
}
