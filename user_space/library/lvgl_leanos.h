#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "lvgl_pointer_queue.h"
#include "window_manager_client.h"

#define LVGL_KEY_RING_SIZE 32
#define LVGL_MAX_SLEEP_MS 250

struct lvgl_window;

typedef int (*lvgl_key_handler_t)(struct lvgl_window *window, uint32_t ch, uint32_t mods);

typedef int (*lvgl_event_handler_t)(struct lvgl_window *window, const window_manager_event_t *event);

#define LVGL_WHEEL_STEP 48

typedef struct lvgl_window {
    window_manager_window_t window;
    lv_display_t *display;
    lv_indev_t *pointer;
    lv_indev_t *keypad;
    lv_group_t *group;
    uint32_t *bound_pixels;
    int32_t pointer_x;
    int32_t pointer_y;
    lvgl_pointer_queue_t pointer_queue;
    /* M200: the union of the areas LVGL flushed this refresh, presented as
       one rectangle on the last flush rather than the whole window. */
    int32_t damage_x0, damage_y0, damage_x1, damage_y1;
    uint8_t damage_pending;
    uint32_t key_ring[LVGL_KEY_RING_SIZE];
    uint8_t key_head;
    uint8_t key_tail;
    uint32_t key_current;
    uint8_t key_held;
    int should_close;
    int confirm_close;
    lvgl_key_handler_t key_handler;
    lvgl_event_handler_t event_handler;
    void *user_data;
} lvgl_window_t;

int lvgl_window_open(uint32_t width, uint32_t height, const char *title, lvgl_window_t *out);

int lvgl_window_open_confirm_close(uint32_t width, uint32_t height, const char *title, lvgl_window_t *out);

void lvgl_window_set_key_handler(lvgl_window_t *window, lvgl_key_handler_t handler);

void lvgl_window_set_event_handler(lvgl_window_t *window, lvgl_event_handler_t handler);

lv_obj_t *lvgl_window_object_at(int32_t x, int32_t y);

int lvgl_window_scroll_at(int32_t x, int32_t y, int32_t wheel);

void lvgl_window_report_geometry(const char *name, lv_obj_t *object);

int lvgl_window_pump(lvgl_window_t *window, int timeout_ms);

void lvgl_window_run(lvgl_window_t *window);

void lvgl_window_release_pointer(lvgl_window_t *window);

void lvgl_window_request_close(lvgl_window_t *window);

void lvgl_window_close(lvgl_window_t *window);
