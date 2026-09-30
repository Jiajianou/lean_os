#include "lvgl_leanos.h"

#include <stdio.h>

#include "input.h"
#include "lvgl_keys.h"
#include "syscall_wrappers.h"

static int lvgl_runtime_ready;

static uint32_t lvgl_tick_now(void) {
    return (uint32_t)sys_uptime_ms();
}

static void lvgl_log_print(lv_log_level_t level, const char *text) {
    (void)level;
    fputs(text, stderr);
}

static void lvgl_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    (void)pixels;
    lvgl_window_t *window = (lvgl_window_t *)lv_display_get_user_data(display);
    if (window && area) {
        int32_t x0 = area->x1, y0 = area->y1, x1 = area->x2 + 1, y1 = area->y2 + 1;
        if (!window->damage_pending) {
            window->damage_x0 = x0;
            window->damage_y0 = y0;
            window->damage_x1 = x1;
            window->damage_y1 = y1;
            window->damage_pending = 1;
        } else {
            window->damage_x0 = x0 < window->damage_x0 ? x0 : window->damage_x0;
            window->damage_y0 = y0 < window->damage_y0 ? y0 : window->damage_y0;
            window->damage_x1 = x1 > window->damage_x1 ? x1 : window->damage_x1;
            window->damage_y1 = y1 > window->damage_y1 ? y1 : window->damage_y1;
        }
    }
    if (window && lv_display_flush_is_last(display)) {
        if (window->damage_pending) {
            window_manager_present_rect(&window->window, window->damage_x0, window->damage_y0,
                                        window->damage_x1 - window->damage_x0,
                                        window->damage_y1 - window->damage_y0);
        } else {
            window_manager_present(&window->window);
        }
        window->damage_pending = 0;
    }
    lv_display_flush_ready(display);
}

static void lvgl_read_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
    lvgl_window_t *window = (lvgl_window_t *)lv_indev_get_user_data(indev);
    if (!window) {
        return;
    }
    int32_t x = window->pointer_x;
    int32_t y = window->pointer_y;
    int32_t width = (int32_t)window->window.width;
    int32_t height = (int32_t)window->window.height;
    int outside = (x < 0 || y < 0 || x >= width || y >= height);
    data->point.x = x < 0 ? 0 : (x >= width ? width - 1 : x);
    data->point.y = y < 0 ? 0 : (y >= height ? height - 1 : y);
    data->state = (window->pointer_pressed && !outside) ? LV_INDEV_STATE_PRESSED
                                                        : LV_INDEV_STATE_RELEASED;
}

static void lvgl_read_keypad(lv_indev_t *indev, lv_indev_data_t *data) {
    lvgl_window_t *window = (lvgl_window_t *)lv_indev_get_user_data(indev);
    if (!window) {
        return;
    }
    if (window->key_held) {
        data->key = window->key_current;
        data->state = LV_INDEV_STATE_RELEASED;
        window->key_held = 0;
        data->continue_reading = window->key_head != window->key_tail;
        return;
    }
    if (window->key_head == window->key_tail) {
        data->key = window->key_current;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    window->key_current = window->key_ring[window->key_tail];
    window->key_tail = (uint8_t)((window->key_tail + 1u) % LVGL_KEY_RING_SIZE);
    window->key_held = 1;
    data->key = window->key_current;
    data->state = LV_INDEV_STATE_PRESSED;
    data->continue_reading = 1;
}

static void lvgl_push_key(lvgl_window_t *window, uint32_t key) {
    if (!key) {
        return;
    }
    uint8_t next = (uint8_t)((window->key_head + 1u) % LVGL_KEY_RING_SIZE);
    if (next == window->key_tail) {
        return;
    }
    window->key_ring[window->key_head] = key;
    window->key_head = next;
}

static void lvgl_runtime_init(void) {
    if (lvgl_runtime_ready) {
        return;
    }
    lv_init();
    lv_tick_set_cb(lvgl_tick_now);
    lv_log_register_print_cb(lvgl_log_print);
    lvgl_runtime_ready = 1;
}

static int lvgl_window_attach(lvgl_window_t *window) {
    lvgl_runtime_init();

    window->display = lv_display_create((int32_t)window->window.width, (int32_t)window->window.height);
    if (!window->display) {
        return -1;
    }
    lv_display_set_color_format(window->display, LV_COLOR_FORMAT_XRGB8888);
    window->bound_pixels = window->window.graphics.pixels;
    lv_display_set_buffers(window->display, window->window.graphics.pixels, NULL,
                           (uint32_t)(window->window.width * window->window.height * sizeof(uint32_t)),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(window->display, lvgl_flush);
    lv_display_set_user_data(window->display, window);

    window->pointer = lv_indev_create();
    if (!window->pointer) {
        return -1;
    }
    lv_indev_set_type(window->pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(window->pointer, lvgl_read_pointer);
    lv_indev_set_user_data(window->pointer, window);
    lv_indev_set_display(window->pointer, window->display);

    window->group = lv_group_create();
    if (!window->group) {
        return -1;
    }
    lv_group_set_default(window->group);

    window->keypad = lv_indev_create();
    if (!window->keypad) {
        return -1;
    }
    lv_indev_set_type(window->keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(window->keypad, lvgl_read_keypad);
    lv_indev_set_user_data(window->keypad, window);
    lv_indev_set_display(window->keypad, window->display);
    lv_indev_set_group(window->keypad, window->group);

    return 0;
}

int lvgl_window_open(uint32_t width, uint32_t height, const char *title, lvgl_window_t *out) {
    if (!out) {
        return -1;
    }
    for (unsigned long i = 0; i < sizeof(*out); i++) {
        ((unsigned char *)out)[i] = 0;
    }
    if (window_manager_connect(width, height, title, &out->window) != 0) {
        return -1;
    }
    return lvgl_window_attach(out);
}

int lvgl_window_open_confirm_close(uint32_t width, uint32_t height, const char *title, lvgl_window_t *out) {
    if (!out) {
        return -1;
    }
    for (unsigned long i = 0; i < sizeof(*out); i++) {
        ((unsigned char *)out)[i] = 0;
    }
    if (window_manager_connect_confirm_close(width, height, title, &out->window) != 0) {
        return -1;
    }
    out->confirm_close = 1;
    return lvgl_window_attach(out);
}

static void lvgl_rebind_buffer(lvgl_window_t *window) {
    uint32_t *pixels = window->window.graphics.pixels;
    if (!window->display || !pixels || pixels == window->bound_pixels) {
        return;
    }
    window->bound_pixels = pixels;
    lv_display_set_buffers(window->display, pixels, NULL,
                           (uint32_t)(window->window.width * window->window.height * sizeof(uint32_t)),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_obj_invalidate(lv_screen_active());
}

void lvgl_window_set_key_handler(lvgl_window_t *window, lvgl_key_handler_t handler) {
    if (window) {
        window->key_handler = handler;
    }
}

void lvgl_window_report_geometry(const char *name, lv_obj_t *object) {
    if (!name || !object) {
        return;
    }
    lv_obj_update_layout(lv_obj_get_screen(object));
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    printf("[geometry] %s %d %d %d %d\n", name, (int)area.x1, (int)area.y1,
           (int)(area.x2 - area.x1 + 1), (int)(area.y2 - area.y1 + 1));
}

int lvgl_window_pump(lvgl_window_t *window, int timeout_ms) {
    if (!window) {
        return -1;
    }
    window_manager_wait_ms(&window->window, NULL, 0, timeout_ms);

    window_manager_event_t event;
    while (window_manager_poll_event(&window->window, &event) > 0) {
        switch (event.type) {
        case WINDOW_MANAGER_EVENT_MOUSE_MOVE:
            window->pointer_x = event.x;
            window->pointer_y = event.y;
            break;
        case WINDOW_MANAGER_EVENT_MOUSE_BUTTON:
            window->pointer_x = event.x;
            window->pointer_y = event.y;
            window->pointer_pressed = (uint8_t)(event.buttons & 1u);
            break;
        case WINDOW_MANAGER_EVENT_KEY:
            if (window->key_handler &&
                window->key_handler(window, (uint32_t)(unsigned char)event.ch, (uint32_t)event.mods)) {
                break;
            }
            lvgl_push_key(window, lvgl_translate_key(event.ch, event.mods));
            break;
        case WINDOW_MANAGER_EVENT_EXPOSE:
            lv_obj_invalidate(lv_screen_active());
            break;
        case WINDOW_MANAGER_EVENT_CLOSE_REQUEST:
            window->should_close = 1;
            break;
        default:
            break;
        }
    }
    lvgl_rebind_buffer(window);
    return 0;
}

void lvgl_window_run(lvgl_window_t *window) {
    if (!window) {
        return;
    }
    while (!window->should_close) {
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms > LVGL_MAX_SLEEP_MS) {
            sleep_ms = LVGL_MAX_SLEEP_MS;
        }
        lvgl_window_pump(window, (int)sleep_ms);
    }
}

void lvgl_window_request_close(lvgl_window_t *window) {
    if (window) {
        window->should_close = 1;
    }
}

void lvgl_window_close(lvgl_window_t *window) {
    if (!window) {
        return;
    }
    if (window->window.graphics.pixels && window->window.shared_memory_bytes) {
        sys_shared_memory_unmap(window->window.graphics.pixels, window->window.shared_memory_bytes);
        window->window.graphics.pixels = (uint32_t *)0;
        window->window.graphics.width = 0;
        window->window.graphics.height = 0;
    }
    if (window->window.evt_file_descriptor >= 0) {
        sys_close(window->window.evt_file_descriptor);
        window->window.evt_file_descriptor = -1;
    }
}
