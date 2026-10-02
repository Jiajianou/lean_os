#pragma once

#include <stdint.h>

#define LVGL_POINTER_QUEUE_SIZE 16

/* M212. LVGL samples a pointer once a refresh and sees only its state at that
   instant, so a press and a release that arrive between two samples - which
   is exactly what a tap on a touchpad is - would never be seen at all. Each
   change of the primary button is queued here instead and handed to LVGL one
   sample a read. */
typedef struct {
    int32_t x;
    int32_t y;
    uint8_t pressed;
} lvgl_pointer_sample_t;

typedef struct {
    lvgl_pointer_sample_t samples[LVGL_POINTER_QUEUE_SIZE];
    uint8_t head;
    uint8_t tail;
    uint8_t last_pressed;
    int32_t x;
    int32_t y;
} lvgl_pointer_queue_t;

void lvgl_pointer_queue_move(lvgl_pointer_queue_t *queue, int32_t x, int32_t y);

void lvgl_pointer_queue_button(lvgl_pointer_queue_t *queue, int32_t x, int32_t y, uint8_t pressed);

void lvgl_pointer_queue_release(lvgl_pointer_queue_t *queue);

int lvgl_pointer_queue_read(lvgl_pointer_queue_t *queue, lvgl_pointer_sample_t *out);
