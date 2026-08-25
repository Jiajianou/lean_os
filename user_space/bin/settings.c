/* user_space/bin/settings.c
 *
 * M33: the missing "user-facing configuration surface" - previously zero.
 * Two real, live controls rather than a static info panel that just
 * echoes read-only state back: a desktop background color picker (wm_
 * set_theme, wmclient.h - system_api/include/wm.h's new
 * WM_SETTINGS_PIPE, the compositor's first global, non-per-window
 * setting) and a clipboard viewer/clear button (M32's SYS_clipboard_*).
 * Display resolution and uptime are shown too, read-only, as a
 * lightweight "system info" strip - genuinely live (SYS_fb_info/
 * SYS_uptime_ms), not hardcoded.
 *
 * M38 adds a second swatch row: the focused-window titlebar accent color
 * (compositor.c's second global setting, previously a compile-time
 * TITLEBAR_FOCUS_COLOR constant). Both colors are always sent together
 * (wm_set_theme takes both) - this process tracks its own current choice
 * of each in current_bg/current_accent rather than only ever sending
 * whichever one control just changed, so clicking one swatch row never
 * resets the other back to its default.
 *
 * M44 adds a third control, the wallpaper style (user_space/lib/
 * wallpaper.h), and one correctness fix that matters more than it looks:
 * this window now *queries* the compositor for the current settings at
 * startup (wm_query_settings) instead of assuming the three defaults. It
 * had been assuming since M33, which meant closing Settings and
 * reopening it showed the wrong swatch as selected, and - worse - the
 * next click would send those wrong assumed values back for the two
 * controls you hadn't touched.
 */
#include "shortcuts.h" /* system_api/include/shortcuts.h - M49: the Shortcuts pane is generated from the same table the compositor dispatches from */
#include "str.h"
#include "settings_file.h" /* M47: this window is where the three settings are chosen, so it is also where they get written down */
#include "syscall_wrappers.h"
#include "wallpaper.h"
#include "wmclient.h"

#define WIN_W 320
/* M49: 340 -> 520, for the Shortcuts pane along the bottom. Still well
 * inside what the compositor will place without clamping (a window is
 * kept clear of the taskbar since M45), and taller is the right answer
 * rather than a second tab: this list is reference material you read
 * once, not a control you return to. */
/* M58: 520 -> 632, for the Resolution pane. The Shortcuts list moves
 * down rather than shrinking - it is reference material and the whole
 * point of it is that it is complete. */
#define WIN_H 632

#define BG_COLOR      0x00202430u
#define TEXT_COLOR    0x00E0E0E0u
#define LABEL_COLOR   0x0090A0B0u
#define MARK_COLOR     0x00FFFFFFu /* M57: the checkmark over a selected swatch - white, so it reads on every swatch colour */
#define BORDER_COLOR  0x00404860u
#define BTN_COLOR     0x00445566u
#define BTN_HOVER     0x00607088u

#define SWATCH_SIZE 28
#define SWATCH_GAP  8
#define BG_SWATCH_Y     150
#define ACCENT_SWATCH_Y 204

/* M44: the wallpaper picker - one labeled button per style. Four of them
 * across WIN_W with the same 10px margin every other row here uses. */
#define WALL_BTN_Y  258
#define WALL_BTN_W  68 /* four of these plus their gaps land exactly inside the GFX_PAD margins */
#define WALL_BTN_H  22
#define WALL_BTN_GAP 6
#define WALL_BTN_X(i) (GFX_PAD + (i) * (WALL_BTN_W + WALL_BTN_GAP))

/* M49: the Shortcuts pane. Rows come straight out of SHORTCUTS[] - the
 * same table compositor.c's handle_keyboard dispatches from - so a chord
 * cannot exist without being listed here, and nothing can be listed here
 * that isn't wired up. That is the whole point: a shortcuts list is only
 * worth having if it is true. */
/* M61: the motion switch. On by default, and a real setting rather than
 * a constant because motion that cannot be disabled is a genuine
 * accessibility problem for some people, not a preference. One row, and
 * it fits above the Resolution pane because M61 also moved the Shortcuts
 * list to the 12-row face - a reference list is exactly the dense list
 * that face is for. */
/* M62: the volume control. It shares a row with the Motion switch - one
 * row for the two settings that are about how this desktop *behaves*
 * rather than how it looks. Five
 * steps rather than a draggable slider: this project has no slider
 * widget, a mute and four levels is what a person actually reaches for,
 * and inventing a drag gesture for it would be a widget rather than a
 * setting. */
#define VOL_STEPS   5
#define VOL_BTN_W   22
#define VOL_BTN_H   20
#define VOL_BTN_GAP 4
#define VOL_X(i)    (GFX_PAD + 58 + (i) * (VOL_BTN_W + VOL_BTN_GAP))
#define VOL_Y       288
#define VOL_LABEL_Y 290

#define MOTION_LABEL_Y 290
#define MOTION_BTN_W   50
#define MOTION_BTN_H   20
#define MOTION_BTN_X   (WIN_W - GFX_PAD - MOTION_BTN_W)

/* M58: the Resolution pane. The list is whatever SYS_display_modes
 * offers, which on hardware with no runtime mode-setting interface is
 * nothing at all - and the pane then says so rather than offering
 * buttons that cannot work. */
#define MODE_LABEL_Y  316
#define MODE_BTN_Y    340
#define MODE_BTN_W    92
#define MODE_BTN_H    20
#define MODE_BTN_GAP  6
#define MODE_COLS     3
#define MODE_BTN_X(i) (GFX_PAD + ((i) % MODE_COLS) * (MODE_BTN_W + MODE_BTN_GAP))
#define MODE_ROW_Y(i) (MODE_BTN_Y + ((i) / MODE_COLS) * (MODE_BTN_H + 4))
#define MODE_ROWS     3
#define CONFIRM_Y     (MODE_BTN_Y + MODE_ROWS * (MODE_BTN_H + 4) + 6)
#define CONFIRM_H     20
#define KEEP_BTN_W    64
#define KEEP_BTN_X    (WIN_W - GFX_PAD - KEEP_BTN_W)

#define SHORTCUT_LABEL_Y 452
#define SHORTCUT_ROW_Y   474
/* M61: 18 -> 15, in the 12-row face. Nine chords is a reference list you
 * read once, which is the dense list M57's small face exists for - and
 * the twenty-seven pixels it gives back are what the motion switch above
 * is drawn in. */
#define SHORTCUT_ROW_H   15
#define SHORTCUT_FONT    ui_font_small
#define SHORTCUT_DESC_X  (GFX_PAD + 124) /* clears the longest chord ("Ctrl+Shift+Esc", 14 glyphs) */

#define DEFAULT_BG_COLOR     0x001A1A2Eu /* mirrors compositor.c's own compile-time default - see this file's header comment */
#define DEFAULT_ACCENT_COLOR 0x004C99E6u

static const uint32_t BG_SWATCHES[] = {
    DEFAULT_BG_COLOR, /* the original default */
    0x00203040u,
    0x00301A1Au,
    0x001A3020u,
    0x00302A1Au,
    0x00101018u,
};
#define BG_SWATCH_COUNT ((int)(sizeof(BG_SWATCHES) / sizeof(BG_SWATCHES[0])))

static const uint32_t ACCENT_SWATCHES[] = {
    DEFAULT_ACCENT_COLOR, /* the original default */
    0x00E67E22u, /* orange */
    0x0027AE60u, /* green */
    0x009B59B6u, /* purple */
    0x00E74C3Cu, /* red */
    0x00F1C40Fu, /* yellow */
};
#define ACCENT_SWATCH_COUNT ((int)(sizeof(ACCENT_SWATCHES) / sizeof(ACCENT_SWATCHES[0])))

/* M58: the offered modes, read once at startup - the list is a property
 * of the adapter and cannot change while this program runs. Zero of them
 * is a perfectly ordinary answer (see kernel/drivers/dispi.h) and the
 * pane is written for it. */
static display_mode_t modes[DISPLAY_MAX_MODES];
static int mode_count;

/* A resolution change is on trial until it is confirmed. This is the
 * countdown a person reads; the deadline that actually protects them is
 * the compositor's own (see WM_ACTION_SET_MODE), because the case this
 * whole feature exists for is a screen nobody can read - and a timer
 * living in a window you cannot see would be no protection at all. */
static long confirm_until_ms;

static uint32_t current_bg = DEFAULT_BG_COLOR;
static uint32_t current_accent = DEFAULT_ACCENT_COLOR;
static uint32_t current_wallpaper = WALLPAPER_GRADIENT; /* mirrors compositor.c's own default */
static uint32_t current_animations = 1;
static uint32_t current_volume = 70;    /* mirrors compositor.c's own default */

/* What clicking step `i` sets the volume to. Step 0 is mute; the rest
 * spread evenly to 100, so the rightmost is genuinely all the way up
 * rather than "nearly". */
static int vol_step_percent(int i) {
    return i == 0 ? 0 : 100 * i / (VOL_STEPS - 1);
}                 /* likewise - motion is on unless somebody turned it off */

#define CLEAR_BTN_X (WIN_W - GFX_PAD - CLEAR_BTN_W) /* M44: right-aligned to the same inset every other row uses, rather than a hand-placed 220 */
#define CLEAR_BTN_Y 100
#define CLEAR_BTN_W 80
#define CLEAR_BTN_H 20

static int format_uint(uint32_t v, char *buf) {
    char tmp[10];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    int len = 0;
    for (int i = n - 1; i >= 0; i--) {
        buf[len++] = tmp[i];
    }
    buf[len] = '\0';
    return len;
}

/* M47: every control that changes a setting goes through here rather than
 * calling wm_set_theme directly - the live compositor and the file on
 * disk have to move together, and three call sites each remembering to do
 * both is three chances not to. */
static void apply_theme(void) {
    wm_settings_request_t settings;
    settings.volume = current_volume;
    settings.animations = current_animations;
    settings.bg_color = current_bg;
    settings.accent_color = current_accent;
    settings.wallpaper = current_wallpaper;
    /* M61: every setting together, which is what the struct on the wire
     * was always for - and the reason this function existed from M47 is
     * unchanged: the live compositor and the file on disk have to move
     * together, and four call sites each remembering to do both is four
     * chances not to. */
    wm_set_settings(&settings);
    settings_file_save(&settings);
}

/* M57: which swatch is the current one, said with the checkmark glyph
 * rather than with a one-pixel border colour alone. A border says
 * "different"; a checkmark says "this one" - and against a swatch whose
 * own colour happens to be near the highlight, the border said nothing
 * at all. */
static void draw_selected_mark(wm_window_t *win, int32_t x, int32_t y, int selected) {
    if (!selected) {
        return;
    }
    const ui_font_t *f = gfx_ui_font();
    int32_t gw = gfx_char_advance(f, UI_G_CHECK);
    gfx_draw_char_font(&win->gfx, x + (SWATCH_SIZE - gw) / 2,
                       y + (SWATCH_SIZE - (int32_t)f->height) / 2,
                       UI_G_CHECK, MARK_COLOR, f, 1);
}

static void redraw(wm_window_t *win, int clear_hover, int clear_pressed) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);

    gfx_draw_text(&win->gfx, GFX_PAD, 10, "System", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, 26, WIN_W - GFX_PAD, 26, BORDER_COLOR);

    /* Read once per redraw and used twice - the System strip below and
     * the Resolution pane further down, which marks whichever offered
     * mode is the live one. */
    wm_fb_info_t fb_info;
    uint32_t fb_w = 0, fb_h = 0;
    char line[64];
    if (sys_fb_info(&fb_info) == 0) {
        fb_w = fb_info.width;
        fb_h = fb_info.height;
        int i = 0;
        static const char label[] = "Display: ";
        for (int j = 0; label[j]; j++) {
            line[i++] = label[j];
        }
        i += format_uint(fb_info.width, line + i);
        line[i++] = 'x';
        i += format_uint(fb_info.height, line + i);
        line[i] = '\0';
        gfx_draw_text(&win->gfx, GFX_PAD, 36, line, TEXT_COLOR);
    }

    {
        /* M59: the date, when there is one. This strip showed uptime and
         * nothing else because uptime was all this machine could know;
         * the CMOS clock makes "what day is it" answerable, and the
         * fallback stays for a machine where it is not. */
        os_datetime_t t;
        int i = 0;
        if (sys_time(&t) > 0 && t.valid) {
            i += format_uint(t.year, line + i);
            line[i++] = '-';
            line[i++] = (char)('0' + t.month / 10);
            line[i++] = (char)('0' + t.month % 10);
            line[i++] = '-';
            line[i++] = (char)('0' + t.day / 10);
            line[i++] = (char)('0' + t.day % 10);
            line[i++] = ' ';
            line[i++] = (char)('0' + t.hour / 10);
            line[i++] = (char)('0' + t.hour % 10);
            line[i++] = ':';
            line[i++] = (char)('0' + t.minute / 10);
            line[i++] = (char)('0' + t.minute % 10);
            line[i++] = ' ';
            line[i++] = 'U';
            line[i++] = 'T';
            line[i++] = 'C';
        } else {
            static const char label[] = "Uptime: ";
            for (int j = 0; label[j]; j++) {
                line[i++] = label[j];
            }
            i += format_uint((uint32_t)(sys_uptime_ms() / 1000), line + i);
            line[i++] = 's';
        }
        line[i] = '\0';
        gfx_draw_text(&win->gfx, GFX_PAD, 52, line, TEXT_COLOR);
    }

    gfx_draw_text(&win->gfx, GFX_PAD, 76, "Clipboard", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, 92, WIN_W - GFX_PAD, 92, BORDER_COLOR);

    char clip_buf[40];
    long clip_len = sys_clipboard_get(clip_buf, sizeof(clip_buf) - 1);
    if (clip_len <= 0) {
        static const char empty[] = "(empty)";
        memcpy(clip_buf, empty, sizeof(empty));
    } else {
        if (clip_len > (long)sizeof(clip_buf) - 1) {
            clip_len = (long)sizeof(clip_buf) - 1;
        }
        clip_buf[clip_len] = '\0';
    }
    gfx_draw_text(&win->gfx, GFX_PAD, 102, clip_buf, TEXT_COLOR);

    /* M46: hover *and* press, rather than a button that only ever reacted
     * once the click had already been acted on. */
    gfx_draw_button_state(&win->gfx, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H,
                           clear_hover ? BTN_HOVER : BTN_COLOR, BORDER_COLOR, "Clear", TEXT_COLOR,
                           clear_pressed);

    gfx_draw_text(&win->gfx, GFX_PAD, 130, "Desktop color", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, 146, WIN_W - GFX_PAD, 146, BORDER_COLOR);
    for (int i = 0; i < BG_SWATCH_COUNT; i++) {
        int32_t x = GFX_PAD + i * (SWATCH_SIZE + SWATCH_GAP);
        gfx_fill_rect_rounded(&win->gfx, x, BG_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE, BG_SWATCHES[i]);
        gfx_draw_rect_rounded(&win->gfx, x, BG_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE,
                              BG_SWATCHES[i] == current_bg ? TEXT_COLOR : BORDER_COLOR);
        draw_selected_mark(win, x, BG_SWATCH_Y, BG_SWATCHES[i] == current_bg);
    }

    gfx_draw_text(&win->gfx, GFX_PAD, 184, "Accent color", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, 200, WIN_W - GFX_PAD, 200, BORDER_COLOR);
    for (int i = 0; i < ACCENT_SWATCH_COUNT; i++) {
        int32_t x = GFX_PAD + i * (SWATCH_SIZE + SWATCH_GAP);
        gfx_fill_rect_rounded(&win->gfx, x, ACCENT_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE, ACCENT_SWATCHES[i]);
        gfx_draw_rect_rounded(&win->gfx, x, ACCENT_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE,
                              ACCENT_SWATCHES[i] == current_accent ? TEXT_COLOR : BORDER_COLOR);
        draw_selected_mark(win, x, ACCENT_SWATCH_Y, ACCENT_SWATCHES[i] == current_accent);
    }

    /* M44: the wallpaper row. Each button shows the style's own ramp
     * behind its name rather than a flat button color - a two-word label
     * ("Deep", "Grid") says much less about what you are picking than
     * eight pixels of the actual thing does, and wallpaper_fill draws a
     * 70x22 preview exactly the way it draws a 1024x768 desktop. */
    gfx_draw_text(&win->gfx, GFX_PAD, 238, "Wallpaper", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, 254, WIN_W - GFX_PAD, 254, BORDER_COLOR);
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        int32_t x = WALL_BTN_X(i);
        wallpaper_fill(&win->gfx, x, WALL_BTN_Y, WALL_BTN_W, WALL_BTN_H, i, current_bg);
        gfx_draw_rect_rounded(&win->gfx, x, WALL_BTN_Y, WALL_BTN_W, WALL_BTN_H,
                              (uint32_t)i == current_wallpaper ? TEXT_COLOR : BORDER_COLOR);
        const char *name = wallpaper_name(i);
        int32_t label_w = gfx_text_width(gfx_ui_font(), name);
        gfx_draw_text(&win->gfx, x + (WALL_BTN_W - label_w) / 2,
                      WALL_BTN_Y + (WALL_BTN_H - (int32_t)gfx_ui_font()->height) / 2, name, TEXT_COLOR);
    }

    /* M58: the Resolution pane - the milestone's actual deliverable, and
     * everything the mode-setting driver underneath it costs. */
    gfx_draw_text(&win->gfx, GFX_PAD, MODE_LABEL_Y, "Resolution", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, MODE_LABEL_Y + 16, WIN_W - GFX_PAD, MODE_LABEL_Y + 16, BORDER_COLOR);
    if (mode_count == 0) {
        /* The honest pane on hardware whose adapter this kernel cannot
         * set modes on: the mode the firmware chose, and nothing on
         * offer. Saying so beats offering buttons that do nothing. */
        gfx_draw_text(&win->gfx, GFX_PAD, MODE_BTN_Y,
                       "This display cannot be", LABEL_COLOR);
        gfx_draw_text(&win->gfx, GFX_PAD, MODE_BTN_Y + 16,
                       "resized after boot.", LABEL_COLOR);
    } else {
        for (int i = 0; i < mode_count; i++) {
            char label[16];
            int n = format_uint(modes[i].width, label);
            label[n++] = 'x';
            n += format_uint(modes[i].height, label + n);
            label[n] = '\0';
            int current = (fb_w == modes[i].width && fb_h == modes[i].height);
            gfx_draw_button(&win->gfx, MODE_BTN_X(i), MODE_ROW_Y(i), MODE_BTN_W, MODE_BTN_H,
                             current ? BTN_HOVER : BTN_COLOR,
                             current ? TEXT_COLOR : BORDER_COLOR,
                             label, TEXT_COLOR);
        }
        if (confirm_until_ms != 0) {
            long left_ms = confirm_until_ms - sys_uptime_ms();
            if (left_ms < 0) {
                left_ms = 0;
            }
            char line[40];
            int i = 0;
            static const char ask[] = "Keep this size? ";
            for (int j = 0; ask[j]; j++) {
                line[i++] = ask[j];
            }
            i += format_uint((uint32_t)(left_ms / 1000) + 1, line + i);
            line[i++] = 's';
            line[i] = '\0';
            gfx_draw_text(&win->gfx, GFX_PAD, CONFIRM_Y + 2, line, TEXT_COLOR);
            gfx_draw_button(&win->gfx, KEEP_BTN_X, CONFIRM_Y, KEEP_BTN_W, CONFIRM_H,
                             BTN_COLOR, BORDER_COLOR, "Keep", TEXT_COLOR);
        }
    }

    /* M62: the volume row. The filled steps are the level; the leftmost
     * is mute, which is a step rather than a separate control because
     * "off" is where a volume goes. */
    gfx_draw_text(&win->gfx, GFX_PAD, VOL_LABEL_Y, "Volume", LABEL_COLOR);
    for (int i = 0; i < VOL_STEPS; i++) {
        int filled = (int)current_volume >= vol_step_percent(i) && current_volume > 0;
        if (i == 0) {
            filled = current_volume == 0;
        }
        gfx_fill_rect_rounded(&win->gfx, VOL_X(i), VOL_Y, VOL_BTN_W, VOL_BTN_H,
                               filled ? BTN_HOVER : BTN_COLOR);
        gfx_draw_rect_rounded(&win->gfx, VOL_X(i), VOL_Y, VOL_BTN_W, VOL_BTN_H, BORDER_COLOR);
        if (i == 0) {
            /* Mute gets the close glyph rather than a number - it is not
             * a quieter level, it is the absence of one. */
            gfx_draw_char_font(&win->gfx, VOL_X(i) + 7, VOL_Y + 2, UI_G_CLOSE,
                               TEXT_COLOR, gfx_ui_font(), 0);
        }
    }

    gfx_draw_text(&win->gfx, MOTION_BTN_X - 52, MOTION_LABEL_Y, "Motion", LABEL_COLOR);
    gfx_draw_button(&win->gfx, MOTION_BTN_X, MOTION_LABEL_Y - 2, MOTION_BTN_W, MOTION_BTN_H,
                     current_animations ? BTN_HOVER : BTN_COLOR, BORDER_COLOR,
                     current_animations ? "On" : "Off", TEXT_COLOR);

    gfx_draw_text(&win->gfx, GFX_PAD, SHORTCUT_LABEL_Y, "Shortcuts", LABEL_COLOR);
    gfx_draw_line(&win->gfx, GFX_PAD, SHORTCUT_LABEL_Y + 16, WIN_W - GFX_PAD, SHORTCUT_LABEL_Y + 16, BORDER_COLOR);
    for (int i = 0; i < SHORTCUT_COUNT; i++) {
        int32_t y = SHORTCUT_ROW_Y + i * SHORTCUT_ROW_H;
        gfx_draw_text_font(&win->gfx, GFX_PAD, y, SHORTCUTS[i].chord, TEXT_COLOR, &SHORTCUT_FONT, 0);
        gfx_draw_text_font(&win->gfx, SHORTCUT_DESC_X, y, SHORTCUTS[i].what, LABEL_COLOR, &SHORTCUT_FONT, 0);
    }
}

int main(void) {
    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Settings", &win) != 0) {
        sys_exit(1);
    }

    /* M44: open showing what is actually set, not what the defaults are -
     * see this file's header comment on what assuming cost. */
    {
        wm_settings_request_t settings;
        if (wm_query_settings(&settings) == 0) {
            current_bg = settings.bg_color;
            current_accent = settings.accent_color;
            current_wallpaper = settings.wallpaper;
            current_animations = settings.animations;
            current_volume = settings.volume;
        }
    }

    {
        long n = sys_display_modes(modes, DISPLAY_MAX_MODES);
        mode_count = n < 0 ? 0 : (int)(n > DISPLAY_MAX_MODES ? DISPLAY_MAX_MODES : n);
    }

    int clear_hover = 0;
    int clear_pressed = 0;
    long next_redraw = 0;
    redraw(&win, 0, 0);

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                changed = 1; /* M55: a replacement compositor handed this client a blank buffer - see WM_EVENT_EXPOSE */
            } else if (ev.type == WM_EVENT_MOUSE_MOVE) {
                int hover = gfx_point_in_rect(ev.x, ev.y, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H);
                if (hover != clear_hover) {
                    clear_hover = hover;
                    changed = 1;
                }
                /* M46: the button un-presses if the pointer leaves it
                 * while held, the way a real one does. */
                if (clear_pressed && !((ev.buttons & 1) && hover)) {
                    clear_pressed = 0;
                    changed = 1;
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && !(ev.buttons & 1)) {
                if (clear_pressed) {
                    clear_pressed = 0;
                    changed = 1;
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                if (gfx_point_in_rect(ev.x, ev.y, CLEAR_BTN_X, CLEAR_BTN_Y, CLEAR_BTN_W, CLEAR_BTN_H)) {
                    clear_pressed = 1;
                    sys_clipboard_set("", 0);
                    changed = 1;
                }
                for (int i = 0; i < BG_SWATCH_COUNT; i++) {
                    int32_t x = GFX_PAD + i * (SWATCH_SIZE + SWATCH_GAP);
                    if (gfx_point_in_rect(ev.x, ev.y, x, BG_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE)) {
                        current_bg = BG_SWATCHES[i];
                        apply_theme();
                        changed = 1;
                        break;
                    }
                }
                for (int i = 0; i < ACCENT_SWATCH_COUNT; i++) {
                    int32_t x = GFX_PAD + i * (SWATCH_SIZE + SWATCH_GAP);
                    if (gfx_point_in_rect(ev.x, ev.y, x, ACCENT_SWATCH_Y, SWATCH_SIZE, SWATCH_SIZE)) {
                        current_accent = ACCENT_SWATCHES[i];
                        apply_theme();
                        changed = 1;
                        break;
                    }
                }
                for (int i = 0; i < WALLPAPER_COUNT; i++) {
                    if (gfx_point_in_rect(ev.x, ev.y, WALL_BTN_X(i), WALL_BTN_Y, WALL_BTN_W, WALL_BTN_H)) {
                        current_wallpaper = (uint32_t)i;
                        apply_theme();
                        changed = 1;
                        break;
                    }
                }
                for (int i = 0; i < VOL_STEPS; i++) {
                    if (gfx_point_in_rect(ev.x, ev.y, VOL_X(i), VOL_Y, VOL_BTN_W, VOL_BTN_H)) {
                        current_volume = (uint32_t)vol_step_percent(i);
                        apply_theme();
                        changed = 1;
                        break;
                    }
                }
                if (gfx_point_in_rect(ev.x, ev.y, MOTION_BTN_X, MOTION_LABEL_Y - 2,
                                       MOTION_BTN_W, MOTION_BTN_H)) {
                    current_animations = !current_animations;
                    apply_theme();
                    changed = 1;
                }
                /* M58: pick a resolution, then keep it. Nothing is
                 * written to disk until it is kept - a mode saved the
                 * moment it was applied would come back on the next boot
                 * even if it was the mode that made the screen
                 * unreadable, which is the one outcome the countdown
                 * exists to prevent. */
                if (confirm_until_ms != 0 &&
                    gfx_point_in_rect(ev.x, ev.y, KEEP_BTN_X, CONFIRM_Y, KEEP_BTN_W, CONFIRM_H)) {
                    wm_confirm_display_mode();
                    confirm_until_ms = 0;
                    wm_fb_info_t now_fb;
                    if (sys_fb_info(&now_fb) == 0) {
                        settings_file_save_display(now_fb.width, now_fb.height);
                    }
                    changed = 1;
                } else {
                    for (int i = 0; i < mode_count; i++) {
                        if (gfx_point_in_rect(ev.x, ev.y, MODE_BTN_X(i), MODE_ROW_Y(i), MODE_BTN_W, MODE_BTN_H)) {
                            wm_set_display_mode(modes[i].width, modes[i].height);
                            confirm_until_ms = sys_uptime_ms() + WM_MODE_REVERT_MS;
                            changed = 1;
                            break;
                        }
                    }
                }
            }
        }

        long now = sys_uptime_ms();
        /* The trial is over one way or the other: either it was kept
         * (which clears this itself) or the compositor has already put
         * the old mode back, and the countdown should stop claiming
         * otherwise. */
        if (confirm_until_ms != 0 && now >= confirm_until_ms) {
            confirm_until_ms = 0;
            changed = 1;
        }
        if (now >= next_redraw) {
            next_redraw = now + 500; /* uptime display is the only thing that changes with no input at all */
            changed = 1;
        }

        if (changed) {
            redraw(&win, clear_hover, clear_pressed);
        }
    }
}
