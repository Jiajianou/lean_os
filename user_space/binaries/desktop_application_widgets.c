#include <stdio.h>

#include "desktop_applications.h"
#include "lvgl_theme.h"

#define DEMO_WIDTH 640
#define DEMO_HEIGHT 480
#define MINIMUM_RENDERED_PIXELS 10000

static lvgl_window_t demo_window;
static lv_obj_t *counter_label;
static int click_count;

static void on_button_clicked(lv_event_t *event) {
    (void)event;
    char text[32];
    click_count++;
    snprintf(text, sizeof(text), "Clicked %d", click_count);
    lv_label_set_text(counter_label, text);
}

static void on_slider_changed(lv_event_t *event) {
    lv_obj_t *slider = lv_event_get_target(event);
    lv_obj_t *bar = (lv_obj_t *)lv_event_get_user_data(event);
    lv_bar_set_value(bar, lv_slider_get_value(slider), LV_ANIM_ON);
}

static void build_user_interface(void) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 16, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "lean_os widgets");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lvgl_theme_color(palette->text), LV_PART_MAIN);

    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, 420, 260);
    lv_obj_set_style_bg_color(card, lvgl_theme_color(palette->surface), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lvgl_theme_color(palette->outline), LV_PART_MAIN);
    lv_obj_set_style_radius(card, LVGL_THEME_RADIUS, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 18, LV_PART_MAIN);

    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, 320, 12);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 40, LV_ANIM_OFF);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_width(slider, 320);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 40, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, on_slider_changed, LV_EVENT_VALUE_CHANGED, bar);

    lv_obj_t *toggle = lv_switch_create(card);
    lv_obj_add_state(toggle, LV_STATE_CHECKED);

    lv_obj_t *button = lvgl_theme_button(card, "Clicked 0", 1);
    lv_obj_add_event_cb(button, on_button_clicked, LV_EVENT_CLICKED, NULL);
    counter_label = lv_obj_get_child(button, 0);
}

static unsigned long count_rendered_pixels(void) {
    const desktop_palette_t *palette = lvgl_theme_palette();
    const uint32_t *pixels = demo_window.window.graphics.pixels;
    unsigned long total = (unsigned long)demo_window.window.width * demo_window.window.height;
    unsigned long drawn = 0;
    for (unsigned long i = 0; i < total; i++) {
        if ((pixels[i] & 0x00FFFFFFu) != palette->window) {
            drawn++;
        }
    }
    return drawn;
}

int desktop_application_widgets(int selftest) {
    if (lvgl_window_open(DEMO_WIDTH, DEMO_HEIGHT, "Widgets", &demo_window) != 0) {
        printf("[m125] lvgl_demo: window open failed\n");
        return 1;
    }
    printf("[m125] lvgl display %ux%u xrgb8888\n", demo_window.window.width, demo_window.window.height);

    lvgl_theme_apply(&demo_window);
    build_user_interface();

    for (int frame = 0; frame < 8; frame++) {
        lv_timer_handler();
        lvgl_window_pump(&demo_window, 16);
    }

    unsigned long drawn = count_rendered_pixels();
    printf("[m125] lvgl rendered %lu pixels into its own window\n", drawn);
    if (drawn < MINIMUM_RENDERED_PIXELS) {
        printf("[m125] lvgl render FAILED\n");
        lvgl_window_close(&demo_window);
        return 1;
    }

    if (selftest) {
        lvgl_window_close(&demo_window);
        return 0;
    }

    lvgl_window_run(&demo_window);
    lvgl_window_close(&demo_window);
    return 0;
}
