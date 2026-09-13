#pragma once

#include "desktop_palette.h"
#include "lvgl_leanos.h"

#define LVGL_THEME_GAP        10
#define LVGL_THEME_PAD        14
#define LVGL_THEME_RADIUS     10
#define LVGL_THEME_CONTROL_R   7
#define LVGL_THEME_ROW_HEIGHT 34
#define LVGL_THEME_LABEL_WIDTH 84

void lvgl_theme_apply(lvgl_window_t *window);

const desktop_palette_t *lvgl_theme_palette(void);

lv_color_t lvgl_theme_color(uint32_t rgb);

lv_obj_t *lvgl_theme_page(lv_obj_t *parent);

lv_obj_t *lvgl_theme_card(lv_obj_t *page, const char *heading);

lv_obj_t *lvgl_theme_row(lv_obj_t *card, const char *label);

lv_obj_t *lvgl_theme_caption(lv_obj_t *parent, const char *text);

lv_obj_t *lvgl_theme_value(lv_obj_t *parent, const char *text);

lv_obj_t *lvgl_theme_button(lv_obj_t *parent, const char *label, int prominent);

lv_obj_t *lvgl_theme_swatch(lv_obj_t *parent, uint32_t rgb);

void lvgl_theme_swatch_select(lv_obj_t *swatch, int selected);

lv_obj_t *lvgl_theme_choice(lv_obj_t *parent, const char *label);

void lvgl_theme_choice_select(lv_obj_t *choice, int selected);
