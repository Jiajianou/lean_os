#include "lvgl_pointer_queue.h"

void lvgl_pointer_queue_move(lvgl_pointer_queue_t *queue, int32_t x, int32_t y) {
    queue->x = x;
    queue->y = y;
}

void lvgl_pointer_queue_button(lvgl_pointer_queue_t *queue, int32_t x, int32_t y, uint8_t pressed) {
    queue->x = x;
    queue->y = y;
    pressed = pressed ? 1 : 0;
    if (pressed == queue->last_pressed) {
        return;
    }
    uint8_t next = (uint8_t)((queue->head + 1u) % LVGL_POINTER_QUEUE_SIZE);
    if (next == queue->tail) {
        /* Full: drop the oldest pair rather than the newest change, so the
           state LVGL ends on is still the state the hand left the button in. */
        queue->tail = (uint8_t)((queue->tail + 2u) % LVGL_POINTER_QUEUE_SIZE);
    }
    queue->samples[queue->head] = (lvgl_pointer_sample_t){x, y, pressed};
    queue->head = next;
    queue->last_pressed = pressed;
}

void lvgl_pointer_queue_release(lvgl_pointer_queue_t *queue) {
    queue->head = queue->tail;
    queue->last_pressed = 0;
}

int lvgl_pointer_queue_read(lvgl_pointer_queue_t *queue, lvgl_pointer_sample_t *out) {
    if (queue->head == queue->tail) {
        out->x = queue->x;
        out->y = queue->y;
        out->pressed = queue->last_pressed;
        return 0;
    }
    *out = queue->samples[queue->tail];
    queue->tail = (uint8_t)((queue->tail + 1u) % LVGL_POINTER_QUEUE_SIZE);
    return queue->head != queue->tail;
}
