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
    (void)area;
    (void)pixels;
    lvgl_window_t *window = (lvgl_window_t *)lv_display_get_user_data(display);
    if (window && lv_display_flush_is_last(display)) {
        window_manager_present(&window->window);
    }
    lv_display_flush_ready(display);
}

static void lvgl_read_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
    lvgl_window_t *window = (lvgl_window_t *)lv_indev_get_user_data(indev);
    if (!window) {
        return;
    }
    data->point.x = window->pointer_x;
    data->point.y = window->pointer_y;
    data->state = window->pointer_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
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
