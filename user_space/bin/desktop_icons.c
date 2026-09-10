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
#include "icon.h" /* system_api/include/icon.h - icon_bytes/icon_valid, M63: icons are files now */
#include "icons.h" /* user_space/lib/icons.h - M56: real icons, from a real format */
#include "children.h" /* M54: this process launches things and must reap them - see children.h */
#include "paths.h" /* system_api/include/paths.h - M53: /bin is where programs live now */
/* M57: icon labels are UI text - the 16-row proportional face, measured
 * with gfx_text_width rather than counted in fixed cells. */
#define LABEL_H UI_FONT_UI_HEIGHT
#include "str.h" /* strlen - ICONS[].label is data-driven now, not a compile-time sizeof() */
#include "spawn_error.h" /* system_api/include/spawn_error.h - M48 */
#include "syscall_wrappers.h"
#include "wallpaper.h" /* M44 - see refresh_theme */
#include "wmclient.h"

#define ICON_BOX_COLOR 0x004C99E6u
#define ICON_HOVER_COLOR 0x006CB9FFu
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
#define CTX_MENU_ITEM_H (LABEL_H + 4)
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
/* M74: `arg` may be NULL or empty, which is what every icon that just
 * starts a program passes. sys_spawn already treats an empty string as
 * "no argument", so there is one path rather than two. */
static void launch_with(const char *label, const char *program, const char *arg) {
    long rc = sys_spawn(program, arg ? arg : "");
    if (rc < 0) {
        wm_notify(WM_NOTIFY_ERROR, label, spawn_error_message(rc));
    }
    child_track(rc); /* M54: so its task slot comes back when it closes - see children.h */
}

static void launch(const char *label, const char *program) {
    launch_with(label, program, "");
}

/* M56: 24x24 blobs drawn at 2x inside the 48px box. */
#define ICON_IMAGE_SCALE 2

typedef struct {
    const char *label;
    const char *program;
    /* M74: what to hand the program. Empty for every icon that just
     * starts something; the README icon is the first thing on this
     * desktop that opens a *file*, which is the difference between a
     * desktop that has applications on it and one that has content. */
    const char *arg;
    const uint8_t *image; /* M56: an icon.h blob - the "no separate icon-image format exists" note this field replaced was in three files */
} icon_def_t;

/* M53: absolute paths. Every icon named a bare program before, which
 * worked only because the namespace was flat enough for a name to be a
 * location - it is not any more, and an icon that resolved by luck is an
 * icon that stops working the moment somebody creates a file with the
 * same name somewhere else. */
static const icon_def_t ICONS[] = {
    {"Terminal", PATH_BIN_DIR "gui_terminal", "", ICON_TERMINAL},
    {"Editor",   PATH_BIN_DIR "text_editor",  "", ICON_EDITOR},
    {"Files",    PATH_BIN_DIR "file_manager", "", ICON_FILES},
    {"Settings", PATH_BIN_DIR "settings",     "", ICON_SETTINGS},
    {"Clock",    PATH_BIN_DIR "gui_clock",    "", ICON_CLOCK},
    {"Paint",    PATH_BIN_DIR "gui_paint",    "", ICON_PAINT},
    {"Tasks",    PATH_BIN_DIR "task_manager", "", ICON_TASKS},
    /* M74: the README, on the desktop rather than only in /home. A fresh
     * machine that boots to seven application icons and nothing to read
     * is a demo; one that boots with the thing that explains it sitting
     * in plain sight is a machine somebody just got. It lands in a second
     * column because the first one is full at this height, which is
     * layout_icons doing exactly what it was written to do. */
    {"README",   PATH_BIN_DIR "text_editor",  PATH_HOME_DIR "readme.txt", ICON_EDITOR},
    /* M100: the browser, LAST rather than in any of the places it would
     * more naturally go. Every icon before it keeps the index and the
     * screen position it already had, which matters because
     * tools/qemu_input_suite.py names several of them by coordinate -
     * inserting this beside Terminal would have moved README into a
     * different column and failed a handful of tests for a reason that
     * has nothing to do with what they check. It lands second in the
     * second column, which is layout_icons doing what M74 described. */
    {"Browser",  PATH_BIN_DIR "netsurf",      "", ICON_BROWSER},
};
#define ICON_COUNT ((int)(sizeof(ICONS) / sizeof(ICONS[0])))

static int32_t icon_x[ICON_COUNT], icon_y[ICON_COUNT]; /* filled by layout_icons from the window's own actual geometry */

/* ---- M63 stretch goal: icons are files ------------------------------
 *
 * icon.h predicted this at M56 and said what it would cost: "the day
 * icons are user-replaceable, the format does not change - only where
 * the bytes are read from". That turned out to be exactly true - the
 * format and the loader are untouched, and this is the whole change.
 *
 * The compiled-in blobs stay, as the *seed*. A fresh disk has no icons
 * on it and something has to put them there, which is the same reason
 * kernel.c seeds /bin from blobs inside kernel.bin. This writes each one
 * out once and then reads every one back from disk, so what is on screen
 * is always what is in the file - which is what makes replacing an icon
 * a matter of replacing a file rather than of rebuilding the OS.
 *
 * A file that is missing or malformed falls back to the blob it was
 * seeded from. An icon is decoration; a desktop that refused to start
 * because somebody wrote nonsense into /icons would be a worse outcome
 * than one that shows the picture it shipped with. */
#define ICON_FILE_MAX 512 /* a 24x24 16-colour blob is 344 bytes; this is the format's own ceiling for one that size */
static uint8_t icon_file_data[ICON_COUNT][ICON_FILE_MAX];
static const uint8_t *icon_image[ICON_COUNT];

static int icon_path_for(int i, char *out) {
    int n = 0;
    for (const char *s = PATH_ICONS_DIR; *s; s++) {
        out[n++] = *s;
    }
    for (const char *s = ICONS[i].label; *s; s++) {
        if (n >= PATH_MAX_LEN - 5) {
            return -1;
        }
        out[n++] = *s;
    }
    static const char ext[] = ".icn";
    for (int k = 0; ext[k]; k++) {
        out[n++] = ext[k];
    }
    out[n] = '\0';
    return 0;
}

static void load_icons(void) {
    /* The directory first. Missing is the ordinary case on a fresh disk;
     * "already there" is an error from sys_mkdir rather than a no-op
     * (see leanfs.h), so this asks before creating. */
    os_stat_t st;
    if (sys_stat(PATH_ICONS, &st) != 0) {
        sys_mkdir(PATH_ICONS);
    }
    for (int i = 0; i < ICON_COUNT; i++) {
        icon_image[i] = ICONS[i].image; /* the fallback, until a file replaces it */
        char path[PATH_MAX_LEN];
        if (icon_path_for(i, path) != 0) {
            continue;
        }
        if (sys_stat(path, &st) != 0) {
            sys_writefile(path, ICONS[i].image, (size_t)icon_bytes(ICONS[i].image));
        }
        long n = sys_readfile(path, icon_file_data[i], ICON_FILE_MAX);
        if (n >= ICON_HEADER_BYTES && n <= ICON_FILE_MAX &&
            icon_valid(icon_file_data[i]) &&
            icon_bytes(icon_file_data[i]) <= (int)n) {
            icon_image[i] = icon_file_data[i];
        }
    }
}

/* Top-to-bottom columns: place icons one under another until the next one
 * wouldn't clear PANEL_MARGIN above the bottom edge, then start a new
 * column back at the top - the same wrapping shape a real desktop's icon
 * grid uses. `win_h > 0` on the guard (not just "does it fit") is what
 * stops a single icon that itself doesn't fit from wrapping forever. */
static void layout_icons(int32_t win_h) {
    int32_t x = ICON_MARGIN, y = ICON_MARGIN;
    int32_t max_y = win_h - PANEL_MARGIN - ICON_SIZE - LABEL_H - 4;
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
 * pulled up by LABEL_H+4 and the height grown by twice that so the
 * label drawn below the box (redraw_icon) is clickable too, not just the
 * box itself. */
static int point_in_icon(int i, int32_t x, int32_t y) {
    return gfx_point_in_rect(x, y, icon_x[i], icon_y[i] - LABEL_H - 4,
                              ICON_SIZE, ICON_SIZE + 2 * (LABEL_H + 4));
}

static void redraw_icon(wm_window_t *self, int i, int pressed) {
    uint32_t box_color = pressed ? ICON_HOVER_COLOR : ICON_BOX_COLOR;
    gfx_fill_rect_rounded(&self->gfx, icon_x[i], icon_y[i], ICON_SIZE, ICON_SIZE, box_color);

    /* M56: a real picture, from a real format (system_api/include/icon.h),
     * instead of the dark rectangle with two letters on it that stood in
     * for one here since M32. The blobs are 24x24 and ICON_SIZE is 48, so
     * they draw at scale 2 - centered, which is (48 - 24*2)/2 = 0, but
     * written as the arithmetic rather than as zero so changing either
     * number keeps working. */
    {
        const uint8_t *blob = icon_image[i];
        int32_t drawn = 24 * ICON_IMAGE_SCALE;
        icon_draw(&self->gfx, icon_x[i] + (ICON_SIZE - drawn) / 2,
                   icon_y[i] + (ICON_SIZE - drawn) / 2, blob, ICON_IMAGE_SCALE);
    }

    int32_t label_w = gfx_text_width(gfx_ui_font(), ICONS[i].label);
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
    /* Before the window: the icons come off disk, and on a fresh disk
     * they go onto it first. Doing this before connecting means the very
     * first frame this process draws is already the one the files
     * describe, rather than the compiled-in pictures replaced a moment
     * later. */
    load_icons();

    wm_window_t win;
    if (wm_connect_desktop(&win) != 0) {
        /* M56: loud. A desktop client that cannot get a window used to
         * exit silently, and the only evidence was a screen with nothing
         * on it - which looks identical to a client that connected and
         * never drew. init restarts the session either way; saying so is
         * what makes the log able to tell them apart. */
        const char msg[] = "desktop_icons: no window from the compositor - exiting so init restarts the session\n";
        sys_write(1, msg, sizeof(msg) - 1);
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
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                changed = 1; /* M55 - see WM_EVENT_EXPOSE */
            } else if (ev.type == WM_EVENT_DROP) {
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
                        launch_with(ICONS[i].label, ICONS[i].program, ICONS[i].arg);
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
