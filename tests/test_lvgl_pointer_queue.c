#include "check.h"

#include "../user_space/library/lvgl_pointer_queue.c"

static int read_all(lvgl_pointer_queue_t *queue, lvgl_pointer_sample_t *out, int limit) {
    int count = 0;
    int more = 1;
    while (more && count < limit) {
        more = lvgl_pointer_queue_read(queue, &out[count]);
        count++;
    }
    return count;
}

TEST(lvgl_pointer_queue, a_tap_between_two_reads_is_a_press_and_then_a_release) {
    lvgl_pointer_queue_t queue = {0};
    lvgl_pointer_queue_move(&queue, 40, 50);
    lvgl_pointer_queue_button(&queue, 40, 50, 1);
    lvgl_pointer_queue_button(&queue, 40, 50, 0);

    lvgl_pointer_sample_t samples[8];
    int count = read_all(&queue, samples, 8);
    CHECK_EQ(count, 2);
    CHECK_EQ(samples[0].pressed, 1);
    CHECK_EQ(samples[0].x, 40);
    CHECK_EQ(samples[0].y, 50);
    CHECK_EQ(samples[1].pressed, 0);
}

TEST(lvgl_pointer_queue, with_nothing_queued_a_read_reports_where_the_pointer_is_now) {
    lvgl_pointer_queue_t queue = {0};
    lvgl_pointer_queue_button(&queue, 10, 10, 1);
    lvgl_pointer_sample_t sample;
    CHECK_EQ(lvgl_pointer_queue_read(&queue, &sample), 0);
    CHECK_EQ(sample.pressed, 1);
    lvgl_pointer_queue_move(&queue, 30, 35);
    CHECK_EQ(lvgl_pointer_queue_read(&queue, &sample), 0);
    CHECK_EQ(sample.pressed, 1);
    CHECK_EQ(sample.x, 30);
    CHECK_EQ(sample.y, 35);
}

TEST(lvgl_pointer_queue, a_button_event_that_changes_nothing_is_not_queued) {
    lvgl_pointer_queue_t queue = {0};
    lvgl_pointer_queue_button(&queue, 1, 1, 0);
    lvgl_pointer_sample_t sample;
    CHECK_EQ(lvgl_pointer_queue_read(&queue, &sample), 0);
    CHECK_EQ(queue.head, queue.tail);
    lvgl_pointer_queue_button(&queue, 1, 1, 1);
    lvgl_pointer_queue_button(&queue, 2, 2, 1);
    lvgl_pointer_sample_t samples[8];
    CHECK_EQ(read_all(&queue, samples, 8), 1);
}

TEST(lvgl_pointer_queue, a_double_tap_is_two_clicks_in_order) {
    lvgl_pointer_queue_t queue = {0};
    lvgl_pointer_queue_button(&queue, 5, 6, 1);
    lvgl_pointer_queue_button(&queue, 5, 6, 0);
    lvgl_pointer_queue_button(&queue, 7, 8, 1);
    lvgl_pointer_queue_button(&queue, 7, 8, 0);
    lvgl_pointer_sample_t samples[8];
    CHECK_EQ(read_all(&queue, samples, 8), 4);
    CHECK_EQ(samples[0].pressed, 1);
    CHECK_EQ(samples[1].pressed, 0);
    CHECK_EQ(samples[2].pressed, 1);
    CHECK_EQ(samples[2].x, 7);
    CHECK_EQ(samples[3].pressed, 0);
}

TEST(lvgl_pointer_queue, overflow_keeps_the_final_state_and_alternation) {
    lvgl_pointer_queue_t queue = {0};
    for (int i = 0; i < 41; i++) {
        lvgl_pointer_queue_button(&queue, i, i, (uint8_t)((i + 1) & 1));
    }
    lvgl_pointer_sample_t samples[LVGL_POINTER_QUEUE_SIZE + 2];
    int count = read_all(&queue, samples, LVGL_POINTER_QUEUE_SIZE + 2);
    CHECK(count < LVGL_POINTER_QUEUE_SIZE);
    for (int i = 1; i < count; i++) {
        CHECK_NE(samples[i].pressed, samples[i - 1].pressed);
    }
    CHECK_EQ(samples[count - 1].pressed, 1);
    CHECK_EQ(samples[count - 1].x, 40);
}

TEST(lvgl_pointer_queue, release_forgets_what_was_queued) {
    lvgl_pointer_queue_t queue = {0};
    lvgl_pointer_queue_button(&queue, 1, 1, 1);
    lvgl_pointer_queue_release(&queue);
    lvgl_pointer_sample_t sample;
    CHECK_EQ(lvgl_pointer_queue_read(&queue, &sample), 0);
    CHECK_EQ(sample.pressed, 0);
    lvgl_pointer_queue_button(&queue, 1, 1, 1);
    CHECK_EQ(lvgl_pointer_queue_read(&queue, &sample), 0);
    CHECK_EQ(sample.pressed, 1);
}
