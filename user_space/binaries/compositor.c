#include "children.h"
#include "paths.h"
#include "user_interface_font.h"

#define UI_FONT        ui_font_ui
#define UI_FONT_HEIGHT UI_FONT_UI_HEIGHT
#include "graphics.h"
#include "power_mode.h"
#include "spawn_error.h"
#include "settings_file.h"
#include "shortcuts.h"
#include "signal.h"
#include <stdlib.h>

#include "recent.h"
#include "string_utilities.h"
#include "syscall_wrappers.h"
#include "window_manager.h"

#define MAX_WINDOWS          WINDOW_MANAGER_MAX_ROUTABLE_WINDOWS
#define TITLEBAR_H           28
#define BORDER               1
#define FRAME_RADIUS         8
#define DEFAULT_BG_COLOR     0x001A1A2Eu
#define BORDER_COLOR         0x00000000u
#define BORDER_ALPHA         150u
#define FRAME_HAIRLINE       0x00FFFFFFu
#define FRAME_HAIRLINE_ALPHA 26u
#define TITLEBAR_TOP_COLOR       0x002A3140u
#define TITLEBAR_BOTTOM_COLOR    0x001E2430u
#define TITLEBAR_DIM_TOP_COLOR   0x00212733u
#define TITLEBAR_DIM_BOTTOM_COLOR 0x001A1F29u
#define TITLEBAR_FOCUS_COLOR 0x004C99E6u
#define TITLEBAR_ACCENT_ALPHA 255u
#define CURSOR_COLOR         0x00FFFFFFu
#define CURSOR_SIZE          8

#define BTN_SIZE   12
#define BTN_GAP    8
#define BTN_MARGIN 10
#define BTN_CLOSE_COLOR    0x00FF5F57u
#define BTN_MAXIMIZE_COLOR 0x0028C840u
#define BTN_MINIMIZE_COLOR 0x00FEBC2Eu
#define BTN_IDLE_COLOR     0x005A6270u
#define BTN_GLYPH_COLOR 0x00202020u
#define BTN_GLYPH_INSET 3
#define BTN_HOVER_LIGHTEN 5

#define TITLE_COLOR 0x00EDF1F7u
#define TITLE_DIM_COLOR 0x008B95A5u
#define TITLE_MARGIN 10
#define TITLE_BTN_GAP 10

#define SHADOW_RINGS   8
#define SHADOW_DROP    5
#define SHADOW_PEAK        52u
#define SHADOW_FOCUS_PEAK  84u

#define SNAP_PREVIEW_NUMBER 1
#define SNAP_PREVIEW_DEN 4
#define SNAP_FADE_DEN (SNAP_PREVIEW_DEN * 4)

#define TRANSLUCENT_NUMBER 3
#define TRANSLUCENT_DEN 4
#define LAUNCHER_OPACITY_NUMBER 4
#define LAUNCHER_OPACITY_DEN 5

#define LAUNCHER_W 480
#define LAUNCHER_H 320
#define LAUNCHER_PAD      GRAPHICS_PAD
#define LAUNCHER_INPUT_H  (UI_FONT_HEIGHT + 8)
#define LAUNCHER_ROW_H    20
#define LAUNCHER_LIST_Y   (LAUNCHER_PAD + LAUNCHER_INPUT_H + 10)
#define LAUNCHER_ROWS     10
#define LAUNCHER_MAX_ENTRIES 128
#define LAUNCHER_NAME_MAX 32
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#define LAUNCHER_QUERY_MAX 24
#define LAUNCHER_LIST_BUFFER 2048

#define LAUNCHER_BG      0x001C2233u
#define LAUNCHER_BORDER  0x005B6B85u
#define LAUNCHER_INPUT_BG 0x00131A27u
#define LAUNCHER_TEXT    0x00FFFFFFu
#define LAUNCHER_HINT    0x006C8098u
#define LAUNCHER_SEL_BG  0x00335577u
#define LAUNCHER_ROW_FG  0x00C8D4E4u

#define POWER_BTN_W   96
#define POWER_BTN_H   22
#define POWER_BTN_GAP 8
#define POWER_BTN_Y   (LAUNCHER_H - LAUNCHER_PAD - POWER_BTN_H)
#define POWER_OFF_X   (LAUNCHER_W - LAUNCHER_PAD - 2 * POWER_BTN_W - POWER_BTN_GAP)
#define POWER_REBOOT_X (LAUNCHER_W - LAUNCHER_PAD - POWER_BTN_W)
#define POWER_BTN_BG      0x002B3548u
#define POWER_BTN_HOVER   0x004C6699u
#define POWER_CONFIRM_W   300
#define POWER_CONFIRM_H   96
#define POWER_CONFIRM_BG  0x00202838u

#define POWER_CONFIRM_NONE (-1)

#define WMENU_W        124
#define WMENU_ITEM_H   22
#define WMENU_COUNT    3
#define WMENU_BG       0x001E2430u
#define WMENU_HOVER_BG 0x003A5A80u
#define WMENU_BORDER   0x00495568u
#define WMENU_TEXT     0x00FFFFFFu

#define TOAST_MAX      4
#define TOAST_W        300
#define TOAST_H        56
#define TOAST_GAP      8
#define TOAST_MARGIN   12
#define TOAST_STRIPE_W 4
#define TOAST_TTL_MS   4000
#define TOAST_BG       0x001E2430u
#define TOAST_BORDER   0x00495568u
#define TOAST_TITLE_FG 0x00FFFFFFu
#define TOAST_BODY_FG  0x00B4C0D0u
#define TOAST_INFO_C   0x004C99E6u
#define TOAST_WARN_C   0x00E0A33Cu
#define TOAST_ERROR_C  0x00E05C55u

#define DRAG_LABEL_H      20
#define DRAG_LABEL_PAD    6
#define DRAG_LABEL_BG     0x00335577u
#define DRAG_LABEL_BORDER 0x004C99E6u
#define DRAG_LABEL_FG     0x00FFFFFFu

#define RESIZE_MARGIN 5
#define RESIZE_LEFT   1
#define RESIZE_RIGHT  2
#define RESIZE_TOP    4
#define RESIZE_BOTTOM 8

typedef struct {
    int32_t x, y, w, h;
    int32_t buffer_w, buffer_h;
    uint32_t *pixels;
    int32_t shared_memory_id;
    int evt_write_file_descriptor;
    uint8_t is_panel;
    int32_t buffer_y0;
    int32_t overhang;
    uint8_t translucent;
    uint8_t is_desktop;
    uint8_t minimized;
    uint8_t maximized;
    int32_t saved_x, saved_y, saved_w, saved_h;
    uint8_t needs_rebuffer;
    int8_t workspace;
    uint8_t alive;
    uint8_t close_requested;
    uint8_t confirm_close;
    long close_deadline_ms;
    int32_t client_pid;
    uint8_t is_popup;
    int32_t parent_window;
    char title[WINDOW_MANAGER_TITLE_MAX];
} window_t;

static window_t windows[MAX_WINDOWS];
static int window_count;

static int focused_window = -1;
static uint32_t bg_color = DEFAULT_BG_COLOR;
static uint32_t accent_color = TITLEBAR_FOCUS_COLOR;
static uint32_t wallpaper_id = 1;

static int point_in_window(const window_t *win, int32_t x, int32_t y) {
    int32_t top = (win->is_panel || win->is_desktop || win->is_popup) ? win->y - win->overhang : win->y - TITLEBAR_H;
    return x >= win->x && x < win->x + win->w && y >= top && y < win->y + win->h;
}

static int point_in_titlebar(const window_t *win, int32_t x, int32_t y) {
    return x >= win->x && x < win->x + win->w &&
           y >= win->y - TITLEBAR_H && y < win->y;
}

static int resize_hit_mask(const window_t *win, int32_t px, int32_t py) {
    int32_t x0 = win->x - BORDER;
    int32_t y0 = win->y - TITLEBAR_H - BORDER;
    int32_t x1 = win->x + win->w + BORDER;
    int32_t y1 = win->y + win->h + BORDER;
    int within_x = px >= x0 - RESIZE_MARGIN && px < x1 + RESIZE_MARGIN;
    int within_y = py >= y0 - RESIZE_MARGIN && py < y1 + RESIZE_MARGIN;
    int mask = 0;
    if (within_y) {
        if (px >= x0 - RESIZE_MARGIN && px < x0 + RESIZE_MARGIN) {
            mask |= RESIZE_LEFT;
        }
        if (px >= x1 - RESIZE_MARGIN && px < x1 + RESIZE_MARGIN) {
            mask |= RESIZE_RIGHT;
        }
    }
    if (within_x) {
        if (py >= y0 - RESIZE_MARGIN && py < y0 + RESIZE_MARGIN) {
            mask |= RESIZE_TOP;
        }
        if (py >= y1 - RESIZE_MARGIN && py < y1 + RESIZE_MARGIN) {
            mask |= RESIZE_BOTTOM;
        }
    }
    return mask;
}

static window_manager_framebuffer_info_t framebuffer_info;
static window_manager_framebuffer_info_t physical_info;
static uint32_t display_scale = 1;
static uint32_t *real_framebuffer;
static uint32_t framebuffer_pitch_pixels;

/* Everything here - windows, the cursor, the back buffer - is in DESKTOP
   pixels, the size the kernel tells every program the screen is. The real
   framebuffer is display_scale times that along each axis, and present() is
   the one place the two meet. */
static int read_display_modes(void) {
    if (sys_framebuffer_info(&framebuffer_info) != 0 ||
        sys_framebuffer_info_physical(&physical_info) != 0 ||
        framebuffer_info.width == 0 || framebuffer_info.height == 0) {
        return -1;
    }
    display_scale = physical_info.width / framebuffer_info.width;
    if (display_scale < 1 || display_scale > 2 ||
        framebuffer_info.height * display_scale > physical_info.height) {
        display_scale = 1;
        framebuffer_info = physical_info;
    }
    framebuffer_pitch_pixels = physical_info.pitch / (uint32_t)sizeof(uint32_t);
    return 0;
}

static uint32_t *back_buffer;
static uint32_t back_pitch_pixels;
static long back_shared_memory_id = -1;

static uint32_t mode_previous_w, mode_previous_h;
static long mode_revert_at_ms;

#define ANIM_MS         140
#define FRAME_MS        16
#define FRAME_BUDGET_MS 16
#define ANIM_MAX        6
#define ANIM_MIN_FRAMES 5

typedef enum {
    ANIM_NONE = 0,
    ANIM_MINIMIZE,
    ANIM_RESTORE,
    ANIM_OPEN,
    ANIM_CLOSE,
} anim_kind_t;

typedef struct {
    uint8_t kind;
    long start_ms;
    uint16_t frames;
    int32_t fx, fy, fw, fh;
    int32_t tx, ty, tw, th;
    int32_t lx, ly, lw, lh;
    uint8_t drawn;
} anim_t;

static anim_t anims[ANIM_MAX];
static int animations_enabled = 1;
static uint32_t audio_volume = 70;
static long frame_due_ms;
static int frame_settle_owed;
static uint32_t frames_over_budget;
static uint32_t frames_drawn;
static long worst_frame_ms;
static long worst_gap_ms;
static long last_frame_ms;
static long anim_run_start_ms;

static int32_t slot_x[MAX_WINDOWS];
static int32_t slot_w[MAX_WINDOWS];

static int current_workspace;

static int window_here(const window_t *win) {
    return win->workspace < 0 || win->workspace == current_workspace;
}

static long launcher_fade_start_ms;
static long snap_fade_start_ms;

static int launcher_fading(void);
static int32_t launcher_opacity_number(void);
static int snap_fading(void);
static int32_t snap_preview_number(void);

static int32_t ease_out(int32_t t_permille) {
    if (t_permille <= 0) {
        return 0;
    }
    if (t_permille >= 1000) {
        return 1000;
    }
    return (2000 * t_permille - t_permille * t_permille) / 1000;
}

static int32_t lerp(int32_t from, int32_t to, int32_t p) {
    return from + (to - from) * p / 1000;
}

static int32_t anim_min_frames(const anim_t *a) {
    return (a->kind == ANIM_MINIMIZE || a->kind == ANIM_RESTORE) ? ANIM_MIN_FRAMES : 1;
}

static int anim_rect_now(anim_t *a, long now, int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
    long elapsed = now - a->start_ms;
    int32_t by_clock = elapsed <= 0 ? 0 : (int32_t)(elapsed * 1000 / ANIM_MS);
    int32_t by_frames = (int32_t)a->frames * 1000 / anim_min_frames(a);
    int32_t t = by_clock < by_frames ? by_clock : by_frames;
    if (t >= 1000) {
        return 0;
    }
    int32_t p = ease_out(t);
    *x = lerp(a->fx, a->tx, p);
    *y = lerp(a->fy, a->ty, p);
    *w = lerp(a->fw, a->tw, p);
    *h = lerp(a->fh, a->th, p);
    return 1;
}

static void anim_window_minimize(int idx);
static void anim_window_restore(int idx);
static void anim_window_open(int idx);
static void anim_window_close(int idx);

static int32_t self_pid;

static int32_t cursor_x, cursor_y;
static uint8_t previous_buttons;
static int last_hovered_panel = -1;
static int32_t last_drawn_cursor_x, last_drawn_cursor_y;

static int dirty = 1;
static int present_pending;
static int32_t present_x0, present_y0, present_x1, present_y1;

static int zorder[MAX_WINDOWS];
static int z_count;

#define ZBAND_DESKTOP  0
#define ZBAND_ORDINARY 1
#define ZBAND_POPUP    2
#define ZBAND_PANEL    3

static int window_band(const window_t *win) {
    if (win->is_desktop) {
        return ZBAND_DESKTOP;
    }
    if (win->is_panel) {
        return ZBAND_PANEL;
    }
    if (win->is_popup) {
        return ZBAND_POPUP;
    }
    return ZBAND_ORDINARY;
}

static int z_position_of(int idx) {
    for (int z = 0; z < z_count; z++) {
        if (zorder[z] == idx) {
            return z;
        }
    }
    return -1;
}

static void z_remove(int idx) {
    int z = z_position_of(idx);
    if (z < 0) {
        return;
    }
    for (int k = z; k + 1 < z_count; k++) {
        zorder[k] = zorder[k + 1];
    }
    z_count--;
}

static void z_insert_top_of_band(int idx) {
    z_remove(idx);
    int band = window_band(&windows[idx]);
    int at = z_count;
    for (int z = 0; z < z_count; z++) {
        if (window_band(&windows[zorder[z]]) > band) {
            at = z;
            break;
        }
    }
    for (int k = z_count; k > at; k--) {
        zorder[k] = zorder[k - 1];
    }
    zorder[at] = idx;
    z_count++;
}

static void z_raise(int idx) {
    if (z_count > 0 && zorder[z_count - 1] == idx) {
        return;
    }
    int before = z_position_of(idx);
    z_insert_top_of_band(idx);
    if (z_position_of(idx) != before) {
        dirty = 1;
    }
}
static int launcher_open;
static char launcher_entries[LAUNCHER_MAX_ENTRIES][LAUNCHER_NAME_MAX];
static int launcher_entry_count;
static char launcher_recent_path[RECENT_MAX][PATH_MAX_LENGTH];
static int launcher_recent_count;
static int launcher_matches[LAUNCHER_MAX_ENTRIES];
static int launcher_match_count;
static int launcher_selected;
static int launcher_scroll;
static char launcher_query[LAUNCHER_QUERY_MAX];
static int launcher_query_length;
static int power_confirm = POWER_CONFIRM_NONE;
static int shutdown_pending_mode = POWER_CONFIRM_NONE;
static long shutdown_deadline_ms;
static int shutdown_vetoed_by = -1;
#define SHUTDOWN_QUERY_MS 600
static int power_hover = POWER_CONFIRM_NONE;

typedef struct {
    uint32_t level;
    char title[WINDOW_MANAGER_NOTIFY_TITLE_MAX];
    char body[WINDOW_MANAGER_NOTIFY_BODY_MAX];
    long expires_ms;
} toast_t;

static toast_t toasts[TOAST_MAX];
static int toast_count;

static int drag_data_write_file_descriptor = -1;

static int client_drag_active;
static char client_drag_payload[WINDOW_MANAGER_DRAG_PAYLOAD_MAX];
static int client_drag_last_target = -1;

static int wmenu_window = -1;
static int32_t wmenu_x, wmenu_y;
static int wmenu_hover = -1;

static int snap_preview_active;
static int32_t snap_preview_x, snap_preview_y, snap_preview_w, snap_preview_h;

static int32_t clip_x0, clip_y0, clip_x1, clip_y1;

static inline int32_t min_i32(int32_t a, int32_t b) {
    return a < b ? a : b;
}

static inline int32_t max_i32(int32_t a, int32_t b) {
    return a > b ? a : b;
}

static const uint8_t cursor_shape[CURSOR_SIZE] = {
    0b10000000,
    0b11000000,
    0b10100000,
    0b10010000,
    0b10001000,
    0b10111000,
    0b11000100,
    0b10000100,
};

static const uint8_t cursor_shape_horizontal[CURSOR_SIZE] = {
    0b00011000,
    0b00111100,
    0b01100110,
    0b11000011,
    0b11000011,
    0b01100110,
    0b00111100,
    0b00011000,
};

static const uint8_t cursor_shape_vertical[CURSOR_SIZE] = {
    0b00011000,
    0b00111100,
    0b01111110,
    0b00011000,
    0b00011000,
    0b01111110,
    0b00111100,
    0b00011000,
};

static const uint8_t cursor_shape_diag_nw_se[CURSOR_SIZE] = {
    0b11110000,
    0b11000000,
    0b10000000,
    0b00000000,
    0b00000000,
    0b00000001,
    0b00000011,
    0b00001111,
};

static const uint8_t cursor_shape_move[CURSOR_SIZE] = {
    0b00011000,
    0b00111100,
    0b01011010,
    0b11011011,
    0b11011011,
    0b01011010,
    0b00111100,
    0b00011000,
};

static const uint8_t cursor_shape_diag_ne_sw[CURSOR_SIZE] = {
    0b00001111,
    0b00000011,
    0b00000001,
    0b00000000,
    0b00000000,
    0b10000000,
    0b11000000,
    0b11110000,
};

static long read_exact(int fd, void *buffer, size_t length) {
    uint8_t *p = (uint8_t *)buffer;
    size_t got = 0;
    while (got < length) {
        long n = sys_read(fd, p + got, length - got);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    return (long)got;
}

static inline void put_pixel(int32_t x, int32_t y, uint32_t color) {
    back_buffer[(uint32_t)y * back_pitch_pixels + (uint32_t)x] = color;
}

static inline void blend_pixel(int32_t x, int32_t y, uint32_t color, uint32_t alpha) {
    if (alpha >= 255) {
        put_pixel(x, y, color);
        return;
    }
    uint32_t *at = &back_buffer[(uint32_t)y * back_pitch_pixels + (uint32_t)x];
    uint32_t behind = *at;
    uint32_t inverse = 255u - alpha;
    uint32_t r = GRAPHICS_OVER_255((color >> 16 & 0xFFu) * alpha + (behind >> 16 & 0xFFu) * inverse);
    uint32_t g = GRAPHICS_OVER_255((color >> 8 & 0xFFu) * alpha + (behind >> 8 & 0xFFu) * inverse);
    uint32_t b = GRAPHICS_OVER_255((color & 0xFFu) * alpha + (behind & 0xFFu) * inverse);
    *at = (behind & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

static inline void put_pixel_clipped(int32_t x, int32_t y, uint32_t color) {
    if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1) {
        put_pixel(x, y, color);
    }
}

static void present(void) {
    if (display_scale == 1) {
        for (int32_t y = clip_y0; y < clip_y1; y++) {
            memcpy(&real_framebuffer[(uint32_t)y * framebuffer_pitch_pixels + (uint32_t)clip_x0],
                   &back_buffer[(uint32_t)y * back_pitch_pixels + (uint32_t)clip_x0],
                   (size_t)(clip_x1 - clip_x0) * sizeof(uint32_t));
        }
        return;
    }
    /* Each desktop pixel becomes a 2x2 block, written as one 64-bit store to
       each of the two rows it covers. The framebuffer is write-combining
       memory and is only ever written: reading a row back to copy it would
       cost more than building it twice. */
    for (int32_t y = clip_y0; y < clip_y1; y++) {
        const uint32_t *source = &back_buffer[(uint32_t)y * back_pitch_pixels];
        uint64_t *top = (uint64_t *)&real_framebuffer[(uint32_t)(2 * y) * framebuffer_pitch_pixels];
        uint64_t *bottom = (uint64_t *)&real_framebuffer[(uint32_t)(2 * y + 1) * framebuffer_pitch_pixels];
        for (int32_t x = clip_x0; x < clip_x1; x++) {
            uint64_t pair = (uint64_t)source[x] | ((uint64_t)source[x] << 32);
            top[x] = pair;
            bottom[x] = pair;
        }
    }
}

static void fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    int32_t x0 = max_i32(x, clip_x0);
    int32_t y0 = max_i32(y, clip_y0);
    int32_t x1 = min_i32(x + w, clip_x1);
    int32_t y1 = min_i32(y + h, clip_y1);
    for (int32_t row = y0; row < y1; row++) {
        uint32_t *destination = back_buffer + (uint32_t)row * back_pitch_pixels;
        for (int32_t col = x0; col < x1; col++) {
            destination[col] = color;
        }
    }
}

static void fill_rect_blend(int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t color, uint32_t num, uint32_t den) {
    int32_t x0 = max_i32(x, clip_x0);
    int32_t y0 = max_i32(y, clip_y0);
    int32_t x1 = min_i32(x + w, clip_x1);
    int32_t y1 = min_i32(y + h, clip_y1);
    uint32_t sr = (color >> 16) & 0xFF;
    uint32_t sg = (color >> 8) & 0xFF;
    uint32_t sb = color & 0xFF;
    for (int32_t row = y0; row < y1; row++) {
        for (int32_t col = x0; col < x1; col++) {
            uint32_t existing = back_buffer[(uint32_t)row * back_pitch_pixels + (uint32_t)col];
            uint32_t r = (((existing >> 16) & 0xFF) * (den - num) + sr * num) / den;
            uint32_t g = (((existing >> 8) & 0xFF) * (den - num) + sg * num) / den;
            uint32_t b = ((existing & 0xFF) * (den - num) + sb * num) / den;
            put_pixel(col, row, (r << 16) | (g << 8) | b);
        }
    }
}

static int32_t content_bottom_limit(void);

static void blend_pixel_clipped(int32_t x, int32_t y, uint32_t color, uint32_t alpha) {
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1 || alpha == 0) {
        return;
    }
    blend_pixel(x, y, color, alpha);
}

static void fill_rounded(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                         uint32_t color, uint32_t alpha) {
    if (w <= 0 || h <= 0 || alpha == 0) {
        return;
    }
    radius = graphics_clamp_radius(w, h, radius);
    for (int32_t row = 0; row < h; row++) {
        if (y + row < clip_y0 || y + row >= clip_y1) {
            continue;
        }
        int32_t left = graphics_rounded_edge_subpixels(row, h, radius);
        if (left == 0) {
            if (alpha >= 255) {
                fill_rect(x, y + row, w, 1, color);
            } else {
                for (int32_t column = 0; column < w; column++) {
                    blend_pixel_clipped(x + column, y + row, color, alpha);
                }
            }
            continue;
        }
        int32_t right = w * GRAPHICS_SUBPIXEL - left;
        for (int32_t column = left / GRAPHICS_SUBPIXEL;
             column < (right + GRAPHICS_SUBPIXEL - 1) / GRAPHICS_SUBPIXEL && column < w; column++) {
            uint32_t coverage = graphics_span_coverage(column, left, right);
            blend_pixel_clipped(x + column, y + row, color, coverage * alpha / 255);
        }
    }
}

static void stroke_rounded(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                           uint32_t color, uint32_t alpha) {
    if (w <= 2 || h <= 2 || alpha == 0) {
        return;
    }
    radius = graphics_clamp_radius(w, h, radius);
    int32_t inner_radius = radius > 0 ? radius - 1 : 0;
    for (int32_t row = 0; row < h; row++) {
        if (y + row < clip_y0 || y + row >= clip_y1) {
            continue;
        }
        int32_t outer_left = graphics_rounded_edge_subpixels(row, h, radius);
        if (outer_left == 0) {
            if (row == 0 || row == h - 1) {
                for (int32_t column = 0; column < w; column++) {
                    blend_pixel_clipped(x + column, y + row, color, alpha);
                }
            } else {
                blend_pixel_clipped(x, y + row, color, alpha);
                blend_pixel_clipped(x + w - 1, y + row, color, alpha);
            }
            continue;
        }
        int32_t outer_right = w * GRAPHICS_SUBPIXEL - outer_left;
        int32_t inner_left;
        int32_t inner_right;
        if (row == 0 || row == h - 1) {
            inner_left = outer_right;
            inner_right = outer_right;
        } else {
            inner_left = GRAPHICS_SUBPIXEL + graphics_rounded_edge_subpixels(row - 1, h - 2, inner_radius);
            inner_right = (w - 1) * GRAPHICS_SUBPIXEL - (inner_left - GRAPHICS_SUBPIXEL);
        }
        int32_t bands[2][2];
        int band_count;
        if (inner_left >= inner_right) {
            bands[0][0] = outer_left / GRAPHICS_SUBPIXEL;
            bands[0][1] = (outer_right + GRAPHICS_SUBPIXEL - 1) / GRAPHICS_SUBPIXEL;
            band_count = 1;
        } else {
            bands[0][0] = outer_left / GRAPHICS_SUBPIXEL;
            bands[0][1] = (inner_left + GRAPHICS_SUBPIXEL - 1) / GRAPHICS_SUBPIXEL;
            bands[1][0] = inner_right / GRAPHICS_SUBPIXEL;
            bands[1][1] = (outer_right + GRAPHICS_SUBPIXEL - 1) / GRAPHICS_SUBPIXEL;
            band_count = 2;
        }
        for (int band = 0; band < band_count; band++) {
            int32_t from = bands[band][0] < 0 ? 0 : bands[band][0];
            int32_t to = bands[band][1] > w ? w : bands[band][1];
            for (int32_t column = from; column < to; column++) {
                int32_t outer = (int32_t)graphics_span_coverage(column, outer_left, outer_right);
                int32_t inner = (int32_t)graphics_span_coverage(column, inner_left, inner_right);
                int32_t coverage = outer - inner;
                if (coverage > 0) {
                    blend_pixel_clipped(x + column, y + row, color, (uint32_t)coverage * alpha / 255);
                }
            }
        }
    }
}

static int32_t content_bottom_limit(void);

static void draw_window_shadow(const window_t *win, int focused) {
    int32_t x = win->x - BORDER;
    int32_t y = win->y - TITLEBAR_H - BORDER;
    int32_t w = win->w + 2 * BORDER;
    int32_t h = win->h + TITLEBAR_H + 2 * BORDER;
    uint32_t peak = focused ? SHADOW_FOCUS_PEAK : SHADOW_PEAK;
    int32_t limit = content_bottom_limit();
    for (int32_t ring = SHADOW_RINGS - 1; ring >= 0; ring--) {
        int32_t grow = ring + 1;
        int32_t remaining = SHADOW_RINGS - ring;
        uint32_t alpha = peak * (uint32_t)(remaining * remaining) /
                         (uint32_t)(SHADOW_RINGS * SHADOW_RINGS);
        int32_t ry = y + SHADOW_DROP;
        int32_t rh = h + 2 * grow;
        if (ry - grow + rh > limit) {
            rh = limit - (ry - grow);
        }
        if (rh <= 2) {
            continue;
        }
        stroke_rounded(x - grow, ry - grow, w + 2 * grow, rh,
                       FRAME_RADIUS + grow, 0x00000000u, alpha);
    }
}

static void blit_window(const window_t *win) {
    int32_t x0 = max_i32(win->x, clip_x0);
    int32_t y0 = max_i32(win->y - win->overhang, clip_y0);
    int32_t x1 = min_i32(win->x + win->w, clip_x1);
    int32_t y1 = min_i32(win->y + win->h, clip_y1);
    if (x1 <= x0) {
        return;
    }
    if (win->translucent) {
        for (int32_t row = y0; row < y1; row++) {
            const uint32_t *source_row = win->pixels + (uint32_t)(row - win->y + win->buffer_y0) * (uint32_t)win->buffer_w;
            uint32_t *destination = back_buffer + (uint32_t)row * back_pitch_pixels;
            for (int32_t col = x0; col < x1; col++) {
                uint32_t source = source_row[col - win->x];
                uint32_t under = destination[col];
                uint32_t r = (((under >> 16) & 0xFF) * (TRANSLUCENT_DEN - TRANSLUCENT_NUMBER) + ((source >> 16) & 0xFF) * TRANSLUCENT_NUMBER) / TRANSLUCENT_DEN;
                uint32_t g = (((under >> 8) & 0xFF) * (TRANSLUCENT_DEN - TRANSLUCENT_NUMBER) + ((source >> 8) & 0xFF) * TRANSLUCENT_NUMBER) / TRANSLUCENT_DEN;
                uint32_t b = ((under & 0xFF) * (TRANSLUCENT_DEN - TRANSLUCENT_NUMBER) + (source & 0xFF) * TRANSLUCENT_NUMBER) / TRANSLUCENT_DEN;
                destination[col] = (r << 16) | (g << 8) | b;
            }
        }
        return;
    }
    size_t row_bytes = (size_t)(x1 - x0) * sizeof(uint32_t);
    int32_t radius = (win->is_panel || win->is_desktop || win->is_popup)
                         ? 0
                         : graphics_clamp_radius(win->w, 2 * FRAME_RADIUS, FRAME_RADIUS);
    for (int32_t row = y0; row < y1; row++) {
        const uint32_t *source_row = win->pixels + (uint32_t)(row - win->y + win->buffer_y0) * (uint32_t)win->buffer_w;
        int32_t from_bottom = win->y + win->h - 1 - row;
        if (radius > 0 && from_bottom < radius) {
            int32_t left = graphics_rounded_edge_subpixels(2 * radius - 1 - from_bottom,
                                                           2 * radius, radius);
            int32_t right = win->w * GRAPHICS_SUBPIXEL - left;
            for (int32_t col = x0; col < x1; col++) {
                uint32_t coverage = graphics_span_coverage(col - win->x, left, right);
                if (coverage == 0) {
                    continue;
                }
                blend_pixel(col, row, source_row[col - win->x], coverage);
            }
            continue;
        }
        memcpy(back_buffer + (uint32_t)row * back_pitch_pixels + (uint32_t)x0,
               source_row + (x0 - win->x), row_bytes);
    }
}

typedef enum { BTN_MINIMIZE = 0, BTN_MAXIMIZE = 1, BTN_CLOSE = 2, BTN_COUNT } titlebar_button_t;

static const int BTN_SLOT_FROM_RIGHT[BTN_COUNT] = {
    [BTN_MINIMIZE] = 1,
    [BTN_MAXIMIZE] = 0,
    [BTN_CLOSE]    = 2,
};

static void titlebar_button_rect(const window_t *win, titlebar_button_t btn, int32_t *out_x, int32_t *out_y) {
    int32_t by = win->y - TITLEBAR_H + (TITLEBAR_H - BTN_SIZE) / 2;
    int32_t slot = BTN_SLOT_FROM_RIGHT[btn];
    int32_t bx = win->x + win->w - BTN_MARGIN - BTN_SIZE - slot * (BTN_SIZE + BTN_GAP);
    *out_x = bx;
    *out_y = by;
}

static int hover_btn_window = -1;
static titlebar_button_t hover_btn;

typedef enum {
    HIT_FRAME,
    HIT_TITLEBAR,
    HIT_BUTTON,
    HIT_RESIZE,
} hit_region_t;

#define WCLASS_DESKTOP  1u
#define WCLASS_ORDINARY 2u
#define WCLASS_PANEL    4u
#define WCLASS_POPUP    8u
#define WCLASS_ALL      (WCLASS_DESKTOP | WCLASS_ORDINARY | WCLASS_PANEL | WCLASS_POPUP)

static int point_occluded_by(const window_t *win, int32_t px, int32_t py) {
    if (win->is_panel || win->is_desktop || win->is_popup) {
        return point_in_window(win, px, py);
    }
    return px >= win->x - BORDER && px < win->x + win->w + BORDER &&
           py >= win->y - TITLEBAR_H - BORDER && py < win->y + win->h + BORDER;
}

static int window_class_bit(const window_t *win) {
    if (win->is_desktop) {
        return WCLASS_DESKTOP;
    }
    if (win->is_panel) {
        return WCLASS_PANEL;
    }
    if (win->is_popup) {
        return WCLASS_POPUP;
    }
    return WCLASS_ORDINARY;
}

static int hit_region_matches(const window_t *win, int32_t px, int32_t py,
                               hit_region_t region, int *out_detail) {
    switch (region) {
    case HIT_FRAME:
        return point_occluded_by(win, px, py);
    case HIT_TITLEBAR:
        return !win->is_panel && !win->is_desktop && !win->is_popup && point_in_titlebar(win, px, py);
    case HIT_BUTTON:
        if (win->is_panel || win->is_desktop || win->is_popup) {
            return 0;
        }
        for (int b = 0; b < BTN_COUNT; b++) {
            int32_t bx, by;
            titlebar_button_rect(win, (titlebar_button_t)b, &bx, &by);
            if (graphics_point_in_rect(px, py, bx, by, BTN_SIZE, BTN_SIZE)) {
                if (out_detail) {
                    *out_detail = b;
                }
                return 1;
            }
        }
        return 0;
    case HIT_RESIZE: {
        if (win->is_panel || win->is_desktop || win->is_popup) {
            return 0;
        }
        int mask = resize_hit_mask(win, px, py);
        if (mask && out_detail) {
            *out_detail = mask;
        }
        return mask != 0;
    }
    }
    return 0;
}

static int z_hit_test(int32_t px, int32_t py, unsigned classes, hit_region_t region, int *out_detail) {
    for (int z = z_count - 1; z >= 0; z--) {
        int idx = zorder[z];
        const window_t *w = &windows[idx];
        if (!w->alive || w->minimized || !window_here(w)) {
            continue;
        }
        if ((window_class_bit(w) & classes) &&
            hit_region_matches(w, px, py, region, out_detail)) {
            return idx;
        }
        if (point_occluded_by(w, px, py)) {
            return -1;
        }
    }
    return -1;
}

static int titlebar_button_at(int32_t px, int32_t py, titlebar_button_t *out_btn) {
    int detail = 0;
    int idx = z_hit_test(px, py, WCLASS_ORDINARY, HIT_BUTTON, &detail);
    if (idx >= 0) {
        *out_btn = (titlebar_button_t)detail;
    }
    return idx;
}

static uint32_t lighten(uint32_t color, uint32_t num, uint32_t den) {
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        uint32_t c = (color >> shift) & 0xFF;
        out |= (c + (255 - c) * num / den) << shift;
    }
    return out;
}

static void fill_circle(int32_t x, int32_t y, int32_t diameter, uint32_t color) {
    fill_rounded(x, y, diameter, diameter, diameter / 2, color, 255);
}

static void draw_button_glyph(int32_t bx, int32_t by, titlebar_button_t btn) {
    int32_t g0 = BTN_GLYPH_INSET;
    int32_t g1 = BTN_SIZE - 1 - BTN_GLYPH_INSET;
    int32_t mid = BTN_SIZE / 2;
    if (btn == BTN_CLOSE) {
        for (int32_t k = 0; k <= g1 - g0; k++) {
            put_pixel_clipped(bx + g0 + k, by + g0 + k, BTN_GLYPH_COLOR);
            put_pixel_clipped(bx + g1 - k, by + g0 + k, BTN_GLYPH_COLOR);
        }
        return;
    }
    if (btn == BTN_MINIMIZE) {
        fill_rect(bx + g0, by + mid - 1, g1 - g0 + 1, 2, BTN_GLYPH_COLOR);
        return;
    }
    fill_rect(bx + g0, by + g0, g1 - g0 + 1, 2, BTN_GLYPH_COLOR);
    fill_rect(bx + g0, by + g1 - 1, g1 - g0 + 1, 2, BTN_GLYPH_COLOR);
    fill_rect(bx + g0, by + g0, 2, g1 - g0 + 1, BTN_GLYPH_COLOR);
    fill_rect(bx + g1 - 1, by + g0, 2, g1 - g0 + 1, BTN_GLYPH_COLOR);
}

static void draw_titlebar_buttons(const window_t *win, int idx, int focused) {
    static const uint32_t colors[BTN_COUNT] = {BTN_MINIMIZE_COLOR, BTN_MAXIMIZE_COLOR, BTN_CLOSE_COLOR};
    int hovered_here = (hover_btn_window == idx);
    for (int b = 0; b < BTN_COUNT; b++) {
        int32_t bx, by;
        titlebar_button_rect(win, (titlebar_button_t)b, &bx, &by);
        uint32_t color = (focused || hovered_here) ? colors[b] : BTN_IDLE_COLOR;
        if (hovered_here && hover_btn == (titlebar_button_t)b) {
            color = lighten(color, 1, BTN_HOVER_LIGHTEN);
        }
        fill_circle(bx, by, BTN_SIZE, color);
        if (hovered_here) {
            draw_button_glyph(bx, by, (titlebar_button_t)b);
        }
    }
}

static void draw_char_clipped(int32_t x, int32_t y, char c, uint32_t color, int bold) {
    uint8_t code = (uint8_t)c;
    if (code >= 128) {
        return;
    }
    const ui_font_t *f = &UI_FONT;
    if (f->width[code] == 0) {
        return;
    }
    int use_bold = (bold && f->coverage_bold) ? 1 : 0;
    const uint8_t *coverage = use_bold ? f->coverage_bold : f->coverage;
    const uint16_t *offset = use_bold ? f->offset_bold : f->offset;
    int32_t w = f->width[code] + use_bold;
    const uint8_t *glyph = coverage + offset[code];
    for (int32_t row = 0; row < f->height; row++) {
        int32_t py = y + row;
        if (py < clip_y0 || py >= clip_y1) {
            continue;
        }
        const uint8_t *line = glyph + (int32_t)row * w;
        for (int32_t col = 0; col < w; col++) {
            int32_t px = x + col;
            if (px < clip_x0 || px >= clip_x1) {
                continue;
            }
            if (line[col]) {
                blend_pixel(px, py, color, line[col]);
            }
        }
    }
}

static int32_t text_width(const char *s) {
    return graphics_text_width(&UI_FONT, s);
}

static void draw_text_clipped(int32_t x, int32_t y, const char *s, uint32_t color, int bold) {
    int32_t cx = x;
    for (const char *p = s; *p; p++) {
        draw_char_clipped(cx, y, *p, color, bold);
        cx += graphics_char_advance(&UI_FONT, *p);
    }
}

static int32_t title_available_width(const window_t *win) {
    int32_t leftmost_btn_x;
    int32_t unused_y;
    titlebar_button_rect(win, (titlebar_button_t)(BTN_COUNT - 1), &leftmost_btn_x, &unused_y);
    int32_t avail = leftmost_btn_x - TITLE_BTN_GAP - (win->x + TITLE_MARGIN);
    return avail < 0 ? 0 : avail;
}

static void fit_title(const window_t *win, char *out) {
    int32_t avail = title_available_width(win);
    if (avail < 0) {
        avail = 0;
    }
    int32_t max_chars = graphics_text_fit(&UI_FONT, win->title, avail);
    int i = 0;
    for (; win->title[i] && i < max_chars && i < WINDOW_MANAGER_TITLE_MAX - 2; i++) {
        out[i] = win->title[i];
    }
    if (win->title[i]) {
        int32_t ell = graphics_char_advance(&UI_FONT, UI_G_ELLIPSIS);
        while (i > 0 && graphics_text_width_n(&UI_FONT, out, i) + ell > avail) {
            i--;
        }
        out[i++] = UI_G_ELLIPSIS;
    }
    out[i] = '\0';
}

static int hovered_resize_mask(void);

static int cursor_over_titlebar(void);

static void draw_cursor(const uint8_t *shape) {
    int32_t x0 = max_i32(cursor_x, clip_x0);
    int32_t y0 = max_i32(cursor_y, clip_y0);
    int32_t x1 = min_i32(cursor_x + CURSOR_SIZE, clip_x1);
    int32_t y1 = min_i32(cursor_y + CURSOR_SIZE, clip_y1);
    for (int32_t row = y0; row < y1; row++) {
        uint8_t bits = shape[row - cursor_y];
        for (int32_t col = x0; col < x1; col++) {
            if (bits & (0x80 >> (col - cursor_x))) {
                put_pixel(col, row, CURSOR_COLOR);
            }
        }
    }
}

static void stroke_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

#define OVERLAY_RADIUS 9

static void fill_rect_rounded(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    fill_rounded(x, y, w, h, OVERLAY_RADIUS, color, 255);
}

static void fill_rect_rounded_blend(int32_t x, int32_t y, int32_t w, int32_t h,
                                     uint32_t color, uint32_t num, uint32_t den) {
    fill_rounded(x, y, w, h, OVERLAY_RADIUS, color, num * 255u / den);
}

static void draw_titlebar(const window_t *win, int focused) {
    int32_t x = win->x;
    int32_t y = win->y - TITLEBAR_H;
    int32_t w = win->w;
    uint32_t top = focused ? TITLEBAR_TOP_COLOR : TITLEBAR_DIM_TOP_COLOR;
    uint32_t bottom = focused ? TITLEBAR_BOTTOM_COLOR : TITLEBAR_DIM_BOTTOM_COLOR;
    int32_t radius = graphics_clamp_radius(w, 2 * FRAME_RADIUS, FRAME_RADIUS);
    for (int32_t row = 0; row < TITLEBAR_H; row++) {
        if (y + row < clip_y0 || y + row >= clip_y1) {
            continue;
        }
        uint32_t color = 0;
        for (int shift = 16; shift >= 0; shift -= 8) {
            int32_t a = (int32_t)((top >> shift) & 0xFFu);
            int32_t b = (int32_t)((bottom >> shift) & 0xFFu);
            color |= (uint32_t)(a + (b - a) * row / (TITLEBAR_H - 1)) << shift;
        }
        if (row >= radius) {
            fill_rect(x, y + row, w, 1, color);
            continue;
        }
        int32_t left = graphics_rounded_edge_subpixels(row, 2 * radius, radius);
        int32_t right = w * GRAPHICS_SUBPIXEL - left;
        for (int32_t column = 0; column < w; column++) {
            uint32_t coverage = graphics_span_coverage(column, left, right);
            blend_pixel_clipped(x + column, y + row, color, coverage);
        }
    }
    if (focused) {
        int32_t left = graphics_rounded_edge_subpixels(0, 2 * radius, radius);
        int32_t right = w * GRAPHICS_SUBPIXEL - left;
        for (int32_t column = 0; column < w; column++) {
            uint32_t coverage = graphics_span_coverage(column, left, right);
            blend_pixel_clipped(x + column, y, accent_color,
                                coverage * TITLEBAR_ACCENT_ALPHA / 255);
        }
    }
    for (int32_t column = 0; column < w; column++) {
        blend_pixel_clipped(x + column, y + TITLEBAR_H - 1, 0x00000000u, 90u);
    }
}

static void stroke_rect_rounded(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    stroke_rounded(x, y, w, h, OVERLAY_RADIUS, color, 255);
}

static void launcher_rect(int32_t *out_x, int32_t *out_y) {
    *out_x = ((int32_t)framebuffer_info.width - LAUNCHER_W) / 2;
    *out_y = ((int32_t)framebuffer_info.height - LAUNCHER_H) / 3;
}

static int32_t launcher_row_y(int32_t launcher_y, int i) {
    return launcher_y + LAUNCHER_LIST_Y + i * LAUNCHER_ROW_H;
}

static void draw_power_button(int32_t x, int32_t y, int32_t bx, const char *label, int mode) {
    int32_t px = x + bx;
    int32_t py = y + POWER_BTN_Y;
    fill_rect_rounded(px, py, POWER_BTN_W, POWER_BTN_H,
                       power_hover == mode ? POWER_BTN_HOVER : POWER_BTN_BG);
    stroke_rect_rounded(px, py, POWER_BTN_W, POWER_BTN_H, LAUNCHER_BORDER);
    int32_t label_w = text_width(label);
    draw_text_clipped(px + (POWER_BTN_W - label_w) / 2, py + (POWER_BTN_H - UI_FONT_HEIGHT) / 2,
                       label, LAUNCHER_TEXT, 0);
}

static void draw_power_row(int32_t x, int32_t y) {
    draw_power_button(x, y, POWER_OFF_X, "Shut Down", POWER_OFF);
    draw_power_button(x, y, POWER_REBOOT_X, "Restart", POWER_REBOOT);
}

static void draw_power_confirm(int32_t x, int32_t y) {
    int32_t cx = x + (LAUNCHER_W - POWER_CONFIRM_W) / 2;
    int32_t cy = y + (LAUNCHER_H - POWER_CONFIRM_H) / 2;
    fill_rect_rounded(cx, cy, POWER_CONFIRM_W, POWER_CONFIRM_H, POWER_CONFIRM_BG);
    stroke_rect_rounded(cx, cy, POWER_CONFIRM_W, POWER_CONFIRM_H, LAUNCHER_BORDER);
    draw_text_clipped(cx + GRAPHICS_PAD, cy + GRAPHICS_PAD,
                       power_confirm == POWER_REBOOT ? "Restart this machine?" : "Shut down this machine?",
                       LAUNCHER_TEXT, 1);
    draw_text_clipped(cx + GRAPHICS_PAD, cy + GRAPHICS_PAD + 2 * UI_FONT_HEIGHT,
                       "Y / Enter = yes", LAUNCHER_ROW_FG, 0);
    draw_text_clipped(cx + GRAPHICS_PAD, cy + GRAPHICS_PAD + 3 * UI_FONT_HEIGHT,
                       "N / Esc / click = cancel", LAUNCHER_ROW_FG, 0);
}

static void draw_launcher(void) {
    int32_t x, y;
    launcher_rect(&x, &y);
    fill_rect_rounded_blend(x, y, LAUNCHER_W, LAUNCHER_H, LAUNCHER_BG,
                             launcher_opacity_number(), LAUNCHER_OPACITY_DEN);
    stroke_rect_rounded(x, y, LAUNCHER_W, LAUNCHER_H, LAUNCHER_BORDER);

    int32_t input_x = x + LAUNCHER_PAD;
    int32_t input_y = y + LAUNCHER_PAD;
    int32_t input_w = LAUNCHER_W - 2 * LAUNCHER_PAD;
    fill_rect_rounded(input_x, input_y, input_w, LAUNCHER_INPUT_H, LAUNCHER_INPUT_BG);
    stroke_rect_rounded(input_x, input_y, input_w, LAUNCHER_INPUT_H, LAUNCHER_BORDER);
    int32_t text_y = input_y + (LAUNCHER_INPUT_H - UI_FONT_HEIGHT) / 2;
    if (launcher_query_length > 0) {
        draw_text_clipped(input_x + 6, text_y, launcher_query, LAUNCHER_TEXT, 0);
    } else {
        draw_text_clipped(input_x + 6, text_y, "Type to search", LAUNCHER_HINT, 0);
    }
    fill_rect(input_x + 6 + (launcher_query_length > 0 ? text_width(launcher_query) : 0),
               text_y, 2, UI_FONT_HEIGHT, LAUNCHER_TEXT);

    if (launcher_match_count == 0) {
        draw_text_clipped(x + LAUNCHER_PAD, launcher_row_y(y, 0) + 2, "No matches", LAUNCHER_HINT, 0);
    } else {
        for (int i = 0; i < LAUNCHER_ROWS; i++) {
            int m = launcher_scroll + i;
            if (m >= launcher_match_count) {
                break;
            }
            int32_t ry = launcher_row_y(y, i);
            if (m == launcher_selected) {
                fill_rect_rounded(x + LAUNCHER_PAD / 2, ry, LAUNCHER_W - LAUNCHER_PAD, LAUNCHER_ROW_H, LAUNCHER_SEL_BG);
            }
            int32_t mark_w = graphics_char_advance(&UI_FONT, UI_G_ARROW_RIGHT);
            if (m == launcher_selected) {
                draw_text_clipped(x + LAUNCHER_PAD, ry + 2, UI_S_ARROW_RIGHT, LAUNCHER_TEXT, 0);
            }
            draw_text_clipped(x + LAUNCHER_PAD + mark_w + 2, ry + 2, launcher_entries[launcher_matches[m]],
                               m == launcher_selected ? LAUNCHER_TEXT : LAUNCHER_ROW_FG,
                               m == launcher_selected);
        }
    }

    draw_power_row(x, y);
    if (power_confirm != POWER_CONFIRM_NONE) {
        draw_power_confirm(x, y);
    }
}

static void toast_rect(int i, int32_t *out_x, int32_t *out_y) {
    *out_x = (int32_t)framebuffer_info.width - TOAST_W - TOAST_MARGIN;
    *out_y = TOAST_MARGIN + i * (TOAST_H + TOAST_GAP);
}

static uint32_t toast_accent(uint32_t level) {
    if (level == WINDOW_MANAGER_NOTIFY_ERROR) {
        return TOAST_ERROR_C;
    }
    return level == WINDOW_MANAGER_NOTIFY_WARN ? TOAST_WARN_C : TOAST_INFO_C;
}

static void draw_toasts(void) {
    for (int i = 0; i < toast_count; i++) {
        int32_t x, y;
        toast_rect(i, &x, &y);
        fill_rect_rounded(x, y, TOAST_W, TOAST_H, TOAST_BG);
        stroke_rect_rounded(x, y, TOAST_W, TOAST_H, TOAST_BORDER);
        fill_rect(x + 1, y + GRAPHICS_CORNER_R, TOAST_STRIPE_W, TOAST_H - 2 * GRAPHICS_CORNER_R,
                   toast_accent(toasts[i].level));
        draw_text_clipped(x + TOAST_STRIPE_W + 10, y + 10, toasts[i].title, TOAST_TITLE_FG, 1  );
        draw_text_clipped(x + TOAST_STRIPE_W + 10, y + 10 + UI_FONT_HEIGHT + 4, toasts[i].body, TOAST_BODY_FG, 0);
    }
}

static const char *wmenu_label(int idx, const window_t *win) {
    if (idx == 0) {
        return win->minimized ? "Restore" : "Minimize";
    }
    return idx == 1 ? "Close" : "Force Quit";
}

static void draw_window_menu(void) {
    const window_t *win = &windows[wmenu_window];
    int32_t h = WMENU_ITEM_H * WMENU_COUNT;
    fill_rect_rounded(wmenu_x, wmenu_y, WMENU_W, h, WMENU_BG);
    stroke_rect_rounded(wmenu_x, wmenu_y, WMENU_W, h, WMENU_BORDER);
    for (int i = 0; i < WMENU_COUNT; i++) {
        int32_t ry = wmenu_y + i * WMENU_ITEM_H;
        if (i == wmenu_hover) {
            fill_rect_rounded(wmenu_x + 2, ry + 1, WMENU_W - 4, WMENU_ITEM_H - 2, WMENU_HOVER_BG);
        }
        draw_text_clipped(wmenu_x + 8, ry + (WMENU_ITEM_H - UI_FONT_HEIGHT) / 2,
                           wmenu_label(i, win), WMENU_TEXT, 0);
    }
}

#define ANIM_FILL_NUMBER 2
#define ANIM_FILL_DEN 5

static void draw_animations(void) {
    long now = sys_uptime_ms();
    for (int i = 0; i < ANIM_MAX; i++) {
        anim_t *a = &anims[i];
        if (a->kind == ANIM_NONE) {
            continue;
        }
        int32_t x, y, w, h;
        if (!anim_rect_now(a, now, &x, &y, &w, &h)) {
            continue;
        }
        if (w < 2 || h < 2) {
            continue;
        }
        fill_rect_rounded_blend(x, y, w, h, accent_color, ANIM_FILL_NUMBER, ANIM_FILL_DEN);
        stroke_rect_rounded(x, y, w, h, accent_color);
        a->lx = x; a->ly = y; a->lw = w; a->lh = h;
        a->drawn = 1;
    }
}

static int anim_step(long now, int32_t *out_x0, int32_t *out_y0, int32_t *out_x1, int32_t *out_y1) {
    int32_t x0 = (int32_t)framebuffer_info.width, y0 = (int32_t)framebuffer_info.height, x1 = 0, y1 = 0;
    int any = 0;
    for (int i = 0; i < ANIM_MAX; i++) {
        anim_t *a = &anims[i];
        if (a->kind == ANIM_NONE) {
            continue;
        }
        if (a->frames < 0xFFFFu) {
            a->frames++;
        }
        if (a->drawn) {
            x0 = min_i32(x0, a->lx);
            y0 = min_i32(y0, a->ly);
            x1 = max_i32(x1, a->lx + a->lw);
            y1 = max_i32(y1, a->ly + a->lh);
            any = 1;
        }
        int32_t nx, ny, nw, nh;
        if (!anim_rect_now(a, now, &nx, &ny, &nw, &nh)) {
            a->kind = ANIM_NONE;
            a->drawn = 0;
            continue;
        }
        x0 = min_i32(x0, nx);
        y0 = min_i32(y0, ny);
        x1 = max_i32(x1, nx + nw);
        y1 = max_i32(y1, ny + nh);
        any = 1;
    }
    if (!any) {
        return 0;
    }
    *out_x0 = x0 - 1;
    *out_y0 = y0 - 1;
    *out_x1 = x1 + 1;
    *out_y1 = y1 + 1;
    return 1;
}

static void redraw_rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    clip_x0 = max_i32(x0, 0);
    clip_y0 = max_i32(y0, 0);
    clip_x1 = min_i32(x1, (int32_t)framebuffer_info.width);
    clip_y1 = min_i32(y1, (int32_t)framebuffer_info.height);
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) {
        return;
    }

    fill_rect(0, 0, (int32_t)framebuffer_info.width, (int32_t)framebuffer_info.height, bg_color);
    for (int z = 0; z < z_count; z++) {
        int i = zorder[z];
        const window_t *win = &windows[i];
        if (!win->alive || win->minimized || win->is_panel || !window_here(win)) {
            continue;
        }
        if (win->is_desktop) {
            blit_window(win);
            continue;
        }
        if (win->is_popup) {
            blit_window(win);
            stroke_rounded(win->x - 1, win->y - 1, win->w + 2, win->h + 2, 0, BORDER_COLOR, BORDER_ALPHA);
            continue;
        }
        int focused = (i == focused_window);
        draw_window_shadow(win, focused);
        stroke_rounded(win->x - BORDER, win->y - TITLEBAR_H - BORDER,
                        win->w + 2 * BORDER, win->h + TITLEBAR_H + 2 * BORDER,
                        FRAME_RADIUS + BORDER, BORDER_COLOR, BORDER_ALPHA);
        draw_titlebar(win, focused);
        char fitted_title[WINDOW_MANAGER_TITLE_MAX];
        fit_title(win, fitted_title);
        int32_t title_w = text_width(fitted_title);
        int32_t title_x = win->x + (win->w - title_w) / 2;
        int32_t title_left = win->x + TITLE_MARGIN;
        if (title_x < title_left) {
            title_x = title_left;
        }
        if (title_x + title_w > title_left + title_available_width(win)) {
            title_x = title_left;
        }
        draw_text_clipped(title_x, win->y - TITLEBAR_H + (TITLEBAR_H - UI_FONT_HEIGHT) / 2,
                           fitted_title, focused ? TITLE_COLOR : TITLE_DIM_COLOR, 1  );
        draw_titlebar_buttons(win, i, focused);
        blit_window(win);
    }
    if (snap_preview_active) {
        fill_rect_blend(snap_preview_x, snap_preview_y, snap_preview_w, snap_preview_h,
                         accent_color, snap_preview_number(), SNAP_FADE_DEN);
        stroke_rect(snap_preview_x, snap_preview_y, snap_preview_w, snap_preview_h, accent_color);
    }
    for (int z = 0; z < z_count; z++) {
        const window_t *win = &windows[zorder[z]];
        if (win->alive && win->is_panel && !win->minimized) {
            blit_window(win);
        }
    }
    if (wmenu_window >= 0 && windows[wmenu_window].alive) {
        draw_window_menu();
    }
    draw_animations();
    if (launcher_open) {
        draw_launcher();
    }
    draw_toasts();
    if (client_drag_active) {
        int32_t lw = text_width(client_drag_payload) + 2 * DRAG_LABEL_PAD;
        int32_t lx = min_i32(cursor_x + CURSOR_SIZE, (int32_t)framebuffer_info.width - lw);
        int32_t ly = min_i32(cursor_y + CURSOR_SIZE, (int32_t)framebuffer_info.height - DRAG_LABEL_H);
        fill_rect_rounded(lx, ly, lw, DRAG_LABEL_H, DRAG_LABEL_BG);
        stroke_rect_rounded(lx, ly, lw, DRAG_LABEL_H, DRAG_LABEL_BORDER);
        draw_text_clipped(lx + DRAG_LABEL_PAD, ly + (DRAG_LABEL_H - UI_FONT_HEIGHT) / 2,
                           client_drag_payload, DRAG_LABEL_FG, 0);
    }
    const uint8_t *cursor_shape_now = cursor_shape;
    int rmask = hovered_resize_mask();
    if ((rmask & (RESIZE_TOP | RESIZE_LEFT)) == (RESIZE_TOP | RESIZE_LEFT) ||
        (rmask & (RESIZE_BOTTOM | RESIZE_RIGHT)) == (RESIZE_BOTTOM | RESIZE_RIGHT)) {
        cursor_shape_now = cursor_shape_diag_nw_se;
    } else if ((rmask & (RESIZE_TOP | RESIZE_RIGHT)) == (RESIZE_TOP | RESIZE_RIGHT) ||
               (rmask & (RESIZE_BOTTOM | RESIZE_LEFT)) == (RESIZE_BOTTOM | RESIZE_LEFT)) {
        cursor_shape_now = cursor_shape_diag_ne_sw;
    } else if (rmask & (RESIZE_LEFT | RESIZE_RIGHT)) {
        cursor_shape_now = cursor_shape_horizontal;
    } else if (rmask & (RESIZE_TOP | RESIZE_BOTTOM)) {
        cursor_shape_now = cursor_shape_vertical;
    } else if (cursor_over_titlebar()) {
        cursor_shape_now = cursor_shape_move;
    }
    draw_cursor(cursor_shape_now);
    present();
}

static void redraw(void) {
    redraw_rect(0, 0, (int32_t)framebuffer_info.width, (int32_t)framebuffer_info.height);
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

typedef enum { DRAG_NONE = 0, DRAG_MOVE, DRAG_RESIZE } drag_mode_t;
static drag_mode_t drag_mode = DRAG_NONE;
static int drag_window = -1;
static int drag_resize_mask;
static int32_t drag_start_cursor_x, drag_start_cursor_y;
static int32_t drag_start_x, drag_start_y, drag_start_w, drag_start_h;

#define MIN_WIN_W 60
#define MIN_WIN_H 40
#define MOVE_MIN_VISIBLE 40

#define TITLEBAR_DOUBLE_CLICK_MS 500

#define SNAP_EDGE_MARGIN 8
#define SNAP_NONE  0
#define SNAP_LEFT  1
#define SNAP_RIGHT 2
static int drag_snap_hint = SNAP_NONE;

static int titlebar_last_click_window = -1;
static uint32_t titlebar_last_click_ms;

static int hovered_resize_mask(void) {
    if (drag_mode == DRAG_RESIZE) {
        return drag_resize_mask;
    }
    int mask = 0;
    return z_hit_test(cursor_x, cursor_y, WCLASS_ORDINARY, HIT_RESIZE, &mask) >= 0 ? mask : 0;
}

static int cursor_over_titlebar(void) {
    if (drag_mode == DRAG_MOVE) {
        return 1;
    }
    if (drag_mode != DRAG_NONE || hover_btn_window >= 0) {
        return 0;
    }
    return z_hit_test(cursor_x, cursor_y, WCLASS_ORDINARY, HIT_TITLEBAR, 0) >= 0;
}

static void send_event(const window_t *win, const window_manager_event_t *ev) {
    sys_write(win->evt_write_file_descriptor, ev, sizeof(*ev));
}

static void set_focus(int idx);

static void switch_workspace(int to) {
    to = ((to % WINDOW_MANAGER_WORKSPACE_COUNT) + WINDOW_MANAGER_WORKSPACE_COUNT) % WINDOW_MANAGER_WORKSPACE_COUNT;
    if (to == current_workspace) {
        return;
    }
    current_workspace = to;

    if (focused_window >= 0 && !window_here(&windows[focused_window])) {
        set_focus(-1);
    }
    if (focused_window < 0) {
        for (int z = z_count - 1; z >= 0; z--) {
            window_t *w = &windows[zorder[z]];
            if (w->alive && !w->minimized && !w->is_panel && !w->is_desktop && !w->is_popup && window_here(w)) {
                set_focus(zorder[z]);
                break;
            }
        }
    }
    dirty = 1;
}

static void move_window_to_workspace(int idx, int to) {
    window_t *win = &windows[idx];
    if (win->workspace < 0) {
        return;
    }
    to = ((to % WINDOW_MANAGER_WORKSPACE_COUNT) + WINDOW_MANAGER_WORKSPACE_COUNT) % WINDOW_MANAGER_WORKSPACE_COUNT;
    win->workspace = (int8_t)to;
    switch_workspace(to);
    set_focus(idx);
    dirty = 1;
}

static void set_focus(int idx) {
    if (idx >= 0) {
        z_raise(idx);
    }
    if (focused_window == idx) {
        return;
    }
    if (focused_window >= 0) {
        window_manager_event_t ev = {0};
        ev.type = WINDOW_MANAGER_EVENT_UNFOCUS;
        send_event(&windows[focused_window], &ev);
    }
    focused_window = idx;
    if (focused_window >= 0) {
        window_manager_event_t ev = {0};
        ev.type = WINDOW_MANAGER_EVENT_FOCUS;
        send_event(&windows[focused_window], &ev);
    }
    dirty = 1;
}

static void toast_post(uint32_t level, const char *title, const char *body) {
    if (level == WINDOW_MANAGER_NOTIFY_ERROR) {
        sys_beep(660, 90);
    }
    if (toast_count == TOAST_MAX) {
        for (int i = 1; i < TOAST_MAX; i++) {
            toasts[i - 1] = toasts[i];
        }
        toast_count--;
    }
    toast_t *t = &toasts[toast_count++];
    t->level = level;
    int i = 0;
    for (; title && title[i] && i < WINDOW_MANAGER_NOTIFY_TITLE_MAX - 1; i++) {
        t->title[i] = title[i];
    }
    t->title[i] = '\0';
    i = 0;
    for (; body && body[i] && i < WINDOW_MANAGER_NOTIFY_BODY_MAX - 1; i++) {
        t->body[i] = body[i];
    }
    t->body[i] = '\0';
    t->expires_ms = sys_uptime_ms() + TOAST_TTL_MS;
    dirty = 1;
}

static void toasts_expire(long now_ms) {
    int out = 0;
    for (int i = 0; i < toast_count; i++) {
        if (toasts[i].expires_ms > now_ms) {
            if (out != i) {
                toasts[out] = toasts[i];
            }
            out++;
        }
    }
    if (out != toast_count) {
        toast_count = out;
        dirty = 1;
    }
}

static int toast_click(int32_t px, int32_t py) {
    for (int i = 0; i < toast_count; i++) {
        int32_t x, y;
        toast_rect(i, &x, &y);
        if (graphics_point_in_rect(px, py, x, y, TOAST_W, TOAST_H)) {
            for (int j = i + 1; j < toast_count; j++) {
                toasts[j - 1] = toasts[j];
            }
            toast_count--;
            dirty = 1;
            return 1;
        }
    }
    return 0;
}

static int capture_window = -1;

static void reclaim_window(int idx) {
    window_t *win = &windows[idx];
    if (!win->alive) {
        return;
    }
    if (!win->is_popup) {
        for (int p = 0; p < window_count; p++) {
            if (windows[p].alive && windows[p].is_popup && windows[p].parent_window == idx) {
                reclaim_window(p);
            }
        }
    }
    if (capture_window == idx) {
        capture_window = -1;
    }
    if (!win->is_panel && !win->is_desktop && !win->is_popup && !win->minimized) {
        anim_window_close(idx);
    }
    slot_w[idx] = -1;
    if (win->shared_memory_id >= 0) {
        sys_shared_memory_free(win->shared_memory_id, win->pixels);
        win->shared_memory_id = -1;
        win->pixels = (uint32_t *)0;
    }
    win->alive = 0;
    z_remove(idx);
    win->minimized = 0;
    win->overhang = 0;
    win->close_requested = 0;
    win->client_pid = -1;
    if (wmenu_window == idx) {
        wmenu_window = -1;
        wmenu_hover = -1;
    }
    if (focused_window == idx) {
        set_focus(-1);
    }
    dirty = 1;
}

/* SIGTERM asks; a program whose event loop is wedged never answers, and
   its window stays on the screen after the person pressed close - a black
   browser window that would not go away was exactly that (M196). Past the
   grace period the ask becomes SIGKILL, which is what the window's own
   "Force quit" does anyway. */
#define CLOSE_GRACE_MS 5000

static void enforce_close_requests(void) {
    long now = sys_uptime_ms();
    for (int i = 0; i < window_count; i++) {
        window_t *w = &windows[i];
        if (w->alive && w->close_requested && w->close_deadline_ms > 0 &&
            now >= w->close_deadline_ms && w->client_pid > 0) {
            w->close_deadline_ms = 0;
            sys_kill(w->client_pid, SIGKILL);
        }
    }
}

static void reap_dead_clients(void) {
    enforce_close_requests();
    for (int i = 0; i < window_count; i++) {
        /* A client that crashed, or is gone altogether, loses its window.
           One that exited 0 keeps it - a program may draw and leave, and
           wm_demo's self-test is exactly that - unless the window had been
           asked to close. That is what the titlebar button and Chromium's
           own quit both do, and Chromium answers SIGTERM by exiting 0: the
           browser's window used to stay on the screen, dead, until its
           process slot was recycled (M199). */
        long alive = windows[i].alive ? sys_task_alive(windows[i].client_pid) : 1;
        if (alive != 1 && (alive != 2 || windows[i].close_requested)) {
            if (!windows[i].close_requested) {
                toast_post(WINDOW_MANAGER_NOTIFY_ERROR,
                            windows[i].title[0] ? windows[i].title : "A program",
                            "stopped unexpectedly.");
            }
            reclaim_window(i);
        }
    }
}

static int32_t connected_panel_height(void) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i].alive && windows[i].is_panel) {
            return windows[i].h;
        }
    }
    return 0;
}

static int32_t content_top_limit(void) {
    return TITLEBAR_H + BORDER;
}

static int32_t content_bottom_limit(void) {
    return (int32_t)framebuffer_info.height - connected_panel_height();
}

static void snap_rect(const window_t *win, uint32_t action,
                       int32_t *out_x, int32_t *out_y, int32_t *out_w, int32_t *out_h) {
    int32_t half_w = max_i32((int32_t)framebuffer_info.width / 2 - 2 * BORDER, MIN_WIN_W);
    int32_t avail_h = max_i32(content_bottom_limit() - content_top_limit() - BORDER, MIN_WIN_H);
    *out_w = min_i32(win->buffer_w, half_w);
    *out_h = min_i32(win->buffer_h, avail_h);
    *out_x = (action == WINDOW_MANAGER_ACTION_SNAP_LEFT) ? BORDER : (int32_t)framebuffer_info.width / 2 + BORDER;
    *out_y = content_top_limit();
}

static void refuse_window(int response_write_file_descriptor, int32_t client_pid, const char *reason) {
    window_manager_create_response_t response;
    response.window_id = -1;
    response.shared_memory_id = -1;
    response.width = 0;
    response.height = 0;
    response.client_pid = client_pid;
    response.compositor_pid = self_pid;
    sys_write(response_write_file_descriptor, &response, sizeof(response));

    toast_post(WINDOW_MANAGER_NOTIFY_WARN, "Window refused", reason);

    const char prefix[] = "[wm] window request refused: ";
    sys_write(1, prefix, sizeof(prefix) - 1);
    int length = 0;
    while (reason[length]) {
        length++;
    }
    sys_write(1, reason, (size_t)length);
    sys_write(1, "\n", 1);
}

static void clamp_window_on_screen(int idx);

static int rebuffer_window(int idx) {
    window_t *win = &windows[idx];
    uint32_t width = (win->is_panel || win->is_desktop) ? framebuffer_info.width : (uint32_t)win->buffer_w;
    uint32_t height = win->is_desktop ? framebuffer_info.height : (uint32_t)win->buffer_h;

    long id = sys_shared_memory_create((size_t)width * height * sizeof(uint32_t));
    long vaddr = id < 0 ? -1 : sys_shared_memory_map(id);
    if (vaddr < 0) {
        if (id >= 0) {
            sys_shared_memory_free(id, (void *)0);
        }
        return -1;
    }
    if (win->shared_memory_id >= 0) {
        sys_shared_memory_free(win->shared_memory_id, win->pixels);
    }
    win->shared_memory_id = (int32_t)id;
    win->pixels = (uint32_t *)vaddr;
    win->buffer_w = (int32_t)width;
    win->buffer_h = (int32_t)height;
    if (win->is_panel) {
        win->w = (int32_t)width;
    } else if (win->is_desktop) {
        win->w = (int32_t)width;
        win->h = (int32_t)height;
    }
    win->needs_rebuffer = 0;
    clamp_window_on_screen(idx);
    dirty = 1;
    return 0;
}

typedef struct {
    char program[24];
    int32_t x, y, w, h;
    int8_t workspace;
    uint8_t claimed;
} session_entry_t;
static session_entry_t *session_claim(int client_pid);

static void accept_pending_window(int request_read_file_descriptor, int response_write_file_descriptor) {
    if (sys_pipe_poll(request_read_file_descriptor) < (long)sizeof(window_manager_create_request_t)) {
        return;
    }
    window_manager_create_request_t request;
    long n = read_exact(request_read_file_descriptor, &request, sizeof(request));
    window_manager_create_response_t response;
    if (n != (long)sizeof(request)) {
        refuse_window(response_write_file_descriptor, -1, "short/torn create request");
        return;
    }

    if (request.client_pid > 0 && !request.popup) {
        for (int i = 0; i < window_count; i++) {
            if (windows[i].alive && windows[i].client_pid == request.client_pid) {
                if (windows[i].needs_rebuffer && rebuffer_window(i) != 0) {
                    refuse_window(response_write_file_descriptor, request.client_pid,
                                   "could not reallocate this window's buffer for the new display size");
                    return;
                }
                response.window_id = i;
                response.shared_memory_id = windows[i].shared_memory_id;
                response.width = (uint32_t)windows[i].buffer_w;
                response.height = (uint32_t)windows[i].buffer_h;
                response.compositor_pid = self_pid;
                response.client_pid = request.client_pid;
                sys_write(response_write_file_descriptor, &response, sizeof(response));
                return;
            }
        }
    }

    int idx = -1;
    for (int i = 0; i < window_count; i++) {
        if (!windows[i].alive) {
            idx = i;
            break;
        }
    }
    int reused_slot = (idx >= 0);
    if (idx < 0) {
        if (window_count >= MAX_WINDOWS) {
            refuse_window(response_write_file_descriptor, request.client_pid, "no free window slot (MAX_WINDOWS)");
            return;
        }
        idx = window_count;
    }

    uint32_t width = (request.panel || request.desktop) ? framebuffer_info.width : request.width;
    uint32_t height = request.desktop ? framebuffer_info.height : request.height;
    int popup_parent = -1;
    if (request.popup) {
        int pw = request.parent_window_id;
        if (pw < 0 || pw >= window_count || !windows[pw].alive || windows[pw].is_popup ||
            windows[pw].client_pid != request.client_pid) {
            refuse_window(response_write_file_descriptor, request.client_pid,
                          "a popup needs a live parent window of its own process");
            return;
        }
        popup_parent = pw;
    }

    long shared_memory_id = sys_shared_memory_create((size_t)width * height * sizeof(uint32_t));
    long vaddr = shared_memory_id < 0 ? -1 : sys_shared_memory_map(shared_memory_id);
    if (shared_memory_id < 0 || vaddr < 0) {
        refuse_window(response_write_file_descriptor, request.client_pid, "SYS_shm_create/SYS_shm_map failed (MAX_SHM_SEGMENTS, or out of memory)");
        return;
    }

    int evt_write_file_descriptor;
    if (reused_slot) {
        evt_write_file_descriptor = windows[idx].evt_write_file_descriptor;
        sys_pipe_reset(evt_write_file_descriptor);
    } else {
        char evt_name[WINDOW_MANAGER_EVENT_PIPE_NAME_LENGTH];
        window_manager_event_pipe_name(idx, evt_name);
        int evt_file_descriptors[2];
        if (sys_pipe_open(evt_name, evt_file_descriptors) != 0) {
            refuse_window(response_write_file_descriptor, request.client_pid, "SYS_pipe_open for this window's event pipe failed (this process's MAX_FDS, or MAX_NAMED_PIPES)");
            return;
        }
        evt_write_file_descriptor = evt_file_descriptors[1];
    }

    window_t *win = &windows[idx];
    win->is_panel = request.panel;
    session_entry_t *restored = 0;
    int32_t dock_h = (int32_t)height;
    if (request.panel && request.panel_dock_h > 0 && request.panel_dock_h < height) {
        dock_h = (int32_t)request.panel_dock_h;
    }
    if (request.panel) {
        win->x = 0;
        win->y = (int32_t)framebuffer_info.height - dock_h;
    } else if (request.desktop) {
        win->x = 0;
        win->y = 0;
    } else if (request.popup) {
        const window_t *parent = &windows[popup_parent];
        win->x = max_i32(min_i32(parent->x + request.popup_x, (int32_t)framebuffer_info.width - (int32_t)width), 0);
        win->y = max_i32(min_i32(parent->y + request.popup_y, (int32_t)framebuffer_info.height - (int32_t)height), 0);
    } else {
        /* The cascade steps right with every slot, and a wide window on
           a small screen stepped its own titlebar buttons off the right
           edge - a browser nobody could close with the pointer. */
        win->x = max_i32(min_i32(100 + idx * 40, (int32_t)framebuffer_info.width - (int32_t)width), 0);
        int32_t cascade_y = 100 + idx * 40;
        int32_t bottom_fit = content_bottom_limit() - (int32_t)height;
        win->y = max_i32(min_i32(cascade_y, bottom_fit), content_top_limit());

        restored = session_claim(request.client_pid);
        if (restored) {
            win->x = max_i32(min_i32(restored->x, (int32_t)framebuffer_info.width - 40), 0);
            int32_t rb = content_bottom_limit() - (int32_t)height;
            win->y = max_i32(min_i32(restored->y, rb), content_top_limit());
        }
    }
    win->w = (int32_t)width;
    win->h = request.panel ? dock_h : (int32_t)height;
    win->buffer_w = (int32_t)width;
    win->buffer_h = (int32_t)height;
    win->buffer_y0 = (int32_t)height - win->h;
    win->overhang = 0;
    win->pixels = (uint32_t *)vaddr;
    win->shared_memory_id = (int32_t)shared_memory_id;
    win->evt_write_file_descriptor = evt_write_file_descriptor;
    win->translucent = request.translucent;
    win->is_desktop = request.desktop;
    win->minimized = 0;
    win->maximized = 0;
    win->alive = 1;
    win->close_requested = 0;
    win->close_deadline_ms = 0;
    win->confirm_close = request.confirm_close;
    win->client_pid = request.client_pid;
    win->is_popup = request.popup ? 1 : 0;
    win->parent_window = popup_parent;
    if (!request.panel && !request.desktop && !request.popup) {
        anim_window_open(idx);
    }
    win->workspace = (request.panel || request.desktop) ? (int8_t)-1
                   : request.popup ? windows[popup_parent].workspace
                   : (restored ? restored->workspace : (int8_t)current_workspace);
    slot_x[idx] = 0;
    slot_w[idx] = -1;
    int ti = 0;
    for (; request.title[ti] && ti < WINDOW_MANAGER_TITLE_MAX - 1; ti++) {
        win->title[ti] = request.title[ti];
    }
    win->title[ti] = '\0';

    z_insert_top_of_band(idx);

    response.window_id = idx;
    response.shared_memory_id = (int32_t)shared_memory_id;
    response.width = width;
    response.height = height;
    response.compositor_pid = self_pid;
    response.client_pid = request.client_pid;
    if (!reused_slot) {
        window_count++;
    }
    sys_write(response_write_file_descriptor, &response, sizeof(response));
    if (!win->is_panel && !win->is_popup) {
        set_focus(idx);
    }
}

static void accept_pending_query(int query_read_file_descriptor, int query_response_write_file_descriptor) {
    if (sys_pipe_poll(query_read_file_descriptor) < 1) {
        return;
    }
    uint8_t ping;
    sys_read(query_read_file_descriptor, &ping, sizeof(ping));

    window_manager_query_response_t response;
    response.count = 0;
    response.current_workspace = current_workspace;
    for (int i = 0; i < window_count; i++) {
        const window_t *win = &windows[i];
        if (!win->alive || win->is_popup) {
            continue;
        }
        int out = response.count;
        response.windows[out].window_id = i;
        response.windows[out].x = win->x;
        response.windows[out].y = win->y;
        response.windows[out].w = win->w;
        response.windows[out].h = win->h;
        response.windows[out].focused = (i == focused_window);
        response.windows[out].minimized = win->minimized;
        response.windows[out].maximized = win->maximized;
        response.windows[out].is_panel = win->is_panel;
        response.windows[out].is_desktop = win->is_desktop;
        response.windows[out].z_index = z_position_of(i);
        response.windows[out].workspace = win->workspace;
        memcpy(response.windows[out].title, win->title, WINDOW_MANAGER_TITLE_MAX);
        response.count++;
    }
    sys_write(query_response_write_file_descriptor, &response, sizeof(response));
}

static void apply_window_action(int idx, uint32_t action, int32_t value) {
    window_t *win = &windows[idx];
    if (action == WINDOW_MANAGER_ACTION_FOCUS) {
        win->minimized = 0;
        if (win->workspace >= 0 && win->workspace != current_workspace) {
            switch_workspace(win->workspace);
        }
        set_focus(idx);
    } else if (action == WINDOW_MANAGER_ACTION_TOGGLE_MINIMIZE) {
        if (win->minimized) {
            anim_window_restore(idx);
        } else {
            anim_window_minimize(idx);
        }
        win->minimized = !win->minimized;
        if (win->minimized && focused_window == idx) {
            set_focus(-1);
        }
        dirty = 1;
    } else if (action == WINDOW_MANAGER_ACTION_CAPTURE) {
        capture_window = idx;
    } else if (action == WINDOW_MANAGER_ACTION_RELEASE_CAPTURE) {
        if (capture_window == idx) {
            capture_window = -1;
        }
    } else if (action == WINDOW_MANAGER_ACTION_CLOSE && win->is_popup) {
        reclaim_window(idx);
    } else if (action == WINDOW_MANAGER_ACTION_CLOSE) {
        if (win->confirm_close) {
            window_manager_event_t ev = {0};
            ev.type = WINDOW_MANAGER_EVENT_CLOSE_REQUEST;
            send_event(win, &ev);
        } else {
            win->close_requested = 1;
            win->close_deadline_ms = sys_uptime_ms() + CLOSE_GRACE_MS;
            sys_kill(win->client_pid, SIGTERM);
        }
    } else if (action == WINDOW_MANAGER_ACTION_KILL) {
        win->close_requested = 1;
        sys_kill(win->client_pid, SIGKILL);
    } else if (action == WINDOW_MANAGER_ACTION_SET_PANEL_OVERHANG) {
        int32_t want = value;
        if (!win->is_panel) {
            want = 0;
        }
        if (want < 0) {
            want = 0;
        }
        if (want > win->buffer_y0) {
            want = win->buffer_y0;
        }
        if (want != win->overhang) {
            win->overhang = want;
            dirty = 1;
        }
    } else if (action == WINDOW_MANAGER_ACTION_MAXIMIZE) {
        if (!win->maximized) {
            win->saved_x = win->x;
            win->saved_y = win->y;
            win->saved_w = win->w;
            win->saved_h = win->h;
            int32_t avail_w = (int32_t)framebuffer_info.width - 2 * BORDER;
            int32_t avail_h = content_bottom_limit() - content_top_limit() - BORDER;
            win->x = BORDER;
            win->y = content_top_limit();
            win->w = min_i32(win->buffer_w, avail_w);
            win->h = min_i32(win->buffer_h, avail_h);
            win->maximized = 1;
            dirty = 1;
        }
    } else if (action == WINDOW_MANAGER_ACTION_SNAP_LEFT || action == WINDOW_MANAGER_ACTION_SNAP_RIGHT) {
        snap_rect(win, action, &win->x, &win->y, &win->w, &win->h);
        win->maximized = 0;
        dirty = 1;
    } else if (action == WINDOW_MANAGER_ACTION_RESTORE) {
        if (win->maximized) {
            win->x = win->saved_x;
            win->y = win->saved_y;
            win->w = win->saved_w;
            win->h = win->saved_h;
            win->maximized = 0;
            dirty = 1;
        }
    }
}

static void wmenu_open_at(int idx, int32_t px, int32_t py) {
    wmenu_window = idx;
    wmenu_hover = -1;
    int32_t h = WMENU_ITEM_H * WMENU_COUNT;
    wmenu_x = min_i32(px, (int32_t)framebuffer_info.width - WMENU_W);
    wmenu_y = min_i32(py, (int32_t)framebuffer_info.height - h);
    wmenu_x = max_i32(wmenu_x, 0);
    wmenu_y = max_i32(wmenu_y, 0);
    dirty = 1;
}

static void wmenu_close(void) {
    if (wmenu_window >= 0) {
        wmenu_window = -1;
        wmenu_hover = -1;
        dirty = 1;
    }
}

static int wmenu_row_at(int32_t px, int32_t py) {
    if (!graphics_point_in_rect(px, py, wmenu_x, wmenu_y, WMENU_W, WMENU_ITEM_H * WMENU_COUNT)) {
        return -1;
    }
    return (py - wmenu_y) / WMENU_ITEM_H;
}

static void wmenu_click(int32_t px, int32_t py) {
    int idx = wmenu_window;
    int row = wmenu_row_at(px, py);
    wmenu_close();
    if (row < 0 || idx < 0 || !windows[idx].alive) {
        return;
    }
    if (row == 0) {
        apply_window_action(idx, WINDOW_MANAGER_ACTION_TOGGLE_MINIMIZE, 0);
    } else if (row == 1) {
        apply_window_action(idx, WINDOW_MANAGER_ACTION_CLOSE, 0);
    } else {
        apply_window_action(idx, WINDOW_MANAGER_ACTION_KILL, 0);
    }
}

static char lower_char(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int launcher_name_matches(const char *name, const char *query) {
    if (!query[0]) {
        return 1;
    }
    for (int i = 0; name[i]; i++) {
        int j = 0;
        while (query[j] && lower_char(name[i + j]) == lower_char(query[j])) {
            j++;
        }
        if (!query[j]) {
            return 1;
        }
    }
    return 0;
}

static void launcher_clamp_scroll(void) {
    if (launcher_selected < 0) {
        launcher_selected = 0;
    }
    if (launcher_selected >= launcher_match_count) {
        launcher_selected = launcher_match_count - 1;
    }
    if (launcher_selected < launcher_scroll) {
        launcher_scroll = launcher_selected;
    }
    if (launcher_selected >= launcher_scroll + LAUNCHER_ROWS) {
        launcher_scroll = launcher_selected - LAUNCHER_ROWS + 1;
    }
    if (launcher_scroll < 0) {
        launcher_scroll = 0;
    }
}

static void launcher_apply_filter(void) {
    launcher_match_count = 0;
    for (int i = 0; i < launcher_entry_count; i++) {
        if (launcher_name_matches(launcher_entries[i], launcher_query)) {
            launcher_matches[launcher_match_count++] = i;
        }
    }
    launcher_selected = 0;
    launcher_scroll = 0;
}

static void launcher_reload(void) {
    static char buffer[LAUNCHER_LIST_BUFFER];
    launcher_entry_count = 0;
    launcher_recent_count = recent_load(launcher_recent_path, RECENT_MAX);
    for (int i = 0; i < launcher_recent_count; i++) {
        const char *base = launcher_recent_path[i];
        for (const char *c = launcher_recent_path[i]; *c; c++) {
            if (*c == '/') {
                base = c + 1;
            }
        }
        int col = 0;
        for (; base[col] && col < LAUNCHER_NAME_MAX - 1; col++) {
            launcher_entries[launcher_entry_count][col] = base[col];
        }
        launcher_entries[launcher_entry_count][col] = '\0';
        launcher_entry_count++;
    }
    long n = sys_listdir(PATH_BIN, buffer, sizeof(buffer));
    if (n <= 0) {
        return;
    }
    if (n > (long)sizeof(buffer)) {
        n = (long)sizeof(buffer);
    }
    int col = 0;
    int truncated = 0;
    for (long i = 0; i < n; i++) {
        if (launcher_entry_count >= LAUNCHER_MAX_ENTRIES) {
            truncated = 1;
            break;
        }
        if (buffer[i] == '\n') {
            if (col > 0 && launcher_entries[launcher_entry_count][col - 1] == '/') {
                col = 0;
                continue;
            }
            launcher_entries[launcher_entry_count][col] = '\0';
            launcher_entry_count++;
            col = 0;
        } else if (col < LAUNCHER_NAME_MAX - 1) {
            launcher_entries[launcher_entry_count][col++] = buffer[i];
        }
    }
    if (truncated) {
        static const char message[] =
            "[launcher] more than " STRINGIFY(LAUNCHER_MAX_ENTRIES)
            " programs in /bin - the rest are not listed. Raise "
            "LAUNCHER_MAX_ENTRIES in compositor.c.\n";
        sys_write(1, message, sizeof(message) - 1);
    }
}

static void launcher_set_open(int open) {
    if (open && !launcher_open && animations_enabled) {
        launcher_fade_start_ms = sys_uptime_ms();
    } else {
        launcher_fade_start_ms = 0;
    }
    launcher_open = open;
    power_confirm = POWER_CONFIRM_NONE;
    power_hover = POWER_CONFIRM_NONE;
    if (open) {
        launcher_query[0] = '\0';
        launcher_query_length = 0;
        launcher_reload();
        launcher_apply_filter();
    }
    dirty = 1;
}

static void launcher_launch_selected(void) {
    if (launcher_selected >= 0 && launcher_selected < launcher_match_count) {
        int entry = launcher_matches[launcher_selected];
        const char *name = launcher_entries[entry];
        if (entry < launcher_recent_count) {
            long rc = sys_spawn(PATH_BIN_DIRECTORY "text_editor", launcher_recent_path[entry]);
            if (rc < 0) {
                toast_post(WINDOW_MANAGER_NOTIFY_ERROR, name, spawn_error_message(rc));
            }
            child_track(rc);
            launcher_set_open(0);
            return;
        }
        char path[PATH_MAX_LENGTH];
        if (path_join(path, PATH_BIN_DIRECTORY, name) != 0) {
            toast_post(WINDOW_MANAGER_NOTIFY_ERROR, name, "Name too long to launch.");
        } else {
            long rc = sys_spawn(path, "");
            if (rc < 0) {
                toast_post(WINDOW_MANAGER_NOTIFY_ERROR, name, spawn_error_message(rc));
            }
            child_track(rc);
        }
    }
    launcher_set_open(0);
}

static void shutdown_begin(int mode) {
    int asked = 0;
    window_manager_event_t ev;
    ev.type = WINDOW_MANAGER_EVENT_QUERY_SHUTDOWN;
    ev.ch = 0;
    ev.x = 0;
    ev.y = 0;
    ev.buttons = 0;
    ev.wheel = 0;
    ev.mods = 0;
    ev.time_ms = (uint32_t)sys_uptime_ms();
    for (int i = 0; i < window_count; i++) {
        window_t *w = &windows[i];
        if (!w->alive || !w->confirm_close) {
            continue;
        }
        send_event(w, &ev);
        asked++;
    }
    if (asked == 0) {
        sys_shutdown(mode);
        return;
    }
    shutdown_pending_mode = mode;
    shutdown_vetoed_by = -1;
    shutdown_deadline_ms = sys_uptime_ms() + SHUTDOWN_QUERY_MS;
}

static void shutdown_tick(long now) {
    if (shutdown_pending_mode == POWER_CONFIRM_NONE) {
        return;
    }
    if (shutdown_vetoed_by >= 0) {
        int idx = shutdown_vetoed_by;
        const char *who = (idx >= 0 && idx < window_count && windows[idx].alive)
                              ? windows[idx].title : "A program";
        shutdown_pending_mode = POWER_CONFIRM_NONE;
        shutdown_vetoed_by = -1;
        if (idx >= 0 && idx < window_count && windows[idx].alive) {
            apply_window_action(idx, WINDOW_MANAGER_ACTION_FOCUS, 0);
        }
        toast_post(WINDOW_MANAGER_NOTIFY_WARN, who, "Unsaved changes - not shutting down.");
        dirty = 1;
        return;
    }
    if (now >= shutdown_deadline_ms) {
        int mode = shutdown_pending_mode;
        shutdown_pending_mode = POWER_CONFIRM_NONE;
        sys_shutdown(mode);
    }
}

static void launcher_key(char ch) {
    if (power_confirm != POWER_CONFIRM_NONE) {
        if (ch == 'y' || ch == 'Y' || ch == '\n' || ch == '\r') {
            shutdown_begin(power_confirm);
        }
        power_confirm = POWER_CONFIRM_NONE;
        dirty = 1;
        return;
    }
    if (ch == 27) {
        launcher_set_open(0);
        return;
    }
    if (ch == '\n' || ch == '\r') {
        launcher_launch_selected();
        return;
    }
    if (ch == (char)KEYBOARD_KEY_UP) {
        launcher_selected--;
    } else if (ch == (char)KEYBOARD_KEY_DOWN) {
        launcher_selected++;
    } else if (ch == '\b' || ch == 0x7F) {
        if (launcher_query_length > 0) {
            launcher_query[--launcher_query_length] = '\0';
            launcher_apply_filter();
        }
    } else if (ch >= 0x20 && ch < 0x7F && launcher_query_length < LAUNCHER_QUERY_MAX - 1) {
        launcher_query[launcher_query_length++] = ch;
        launcher_query[launcher_query_length] = '\0';
        launcher_apply_filter();
    }
    launcher_clamp_scroll();
    dirty = 1;
}

static int power_button_at(int32_t px, int32_t py) {
    int32_t lx, ly;
    launcher_rect(&lx, &ly);
    if (graphics_point_in_rect(px, py, lx + POWER_OFF_X, ly + POWER_BTN_Y, POWER_BTN_W, POWER_BTN_H)) {
        return POWER_OFF;
    }
    if (graphics_point_in_rect(px, py, lx + POWER_REBOOT_X, ly + POWER_BTN_Y, POWER_BTN_W, POWER_BTN_H)) {
        return POWER_REBOOT;
    }
    return POWER_CONFIRM_NONE;
}

static int launcher_click(int32_t px, int32_t py) {
    int32_t lx, ly;
    launcher_rect(&lx, &ly);
    if (power_confirm != POWER_CONFIRM_NONE) {
        power_confirm = POWER_CONFIRM_NONE;
        dirty = 1;
        return 1;
    }
    if (!graphics_point_in_rect(px, py, lx, ly, LAUNCHER_W, LAUNCHER_H)) {
        launcher_set_open(0);
        return 1;
    }
    int power = power_button_at(px, py);
    if (power != POWER_CONFIRM_NONE) {
        power_confirm = power;
        dirty = 1;
        return 1;
    }
    for (int i = 0; i < LAUNCHER_ROWS; i++) {
        if (launcher_scroll + i >= launcher_match_count) {
            break;
        }
        if (graphics_point_in_rect(px, py, lx + LAUNCHER_PAD / 2, launcher_row_y(ly, i),
                               LAUNCHER_W - LAUNCHER_PAD, LAUNCHER_ROW_H)) {
            launcher_selected = launcher_scroll + i;
            launcher_launch_selected();
            return 1;
        }
    }
    return 1;
}

static void launcher_wheel(int32_t detents) {
    if (launcher_match_count == 0) {
        return;
    }
    launcher_selected += detents;
    launcher_clamp_scroll();
    dirty = 1;
}

static void launcher_hover(int32_t px, int32_t py) {
    int32_t lx, ly;
    launcher_rect(&lx, &ly);
    int power = power_button_at(px, py);
    if (power != power_hover) {
        power_hover = power;
        dirty = 1;
    }
    for (int i = 0; i < LAUNCHER_ROWS; i++) {
        if (launcher_scroll + i >= launcher_match_count) {
            break;
        }
        if (graphics_point_in_rect(px, py, lx + LAUNCHER_PAD / 2, launcher_row_y(ly, i),
                               LAUNCHER_W - LAUNCHER_PAD, LAUNCHER_ROW_H)) {
            if (launcher_selected != launcher_scroll + i) {
                launcher_selected = launcher_scroll + i;
                dirty = 1;
            }
            return;
        }
    }
}

static int format_uint(uint32_t v, char *out) {
    char temporary[12];
    int n = 0;
    do {
        temporary[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v);
    int length = 0;
    while (n > 0) {
        out[length++] = temporary[--n];
    }
    return length;
}

static int anim_any_active(void) {
    for (int i = 0; i < ANIM_MAX; i++) {
        if (anims[i].kind != ANIM_NONE) {
            return 1;
        }
    }
    return launcher_fading() || snap_fading();
}

static int snap_fading(void) {
    return snap_fade_start_ms != 0 && sys_uptime_ms() - snap_fade_start_ms < ANIM_MS;
}

static int32_t snap_preview_number(void) {
    if (!snap_fade_start_ms) {
        return SNAP_PREVIEW_NUMBER * 4;
    }
    long elapsed = sys_uptime_ms() - snap_fade_start_ms;
    if (elapsed >= ANIM_MS) {
        return SNAP_PREVIEW_NUMBER * 4;
    }
    int32_t p = ease_out((int32_t)(elapsed * 1000 / ANIM_MS));
    int32_t num = SNAP_PREVIEW_NUMBER * 4 * p / 1000;
    return num < 1 ? 1 : num;
}

static int launcher_fading(void) {
    return launcher_fade_start_ms != 0 &&
           sys_uptime_ms() - launcher_fade_start_ms < ANIM_MS;
}

static int32_t launcher_opacity_number(void) {
    if (!launcher_fade_start_ms) {
        return LAUNCHER_OPACITY_NUMBER;
    }
    long elapsed = sys_uptime_ms() - launcher_fade_start_ms;
    if (elapsed >= ANIM_MS) {
        return LAUNCHER_OPACITY_NUMBER;
    }
    int32_t p = ease_out((int32_t)(elapsed * 1000 / ANIM_MS));
    int32_t num = LAUNCHER_OPACITY_NUMBER * p / 1000;
    return num < 1 ? 1 : num;
}

static void anim_start(anim_kind_t kind,
                        int32_t fx, int32_t fy, int32_t fw, int32_t fh,
                        int32_t tx, int32_t ty, int32_t tw, int32_t th) {
    if (!animations_enabled) {
        return;
    }
    int slot = -1;
    for (int i = 0; i < ANIM_MAX; i++) {
        if (anims[i].kind == ANIM_NONE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return;
    }
    if (!anim_any_active()) {
        anim_run_start_ms = sys_uptime_ms();
        frames_drawn = 0;
        frames_over_budget = 0;
    }
    anim_t *a = &anims[slot];
    a->kind = (uint8_t)kind;
    a->start_ms = sys_uptime_ms();
    a->frames = 0;
    a->fx = fx; a->fy = fy; a->fw = fw; a->fh = fh;
    a->tx = tx; a->ty = ty; a->tw = tw; a->th = th;
    a->drawn = 0;
    frame_due_ms = 0;
}

static void taskbar_target(int idx, const window_t *win, int32_t *x, int32_t *y,
                            int32_t *w, int32_t *h) {
    int32_t panel_top = content_bottom_limit();
    if (slot_w[idx] > 0) {
        *x = slot_x[idx];
        *w = slot_w[idx];
    } else {
        *w = min_i32(win->w, 96);
        *x = win->x + (win->w - *w) / 2;
    }
    *y = panel_top;
    *h = (int32_t)framebuffer_info.height - panel_top;
    if (*h <= 0) {
        *h = 8;
        *y = (int32_t)framebuffer_info.height - 8;
    }
}

static void anim_window_minimize(int idx) {
    const window_t *win = &windows[idx];
    int32_t tx, ty, tw, th;
    taskbar_target(idx, win, &tx, &ty, &tw, &th);
    anim_start(ANIM_MINIMIZE, win->x, win->y - TITLEBAR_H, win->w, win->h + TITLEBAR_H,
                tx, ty, tw, th);
}

static void anim_window_restore(int idx) {
    const window_t *win = &windows[idx];
    int32_t fx, fy, fw, fh;
    taskbar_target(idx, win, &fx, &fy, &fw, &fh);
    anim_start(ANIM_RESTORE, fx, fy, fw, fh,
                win->x, win->y - TITLEBAR_H, win->w, win->h + TITLEBAR_H);
}

static void anim_window_open(int idx) {
    const window_t *win = &windows[idx];
    int32_t x = win->x, y = win->y - TITLEBAR_H;
    int32_t w = win->w, h = win->h + TITLEBAR_H;
    anim_start(ANIM_OPEN, x + w / 4, y + h / 4, w / 2, h / 2, x, y, w, h);
}

static void anim_window_close(int idx) {
    const window_t *win = &windows[idx];
    int32_t x = win->x, y = win->y - TITLEBAR_H;
    int32_t w = win->w, h = win->h + TITLEBAR_H;
    anim_start(ANIM_CLOSE, x, y, w, h, x + w / 4, y + h / 4, w / 2, h / 2);
}

static int apply_display_mode(uint32_t w, uint32_t h) {
    if ((w == framebuffer_info.width && h == framebuffer_info.height) ||
        (w == physical_info.width && h == physical_info.height)) {
        return 0;
    }
    if (sys_display_set_mode(w, h) != 0) {
        return -1;
    }
    if (read_display_modes() != 0) {
        return -1;
    }
    long framebuffer_vaddr = sys_framebuffer_map();
    if (framebuffer_vaddr < 0) {
        return -1;
    }
    real_framebuffer = (uint32_t *)framebuffer_vaddr;

    long new_id = sys_shared_memory_create((size_t)framebuffer_info.width * framebuffer_info.height * sizeof(uint32_t));
    long new_vaddr = new_id < 0 ? -1 : sys_shared_memory_map(new_id);
    if (new_vaddr < 0) {
        if (new_id >= 0) {
            sys_shared_memory_free(new_id, (void *)0);
        }
        sys_display_set_mode(mode_previous_w ? mode_previous_w : (uint32_t)back_pitch_pixels, h);
        read_display_modes();
        return -1;
    }
    if (back_shared_memory_id >= 0) {
        sys_shared_memory_free(back_shared_memory_id, back_buffer);
    }
    back_shared_memory_id = new_id;
    back_buffer = (uint32_t *)new_vaddr;
    back_pitch_pixels = framebuffer_info.width;

    cursor_x = clamp_i32(cursor_x, 0, (int32_t)framebuffer_info.width - 1);
    cursor_y = clamp_i32(cursor_y, 0, (int32_t)framebuffer_info.height - 1);
    last_drawn_cursor_x = cursor_x;
    last_drawn_cursor_y = cursor_y;

    window_manager_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = WINDOW_MANAGER_EVENT_DISPLAY_CHANGED;
    for (int i = 0; i < window_count; i++) {
        if (!windows[i].alive) {
            continue;
        }
        windows[i].needs_rebuffer = 1;
        send_event(&windows[i], &ev);
    }
    dirty = 1;
    return 0;
}

static void clamp_window_on_screen(int idx) {
    window_t *win = &windows[idx];
    if (win->is_panel) {
        win->x = 0;
        win->y = (int32_t)framebuffer_info.height - win->h;
        return;
    }
    if (win->is_desktop) {
        win->x = 0;
        win->y = 0;
        return;
    }
    if (win->maximized) {
        win->maximized = 0;
        apply_window_action(idx, WINDOW_MANAGER_ACTION_MAXIMIZE, 0);
        return;
    }
    win->x = clamp_i32(win->x, 0, max_i32(0, (int32_t)framebuffer_info.width - win->w));
    win->y = clamp_i32(win->y, content_top_limit(),
                        max_i32(content_top_limit(), content_bottom_limit() - win->h));
}

static void present_window(int id) {
    const window_t *win = &windows[id];
    if (win->minimized || !window_here(win)) {
        return;
    }
    int32_t x0 = win->x, y0 = win->y, x1 = win->x + win->w, y1 = win->y + win->h;
    if (!present_pending) {
        present_x0 = x0; present_y0 = y0; present_x1 = x1; present_y1 = y1;
        present_pending = 1;
        return;
    }
    present_x0 = min_i32(present_x0, x0);
    present_y0 = min_i32(present_y0, y0);
    present_x1 = max_i32(present_x1, x1);
    present_y1 = max_i32(present_y1, y1);
}

static void accept_one_action(int action_read_file_descriptor) {
    window_manager_action_request_t request;
    if (read_exact(action_read_file_descriptor, &request, sizeof(request)) != (long)sizeof(request)) {
        return;
    }
    if (request.action == WINDOW_MANAGER_ACTION_PRESENT) {
        if (request.window_id >= 0 && request.window_id < window_count && windows[request.window_id].alive) {
            present_window(request.window_id);
        }
        return;
    }
    if (request.action == WINDOW_MANAGER_ACTION_SET_TASKBAR_SLOT) {
        if (request.window_id >= 0 && request.window_id < MAX_WINDOWS) {
            slot_x[request.window_id] = (int32_t)(((uint32_t)request.value >> 16) & 0xFFFFu);
            slot_w[request.window_id] = (int32_t)((uint32_t)request.value & 0xFFFFu);
        }
        return;
    }
    if (request.action == WINDOW_MANAGER_ACTION_TOGGLE_LAUNCHER) {
        launcher_set_open(!launcher_open);
        return;
    }
    if (request.action == WINDOW_MANAGER_ACTION_SET_MODE) {
        uint32_t previous_w = framebuffer_info.width, previous_h = framebuffer_info.height;
        if (apply_display_mode(window_manager_mode_width(request.value), window_manager_mode_height(request.value)) != 0) {
            toast_post(WINDOW_MANAGER_NOTIFY_ERROR, "Display unchanged",
                        "The adapter refused that resolution");
            return;
        }
        if (mode_revert_at_ms == 0) {
            mode_previous_w = previous_w;
            mode_previous_h = previous_h;
        }
        mode_revert_at_ms = sys_uptime_ms() + WINDOW_MANAGER_MODE_REVERT_MS;
        return;
    }
    if (request.action == WINDOW_MANAGER_ACTION_CONFIRM_MODE) {
        mode_revert_at_ms = 0;
        return;
    }
    if (request.action == WINDOW_MANAGER_ACTION_VETO_SHUTDOWN) {
        if (shutdown_pending_mode != POWER_CONFIRM_NONE && shutdown_vetoed_by < 0) {
            shutdown_vetoed_by = request.window_id;
        }
        return;
    }
    if (request.window_id < 0 || request.window_id >= window_count || !windows[request.window_id].alive) {
        return;
    }
    apply_window_action(request.window_id, request.action, request.value);
}

static void accept_pending_action(int action_read_file_descriptor) {
    for (int i = 0; i < 32; i++) {
        if (sys_pipe_poll(action_read_file_descriptor) < (long)sizeof(window_manager_action_request_t)) {
            return;
        }
        accept_one_action(action_read_file_descriptor);
    }
}

static uint32_t luma_of(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFFu;
    uint32_t g = (rgb >> 8) & 0xFFu;
    uint32_t b = rgb & 0xFFu;
    return (77u * r + 150u * g + 29u * b) >> 8;
}

#define THEME_MIN_CONTRAST 60u

static int theme_is_readable(uint32_t bg, uint32_t accent) {
    uint32_t white = luma_of(0x00FFFFFFu);
    uint32_t lb = luma_of(bg);
    uint32_t la = luma_of(accent);
    uint32_t d_bg = white > lb ? white - lb : lb - white;
    uint32_t d_ac = white > la ? white - la : la - white;
    return d_bg >= THEME_MIN_CONTRAST && d_ac >= THEME_MIN_CONTRAST;
}

static void accept_pending_settings(int settings_read_file_descriptor) {
    if (sys_pipe_poll(settings_read_file_descriptor) < (long)sizeof(window_manager_settings_request_t)) {
        return;
    }
    window_manager_settings_request_t request;
    if (read_exact(settings_read_file_descriptor, &request, sizeof(request)) != (long)sizeof(request)) {
        return;
    }
    if (theme_is_readable(request.bg_color, request.accent_color)) {
        bg_color = request.bg_color;
        accent_color = request.accent_color;
    } else {
        bg_color = DEFAULT_BG_COLOR;
        accent_color = TITLEBAR_FOCUS_COLOR;
        toast_post(WINDOW_MANAGER_NOTIFY_WARN, "Colours",
                    "Those colours are unreadable - the defaults are back.");
    }
    wallpaper_id = request.wallpaper;
    animations_enabled = request.animations != 0;
    audio_volume = request.volume > 100 ? 100 : request.volume;
    sys_audio_volume(audio_volume);
    dirty = 1;
}

static void accept_pending_drag(int drag_read_file_descriptor) {
    if (sys_pipe_poll(drag_read_file_descriptor) < (long)sizeof(window_manager_drag_request_t)) {
        return;
    }
    window_manager_drag_request_t request;
    if (read_exact(drag_read_file_descriptor, &request, sizeof(request)) != (long)sizeof(request)) {
        return;
    }
    request.payload[WINDOW_MANAGER_DRAG_PAYLOAD_MAX - 1] = '\0';
    memcpy(client_drag_payload, request.payload, WINDOW_MANAGER_DRAG_PAYLOAD_MAX);
    client_drag_active = 1;
    client_drag_last_target = -1;
    dirty = 1;
}

static void accept_pending_notify(int notify_read_file_descriptor) {
    if (sys_pipe_poll(notify_read_file_descriptor) < (long)sizeof(window_manager_notify_request_t)) {
        return;
    }
    window_manager_notify_request_t request;
    if (read_exact(notify_read_file_descriptor, &request, sizeof(request)) != (long)sizeof(request)) {
        return;
    }
    request.title[WINDOW_MANAGER_NOTIFY_TITLE_MAX - 1] = '\0';
    request.body[WINDOW_MANAGER_NOTIFY_BODY_MAX - 1] = '\0';
    toast_post(request.level, request.title, request.body);
}

static void accept_pending_settings_query(int query_read_file_descriptor, int query_response_write_file_descriptor) {
    if (sys_pipe_poll(query_read_file_descriptor) < 1) {
        return;
    }
    uint8_t ping;
    if (read_exact(query_read_file_descriptor, &ping, sizeof(ping)) != (long)sizeof(ping)) {
        return;
    }
    window_manager_settings_request_t response;
    response.volume = audio_volume;
    response.animations = (uint32_t)animations_enabled;
    response.bg_color = bg_color;
    response.accent_color = accent_color;
    response.wallpaper = wallpaper_id;
    sys_write(query_response_write_file_descriptor, &response, sizeof(response));
}

static int window_under_cursor(void) {
    return z_hit_test(cursor_x, cursor_y, WCLASS_ALL, HIT_FRAME, 0);
}

static void focus_window_under_cursor(void) {
    int hit = window_under_cursor();
    if (hit >= 0 && !windows[hit].is_panel && !windows[hit].is_popup) {
        set_focus(hit);
    }
}

static void handle_mouse(void) {
    mouse_event_t mev;
    while (sys_mouse_read(&mev)) {
        cursor_x += mev.dx;
        cursor_y += mev.dy;
        if (cursor_x < 0) {
            cursor_x = 0;
        }
        if (cursor_y < 0) {
            cursor_y = 0;
        }
        if (cursor_x >= (int32_t)framebuffer_info.width) {
            cursor_x = (int32_t)framebuffer_info.width - 1;
        }
        if (cursor_y >= (int32_t)framebuffer_info.height) {
            cursor_y = (int32_t)framebuffer_info.height - 1;
        }

        if (capture_window >= 0 && windows[capture_window].alive && drag_mode == DRAG_NONE) {
            const window_t *cw = &windows[capture_window];
            window_manager_event_t ev = {0};
            ev.type = WINDOW_MANAGER_EVENT_MOUSE_MOVE;
            ev.x = cursor_x - cw->x;
            ev.y = cursor_y - cw->y;
            ev.buttons = mev.buttons;
            ev.time_ms = mev.time_ms;
            send_event(cw, &ev);
            if (mev.buttons != previous_buttons) {
                ev.type = WINDOW_MANAGER_EVENT_MOUSE_BUTTON;
                send_event(cw, &ev);
            }
            if (mev.wheel != 0) {
                ev.type = WINDOW_MANAGER_EVENT_MOUSE_WHEEL;
                ev.wheel = mev.wheel;
                send_event(cw, &ev);
            }
            previous_buttons = mev.buttons;
            dirty = 1;
            continue;
        }

        int left_down_edge = (mev.buttons & 1) && !(previous_buttons & 1);
        int left_up_edge = !(mev.buttons & 1) && (previous_buttons & 1);
        int right_down_edge = (mev.buttons & 2) && !(previous_buttons & 2);

        {
            titlebar_button_t over_btn = BTN_CLOSE;
            int over_index = (drag_mode == DRAG_NONE) ? titlebar_button_at(cursor_x, cursor_y, &over_btn) : -1;
            if (over_index != hover_btn_window || (over_index >= 0 && over_btn != hover_btn)) {
                hover_btn_window = over_index;
                hover_btn = over_btn;
                dirty = 1;
            }
        }

        if (mev.wheel != 0) {
            if (launcher_open) {
                launcher_wheel(mev.wheel);
            } else {
                int target = window_under_cursor();
                if (target >= 0 && !windows[target].is_panel) {
                    const window_t *win = &windows[target];
                    window_manager_event_t ev = {0};
                    ev.type = WINDOW_MANAGER_EVENT_MOUSE_WHEEL;
                    ev.x = cursor_x - win->x;
                    ev.y = cursor_y - win->y;
                    ev.buttons = mev.buttons;
                    ev.time_ms = mev.time_ms;
                    ev.wheel = mev.wheel;
                    send_event(win, &ev);
                }
            }
            previous_buttons = mev.buttons;
            continue;
        }

        if (client_drag_active) {
            int target = window_under_cursor();
            if (target >= 0 && windows[target].is_panel) {
                target = -1;
            }
            if (left_up_edge) {
                if (target >= 0) {
                    window_manager_drag_request_t data;
                    memcpy(data.payload, client_drag_payload, WINDOW_MANAGER_DRAG_PAYLOAD_MAX);
                    sys_write(drag_data_write_file_descriptor, &data, sizeof(data));
                    window_manager_event_t ev = {0};
                    ev.type = WINDOW_MANAGER_EVENT_DROP;
                    ev.x = cursor_x - windows[target].x;
                    ev.y = cursor_y - windows[target].y;
                    ev.time_ms = mev.time_ms;
                    send_event(&windows[target], &ev);
                }
                client_drag_active = 0;
                client_drag_last_target = -1;
                dirty = 1;
            } else {
                if (target != client_drag_last_target) {
                    client_drag_last_target = target;
                    if (target >= 0) {
                        window_manager_event_t ev = {0};
                        ev.type = WINDOW_MANAGER_EVENT_DRAG_MOTION;
                        ev.x = cursor_x - windows[target].x;
                        ev.y = cursor_y - windows[target].y;
                        ev.time_ms = mev.time_ms;
                        send_event(&windows[target], &ev);
                    }
                }
                dirty = 1;
            }
            previous_buttons = mev.buttons;
            continue;
        }

        if (left_down_edge && toast_click(cursor_x, cursor_y)) {
            previous_buttons = mev.buttons;
            continue;
        }

        if (launcher_open) {
            if (left_down_edge) {
                launcher_click(cursor_x, cursor_y);
            } else if (!(mev.buttons & 1)) {
                launcher_hover(cursor_x, cursor_y);
            }
            previous_buttons = mev.buttons;
            continue;
        }

        if (wmenu_window >= 0) {
            if (left_down_edge) {
                wmenu_click(cursor_x, cursor_y);
            } else if (right_down_edge) {
                wmenu_close();
            } else if (!(mev.buttons & 1)) {
                int row = wmenu_row_at(cursor_x, cursor_y);
                if (row != wmenu_hover) {
                    wmenu_hover = row;
                    dirty = 1;
                }
            }
            if (!right_down_edge) {
                previous_buttons = mev.buttons;
                continue;
            }
        }

        if (drag_mode != DRAG_NONE) {
            if (left_up_edge || !windows[drag_window].alive) {
                if (left_up_edge && drag_mode == DRAG_MOVE && drag_snap_hint != SNAP_NONE &&
                    windows[drag_window].alive) {
                    apply_window_action(drag_window, drag_snap_hint == SNAP_LEFT
                                                          ? WINDOW_MANAGER_ACTION_SNAP_LEFT
                                                          : WINDOW_MANAGER_ACTION_SNAP_RIGHT, 0);
                }
                drag_mode = DRAG_NONE;
                drag_window = -1;
                drag_snap_hint = SNAP_NONE;
                if (snap_preview_active) {
                    snap_preview_active = 0;
                    dirty = 1;
                }
            } else if (mev.buttons & 1) {
                window_t *win = &windows[drag_window];
                int32_t dx = cursor_x - drag_start_cursor_x;
                int32_t dy = cursor_y - drag_start_cursor_y;
                if (drag_mode == DRAG_MOVE) {
                    int32_t min_x = -(win->w - MOVE_MIN_VISIBLE);
                    int32_t max_x = (int32_t)framebuffer_info.width - MOVE_MIN_VISIBLE;
                    int32_t min_y = content_top_limit();
                    int32_t max_y = content_bottom_limit();
                    win->x = clamp_i32(drag_start_x + dx, min_x, max_x);
                    win->y = clamp_i32(drag_start_y + dy, min_y, max_y);
                    int hint = SNAP_NONE;
                    if (cursor_x <= SNAP_EDGE_MARGIN) {
                        hint = SNAP_LEFT;
                    } else if (cursor_x >= (int32_t)framebuffer_info.width - 1 - SNAP_EDGE_MARGIN) {
                        hint = SNAP_RIGHT;
                    }
                    if (hint != drag_snap_hint) {
                        drag_snap_hint = hint;
                        snap_preview_active = (hint != SNAP_NONE);
                        snap_fade_start_ms = snap_preview_active && animations_enabled
                                                 ? sys_uptime_ms() : 0;
                        if (snap_preview_active) {
                            int32_t sx, sy, sw, sh;
                            snap_rect(win, hint == SNAP_LEFT ? WINDOW_MANAGER_ACTION_SNAP_LEFT : WINDOW_MANAGER_ACTION_SNAP_RIGHT,
                                       &sx, &sy, &sw, &sh);
                            snap_preview_x = sx - BORDER;
                            snap_preview_y = sy - TITLEBAR_H - BORDER;
                            snap_preview_w = sw + 2 * BORDER;
                            snap_preview_h = sh + TITLEBAR_H + 2 * BORDER;
                        }
                    }
                } else {
                    int32_t new_x = drag_start_x, new_y = drag_start_y;
                    int32_t new_w = drag_start_w, new_h = drag_start_h;
                    if (drag_resize_mask & RESIZE_RIGHT) {
                        new_w = clamp_i32(drag_start_w + dx, MIN_WIN_W, win->buffer_w);
                    } else if (drag_resize_mask & RESIZE_LEFT) {
                        int32_t right_edge = drag_start_x + drag_start_w;
                        new_w = clamp_i32(drag_start_w - dx, MIN_WIN_W, win->buffer_w);
                        new_x = right_edge - new_w;
                    }
                    if (drag_resize_mask & RESIZE_BOTTOM) {
                        new_h = clamp_i32(drag_start_h + dy, MIN_WIN_H, win->buffer_h);
                    } else if (drag_resize_mask & RESIZE_TOP) {
                        int32_t bottom_edge = drag_start_y + drag_start_h;
                        new_h = clamp_i32(drag_start_h - dy, MIN_WIN_H, win->buffer_h);
                        new_y = bottom_edge - new_h;
                    }
                    win->x = new_x;
                    win->y = new_y;
                    win->w = new_w;
                    win->h = new_h;
                }
                dirty = 1;
            }
            previous_buttons = mev.buttons;
            continue;
        }

        if (left_down_edge) {
            titlebar_button_t btn_hit = BTN_CLOSE;
            int btn_hit_index = titlebar_button_at(cursor_x, cursor_y, &btn_hit);
            if (btn_hit_index >= 0) {
                if (btn_hit == BTN_CLOSE) {
                    apply_window_action(btn_hit_index, WINDOW_MANAGER_ACTION_CLOSE, 0);
                } else if (btn_hit == BTN_MINIMIZE) {
                    apply_window_action(btn_hit_index, WINDOW_MANAGER_ACTION_TOGGLE_MINIMIZE, 0);
                } else {
                    apply_window_action(btn_hit_index, windows[btn_hit_index].maximized ? WINDOW_MANAGER_ACTION_RESTORE : WINDOW_MANAGER_ACTION_MAXIMIZE, 0);
                }
                previous_buttons = mev.buttons;
                continue;
            }

            int rz_mask = 0;
            int rz_index = z_hit_test(cursor_x, cursor_y, WCLASS_ORDINARY, HIT_RESIZE, &rz_mask);
            if (rz_index >= 0) {
                drag_mode = DRAG_RESIZE;
                drag_window = rz_index;
                drag_resize_mask = rz_mask;
                drag_start_cursor_x = cursor_x;
                drag_start_cursor_y = cursor_y;
                drag_start_x = windows[rz_index].x;
                drag_start_y = windows[rz_index].y;
                drag_start_w = windows[rz_index].w;
                drag_start_h = windows[rz_index].h;
                set_focus(rz_index);
                previous_buttons = mev.buttons;
                continue;
            }

            int mv_index = z_hit_test(cursor_x, cursor_y, WCLASS_ORDINARY, HIT_TITLEBAR, 0);
            if (mv_index >= 0) {
                if (titlebar_last_click_window == mv_index &&
                    mev.time_ms - titlebar_last_click_ms <= TITLEBAR_DOUBLE_CLICK_MS) {
                    titlebar_last_click_window = -1;
                    set_focus(mv_index);
                    apply_window_action(mv_index,
                                         windows[mv_index].maximized ? WINDOW_MANAGER_ACTION_RESTORE : WINDOW_MANAGER_ACTION_MAXIMIZE, 0);
                    previous_buttons = mev.buttons;
                    continue;
                }
                titlebar_last_click_window = mv_index;
                titlebar_last_click_ms = mev.time_ms;
                drag_mode = DRAG_MOVE;
                drag_window = mv_index;
                drag_start_cursor_x = cursor_x;
                drag_start_cursor_y = cursor_y;
                drag_start_x = windows[mv_index].x;
                drag_start_y = windows[mv_index].y;
                set_focus(mv_index);
                previous_buttons = mev.buttons;
                continue;
            }

            focus_window_under_cursor();
        } else if (right_down_edge) {
            int tb_index = z_hit_test(cursor_x, cursor_y, WCLASS_ORDINARY, HIT_TITLEBAR, 0);
            if (tb_index >= 0) {
                set_focus(tb_index);
                wmenu_open_at(tb_index, cursor_x, cursor_y);
                previous_buttons = mev.buttons;
                continue;
            }
            focus_window_under_cursor();
        }

        int hovered_panel = window_under_cursor();
        if (hovered_panel >= 0 && !windows[hovered_panel].is_panel) {
            hovered_panel = -1;
        }
        if (last_hovered_panel >= 0 && last_hovered_panel != hovered_panel &&
            windows[last_hovered_panel].alive && windows[last_hovered_panel].is_panel) {
            const window_t *left = &windows[last_hovered_panel];
            window_manager_event_t ev = {0};
            ev.type = WINDOW_MANAGER_EVENT_MOUSE_MOVE;
            ev.x = cursor_x - left->x;
            ev.y = cursor_y - left->y;
            ev.buttons = mev.buttons;
            ev.time_ms = mev.time_ms;
            send_event(left, &ev);
        }
        last_hovered_panel = hovered_panel;

        int target = hovered_panel >= 0 ? hovered_panel : focused_window;
        if (target >= 0) {
            const window_t *win = &windows[target];
            window_manager_event_t ev = {0};
            ev.type = WINDOW_MANAGER_EVENT_MOUSE_MOVE;
            ev.x = cursor_x - win->x;
            ev.y = cursor_y - win->y;
            ev.buttons = mev.buttons;
            ev.time_ms = mev.time_ms;
            send_event(win, &ev);
            if (mev.buttons != previous_buttons) {
                ev.type = WINDOW_MANAGER_EVENT_MOUSE_BUTTON;
                send_event(win, &ev);
            }
        }
        previous_buttons = mev.buttons;
    }
}

static void alt_tab_cycle(int direction) {
    if (z_count == 0) {
        return;
    }
    int start = focused_window >= 0 ? z_position_of(focused_window) : z_count;
    if (start < 0) {
        start = z_count;
    }
    int step = -direction;
    for (int n = 1; n <= z_count; n++) {
        int z = (start + n * step) % z_count;
        if (z < 0) {
            z += z_count;
        }
        int idx = zorder[z];
        if (windows[idx].alive && !windows[idx].is_panel && !windows[idx].is_desktop &&
            window_here(&windows[idx])) {
            apply_window_action(idx, WINDOW_MANAGER_ACTION_FOCUS, 0);
            return;
        }
    }
}

static void run_shortcut(int id) {
    switch (id) {
    case SHORTCUT_CYCLE_FORWARD:
        alt_tab_cycle(1);
        return;
    case SHORTCUT_CYCLE_BACKWARD:
        alt_tab_cycle(-1);
        return;
    case SHORTCUT_LAUNCHER:
        launcher_set_open(!launcher_open);
        return;
    case SHORTCUT_WORKSPACE_PREVIOUS:
        switch_workspace(current_workspace - 1);
        return;
    case SHORTCUT_WORKSPACE_NEXT:
        switch_workspace(current_workspace + 1);
        return;
    case SHORTCUT_TASK_MANAGER: {
        long rc = sys_spawn(PATH_BIN_DIRECTORY "task_manager", "");
        child_track(rc);
        if (rc < 0) {
            toast_post(WINDOW_MANAGER_NOTIFY_ERROR, "Task manager", spawn_error_message(rc));
        }
        return;
    }
    default:
        break;
    }

    if (focused_window < 0 || !windows[focused_window].alive) {
        return;
    }
    int idx = focused_window;
    switch (id) {
    case SHORTCUT_CLOSE_WINDOW:
        apply_window_action(idx, WINDOW_MANAGER_ACTION_CLOSE, 0);
        break;
    case SHORTCUT_SNAP_LEFT:
        apply_window_action(idx, WINDOW_MANAGER_ACTION_SNAP_LEFT, 0);
        break;
    case SHORTCUT_SNAP_RIGHT:
        apply_window_action(idx, WINDOW_MANAGER_ACTION_SNAP_RIGHT, 0);
        break;
    case SHORTCUT_MAXIMIZE:
        apply_window_action(idx, WINDOW_MANAGER_ACTION_MAXIMIZE, 0);
        break;
    case SHORTCUT_WINDOW_TO_PREVIOUS:
        move_window_to_workspace(idx, current_workspace - 1);
        break;
    case SHORTCUT_WINDOW_TO_NEXT:
        move_window_to_workspace(idx, current_workspace + 1);
        break;
    case SHORTCUT_MINIMIZE:
        if (windows[idx].maximized) {
            apply_window_action(idx, WINDOW_MANAGER_ACTION_RESTORE, 0);
        } else {
            apply_window_action(idx, WINDOW_MANAGER_ACTION_TOGGLE_MINIMIZE, 0);
        }
        break;
    default:
        break;
    }
}

static void handle_keyboard(void) {
    char ch;
    while (sys_keyboard_read(&ch)) {
        long mods = sys_keyboard_modifiers();
        int shortcut = shortcut_lookup(ch, (int)mods);
        if (shortcut != SHORTCUT_NONE) {
            run_shortcut(shortcut);
            continue;
        }
        if (launcher_open) {
            launcher_key(ch);
            continue;
        }
        if (focused_window >= 0) {
            window_manager_event_t ev = {0};
            ev.type = WINDOW_MANAGER_EVENT_KEY;
            ev.ch = ch;
            ev.mods = (uint8_t)mods;
            send_event(&windows[focused_window], &ev);
        }
    }
}

#define SESSION_PATH   PATH_ETC_DIRECTORY "session.conf"
#define SESSION_MAX    8

static int session_enabled;
static int session_relaunch;

static session_entry_t session_pending[SESSION_MAX];
static int session_pending_count;

static int format_session_line(char *out, int cap, const char *prog,
                                int32_t x, int32_t y, int32_t w, int32_t h,
                                int8_t workspace) {
    int n = 0;
    for (const char *c = prog; *c; c++) {
        if (n >= cap - 1) { return -1; }
        out[n++] = *c;
    }
    const int32_t vals[5] = {x, y, w, h, (int32_t)workspace};
    for (int i = 0; i < 5; i++) {
        if (n >= cap - 1) { return -1; }
        out[n++] = ' ';
        int32_t v = vals[i];
        if (v < 0) {
            if (n >= cap - 1) { return -1; }
            out[n++] = '-';
            v = -v;
        }
        char temporary[12];
        int t = 0;
        if (v == 0) { temporary[t++] = '0'; }
        while (v > 0) { temporary[t++] = (char)('0' + v % 10); v /= 10; }
        while (t > 0) {
            if (n >= cap - 1) { return -1; }
            out[n++] = temporary[--t];
        }
    }
    if (n >= cap - 1) { return -1; }
    out[n++] = '\n';
    return n;
}

static int parse_session_line(const char *in, int length, session_entry_t *out) {
    int p = 0;
    int n = 0;
    while (p < length && in[p] != ' ' && in[p] != '\n' && n < (int)sizeof(out->program) - 1) {
        out->program[n++] = in[p++];
    }
    out->program[n] = '\0';
    if (n == 0) { return -1; }
    int32_t vals[5];
    for (int i = 0; i < 5; i++) {
        while (p < length && in[p] == ' ') { p++; }
        int neg = 0;
        if (p < length && in[p] == '-') { neg = 1; p++; }
        if (p >= length || in[p] < '0' || in[p] > '9') { return -1; }
        int32_t v = 0;
        while (p < length && in[p] >= '0' && in[p] <= '9') {
            v = v * 10 + (in[p++] - '0');
        }
        vals[i] = neg ? -v : v;
    }
    while (p < length && in[p] != '\n') { p++; }
    if (p < length) { p++; }
    out->x = vals[0];
    out->y = vals[1];
    out->w = vals[2];
    out->h = vals[3];
    out->workspace = (int8_t)vals[4];
    return p;
}

#define SESSION_TASKS 40
static void session_program_for_pid(int pid, char *out, int cap) {
    static task_info_t infos[SESSION_TASKS];
    out[0] = '\0';
    long n = sys_taskinfo(infos, SESSION_TASKS);
    for (long i = 0; i < n; i++) {
        if (infos[i].pid == pid) {
            int j = 0;
            for (; infos[i].name[j] && j < cap - 1; j++) {
                out[j] = infos[i].name[j];
            }
            out[j] = '\0';
            return;
        }
    }
}

static uint32_t session_signature(void) {
    uint32_t sig = 0;
    for (int i = 0; i < window_count; i++) {
        window_t *w = &windows[i];
        if (!w->alive || w->is_panel || w->is_desktop) {
            continue;
        }
        sig = sig * 31u + (uint32_t)w->client_pid;
        sig = sig * 31u + (uint32_t)w->x;
        sig = sig * 31u + (uint32_t)w->y;
        sig = sig * 31u + (uint32_t)w->w;
        sig = sig * 31u + (uint32_t)w->h;
        sig = sig * 31u + (uint32_t)w->workspace;
        sig = sig * 31u + (uint32_t)(w->minimized ? 1 : 0);
    }
    return sig;
}

static void session_save(void) {
    if (!session_enabled) {
        return;
    }
    char buffer[512];
    int n = 0;
    int saved = 0;
    for (int i = 0; i < window_count && saved < SESSION_MAX; i++) {
        window_t *w = &windows[i];
        if (!w->alive || w->is_panel || w->is_desktop || w->is_popup) {
            continue;
        }
        char prog[24];
        session_program_for_pid(w->client_pid, prog, sizeof(prog));
        if (!prog[0]) {
            continue;
        }
        int wrote = format_session_line(buffer + n, (int)sizeof(buffer) - n, prog,
                                         w->x, w->y, w->w, w->h, w->workspace);
        if (wrote <= 0) {
            break;
        }
        n += wrote;
        saved++;
    }
    int ok = sys_writefile(SESSION_PATH, buffer, (size_t)n) == 0;
    char message[64];
    int m = 0;
    static const char pre[] = "[wm] session: ";
    for (int i = 0; pre[i]; i++) {
        message[m++] = pre[i];
    }
    m += format_uint((uint32_t)saved, message + m);
    const char *tail = ok ? " window(s) saved\n" : " window(s) could not be saved\n";
    for (int i = 0; tail[i]; i++) {
        message[m++] = tail[i];
    }
    sys_write(1, message, (size_t)m);
}

static void session_restore(void) {
    if (!session_enabled) {
        return;
    }
    static char buffer[512];
    long n = sys_readfile(SESSION_PATH, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        static const char none[] = "[wm] session: nothing saved from last time\n";
        sys_write(1, none, sizeof(none) - 1);
        return;
    }
    buffer[n] = '\0';

    int position = 0;
    while (position < n && session_pending_count < SESSION_MAX) {
        session_entry_t e;
        int used = parse_session_line(buffer + position, (int)(n - position), &e);
        if (used <= 0) {
            break;
        }
        position += used;
        char path[PATH_MAX_LENGTH];
        if (path_join(path, PATH_BIN_DIRECTORY, e.program) != 0) {
            continue;
        }
        if (session_relaunch) {
            long pid = sys_spawn(path, "");
            if (pid < 0) {
                continue;
            }
            child_track(pid);
        }
        e.claimed = 0;
        session_pending[session_pending_count++] = e;
    }
    char message[64];
    int m = 0;
    static const char pre[] = "[wm] session: ";
    for (int i = 0; pre[i]; i++) {
        message[m++] = pre[i];
    }
    m += format_uint((uint32_t)session_pending_count, message + m);
    if (session_relaunch) {
        static const char post[] = " window(s) relaunched\n";
        for (int i = 0; post[i]; i++) {
            message[m++] = post[i];
        }
    } else {
        static const char post[] = " window(s) expected back\n";
        for (int i = 0; post[i]; i++) {
            message[m++] = post[i];
        }
    }
    sys_write(1, message, (size_t)m);
}

static session_entry_t *session_claim(int client_pid) {
    if (!session_enabled || session_pending_count == 0) {
        return 0;
    }
    char prog[24];
    session_program_for_pid(client_pid, prog, sizeof(prog));
    if (!prog[0]) {
        return 0;
    }
    for (int i = 0; i < session_pending_count; i++) {
        if (session_pending[i].claimed) {
            continue;
        }
        int same = 1;
        for (int j = 0;; j++) {
            if (session_pending[i].program[j] != prog[j]) {
                same = 0;
                break;
            }
            if (!prog[j]) {
                break;
            }
        }
        if (same) {
            session_pending[i].claimed = 1;
            return &session_pending[i];
        }
    }
    return 0;
}

/* M197. An idle desktop woke this loop a hundred times a second to run
   twenty-odd syscalls that found nothing, and redrew and presented the whole
   screen once a second besides. Input, a client's present, a request and a
   client's exit all end the wait already; what is left is the handful of
   things with a deadline, so the wait ends at the nearest of them. The cap
   is a backstop for anything that forgets to wake us, not a frame rate. */
#define IDLE_WAIT_CAP_MS 250

static int idle_wait_ms(void) {
    long now = sys_uptime_ms();
    long due = now + IDLE_WAIT_CAP_MS;
    for (int i = 0; i < toast_count; i++) {
        if (toasts[i].expires_ms < due) {
            due = toasts[i].expires_ms;
        }
    }
    for (int i = 0; i < window_count; i++) {
        if (windows[i].alive && windows[i].close_requested && windows[i].close_deadline_ms > 0 &&
            windows[i].close_deadline_ms < due) {
            due = windows[i].close_deadline_ms;
        }
    }
    if (mode_revert_at_ms != 0 && mode_revert_at_ms < due) {
        due = mode_revert_at_ms;
    }
    if (shutdown_pending_mode != POWER_CONFIRM_NONE && now + 10 < due) {
        due = now + 10;
    }
    return due > now ? (int)(due - now) : 0;
}

int main(void) {
    {
        const char *sess = getenv("LEANOS_SESSION");
        session_enabled = sess != 0;
        session_relaunch = sess != 0 && strcmp(sess, "reconnect") != 0;
    }
    if (read_display_modes() != 0) {
        sys_exit(1);
    }
    long framebuffer_vaddr = sys_framebuffer_map();
    if (framebuffer_vaddr < 0) {
        sys_exit(1);
    }
    real_framebuffer = (uint32_t *)framebuffer_vaddr;

    {
        uint32_t saved_w = 0, saved_h = 0;
        if (settings_file_load_display(&saved_w, &saved_h) &&
            (saved_w != framebuffer_info.width || saved_h != framebuffer_info.height)) {
            if (sys_display_set_mode(saved_w, saved_h) == 0) {
                read_display_modes();
                long remapped = sys_framebuffer_map();
                if (remapped >= 0) {
                    real_framebuffer = (uint32_t *)remapped;
                }
            }
        }
    }

    back_shared_memory_id = sys_shared_memory_create((size_t)framebuffer_info.width * framebuffer_info.height * sizeof(uint32_t));
    long back_vaddr = back_shared_memory_id < 0 ? -1 : sys_shared_memory_map(back_shared_memory_id);
    if (back_vaddr < 0) {
        sys_exit(1);
    }
    back_buffer = (uint32_t *)back_vaddr;
    back_pitch_pixels = framebuffer_info.width;

    self_pid = (int32_t)sys_getpid();

    cursor_x = (int32_t)(framebuffer_info.width / 2);
    cursor_y = (int32_t)(framebuffer_info.height / 2);

    {
        window_manager_settings_request_t saved;
        saved.volume = audio_volume;
        saved.animations = (uint32_t)animations_enabled;
        saved.bg_color = bg_color;
        saved.accent_color = accent_color;
        saved.wallpaper = wallpaper_id;
        if (settings_file_load(&saved)) {
            if (theme_is_readable(saved.bg_color, saved.accent_color)) {
                bg_color = saved.bg_color;
                accent_color = saved.accent_color;
            }
            wallpaper_id = saved.wallpaper;
            animations_enabled = saved.animations != 0;
            audio_volume = saved.volume > 100 ? 100 : saved.volume;
        }
        sys_audio_claim();
        sys_audio_volume(audio_volume);
    }

    int request_file_descriptors[2];
    int response_file_descriptors[2];
    if (sys_pipe_open(WINDOW_MANAGER_REQUEST_PIPE, request_file_descriptors) != 0 || sys_pipe_open(WINDOW_MANAGER_RESPONSE_PIPE, response_file_descriptors) != 0) {
        sys_exit(1);
    }
    int query_file_descriptors[2];
    int query_response_file_descriptors[2];
    int action_file_descriptors[2];
    int settings_file_descriptors[2];
    int settings_query_file_descriptors[2];
    int settings_query_response_file_descriptors[2];
    int notify_file_descriptors[2];
    int drag_file_descriptors[2];
    int drag_data_file_descriptors[2];
    if (sys_pipe_open(WINDOW_MANAGER_QUERY_PIPE, query_file_descriptors) != 0 || sys_pipe_open(WINDOW_MANAGER_QUERY_RESPONSE_PIPE, query_response_file_descriptors) != 0 ||
        sys_pipe_open(WINDOW_MANAGER_ACTION_PIPE, action_file_descriptors) != 0 || sys_pipe_open(WINDOW_MANAGER_SETTINGS_PIPE, settings_file_descriptors) != 0 ||
        sys_pipe_open(WINDOW_MANAGER_SETTINGS_QUERY_PIPE, settings_query_file_descriptors) != 0 ||
        sys_pipe_open(WINDOW_MANAGER_SETTINGS_QUERY_RESPONSE_PIPE, settings_query_response_file_descriptors) != 0 ||
        sys_pipe_open(WINDOW_MANAGER_NOTIFY_PIPE, notify_file_descriptors) != 0 ||
        sys_pipe_open(WINDOW_MANAGER_DRAG_PIPE, drag_file_descriptors) != 0 ||
        sys_pipe_open(WINDOW_MANAGER_DRAG_DATA_PIPE, drag_data_file_descriptors) != 0) {
        sys_exit(1);
    }
    drag_data_write_file_descriptor = drag_data_file_descriptors[1];

    sys_pipe_reset(request_file_descriptors[0]);
    sys_pipe_reset(response_file_descriptors[0]);
    sys_pipe_reset(query_file_descriptors[0]);
    sys_pipe_reset(query_response_file_descriptors[0]);
    sys_pipe_reset(action_file_descriptors[0]);
    sys_pipe_reset(settings_file_descriptors[0]);
    sys_pipe_reset(settings_query_file_descriptors[0]);
    sys_pipe_reset(settings_query_response_file_descriptors[0]);
    sys_pipe_reset(notify_file_descriptors[0]);
    sys_pipe_reset(drag_file_descriptors[0]);
    sys_pipe_reset(drag_data_file_descriptors[0]);
    session_restore();

    const char message[] = "[compositor] framebuffer mapped, accepting windows.\n";
    sys_write(1, message, strlen(message));

    redraw();
    dirty = 0;
    last_drawn_cursor_x = cursor_x;
    last_drawn_cursor_y = cursor_y;

    for (;;) {
        accept_pending_window(request_file_descriptors[0], response_file_descriptors[1]);
        accept_pending_query(query_file_descriptors[0], query_response_file_descriptors[1]);
        accept_pending_action(action_file_descriptors[0]);
        accept_pending_settings(settings_file_descriptors[0]);
        accept_pending_settings_query(settings_query_file_descriptors[0], settings_query_response_file_descriptors[1]);
        accept_pending_notify(notify_file_descriptors[0]);
        accept_pending_drag(drag_file_descriptors[0]);
        reap_dead_clients();
        child_reap();
        handle_mouse();
        handle_keyboard();

        long now = sys_uptime_ms();
        if (mode_revert_at_ms != 0 && now >= mode_revert_at_ms) {
            uint32_t w = mode_previous_w, h = mode_previous_h;
            mode_revert_at_ms = 0;
            if (apply_display_mode(w, h) == 0) {
                toast_post(WINDOW_MANAGER_NOTIFY_WARN, "Display reverted",
                            "Nobody confirmed the new resolution");
            }
        }
        toasts_expire(now);
        shutdown_tick(now);
        if (session_enabled) {
            static uint32_t last_sig;
            static uint32_t settling_sig;
            static long next_session_check;
            if (anim_any_active()) {
                next_session_check = now + 500;
            } else if (now >= next_session_check) {
                next_session_check = now + 500;
                uint32_t sig = session_signature();
                if (sig != last_sig) {
                    if (sig == settling_sig) {
                        last_sig = sig;
                        session_save();
                    } else {
                        settling_sig = sig;
                    }
                }
            }
        }
        if ((anim_any_active() || frame_settle_owed) && now >= frame_due_ms) {
            int32_t ax0, ay0, ax1, ay1;
            if (last_frame_ms != 0 && now - last_frame_ms > worst_gap_ms) {
                worst_gap_ms = now - last_frame_ms;
            }
            last_frame_ms = now;
            frame_due_ms = now + FRAME_MS;
            frame_settle_owed = anim_any_active();
            long began = sys_uptime_ms();
            int painted = 0;
            if (anim_step(now, &ax0, &ay0, &ax1, &ay1)) {
                redraw_rect(ax0, ay0, ax1, ay1);
                painted = 1;
            }
            if (launcher_fading()) {
                int32_t lx, ly;
                launcher_rect(&lx, &ly);
                redraw_rect(lx, ly, lx + LAUNCHER_W, ly + LAUNCHER_H);
                painted = 1;
            } else {
                launcher_fade_start_ms = 0;
            }
            if (snap_fading()) {
                redraw_rect(snap_preview_x, snap_preview_y,
                             snap_preview_x + snap_preview_w, snap_preview_y + snap_preview_h);
                painted = 1;
            } else {
                snap_fade_start_ms = 0;
            }
            if (painted) {
                long took = sys_uptime_ms() - began;
                frames_drawn++;
                if (took > FRAME_BUDGET_MS) {
                    frames_over_budget++;
                }
                if (took > worst_frame_ms) {
                    worst_frame_ms = took;
                }
            }
            if (!anim_any_active()) {
                dirty = 1;
                if (frames_drawn > 0) {
                    char message[128];
                    int n = 0;
                    static const char pre[] = "[wm] animation: ";
                    for (int i = 0; pre[i]; i++) {
                        message[n++] = pre[i];
                    }
                    n += format_uint(frames_drawn, message + n);
                    static const char mid[] = " frames, longest composite ";
                    for (int i = 0; mid[i]; i++) {
                        message[n++] = mid[i];
                    }
                    n += format_uint((uint32_t)worst_frame_ms, message + n);
                    static const char mid2[] = " ms, longest gap ";
                    for (int i = 0; mid2[i]; i++) {
                        message[n++] = mid2[i];
                    }
                    n += format_uint((uint32_t)worst_gap_ms, message + n);
                    static const char post[] = " ms\n[perf] anim_frame_gap_ms ";
                    for (int i = 0; post[i]; i++) {
                        message[n++] = post[i];
                    }
                    n += format_uint((uint32_t)worst_gap_ms, message + n);
                    static const char unit[] = " ms\n";
                    for (int i = 0; unit[i]; i++) {
                        message[n++] = unit[i];
                    }
                    sys_write(1, message, (size_t)n);
                }
                frames_over_budget = 0;
                frames_drawn = 0;
                worst_frame_ms = 0;
                worst_gap_ms = 0;
                last_frame_ms = 0;
            }
        }

        if (dirty) {
            redraw();
            dirty = 0;
            present_pending = 0;
            last_drawn_cursor_x = cursor_x;
            last_drawn_cursor_y = cursor_y;
        } else if (present_pending && !anim_any_active()) {
            int32_t x0 = present_x0, y0 = present_y0, x1 = present_x1, y1 = present_y1;
            if (cursor_x != last_drawn_cursor_x || cursor_y != last_drawn_cursor_y) {
                x0 = min_i32(x0, min_i32(last_drawn_cursor_x, cursor_x));
                y0 = min_i32(y0, min_i32(last_drawn_cursor_y, cursor_y));
                x1 = max_i32(x1, max_i32(last_drawn_cursor_x, cursor_x) + CURSOR_SIZE);
                y1 = max_i32(y1, max_i32(last_drawn_cursor_y, cursor_y) + CURSOR_SIZE);
            }
            redraw_rect(x0, y0, x1, y1);
            present_pending = 0;
            last_drawn_cursor_x = cursor_x;
            last_drawn_cursor_y = cursor_y;
        } else if (cursor_x != last_drawn_cursor_x || cursor_y != last_drawn_cursor_y) {
            int32_t x0 = min_i32(last_drawn_cursor_x, cursor_x);
            int32_t y0 = min_i32(last_drawn_cursor_y, cursor_y);
            int32_t x1 = max_i32(last_drawn_cursor_x, cursor_x) + CURSOR_SIZE;
            int32_t y1 = max_i32(last_drawn_cursor_y, cursor_y) + CURSOR_SIZE;
            redraw_rect(x0, y0, x1, y1);
            last_drawn_cursor_x = cursor_x;
            last_drawn_cursor_y = cursor_y;
        }

        int wait_file_descriptors[10];
        int nwait = 0;
        wait_file_descriptors[nwait++] = 0;
        wait_file_descriptors[nwait++] = request_file_descriptors[0];
        wait_file_descriptors[nwait++] = query_file_descriptors[0];
        wait_file_descriptors[nwait++] = action_file_descriptors[0];
        wait_file_descriptors[nwait++] = settings_file_descriptors[0];
        wait_file_descriptors[nwait++] = settings_query_file_descriptors[0];
        wait_file_descriptors[nwait++] = notify_file_descriptors[0];
        wait_file_descriptors[nwait++] = drag_file_descriptors[0];
        if (anim_any_active() || launcher_fading() || snap_fading()) {
            long remaining = frame_due_ms - sys_uptime_ms();
            sys_waitfds(wait_file_descriptors, nwait, remaining > 0 ? (int)remaining : 0);
        } else {
            sys_waitfds(wait_file_descriptors, nwait, idle_wait_ms());
        }
    }
}
