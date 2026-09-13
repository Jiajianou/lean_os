#include "lvgl_theme.h"

#include "window_manager_client.h"

static desktop_palette_t palette;
static int palette_ready;

lv_color_t lvgl_theme_color(uint32_t rgb) {
    return lv_color_hex(rgb & 0x00FFFFFFu);
}

const desktop_palette_t *lvgl_theme_palette(void) {
    if (!palette_ready) {
        desktop_palette_derive(DESKTOP_PALETTE_DEFAULT_BACKGROUND, DESKTOP_PALETTE_DEFAULT_ACCENT, &palette);
        palette_ready = 1;
    }
    return &palette;
}

void lvgl_theme_apply(lvgl_window_t *window) {
    uint32_t background = DESKTOP_PALETTE_DEFAULT_BACKGROUND;
    uint32_t accent = DESKTOP_PALETTE_DEFAULT_ACCENT;

    window_manager_settings_request_t settings;
    if (window_manager_query_settings(&settings) == 0) {
        background = settings.bg_color;
        accent = settings.accent_color;
    }
    desktop_palette_derive(background, accent, &palette);
    palette_ready = 1;

    if (!window || !window->display) {
        return;
    }

    lv_theme_t *theme = lv_theme_default_init(window->display,
                                              lvgl_theme_color(palette.accent),
                                              lvgl_theme_color(palette.surface_raised),
                                              true,
                                              &lv_font_montserrat_14);
    if (theme) {
        lv_display_set_theme(window->display, theme);
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lvgl_theme_color(palette.window), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, lvgl_theme_color(palette.text), LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
}

static void style_scrollbar(lv_obj_t *object) {
    const desktop_palette_t *p = lvgl_theme_palette();
    lv_obj_set_style_bg_color(object, lvgl_theme_color(p->outline), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(object, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(object, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(object, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(object, 2, LV_PART_SCROLLBAR);
}

lv_obj_t *lvgl_theme_page(lv_obj_t *parent) {
    const desktop_palette_t *p = lvgl_theme_palette();
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lvgl_theme_color(p->window), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(page, LVGL_THEME_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, LVGL_THEME_GAP, LV_PART_MAIN);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    style_scrollbar(page);
    return page;
}

lv_obj_t *lvgl_theme_card(lv_obj_t *page, const char *heading) {
    const desktop_palette_t *p = lvgl_theme_palette();
    lv_obj_t *card = lv_obj_create(page);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lvgl_theme_color(p->surface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lvgl_theme_color(p->outline), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, LVGL_THEME_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, LVGL_THEME_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, LVGL_THEME_GAP, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (heading) {
        lv_obj_t *label = lv_label_create(card);
        lv_label_set_text(label, heading);
        lv_obj_set_style_text_color(label, lvgl_theme_color(p->text_dim), LV_PART_MAIN);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(label, 1, LV_PART_MAIN);
    }
    return card;
}

lv_obj_t *lvgl_theme_row(lv_obj_t *card, const char *label) {
    const desktop_palette_t *p = lvgl_theme_palette();
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, LVGL_THEME_GAP, LV_PART_MAIN);
    lv_obj_set_style_min_height(row, LVGL_THEME_ROW_HEIGHT, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (label) {
        lv_obj_t *text = lv_label_create(row);
        lv_label_set_text(text, label);
        lv_obj_set_width(text, LVGL_THEME_LABEL_WIDTH);
        lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(text, lvgl_theme_color(p->text), LV_PART_MAIN);
    }
    return row;
}

lv_obj_t *lvgl_theme_caption(lv_obj_t *parent, const char *text) {
    const desktop_palette_t *p = lvgl_theme_palette();
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lvgl_theme_color(p->text_dim), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
    return label;
}

lv_obj_t *lvgl_theme_value(lv_obj_t *parent, const char *text) {
    const desktop_palette_t *p = lvgl_theme_palette();
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lvgl_theme_color(p->text), LV_PART_MAIN);
    return label;
}

lv_obj_t *lvgl_theme_button(lv_obj_t *parent, const char *label, int prominent) {
    const desktop_palette_t *p = lvgl_theme_palette();
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_height(button, 30);
    lv_obj_set_style_radius(button, LVGL_THEME_CONTROL_R, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(button, 14, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, prominent ? 0 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lvgl_theme_color(p->outline), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button,
                              lvgl_theme_color(prominent ? p->accent : p->surface_raised),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(button,
                              lvgl_theme_color(prominent ? desktop_palette_mix(p->accent, 0x00FFFFFFu, 14u)
                                                         : desktop_palette_mix(p->surface_raised, 0x00FFFFFFu, 10u)),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lvgl_theme_color(prominent ? p->accent_text : p->text), LV_PART_MAIN);

    lv_obj_t *text = lv_label_create(button);
    lv_label_set_text(text, label);
    lv_obj_center(text);
    return button;
}

lv_obj_t *lvgl_theme_swatch(lv_obj_t *parent, uint32_t rgb) {
    lv_obj_t *swatch = lv_button_create(parent);
    lv_obj_set_size(swatch, 30, 30);
    lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(swatch, lvgl_theme_color(rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(swatch, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(swatch, 0, LV_PART_MAIN);
    lvgl_theme_swatch_select(swatch, 0);
    return swatch;
}

void lvgl_theme_swatch_select(lv_obj_t *swatch, int selected) {
    const desktop_palette_t *p = lvgl_theme_palette();
    lv_obj_set_style_outline_width(swatch, selected ? 2 : 1, LV_PART_MAIN);
    lv_obj_set_style_outline_pad(swatch, selected ? 3 : 0, LV_PART_MAIN);
    lv_obj_set_style_outline_color(swatch,
                                   lvgl_theme_color(selected ? p->text : p->outline),
                                   LV_PART_MAIN);
    lv_obj_set_style_outline_opa(swatch, LV_OPA_COVER, LV_PART_MAIN);
}

lv_obj_t *lvgl_theme_choice(lv_obj_t *parent, const char *label) {
    lv_obj_t *choice = lvgl_theme_button(parent, label, 0);
    lv_obj_set_height(choice, 28);
    lv_obj_set_style_pad_hor(choice, 10, LV_PART_MAIN);
    lvgl_theme_choice_select(choice, 0);
    return choice;
}

void lvgl_theme_choice_select(lv_obj_t *choice, int selected) {
    const desktop_palette_t *p = lvgl_theme_palette();
    lv_obj_set_style_bg_color(choice,
                              lvgl_theme_color(selected ? p->accent : p->surface_raised),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(choice,
                                  lvgl_theme_color(selected ? p->accent : p->outline),
                                  LV_PART_MAIN);
    lv_obj_set_style_text_color(choice,
                                lvgl_theme_color(selected ? p->accent_text : p->text),
                                LV_PART_MAIN);
}
