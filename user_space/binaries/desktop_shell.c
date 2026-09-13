#include "syscall_wrappers.h"
#include "wmclient.h"

#define PANEL_HEIGHT      32

#define PANEL_OVERHANG_MAX 96
#define PANEL_BUF_H       (PANEL_OVERHANG_MAX + PANEL_HEIGHT)

#define CTX_W        124
#define CTX_ITEM_H   22
#define CTX_COUNT    3
#define CTX_BG       0x00243040u
#define CTX_HOVER_BG 0x003A5A80u
#define CTX_BORDER   0x00506070u
#define CTX_TEXT     0x00FFFFFFu
#define BTN_H             24
#define BTN_Y             4
#define EDGE_PAD          4

#define START_X       EDGE_PAD
#define START_W       72
#define START_GLYPH_X (START_X + 8)
#define START_TILE    5
#define START_TILE_GAP 2
#define START_TEXT_X  (START_X + 24)

#define SLOT_W            96
#define SLOT_H            BTN_H
#define SLOT_GAP          4
#define SLOTS_X           (START_X + START_W + 8)
#define LABEL_PAD         6
#define LABEL_MAX         10
#define MAX_RUNNING_SLOTS  WM_MAX_ROUTABLE_WINDOWS

#define CLOCK_CHARS   5
#define TRAY_PAD      10
#define WS_DOT      8
#define WS_DOT_GAP  5
#define WS_DOT_ON   0x004C99E6u
#define WS_DOT_OFF  0x00506070u
#define TRAY_ICONS_W  (TRAY_PAD + WM_WORKSPACE_COUNT * WS_DOT + (WM_WORKSPACE_COUNT - 1) * WS_DOT_GAP + TRAY_PAD)

static int32_t clock_text_w(void) {
    return gfx_text_width(gfx_ui_font(), "00:00");
}

static int32_t tray_w(void) {
    return TRAY_ICONS_W + clock_text_w() + TRAY_PAD;
}

#define REFRESH_INTERVAL_MS 300
#define PRESS_FLASH_MS      150

#define PANEL_BG            0x00181828u
#define PANEL_BORDER_COLOR  0x00445566u
#define PANEL_BEVEL_COLOR   0x00223349u
#define SLOT_BORDER_COLOR   0x00445566u
#define RUNNING_SLOT_BG            0x00263447u
#define RUNNING_SLOT_HOVER_BG      0x00365070u
#define RUNNING_SLOT_FOCUS_BG      0x002E4A63u
#define RUNNING_SLOT_FOCUS_BORDER  0x004C99E6u
#define RUNNING_SLOT_MIN_BG        0x00352A20u
#define RUNNING_SLOT_FRONT_BORDER  0x002E5C86u
#define LABEL_COLOR         0x00FFFFFFu

#define START_BG        0x00243447u
#define START_HOVER_BG  0x00365070u
#define START_PRESS_BG  0x004C99E6u
#define START_GLYPH_FG  0x004C99E6u
#define START_PRESS_GLYPH_FG 0x00FFFFFFu
#define TRAY_SEP_COLOR  0x00303C4Eu
#define CLOCK_FG        0x00C8D4E4u

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
    char name[LABEL_MAX + 1];
} running_slot_t;

static running_slot_t running_slots[MAX_RUNNING_SLOTS];
static int running_count;
static int hovered = HOVER_NONE;
static long start_pressed_until_ms;

static int ctx_slot = -1;
static int32_t ctx_x, ctx_y;
static int ctx_hover = -1;
static int32_t ctx_overhang;

static gfx_ctx_t bar_gfx;

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

static void copy_label(char *dst, const char *src) {
    int i = 0;
    if (src && src[0]) {
        for (; i < LABEL_MAX && src[i]; i++) {
            dst[i] = src[i];
        }
    } else {
        static const char fallback[] = "App";
        for (; fallback[i]; i++) {
            dst[i] = fallback[i];
        }
    }
    dst[i] = '\0';
}

static void refresh_running_slots(wm_window_t *self) {
    wm_query_response_t q;
    if (wm_query_windows(&q) != 0) {
        running_count = 0;
        return;
    }
    shown_workspace = q.current_workspace;
    int32_t boundary = (int32_t)self->width - tray_w();
    int32_t x = SLOTS_X;
    running_count = 0;
    for (int32_t i = 0; i < q.count && running_count < MAX_RUNNING_SLOTS; i++) {
        const wm_window_info_t *info = &q.windows[i];
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
        copy_label(slot->name, info->title);
        x += SLOT_W + SLOT_GAP;
        running_count++;
    }
    for (int i = 0; i < running_count; i++) {
        running_slot_t *slot = &running_slots[i];
        if (slot->window_id != slot->reported_id || slot->x != slot->reported_x) {
            wm_set_taskbar_slot(slot->window_id, slot->x, slot->w);
            slot->reported_id = slot->window_id;
            slot->reported_x = slot->x;
        }
    }

    {
        int front = -1;
        int32_t best_z = -1;
        for (int32_t i = 0, k = 0; i < q.count && k < running_count; i++) {
            const wm_window_info_t *info = &q.windows[i];
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

static void draw_start_button(int pressed) {
    uint32_t bg = pressed ? START_PRESS_BG
                          : (hovered == HOVER_START ? START_HOVER_BG : START_BG);
    uint32_t glyph = pressed ? START_PRESS_GLYPH_FG : START_GLYPH_FG;
    gfx_fill_rect_rounded(&bar_gfx, START_X, BTN_Y, START_W, BTN_H, bg);
    gfx_draw_rect_rounded(&bar_gfx, START_X, BTN_Y, START_W, BTN_H, SLOT_BORDER_COLOR);

    int32_t gy = BTN_Y + (BTN_H - (2 * START_TILE + START_TILE_GAP)) / 2;
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 2; col++) {
            gfx_fill_rect(&bar_gfx,
                          START_GLYPH_X + col * (START_TILE + START_TILE_GAP),
                          gy + row * (START_TILE + START_TILE_GAP),
                          START_TILE, START_TILE, glyph);
        }
    }
    gfx_draw_text(&bar_gfx, START_TEXT_X, BTN_Y + (BTN_H - (int32_t)gfx_ui_font()->height) / 2, "Start", LABEL_COLOR);
}

static void draw_tray(wm_window_t *self) {
    int32_t tray_x = (int32_t)self->width - tray_w();
    gfx_draw_line(&bar_gfx, tray_x, BTN_Y + 2, tray_x, BTN_Y + BTN_H - 3, TRAY_SEP_COLOR);

    int32_t dot_y = (PANEL_HEIGHT - WS_DOT) / 2;
    for (int i = 0; i < WM_WORKSPACE_COUNT; i++) {
        int32_t dx = tray_x + TRAY_PAD + i * (WS_DOT + WS_DOT_GAP);
        if (i == shown_workspace) {
            gfx_fill_rect(&bar_gfx, dx, dot_y, WS_DOT, WS_DOT, WS_DOT_ON);
        } else {
            gfx_draw_rect(&bar_gfx, dx, dot_y, WS_DOT, WS_DOT, WS_DOT_OFF);
        }
    }

    char clock_text[CLOCK_CHARS + 1];
    format_clock(sys_uptime_ms(), clock_text);
    gfx_draw_text(&bar_gfx, (int32_t)self->width - TRAY_PAD - clock_text_w(),
                  (PANEL_HEIGHT - (int32_t)gfx_ui_font()->height) / 2, clock_text, CLOCK_FG);
}

static int32_t ctx_needed_overhang(void) {
    if (ctx_slot < 0) {
        return 0;
    }
    return ctx_y < 0 ? -ctx_y : 0;
}

static const char *ctx_label(int i) {
    if (i == 0) {
        return running_slots[ctx_slot].minimized ? "Restore" : "Minimize";
    }
    return i == 1 ? "Close" : "Force Quit";
}

static void draw_ctx_menu(wm_window_t *self) {
    int32_t by = PANEL_OVERHANG_MAX + ctx_y;
    int32_t h = CTX_ITEM_H * CTX_COUNT;
    gfx_fill_rect_rounded(&self->gfx, ctx_x, by, CTX_W, h, CTX_BG);
    gfx_draw_rect_rounded(&self->gfx, ctx_x, by, CTX_W, h, CTX_BORDER);
    for (int i = 0; i < CTX_COUNT; i++) {
        int32_t ry = by + i * CTX_ITEM_H;
        if (i == ctx_hover) {
            gfx_fill_rect_rounded(&self->gfx, ctx_x + 2, ry + 1, CTX_W - 4, CTX_ITEM_H - 2, CTX_HOVER_BG);
        }
        gfx_draw_text(&self->gfx, ctx_x + 8, ry + (CTX_ITEM_H - (int32_t)gfx_ui_font()->height) / 2, ctx_label(i), CTX_TEXT);
    }
}

static int ctx_row_at(int32_t x, int32_t y) {
    if (ctx_slot < 0 ||
        !gfx_point_in_rect(x, y, ctx_x, ctx_y, CTX_W, CTX_ITEM_H * CTX_COUNT)) {
        return -1;
    }
    return (y - ctx_y) / CTX_ITEM_H;
}

static void bar_gfx_bind(const wm_window_t *self) {
    bar_gfx.pixels = self->gfx.pixels + (int32_t)self->width * PANEL_OVERHANG_MAX;
    bar_gfx.width = (int32_t)self->width;
    bar_gfx.height = PANEL_HEIGHT;
}

static void redraw(wm_window_t *self) {
    bar_gfx_bind(self);
    gfx_fill_rect(&bar_gfx, 0, 0, (int32_t)self->width, PANEL_HEIGHT, PANEL_BG);
    gfx_draw_line(&bar_gfx, 0, 0, (int32_t)self->width - 1, 0, PANEL_BORDER_COLOR);
    gfx_draw_line(&bar_gfx, 0, 1, (int32_t)self->width - 1, 1, PANEL_BEVEL_COLOR);

    draw_start_button(sys_uptime_ms() < start_pressed_until_ms);

    for (int i = 0; i < running_count; i++) {
        const running_slot_t *slot = &running_slots[i];
        uint32_t bg = slot->minimized ? RUNNING_SLOT_MIN_BG
                                      : (slot->focused ? RUNNING_SLOT_FOCUS_BG
                                                       : (i == hovered ? RUNNING_SLOT_HOVER_BG : RUNNING_SLOT_BG));
        uint32_t border = slot->focused ? RUNNING_SLOT_FOCUS_BORDER
                                        : (slot->frontmost ? RUNNING_SLOT_FRONT_BORDER : SLOT_BORDER_COLOR);
        gfx_fill_rect_rounded(&bar_gfx, slot->x, BTN_Y, slot->w, SLOT_H, bg);
        gfx_draw_rect_rounded(&bar_gfx, slot->x, BTN_Y, slot->w, SLOT_H, border);
        gfx_draw_text(&bar_gfx, slot->x + LABEL_PAD, BTN_Y + 4, slot->name, LABEL_COLOR);
    }

    draw_tray(self);

    gfx_fill_rect(&self->gfx, 0, 0, (int32_t)self->width, PANEL_OVERHANG_MAX, PANEL_BG);
    if (ctx_slot >= 0) {
        draw_ctx_menu(self);
    }
}

static int button_at(int32_t x, int32_t y) {
    if (y < BTN_Y || y >= BTN_Y + BTN_H) {
        return HOVER_NONE;
    }
    if (gfx_point_in_rect(x, y, START_X, BTN_Y, START_W, BTN_H)) {
        return HOVER_START;
    }
    for (int i = 0; i < running_count; i++) {
        if (gfx_point_in_rect(x, y, running_slots[i].x, BTN_Y, running_slots[i].w, SLOT_H)) {
            return i;
        }
    }
    return HOVER_NONE;
}

static void ctx_open_on(int slot, int32_t x, int32_t win_w) {
    ctx_slot = slot;
    ctx_hover = -1;
    ctx_x = x;
    if (ctx_x > win_w - CTX_W) {
        ctx_x = win_w - CTX_W;
    }
    if (ctx_x < 0) {
        ctx_x = 0;
    }
    ctx_y = -(CTX_ITEM_H * CTX_COUNT);
}

static void ctx_close(void) {
    ctx_slot = -1;
    ctx_hover = -1;
}

static void ctx_activate(int row) {
    if (ctx_slot < 0 || ctx_slot >= running_count) {
        ctx_close();
        return;
    }
    int32_t window_id = running_slots[ctx_slot].window_id;
    ctx_close();
    if (row == 0) {
        wm_send_action(window_id, WM_ACTION_TOGGLE_MINIMIZE);
    } else if (row == 1) {
        wm_send_action(window_id, WM_ACTION_CLOSE);
    } else if (row == 2) {
        wm_send_action(window_id, WM_ACTION_KILL);
    }
}

static void handle_click(int32_t x, int32_t y) {
    int hit = button_at(x, y);
    if (hit == HOVER_START) {
        start_pressed_until_ms = sys_uptime_ms() + PRESS_FLASH_MS;
        wm_toggle_launcher();
        return;
    }
    if (hit >= 0) {
        const running_slot_t *slot = &running_slots[hit];
        wm_send_action(slot->window_id, slot->focused ? WM_ACTION_TOGGLE_MINIMIZE : WM_ACTION_FOCUS);
    }
}

int main(void) {
    wm_window_t win;
    if (wm_connect_panel(PANEL_BUF_H, PANEL_HEIGHT, &win) != 0) {
        const char msg[] = "desktop_shell: no window from the compositor - exiting so init restarts the session\n";
        sys_write(1, msg, sizeof(msg) - 1);
        sys_exit(1);
    }
    bar_gfx_bind(&win);

    refresh_running_slots(&win);
    redraw(&win);
    wm_present(&win);

    long next_refresh = 0;
    for (;;) {
        wm_event_t ev;
        int changed = 0;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 2)) {
                int hit = button_at(ev.x, ev.y);
                if (hit >= 0) {
                    ctx_open_on(hit, running_slots[hit].x, (int32_t)win.width);
                } else {
                    ctx_close();
                }
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1) && ctx_slot >= 0) {
                int row = ctx_row_at(ev.x, ev.y);
                if (row >= 0) {
                    ctx_activate(row);
                } else {
                    ctx_close();
                }
                refresh_running_slots(&win);
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                handle_click(ev.x, ev.y);
                refresh_running_slots(&win);
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_MOVE) {
                if (ctx_slot >= 0) {
                    int row = ctx_row_at(ev.x, ev.y);
                    if (row != ctx_hover) {
                        ctx_hover = row;
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
            refresh_running_slots(&win);
            next_refresh = now + REFRESH_INTERVAL_MS;
            changed = 1;
        }
        static int was_pressed;
        int pressed = now < start_pressed_until_ms;
        if (pressed != was_pressed) {
            was_pressed = pressed;
            changed = 1;
        }
        if (changed) {
            redraw(&win);
            wm_present(&win);
        }
        int32_t want = ctx_needed_overhang();
        if (want != ctx_overhang) {
            ctx_overhang = want;
            wm_set_panel_overhang(win.window_id, want);
        }
        wm_wait_ms(&win, NULL, 0, (pressed || ctx_overhang) ? 50 : (int)(next_refresh - now));
    }
}
