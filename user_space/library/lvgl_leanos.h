#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "window_manager_client.h"

#define LVGL_KEY_RING_SIZE 32
#define LVGL_MAX_SLEEP_MS 250

struct lvgl_window;

typedef int (*lvgl_key_handler_t)(struct lvgl_window *window, uint32_t ch, uint32_t mods);

typedef struct lvgl_window {
    window_manager_window_t window;
    lv_display_t *display;
    lv_indev_t *pointer;
    lv_indev_t *keypad;
    lv_group_t *group;
    uint32_t *bound_pixels;
    int32_t pointer_x;
    int32_t pointer_y;
    uint8_t pointer_pressed;
    uint32_t key_ring[LVGL_KEY_RING_SIZE];
    uint8_t key_head;
    uint8_t key_tail;
    uint32_t key_current;
    uint8_t key_held;
    int should_close;
    int confirm_close;
    lvgl_key_handler_t key_handler;
    void *user_data;
} lvgl_window_t;

int lvgl_window_open(uint32_t width, uint32_t height, const char *title, lvgl_window_t *out);

int lvgl_window_open_confirm_close(uint32_t width, uint32_t height, const char *title, lvgl_window_t *out);

void lvgl_window_set_key_handler(lvgl_window_t *window, lvgl_key_handler_t handler);

void lvgl_window_report_geometry(const char *name, lv_obj_t *object);

int lvgl_window_pump(lvgl_window_t *window, int timeout_ms);

void lvgl_window_run(lvgl_window_t *window);

void lvgl_window_request_close(lvgl_window_t *window);

void lvgl_window_close(lvgl_window_t *window);
