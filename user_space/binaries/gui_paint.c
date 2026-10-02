#include "bitmap_file.h"
#include "malloc.h"
#include "paths.h"
#include "string_utilities.h"
#include "syscall_wrappers.h"
#include "window_manager_client.h"

#include <stdio.h>

#define WIN_W 520
#define WIN_H 380
#define TOOLBAR_H 40
#define TOOLBAR_COLOR  0x002A2F3Au
#define TOOLBAR_EDGE   0x003C4250u
#define PAPER_COLOR    0x00FFFFFFu
#define TEXT_COLOR     0x00F2F4F8u
#define SWATCH_SIZE    20
#define SWATCH_GAP     6
#define SWATCH_X       10
#define BRUSH_X        226
#define BRUSH_SLOT     26
#define BUTTON_W       58
#define BUTTON_H       26
#define CLEAR_X        (WIN_W - 2 * BUTTON_W - 18)
#define SAVE_X         (WIN_W - BUTTON_W - 10)
#define PAINTINGS_MAX  999

static const uint32_t PALETTE[] = {
    0x00202020u, 0x00E74C3Cu, 0x00F39C12u, 0x00F1C40Fu,
    0x002ECC71u, 0x003498DBu, 0x009B59B6u, 0x00FFFFFFu,
};
#define PALETTE_COUNT ((int)(sizeof(PALETTE) / sizeof(PALETTE[0])))

static const int32_t BRUSHES[] = {3, 7, 15};
#define BRUSH_COUNT ((int)(sizeof(BRUSHES) / sizeof(BRUSHES[0])))

#define CANVAS_W WIN_W
#define CANVAS_H (WIN_H - TOOLBAR_H)

/* M211. The painting lives in a buffer of its own rather than only in the
   window's pixels, so the toolbar can be redrawn over nothing and Save has
   the picture without the chrome around it. */
static uint32_t *paper;
static int colour_index = 1;
static int brush_index = 1;
static int32_t last_x = -1;
static int32_t last_y = -1;

static void stamp(int32_t cx, int32_t cy) {
    int32_t radius = BRUSHES[brush_index] / 2;
    for (int32_t dy = -radius; dy <= radius; dy++) {
        for (int32_t dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy > radius * radius + radius) {
                continue;
            }
            int32_t x = cx + dx;
            int32_t y = cy + dy;
            if (x >= 0 && x < CANVAS_W && y >= 0 && y < CANVAS_H) {
                paper[y * CANVAS_W + x] = PALETTE[colour_index];
            }
        }
    }
}

static void stroke_to(int32_t x, int32_t y) {
    if (last_x < 0) {
        stamp(x, y);
    } else {
        int32_t dx = x - last_x;
        int32_t dy = y - last_y;
        int32_t steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
        if (steps == 0) {
            steps = 1;
        }
        for (int32_t i = 1; i <= steps; i++) {
            stamp(last_x + dx * i / steps, last_y + dy * i / steps);
        }
    }
    last_x = x;
    last_y = y;
}

static void clear_paper(void) {
    for (int32_t i = 0; i < CANVAS_W * CANVAS_H; i++) {
        paper[i] = PAPER_COLOR;
    }
}

static int32_t swatch_x(int i) {
    return SWATCH_X + i * (SWATCH_SIZE + SWATCH_GAP);
}

static void draw_toolbar(graphics_context_t *g) {
    graphics_fill_rect(g, 0, 0, WIN_W, TOOLBAR_H, TOOLBAR_COLOR);
    graphics_fill_rect(g, 0, TOOLBAR_H - 1, WIN_W, 1, TOOLBAR_EDGE);
    int32_t top = (TOOLBAR_H - SWATCH_SIZE) / 2;
    for (int i = 0; i < PALETTE_COUNT; i++) {
        if (i == colour_index) {
            graphics_stroke_rounded(g, swatch_x(i) - 3, top - 3, SWATCH_SIZE + 6, SWATCH_SIZE + 6, 8,
                                    0x00FFFFFFu, 230);
        }
        graphics_fill_rounded(g, swatch_x(i), top, SWATCH_SIZE, SWATCH_SIZE, 6, PALETTE[i], 255);
        graphics_stroke_rounded(g, swatch_x(i), top, SWATCH_SIZE, SWATCH_SIZE, 6, 0x00000000u, 60);
    }
    for (int i = 0; i < BRUSH_COUNT; i++) {
        int32_t x = BRUSH_X + i * BRUSH_SLOT;
        if (i == brush_index) {
            graphics_fill_rounded(g, x, top - 2, BRUSH_SLOT - 4, SWATCH_SIZE + 4, 6, 0x00454C5Cu, 255);
        }
        int32_t d = BRUSHES[i] < 14 ? BRUSHES[i] : 14;
        graphics_fill_rounded(g, x + (BRUSH_SLOT - 4 - d) / 2, top + (SWATCH_SIZE - d) / 2, d, d, d / 2,
                              TEXT_COLOR, 255);
    }
    graphics_draw_button_state(g, CLEAR_X, (TOOLBAR_H - BUTTON_H) / 2, BUTTON_W, BUTTON_H, 0x003A4150u,
                               0x00505868u, "Clear", TEXT_COLOR, 0);
    graphics_draw_button_state(g, SAVE_X, (TOOLBAR_H - BUTTON_H) / 2, BUTTON_W, BUTTON_H, 0x004C99E6u,
                               0x004C99E6u, "Save", TEXT_COLOR, 0);
}

static void draw_paper(graphics_context_t *g) {
    for (int32_t y = 0; y < CANVAS_H && TOOLBAR_H + y < g->height; y++) {
        memcpy(g->pixels + (size_t)(TOOLBAR_H + y) * (size_t)g->width, paper + (size_t)y * CANVAS_W,
               (size_t)CANVAS_W * sizeof(uint32_t));
    }
}

static void save_painting(void) {
    os_stat_t status;
    if (sys_stat(PATH_PICTURES, &status) != 0) {
        sys_mkdir(PATH_PICTURES);
    }
    char path[PATH_MAX_LENGTH];
    int number = 1;
    for (; number <= PAINTINGS_MAX; number++) {
        snprintf(path, sizeof(path), PATH_PICTURES "/Painting %d.bmp", number);
        if (sys_stat(path, &status) != 0) {
            break;
        }
    }
    char body[WINDOW_MANAGER_NOTIFY_BODY_MAX];
    if (number > PAINTINGS_MAX || bitmap_file_write(path, paper, CANVAS_W, CANVAS_H, CANVAS_W) != 0) {
        window_manager_notify(WINDOW_MANAGER_NOTIFY_ERROR, "Paint", "The painting could not be saved.");
        return;
    }
    printf("[paint] saved %s\n", path);
    snprintf(body, sizeof(body), "Saved Painting %d in Pictures.", number);
    window_manager_notify(WINDOW_MANAGER_NOTIFY_INFO, "Paint", body);
}

static int toolbar_click(int32_t x, int32_t y) {
    int32_t top = (TOOLBAR_H - SWATCH_SIZE) / 2;
    for (int i = 0; i < PALETTE_COUNT; i++) {
        if (graphics_point_in_rect(x, y, swatch_x(i) - 2, top - 2, SWATCH_SIZE + 4, SWATCH_SIZE + 4)) {
            colour_index = i;
            return 1;
        }
    }
    for (int i = 0; i < BRUSH_COUNT; i++) {
        if (graphics_point_in_rect(x, y, BRUSH_X + i * BRUSH_SLOT, top - 2, BRUSH_SLOT - 4, SWATCH_SIZE + 4)) {
            brush_index = i;
            return 1;
        }
    }
    if (graphics_point_in_rect(x, y, CLEAR_X, (TOOLBAR_H - BUTTON_H) / 2, BUTTON_W, BUTTON_H)) {
        clear_paper();
        return 1;
    }
    if (graphics_point_in_rect(x, y, SAVE_X, (TOOLBAR_H - BUTTON_H) / 2, BUTTON_W, BUTTON_H)) {
        save_painting();
        return 1;
    }
    return 0;
}

int main(void) {
    window_manager_window_t win;
    if (window_manager_connect(WIN_W, WIN_H, "Paint", &win) != 0) {
        sys_exit(1);
    }
    paper = (uint32_t *)malloc((size_t)CANVAS_W * CANVAS_H * sizeof(uint32_t));
    if (!paper) {
        sys_exit(1);
    }
    clear_paper();
    draw_toolbar(&win.graphics);
    draw_paper(&win.graphics);
    window_manager_present(&win);
    uint8_t previous_buttons = 0;

    for (;;) {
        window_manager_event_t ev;
        window_manager_wait_event(&win, &ev);
        int changed = 0;

        if (ev.type == WINDOW_MANAGER_EVENT_EXPOSE || ev.type == WINDOW_MANAGER_EVENT_DISPLAY_CHANGED) {
            changed = 1;
        } else if (ev.type == WINDOW_MANAGER_EVENT_KEY) {
            if ((ev.mods & KEYBOARD_MOD_CTRL) && (ev.ch == 's' || ev.ch == 'S')) {
                save_painting();
            } else if (!(ev.mods & KEYBOARD_MOD_CTRL) && (ev.ch == 'c' || ev.ch == 'C')) {
                clear_paper();
                changed = 1;
            } else if (ev.ch >= '1' && ev.ch < '1' + PALETTE_COUNT) {
                colour_index = ev.ch - '1';
                changed = 1;
            }
        } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_MOVE || ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON) {
            int pressed = (ev.buttons & 1) && !(previous_buttons & 1);
            if (pressed && ev.y < TOOLBAR_H) {
                changed = toolbar_click(ev.x, ev.y);
            } else if ((ev.buttons & 1) && ev.y >= TOOLBAR_H && (pressed || last_x >= 0)) {
                stroke_to(ev.x, ev.y - TOOLBAR_H);
                changed = 1;
            }
            if (!(ev.buttons & 1)) {
                last_x = -1;
                last_y = -1;
            }
            previous_buttons = ev.buttons;
        }

        if (changed) {
            draw_toolbar(&win.graphics);
            draw_paper(&win.graphics);
            window_manager_present(&win);
        }
    }
}
