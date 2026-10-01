#include "icons.h"
#include "string_utilities.h"
#include "syscall_wrappers.h"
#include "window_manager_client.h"
#include "wireless.h"

#define PANEL_HEIGHT      44

#define PANEL_OVERHANG_MAX 96
#define PANEL_BUFFER_H       (PANEL_OVERHANG_MAX + PANEL_HEIGHT)

#define CONTEXT_W        132
#define CONTEXT_ITEM_H   26
#define CONTEXT_COUNT    3
#define CONTEXT_RADIUS   9
#define CONTEXT_BG       0x001E2430u
#define CONTEXT_HOVER_BG 0x00FFFFFFu
#define CONTEXT_HOVER_ALPHA 28u
#define CONTEXT_BORDER   0x00FFFFFFu
#define CONTEXT_BORDER_ALPHA 34u
#define CONTEXT_TEXT     0x00E8ECF4u

#define BTN_H            34
#define BTN_Y            ((PANEL_HEIGHT - BTN_H) / 2)
#define EDGE_PAD         8
#define TILE_RADIUS      9

#define START_X       EDGE_PAD
#define START_W       46
#define START_RING_OUTER 8
#define START_RING_INNER 4
#define START_RING_SAMPLES 4

#define SLOT_ICON        ICON_SMALL_SIZE
#define SLOT_W           130
#define SLOT_H           BTN_H
#define SLOT_GAP         6
#define SLOTS_X          (START_X + START_W + 10)
#define LABEL_PAD        (8 + SLOT_ICON + 8)
#define LABEL_MAX        10
#define MAX_RUNNING_SLOTS  WINDOW_MANAGER_MAX_ROUTABLE_WINDOWS

#define FOCUS_BAR_W      22
#define FOCUS_BAR_H      3
#define RUNNING_DOT      4

#define CLOCK_CHARS   5
#define TRAY_PAD      12
#define WS_DOT_W    14
#define WS_DOT_H    5
#define WS_DOT_GAP  5
#define TRAY_ICONS_W  (TRAY_PAD + WINDOW_MANAGER_WORKSPACE_COUNT * WS_DOT_W + (WINDOW_MANAGER_WORKSPACE_COUNT - 1) * WS_DOT_GAP + TRAY_PAD)

static int32_t clock_text_w(void) {
    return graphics_text_width(graphics_ui_font(), "00:00");
}

/* M207: the radio, as four bars beside the workspaces - lit by the signal
   of the network joined, dark when none is - and a click on them opens the
   Wi-Fi wizard. A machine with no radio shows nothing there, so its tray is
   the width it always was. */
#define WIFI_BARS     4
#define WIFI_BAR_W    3
#define WIFI_BAR_GAP  2
#define WIFI_W        (WIFI_BARS * WIFI_BAR_W + (WIFI_BARS - 1) * WIFI_BAR_GAP)
#define WIFI_TALLEST  14

static os_wireless_status_t wifi_status;

static int wifi_present(void) {
    return wifi_status.state != WIRELESS_STATE_ABSENT;
}

static int32_t tray_w(void) {
    return TRAY_ICONS_W + clock_text_w() + TRAY_PAD + (wifi_present() ? WIFI_W + TRAY_PAD : 0);
}

static int wifi_bars_lit(void) {
    if (wifi_status.state != WIRELESS_STATE_CONNECTED) {
        return 0;
    }
    int8_t signal = wifi_status.signal_dbm;
    return signal >= -55 ? 4 : signal >= -65 ? 3 : signal >= -75 ? 2 : 1;
}

#define REFRESH_INTERVAL_MS 300
#define PRESS_FLASH_MS      150

#define PANEL_TOP_COLOR     0x00222A38u
#define PANEL_BOTTOM_COLOR  0x00141821u
#define PANEL_HAIRLINE      0x00FFFFFFu
#define PANEL_HAIRLINE_ALPHA 30u
#define PANEL_SHADE         0x00000000u
#define PANEL_SHADE_ALPHA   70u

#define ACCENT_COLOR        0x004C99E6u
#define OVERLAY_COLOR       0x00FFFFFFu
#define HOVER_ALPHA         22u
#define FOCUS_ALPHA         38u
#define PRESS_ALPHA         64u
#define EDGE_ALPHA          26u

#define LABEL_COLOR         0x00F0F3F8u
#define LABEL_DIM_COLOR     0x009AA5B6u
#define CLOCK_FG            0x00DCE3EDu
#define WS_DOT_OFF          0x00FFFFFFu
#define WS_DOT_OFF_ALPHA    46u

#define MINIMIZED_TINT      0x00161A24u
#define MINIMIZED_TINT_PCT  55u

#define HOVER_NONE  (-1)
#define HOVER_START (-2)

typedef struct {
    int32_t x, w;
    int32_t window_id;
    uint8_t focused;
    uint8_t frontmost;
    uint8_t minimized;
    int32_t reported_id;
    int32_t reported_x;
    const uint8_t *image;
    char name[LABEL_MAX + 1];
} running_slot_t;

static running_slot_t running_slots[MAX_RUNNING_SLOTS];
static int running_count;
static int hovered = HOVER_NONE;
static long start_pressed_until_ms;

static int context_slot = -1;
static int32_t context_x, context_y;
static int context_hover = -1;
static int32_t context_overhang;

static graphics_context_t bar_graphics;

static int shown_workspace;

static void format_clock(long now_ms, char *out) {
    os_datetime_t t;
    if (sys_time(&t) > 0 && t.valid) {
        out[0] = (char)('0' + t.hour / 10);
        out[1] = (char)('0' + t.hour % 10);
        out[2] = ':';
        out[3] = (char)('0' + t.minute / 10);
        out[4] = (char)('0' + t.minute % 10);
        out[5] = '\0';
        return;
    }
    long total_s = now_ms / 1000;
    long mins = (total_s / 60) % 100;
    long secs = total_s % 60;
    out[0] = (char)('0' + (mins / 10) % 10);
    out[1] = (char)('0' + mins % 10);
    out[2] = ':';
    out[3] = (char)('0' + secs / 10);
    out[4] = (char)('0' + secs % 10);
    out[5] = '\0';
}

typedef struct {
    const char *title;
    const uint8_t *image;
} title_icon_t;

static const title_icon_t TITLE_ICONS[] = {
    {"Terminal", ICON_TERMINAL_SMALL},
    {"Console",  ICON_TERMINAL_SMALL},
    {"Editor",   ICON_EDITOR_SMALL},
    {"Files",    ICON_FILES_SMALL},
    {"Settings", ICON_SETTINGS_SMALL},
    {"Clock",    ICON_CLOCK_SMALL},
    {"Paint",    ICON_PAINT_SMALL},
    {"Tasks",    ICON_TASKS_SMALL},
    {"NetSurf",  ICON_BROWSER_SMALL},
    {"Browser",  ICON_BROWSER_SMALL},
};
#define TITLE_ICON_COUNT ((int)(sizeof(TITLE_ICONS) / sizeof(TITLE_ICONS[0])))

static const uint8_t *icon_for_title(const char *title) {
    if (title) {
        for (int i = 0; i < TITLE_ICON_COUNT; i++) {
            if (strcmp(title, TITLE_ICONS[i].title) == 0) {
                return TITLE_ICONS[i].image;
            }
        }
    }
    return ICON_APPLICATION_SMALL;
}

static void copy_label(char *destination, const char *source) {
    int i = 0;
    if (source && source[0]) {
        for (; i < LABEL_MAX && source[i]; i++) {
            destination[i] = source[i];
        }
    } else {
        static const char fallback[] = "App";
        for (; fallback[i]; i++) {
            destination[i] = fallback[i];
        }
    }
    destination[i] = '\0';
}

/* M197. The panel asked the compositor for its windows every 300 ms and
   then redrew and presented itself whether or not anything had changed -
   three whole-panel composites a second on an idle desktop. It still asks;
   it only presents when what it would draw is different. */
static uint32_t last_signature;

static uint32_t signature_mix(uint32_t hash, uint32_t value) {
    return (hash ^ value) * 16777619u;
}

static void format_clock(long now_ms, char *out);

static uint32_t panel_signature(void) {
    uint32_t hash = 2166136261u;
    hash = signature_mix(hash, (uint32_t)shown_workspace);
    hash = signature_mix(hash, (uint32_t)running_count);
    for (int i = 0; i < running_count; i++) {
        const running_slot_t *slot = &running_slots[i];
        hash = signature_mix(hash, (uint32_t)slot->x);
        hash = signature_mix(hash, (uint32_t)slot->window_id);
        hash = signature_mix(hash, (uint32_t)slot->focused << 8 | slot->minimized);
        hash = signature_mix(hash, (uint32_t)(uintptr_t)slot->image);
        for (const char *c = slot->name; *c; c++) {
            hash = signature_mix(hash, (uint8_t)*c);
        }
    }
    char clock_text[CLOCK_CHARS + 1];
    format_clock(sys_uptime_ms(), clock_text);
    for (const char *c = clock_text; *c; c++) {
        hash = signature_mix(hash, (uint8_t)*c);
    }
    hash = signature_mix(hash, (uint32_t)wifi_status.state << 8 | (uint32_t)wifi_bars_lit());
    return hash;
}

static void refresh_running_slots(window_manager_window_t *self) {
    window_manager_query_response_t q;
    if (window_manager_query_windows(&q) != 0) {
        running_count = 0;
        return;
    }
    shown_workspace = q.current_workspace;
    int32_t boundary = (int32_t)self->width - tray_w();
    int32_t x = SLOTS_X;
    running_count = 0;
    for (int32_t i = 0; i < q.count && running_count < MAX_RUNNING_SLOTS; i++) {
        const window_manager_window_info_t *info = &q.windows[i];
        if (info->is_panel || info->is_desktop || info->window_id == self->window_id) {
            continue;
        }
        if (info->workspace >= 0 && info->workspace != q.current_workspace) {
            continue;
        }
        if (x + SLOT_W > boundary) {
            break;
        }
        running_slot_t *slot = &running_slots[running_count];
        slot->x = x;
        slot->w = SLOT_W;
        slot->window_id = info->window_id;
        slot->focused = info->focused;
        slot->frontmost = 0;
        slot->minimized = info->minimized;
        slot->image = icon_for_title(info->title);
        copy_label(slot->name, info->title);
        x += SLOT_W + SLOT_GAP;
        running_count++;
    }
    for (int i = 0; i < running_count; i++) {
        running_slot_t *slot = &running_slots[i];
        if (slot->window_id != slot->reported_id || slot->x != slot->reported_x) {
            window_manager_set_taskbar_slot(slot->window_id, slot->x, slot->w);
            slot->reported_id = slot->window_id;
            slot->reported_x = slot->x;
        }
    }

    {
        int front = -1;
        int32_t best_z = -1;
        for (int32_t i = 0, k = 0; i < q.count && k < running_count; i++) {
            const window_manager_window_info_t *info = &q.windows[i];
            if (info->is_panel || info->is_desktop || info->window_id == self->window_id) {
                continue;
            }
            if (info->workspace >= 0 && info->workspace != q.current_workspace) {
                continue;
            }
            if (running_slots[k].window_id == info->window_id && !info->minimized &&
                info->z_index > best_z) {
                best_z = info->z_index;
                front = k;
            }
            k++;
        }
        if (front >= 0) {
            running_slots[front].frontmost = 1;
        }
    }
    if (hovered >= running_count) {
        hovered = HOVER_NONE;
    }
}

static void draw_ring(int32_t centre_x, int32_t centre_y, int32_t outer, int32_t inner, uint32_t color) {
    const int32_t n = START_RING_SAMPLES;
    const int32_t outer_squared = outer * outer * 4 * n * n;
    const int32_t inner_squared = inner * inner * 4 * n * n;
    for (int32_t y = centre_y - outer; y < centre_y + outer; y++) {
        for (int32_t x = centre_x - outer; x < centre_x + outer; x++) {
            uint32_t covered = 0;
            for (int32_t sy = 0; sy < n; sy++) {
                for (int32_t sx = 0; sx < n; sx++) {
                    int32_t dx = 2 * ((x - centre_x) * n + sx) + 1;
                    int32_t dy = 2 * ((y - centre_y) * n + sy) + 1;
                    int32_t d = dx * dx + dy * dy;
                    if (d <= outer_squared && d >= inner_squared) {
                        covered++;
                    }
                }
            }
            if (covered == (uint32_t)(n * n)) {
                graphics_put_pixel(&bar_graphics, x, y, color);
            } else if (covered) {
                graphics_blend_pixel(&bar_graphics, x, y, color, covered * 255u / (uint32_t)(n * n));
            }
        }
    }
}

static void draw_start_button(int pressed) {
    uint32_t alpha = pressed ? PRESS_ALPHA : (hovered == HOVER_START ? HOVER_ALPHA : 0u);
    if (alpha) {
        graphics_fill_rounded(&bar_graphics, START_X, BTN_Y, START_W, BTN_H, TILE_RADIUS,
                              OVERLAY_COLOR, alpha);
    }
    graphics_stroke_rounded(&bar_graphics, START_X, BTN_Y, START_W, BTN_H, TILE_RADIUS,
                            OVERLAY_COLOR, EDGE_ALPHA);

    uint32_t glyph = pressed ? OVERLAY_COLOR : ACCENT_COLOR;
    draw_ring(START_X + START_W / 2, BTN_Y + BTN_H / 2, START_RING_OUTER, START_RING_INNER, glyph);
}

static void draw_slot(const running_slot_t *slot, int hover) {
    uint32_t alpha = slot->focused ? FOCUS_ALPHA : (hover ? HOVER_ALPHA : 0u);
    if (alpha) {
        graphics_fill_rounded(&bar_graphics, slot->x, BTN_Y, slot->w, SLOT_H, TILE_RADIUS,
                              OVERLAY_COLOR, alpha);
    }
    if (slot->focused) {
        graphics_stroke_rounded(&bar_graphics, slot->x, BTN_Y, slot->w, SLOT_H, TILE_RADIUS,
                                OVERLAY_COLOR, EDGE_ALPHA);
    }

    int32_t icon_y = BTN_Y + (SLOT_H - SLOT_ICON) / 2;
    if (slot->minimized) {
        icon_draw_tinted(&bar_graphics, slot->x + 8, icon_y, slot->image, 1,
                         MINIMIZED_TINT, MINIMIZED_TINT_PCT);
    } else {
        icon_draw(&bar_graphics, slot->x + 8, icon_y, slot->image, 1);
    }

    int32_t text_y = BTN_Y + (SLOT_H - (int32_t)graphics_ui_font()->height) / 2;
    graphics_draw_text(&bar_graphics, slot->x + LABEL_PAD, text_y, slot->name,
                       slot->minimized ? LABEL_DIM_COLOR : LABEL_COLOR);

    if (slot->focused) {
        graphics_fill_rounded(&bar_graphics, slot->x + (slot->w - FOCUS_BAR_W) / 2,
                              BTN_Y + SLOT_H - FOCUS_BAR_H, FOCUS_BAR_W, FOCUS_BAR_H,
                              1, ACCENT_COLOR, 255);
    } else {
        graphics_fill_rounded(&bar_graphics, slot->x + (slot->w - RUNNING_DOT) / 2,
                              BTN_Y + SLOT_H - FOCUS_BAR_H, RUNNING_DOT, FOCUS_BAR_H,
                              1, OVERLAY_COLOR, slot->minimized ? 48u : 150u);
    }
}

static void draw_wifi(int32_t x) {
    int lit = wifi_bars_lit();
    int joining = wifi_status.state == WIRELESS_STATE_JOINING || wifi_status.state == WIRELESS_STATE_SECURING ||
                  wifi_status.state == WIRELESS_STATE_ADDRESSING;
    int32_t bottom = (PANEL_HEIGHT + WIFI_TALLEST) / 2;
    for (int i = 0; i < WIFI_BARS; i++) {
        int32_t height = 5 + i * 3;
        int32_t bx = x + i * (WIFI_BAR_W + WIFI_BAR_GAP);
        if (i < lit) {
            graphics_fill_rounded(&bar_graphics, bx, bottom - height, WIFI_BAR_W, height, 1, CLOCK_FG, 255);
        } else if (joining) {
            graphics_fill_rounded(&bar_graphics, bx, bottom - height, WIFI_BAR_W, height, 1, ACCENT_COLOR, 200);
        } else {
            graphics_fill_rounded(&bar_graphics, bx, bottom - height, WIFI_BAR_W, height, 1, WS_DOT_OFF,
                                  WS_DOT_OFF_ALPHA);
        }
    }
}

static void draw_tray(window_manager_window_t *self) {
    int32_t tray_x = (int32_t)self->width - tray_w();
    if (wifi_present()) {
        draw_wifi(tray_x + TRAY_PAD);
        tray_x += WIFI_W + TRAY_PAD;
    }

    int32_t dot_y = (PANEL_HEIGHT - WS_DOT_H) / 2;
    for (int i = 0; i < WINDOW_MANAGER_WORKSPACE_COUNT; i++) {
        int32_t dx = tray_x + TRAY_PAD + i * (WS_DOT_W + WS_DOT_GAP);
        if (i == shown_workspace) {
            graphics_fill_rounded(&bar_graphics, dx, dot_y, WS_DOT_W, WS_DOT_H, 2,
                                  ACCENT_COLOR, 255);
        } else {
            graphics_fill_rounded(&bar_graphics, dx, dot_y, WS_DOT_W, WS_DOT_H, 2,
                                  WS_DOT_OFF, WS_DOT_OFF_ALPHA);
        }
    }

    char clock_text[CLOCK_CHARS + 1];
    format_clock(sys_uptime_ms(), clock_text);
    graphics_draw_text(&bar_graphics, (int32_t)self->width - TRAY_PAD - clock_text_w(),
                  (PANEL_HEIGHT - (int32_t)graphics_ui_font()->height) / 2, clock_text, CLOCK_FG);
}

static int32_t context_needed_overhang(void) {
    if (context_slot < 0) {
        return 0;
    }
    return context_y < 0 ? -context_y : 0;
}

static const char *context_label(int i) {
    if (i == 0) {
        return running_slots[context_slot].minimized ? "Restore" : "Minimize";
    }
    return i == 1 ? "Close" : "Force Quit";
}

static void draw_context_menu(window_manager_window_t *self) {
    int32_t by = PANEL_OVERHANG_MAX + context_y;
    int32_t h = CONTEXT_ITEM_H * CONTEXT_COUNT;
    graphics_fill_rounded(&self->graphics, context_x, by, CONTEXT_W, h, CONTEXT_RADIUS,
                          CONTEXT_BG, 255u);
    graphics_stroke_rounded(&self->graphics, context_x, by, CONTEXT_W, h, CONTEXT_RADIUS,
                            CONTEXT_BORDER, CONTEXT_BORDER_ALPHA);
    for (int i = 0; i < CONTEXT_COUNT; i++) {
        int32_t ry = by + i * CONTEXT_ITEM_H;
        if (i == context_hover) {
            graphics_fill_rounded(&self->graphics, context_x + 4, ry + 2, CONTEXT_W - 8,
                                  CONTEXT_ITEM_H - 4, 6, CONTEXT_HOVER_BG, CONTEXT_HOVER_ALPHA);
        }
        graphics_draw_text(&self->graphics, context_x + 12,
                           ry + (CONTEXT_ITEM_H - (int32_t)graphics_ui_font()->height) / 2,
                           context_label(i), CONTEXT_TEXT);
    }
}

static int context_row_at(int32_t x, int32_t y) {
    if (context_slot < 0 ||
        !graphics_point_in_rect(x, y, context_x, context_y, CONTEXT_W, CONTEXT_ITEM_H * CONTEXT_COUNT)) {
        return -1;
    }
    return (y - context_y) / CONTEXT_ITEM_H;
}

static void bar_graphics_bind(const window_manager_window_t *self) {
    bar_graphics.pixels = self->graphics.pixels + (int32_t)self->width * PANEL_OVERHANG_MAX;
    bar_graphics.width = (int32_t)self->width;
    bar_graphics.height = PANEL_HEIGHT;
}

static void redraw(window_manager_window_t *self) {
    bar_graphics_bind(self);
    int32_t width = (int32_t)self->width;
    for (int32_t row = 0; row < PANEL_HEIGHT; row++) {
        uint32_t color = 0;
        for (int shift = 16; shift >= 0; shift -= 8) {
            int32_t top = (int32_t)((PANEL_TOP_COLOR >> shift) & 0xFFu);
            int32_t bottom = (int32_t)((PANEL_BOTTOM_COLOR >> shift) & 0xFFu);
            int32_t value = top + (bottom - top) * row / (PANEL_HEIGHT - 1);
            color |= (uint32_t)value << shift;
        }
        graphics_fill_rect(&bar_graphics, 0, row, width, 1, color);
    }
    for (int32_t col = 0; col < width; col++) {
        graphics_blend_pixel(&bar_graphics, col, 0, PANEL_SHADE, PANEL_SHADE_ALPHA);
        graphics_blend_pixel(&bar_graphics, col, 1, PANEL_HAIRLINE, PANEL_HAIRLINE_ALPHA);
    }

    draw_start_button(sys_uptime_ms() < start_pressed_until_ms);

    for (int i = 0; i < running_count; i++) {
        draw_slot(&running_slots[i], i == hovered);
    }

    draw_tray(self);

    graphics_fill_rect(&self->graphics, 0, 0, width, PANEL_OVERHANG_MAX, PANEL_BOTTOM_COLOR);
    if (context_slot >= 0) {
        draw_context_menu(self);
    }
}

static int button_at(int32_t x, int32_t y) {
    if (y < BTN_Y || y >= BTN_Y + BTN_H) {
        return HOVER_NONE;
    }
    if (graphics_point_in_rect(x, y, START_X, BTN_Y, START_W, BTN_H)) {
        return HOVER_START;
    }
    for (int i = 0; i < running_count; i++) {
        if (graphics_point_in_rect(x, y, running_slots[i].x, BTN_Y, running_slots[i].w, SLOT_H)) {
            return i;
        }
    }
    return HOVER_NONE;
}

static void context_open_on(int slot, int32_t x, int32_t win_w) {
    context_slot = slot;
    context_hover = -1;
    context_x = x;
    if (context_x > win_w - CONTEXT_W) {
        context_x = win_w - CONTEXT_W;
    }
    if (context_x < 0) {
        context_x = 0;
    }
    context_y = -(CONTEXT_ITEM_H * CONTEXT_COUNT);
}

static void context_close(void) {
    context_slot = -1;
    context_hover = -1;
}

static void context_activate(int row) {
    if (context_slot < 0 || context_slot >= running_count) {
        context_close();
        return;
    }
    int32_t window_id = running_slots[context_slot].window_id;
    context_close();
    if (row == 0) {
        window_manager_send_action(window_id, WINDOW_MANAGER_ACTION_TOGGLE_MINIMIZE);
    } else if (row == 1) {
        window_manager_send_action(window_id, WINDOW_MANAGER_ACTION_CLOSE);
    } else if (row == 2) {
        window_manager_send_action(window_id, WINDOW_MANAGER_ACTION_KILL);
    }
}

static int wifi_hit(int32_t x, int32_t y, int32_t width) {
    if (!wifi_present()) {
        return 0;
    }
    int32_t left = width - tray_w() + TRAY_PAD / 2;
    return x >= left && x < left + WIFI_W + TRAY_PAD && y >= 0 && y < PANEL_HEIGHT;
}

/* The wizard, or the one already open - two windows choosing networks for
   one radio would only argue. */
static void open_wifi(void) {
    for (int i = 0; i < running_count; i++) {
        if (strcmp(running_slots[i].name, "Wi-Fi") == 0) {
            window_manager_send_action(running_slots[i].window_id, WINDOW_MANAGER_ACTION_FOCUS);
            return;
        }
    }
    sys_spawn("/bin/wifi", 0);
}

static void handle_click(int32_t x, int32_t y, int32_t width) {
    if (wifi_hit(x, y, width)) {
        open_wifi();
        return;
    }
    int hit = button_at(x, y);
    if (hit == HOVER_START) {
        start_pressed_until_ms = sys_uptime_ms() + PRESS_FLASH_MS;
        window_manager_toggle_launcher();
        return;
    }
    if (hit >= 0) {
        const running_slot_t *slot = &running_slots[hit];
        window_manager_send_action(slot->window_id, slot->focused ? WINDOW_MANAGER_ACTION_TOGGLE_MINIMIZE : WINDOW_MANAGER_ACTION_FOCUS);
    }
}

int main(void) {
    window_manager_window_t win;
    if (window_manager_connect_panel(PANEL_BUFFER_H, PANEL_HEIGHT, &win) != 0) {
        const char message[] = "desktop_shell: no window from the compositor - exiting so init restarts the session\n";
        sys_write(1, message, sizeof(message) - 1);
        sys_exit(1);
    }
    bar_graphics_bind(&win);

    refresh_running_slots(&win);
    redraw(&win);
    window_manager_present(&win);

    long next_refresh = 0;
    for (;;) {
        window_manager_event_t ev;
        int changed = 0;
        while (window_manager_poll_event(&win, &ev)) {
            if (ev.type == WINDOW_MANAGER_EVENT_EXPOSE || ev.type == WINDOW_MANAGER_EVENT_DISPLAY_CHANGED) {
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && (ev.buttons & 2)) {
                int hit = button_at(ev.x, ev.y);
                if (hit >= 0) {
                    context_open_on(hit, running_slots[hit].x, (int32_t)win.width);
                } else {
                    context_close();
                }
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && (ev.buttons & 1) && context_slot >= 0) {
                int row = context_row_at(ev.x, ev.y);
                if (row >= 0) {
                    context_activate(row);
                } else {
                    context_close();
                }
                refresh_running_slots(&win);
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                handle_click(ev.x, ev.y, (int32_t)win.width);
                refresh_running_slots(&win);
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_MOVE) {
                if (context_slot >= 0) {
                    int row = context_row_at(ev.x, ev.y);
                    if (row != context_hover) {
                        context_hover = row;
                        changed = 1;
                    }
                }
                int now_over = button_at(ev.x, ev.y);
                if (now_over != hovered) {
                    hovered = now_over;
                    changed = 1;
                }
            }
        }

        long now = sys_uptime_ms();
        if (now >= next_refresh) {
            if (sys_wireless(WIRELESS_OPERATION_STATUS, &wifi_status, 0) != 0) {
                wifi_status.state = WIRELESS_STATE_ABSENT;
            }
            refresh_running_slots(&win);
            next_refresh = now + REFRESH_INTERVAL_MS;
            uint32_t signature = panel_signature();
            if (signature != last_signature) {
                last_signature = signature;
                changed = 1;
            }
        }
        static int was_pressed;
        int pressed = now < start_pressed_until_ms;
        if (pressed != was_pressed) {
            was_pressed = pressed;
            changed = 1;
        }
        if (changed) {
            redraw(&win);
            window_manager_present(&win);
        }
        int32_t want = context_needed_overhang();
        if (want != context_overhang) {
            context_overhang = want;
            window_manager_set_panel_overhang(win.window_id, want);
        }
        window_manager_wait_ms(&win, NULL, 0, (pressed || context_overhang) ? 50 : (int)(next_refresh - now));
    }
}
