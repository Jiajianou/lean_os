/* user_space/bin/desktop_icons.c
 *
 * The literal "desktop" in "desktop environment": a chrome-less, full-
 * screen background window (wmclient.h's wm_connect_desktop -
 * wm_create_request_t.desktop) drawn underneath every ordinary window and
 * panel (compositor.c's M22-mirroring addition), holding one clickable
 * icon. Double-clicking it (two left-button presses on the same icon
 * within DOUBLE_CLICK_MS of each other - there's no drag/select/rename,
 * so "double-click to launch" is the entire interaction this needs) spawns
 * user_space/bin/gui_terminal.c, the same way desktop_shell.c's launcher
 * slots spawn a program on a single click - a desktop icon is just a
 * different (and, for exactly one program, more familiar) way to reach
 * the same sys_spawn call.
 *
 * M44: this window's background is no longer a flat constant of its own.
 * It paints whichever wallpaper style is currently chosen
 * (user_space/lib/wallpaper.h) over whichever background color is
 * currently chosen - both read from the compositor, which holds them
 * (system_api/include/wm.h's wm_settings_request_t) and which settings.c
 * writes. That also fixes something that had been quietly broken since
 * M32: settings.c's "Desktop color" picker changed the *compositor's*
 * background, which this full-screen window has covered completely ever
 * since it existed, so the control had had no visible effect on a real
 * desktop for eleven milestones.
 *
 * M32: ICONS[] below is the whole "multiple icons" story - a real
 * top-to-bottom, wrap-to-a-new-column grid (layout_icons) laid out once
 * at connect time from the window's own actual height (never hardcoded),
 * leaving PANEL_MARGIN clear at the bottom so an icon in the last row
 * never ends up drawn underneath desktop_shell.c's panel (which is
 * always on top regardless of connection order - compositor.c's own
 * z-order rule - so a covered icon would be genuinely unreachable, not
 * just visually crowded). Everything else (double-click detection,
 * press/hover redraw) is the same per-icon logic the single hardcoded
 * icon already had, just indexed now instead of hardcoded to one.
 */
#include "children.h" /* M54: this process launches things and must reap them - see children.h */
#include "paths.h" /* system_api/include/paths.h - M53: /bin is where programs live now */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "str.h" /* strlen - ICONS[].label is data-driven now, not a compile-time sizeof() */
#include "spawn_error.h" /* system_api/include/spawn_error.h - M48 */
#include "syscall_wrappers.h"
#include "wallpaper.h" /* M44 - see refresh_theme */
#include "wmclient.h"

#define ICON_BOX_COLOR 0x004C99E6u
#define ICON_HOVER_COLOR 0x006CB9FFu
#define ICON_GLYPH_COLOR 0x000A1420u
#define LABEL_COLOR    0x00FFFFFFu

#define ICON_SIZE    48
#define ICON_MARGIN  32  /* top/left/right inset, and gap between grid cells beyond the icon+label's own footprint */
#define ICON_CELL_W  90
#define ICON_CELL_H  90
#define PANEL_MARGIN 40  /* clears desktop_shell.c's PANEL_HEIGHT(32) with room to spare - see this file's header comment on why an icon must never end up under it */
#define DOUBLE_CLICK_MS 500

/* M35: right-click-on-empty-desktop context menu - the standard "quick
 * launch" convention a right-click on a real desktop background gives
 * you, using the same gfx_draw_menu/gfx_menu_hit_test pair text_editor.c's
 * File menu uses (system_api/include/input.h's mouse_event_t.buttons
 * bit1, already decoded end-to-end since M18 - kernel/drivers/mouse.c,
 * compositor.c's own event forwarding - nothing new needed to reach it,
 * just the first client that ever reads it). */
#define CTX_MENU_ITEM_W 120
#define CTX_MENU_ITEM_H (FONT_HEIGHT + 4)
#define CTX_MENU_BG     0x00243040u
#define CTX_MENU_HOVER  0x003A5A80u
#define CTX_MENU_BORDER 0x00506070u
#define CTX_MENU_TEXT   0x00FFFFFFu

static const char *const CTX_MENU_ITEMS[] = {"Terminal", "Settings"};
/* M53: absolute paths, same as ICONS above - a bare name stopped being a location. */
static const char *const CTX_MENU_PROGRAMS[] = {PATH_BIN_DIR "gui_terminal", PATH_BIN_DIR "settings"};
#define CTX_MENU_COUNT ((int)(sizeof(CTX_MENU_ITEMS) / sizeof(CTX_MENU_ITEMS[0])))

static int ctx_menu_open;
static int32_t ctx_menu_x, ctx_menu_y;

/* M44: the desktop's own copy of the two settings it paints with, kept in
 * step by polling the compositor. Polled rather than pushed because the
 * settings channel is one-way by design (settings.c fires and forgets),
 * and because a redraw this size is not something to do on a timer
 * anyway - refresh_theme only reports a change when there actually is
 * one. The defaults here match the compositor's own, so the very first
 * frame - drawn before any reply has arrived - is already right. */
#define DEFAULT_BG_COLOR 0x001A1A2Eu
#define THEME_POLL_MS 500

static uint32_t theme_bg = DEFAULT_BG_COLOR;
static int theme_wallpaper = WALLPAPER_GRADIENT;

static int refresh_theme(void) {
    wm_settings_request_t settings;
    if (wm_query_settings(&settings) != 0) {
        return 0;
    }
    if (settings.bg_color == theme_bg && (int)settings.wallpaper == theme_wallpaper) {
        return 0;
    }
    theme_bg = settings.bg_color;
    theme_wallpaper = (int)settings.wallpaper;
    return 1;
}

/* M48: this is M40's exact symptom - "double-clicking that icon does
 * nothing" - given a voice. The spawn already failed for a specific
 * reason (system_api/include/spawn_error.h); all that was missing was
 * anywhere for that reason to go. Titled with the icon's own label
 * rather than its program name: the person double-clicked "Editor", not
 * "text_editor". */
static void launch(const char *label, const char *program) {
    long rc = sys_spawn(program, "");
    if (rc < 0) {
        wm_notify(WM_NOTIFY_ERROR, label, spawn_error_message(rc));
    }
    child_track(rc); /* M54: so its task slot comes back when it closes - see children.h */
}

typedef struct {
    const char *label;
    const char *program;
    const char *glyph; /* 2-3 chars drawn inside the icon box - this project has no separate icon-image format, see redraw_icon */
} icon_def_t;

/* M53: absolute paths. Every icon named a bare program before, which
 * worked only because the namespace was flat enough for a name to be a
 * location - it is not any more, and an icon that resolved by luck is an
 * icon that stops working the moment somebody creates a file with the
 * same name somewhere else. */
static const icon_def_t ICONS[] = {
    {"Terminal", PATH_BIN_DIR "gui_terminal", ">_"},
    {"Editor",   PATH_BIN_DIR "text_editor",  "Ed"},
    {"Files",    PATH_BIN_DIR "file_manager", "[]"},
    {"Settings", PATH_BIN_DIR "settings",     "**"},
    {"Clock",    PATH_BIN_DIR "gui_clock",    "()"},
    {"Paint",    PATH_BIN_DIR "gui_paint",    "/\\"},
    {"Tasks",    PATH_BIN_DIR "task_manager", "T:"},
};
#define ICON_COUNT ((int)(sizeof(ICONS) / sizeof(ICONS[0])))

static int32_t icon_x[ICON_COUNT], icon_y[ICON_COUNT]; /* filled by layout_icons from the window's own actual geometry */

/* Top-to-bottom columns: place icons one under another until the next one
 * wouldn't clear PANEL_MARGIN above the bottom edge, then start a new
 * column back at the top - the same wrapping shape a real desktop's icon
 * grid uses. `win_h > 0` on the guard (not just "does it fit") is what
 * stops a single icon that itself doesn't fit from wrapping forever. */
static void layout_icons(int32_t win_h) {
    int32_t x = ICON_MARGIN, y = ICON_MARGIN;
    int32_t max_y = win_h - PANEL_MARGIN - ICON_SIZE - FONT_HEIGHT - 4;
    for (int i = 0; i < ICON_COUNT; i++) {
        if (y > max_y && y > ICON_MARGIN) {
            y = ICON_MARGIN;
            x += ICON_CELL_W;
        }
        icon_x[i] = x;
        icon_y[i] = y;
        y += ICON_CELL_H;
    }
}

/* M34: the box-plus-label hit region as one rect (gfx_point_in_rect,
 * user_space/lib/gfx.c) instead of hand-rolled comparisons - top edge is
 * pulled up by FONT_HEIGHT+4 and the height grown by twice that so the
 * label drawn below the box (redraw_icon) is clickable too, not just the
 * box itself. */
static int point_in_icon(int i, int32_t x, int32_t y) {
    return gfx_point_in_rect(x, y, icon_x[i], icon_y[i] - FONT_HEIGHT - 4,
                              ICON_SIZE, ICON_SIZE + 2 * (FONT_HEIGHT + 4));
}

static void redraw_icon(wm_window_t *self, int i, int pressed) {
    uint32_t box_color = pressed ? ICON_HOVER_COLOR : ICON_BOX_COLOR;
    gfx_fill_rect_rounded(&self->gfx, icon_x[i], icon_y[i], ICON_SIZE, ICON_SIZE, box_color);

    /* A tiny glyph inside the icon box: a dark "screen" rect with a
     * short mark on top, entirely gfx primitives (no separate icon-image
     * format exists in this project). */
    gfx_fill_rect(&self->gfx, icon_x[i] + 6, icon_y[i] + 6, ICON_SIZE - 12, ICON_SIZE - 16, ICON_GLYPH_COLOR);
    gfx_draw_text(&self->gfx, icon_x[i] + 10, icon_y[i] + 12, ICONS[i].glyph, box_color);

    int32_t label_w = (int32_t)strlen(ICONS[i].label) * FONT_WIDTH;
    int32_t label_x = icon_x[i] + ICON_SIZE / 2 - label_w / 2;
    gfx_draw_text(&self->gfx, label_x, icon_y[i] + ICON_SIZE + 4, ICONS[i].label, LABEL_COLOR);
}

static void redraw(wm_window_t *self, int pressed_icon) {
    wallpaper_fill(&self->gfx, 0, 0, (int32_t)self->width, (int32_t)self->height,
                    theme_wallpaper, theme_bg);
    for (int i = 0; i < ICON_COUNT; i++) {
        redraw_icon(self, i, i == pressed_icon);
    }
    if (ctx_menu_open) {
        gfx_draw_menu(&self->gfx, ctx_menu_x, ctx_menu_y, CTX_MENU_ITEM_W, CTX_MENU_ITEM_H,
                      CTX_MENU_ITEMS, CTX_MENU_COUNT, -1,
                      CTX_MENU_BG, CTX_MENU_HOVER, CTX_MENU_BORDER, CTX_MENU_TEXT);
    }
}

int main(void) {
    wm_window_t win;
    if (wm_connect_desktop(&win) != 0) {
        sys_exit(1);
    }

    layout_icons((int32_t)win.height);
    refresh_theme();

    long next_theme_poll = 0;
    int last_click_icon = -1;
    long last_click_ms = -1;
    long pressed_until_ms = 0;
    int pressed_icon = -1;

    redraw(&win, -1);

    for (;;) {
        child_reap(); /* M54: hand back the task slot of anything launched from here that has since closed */
        wm_event_t ev;
        int changed = 0;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_DROP) {
                /* M49: dropping a file on the desktop opens it - the same
                 * thing double-clicking it in the file manager does, and
                 * for the same reason (this project has no per-file type
                 * metadata, so "open" means text_editor.c). Goes through
                 * launch() so a failure gets a toast like every other
                 * spawn here. */
                char dropped[WM_DRAG_PAYLOAD_MAX];
                if (wm_drag_payload(dropped, sizeof(dropped)) == 0 && dropped[0]) {
                    long rc = sys_spawn(PATH_BIN_DIR "text_editor", dropped);
                    if (rc < 0) {
                        wm_notify(WM_NOTIFY_ERROR, dropped, spawn_error_message(rc));
                    }
                    child_track(rc); /* M54 - see children.h */
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 2)) {
                /* Right-click always (re)opens the menu at the new click
                 * point, clamped so it can't be drawn partly off the
                 * desktop window's own edge. */
                ctx_menu_open = 1;
                ctx_menu_x = ev.x;
                ctx_menu_y = ev.y;
                int32_t max_x = (int32_t)win.width - CTX_MENU_ITEM_W;
                int32_t max_y = (int32_t)win.height - CTX_MENU_ITEM_H * CTX_MENU_COUNT;
                if (ctx_menu_x > max_x) {
                    ctx_menu_x = max_x;
                }
                if (ctx_menu_y > max_y) {
                    ctx_menu_y = max_y;
                }
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1) && ctx_menu_open) {
                /* A left-click while the menu is open is consumed by it -
                 * either picks an item or just dismisses, same as
                 * text_editor.c's File menu - never also falls through to
                 * an icon double-click underneath it. */
                int idx = gfx_menu_hit_test(ev.x, ev.y, ctx_menu_x, ctx_menu_y,
                                             CTX_MENU_ITEM_W, CTX_MENU_ITEM_H, CTX_MENU_COUNT);
                ctx_menu_open = 0;
                if (idx >= 0) {
                    launch(CTX_MENU_ITEMS[idx], CTX_MENU_PROGRAMS[idx]);
                }
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                for (int i = 0; i < ICON_COUNT; i++) {
                    if (!point_in_icon(i, ev.x, ev.y)) {
                        continue;
                    }
                    /* M40: the event's own timestamp, not sys_uptime_ms()
                     * here. This is the whole difference between timing
                     * the user's two clicks and timing this process's two
                     * trips round its event loop - and with a full-screen
                     * compositor redraw in between, the latter routinely
                     * exceeded DOUBLE_CLICK_MS and dropped the gesture.
                     * See system_api/include/input.h's mouse_event_t. */
                    long now = (long)ev.time_ms;
                    pressed_until_ms = sys_uptime_ms() + 150;
                    pressed_icon = i;
                    if (last_click_icon == i && last_click_ms >= 0 && now - last_click_ms <= DOUBLE_CLICK_MS) {
                        launch(ICONS[i].label, ICONS[i].program);
                        last_click_icon = -1; /* a third quick click starts a fresh pair, not a third launch */
                        last_click_ms = -1;
                    } else {
                        last_click_icon = i;
                        last_click_ms = now;
                    }
                    changed = 1;
                    break;
                }
            }
        }

        long now = sys_uptime_ms();
        if (now >= next_theme_poll) {
            next_theme_poll = now + THEME_POLL_MS;
            if (refresh_theme()) {
                changed = 1;
            }
        }
        int pressed = now < pressed_until_ms ? pressed_icon : -1;
        static int was_pressed_icon = -1;
        if (changed || pressed != was_pressed_icon) {
            redraw(&win, pressed);
            was_pressed_icon = pressed;
        }
    }
}
