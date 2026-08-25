/* user_space/bin/file_manager.c
 *
 * M33: the missing "browse the filesystem" GUI piece - until now the
 * only way was `ls` inside a terminal.
 *
 * M53 makes it a browser rather than a list. leanfs was flat until that
 * milestone, so this window showed the entire namespace at once - this
 * OS's own executables sitting next to your text files - because
 * SYS_listfiles had no other answer to give. It now opens on /home, has
 * a path bar, enters a directory on double-click or Enter, and leaves
 * one through a ".." row. This is the app that most obviously wanted it.
 *
 * ".." is a caller-side string operation on the path this window already
 * holds, not something the filesystem resolves - leanfs deliberately
 * stores no parent link and refuses ".." in a path (see leanfs.c's
 * resolve()), so doing it here is being honest about where the knowledge
 * actually lives rather than teaching the resolver to climb.
 *
 * Selection works two ways, both ending at the same place
 * (open_selected): Up/Down arrow keys move the selection and Enter opens
 * it, or a mouse double-click (same DOUBLE_CLICK_MS pattern desktop_
 * icons.c already uses) on a row does the same thing. "Open" always
 * means "load it in text_editor.c" (M33's other new app) - this project
 * has no per-file type metadata (leanfs inodes don't record one) to make
 * a smarter guess from, and every file this OS ships or a user is likely
 * to create here is plain text anyway.
 *
 * Refreshes its listing every REFRESH_MS so a file saved from a
 * concurrently open text_editor.c shows up without needing to relaunch.
 */
#include "paths.h" /* system_api/include/paths.h - M53: /bin is where programs live now */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "str.h"
#include "spawn_error.h" /* system_api/include/spawn_error.h - M48 */
#include "syscall_wrappers.h"
#include "wmclient.h"

#define WIN_W 280
#define WIN_H 360
#define ROW_H (FONT_HEIGHT + 4)
#define HEADER_H 24
#define SCROLLBAR_W 8 /* M37: reserved strip along the right edge - see redraw()'s gfx_draw_scrollbar call */
#define LIST_W (WIN_W - SCROLLBAR_W)
/* M48: a one-row status strip along the bottom. An error about the file
 * you just double-clicked belongs in the window you double-clicked it in
 * - a toast in the far corner of the screen is the wrong place for
 * something about what you are looking directly at. */
#define STATUS_H (FONT_HEIGHT + 4)
#define LIST_H   (WIN_H - HEADER_H - STATUS_H)
#define ROWS_VISIBLE (LIST_H / ROW_H)

#define BG_COLOR       0x001C1C24u
#define HEADER_COLOR   0x00303850u
#define TEXT_COLOR     0x00D8D8D8u
#define SELECT_COLOR   0x004C6699u
#define LABEL_COLOR    0x0090A0C0u
#define SCROLLBAR_TRACK 0x00141820u
#define SCROLLBAR_THUMB 0x00506080u
#define STATUS_BG      0x00141820u
#define STATUS_ERR_FG  0x00E08878u

#define MAX_FILES    48
#define MAX_NAME_LEN 32 /* leanfs's real cap (LEANFS_MAX_NAME, kernel/fs/leanfs.h) is 27 + a NUL - this just needs to be at least that, kept as its own constant since that header isn't visible to user_space builds */
#define LIST_BUF_SIZE 2048
#define DOUBLE_CLICK_MS 500

static char names[MAX_FILES][MAX_NAME_LEN];
/* M53: whether names[i] is a directory. SYS_listdir marks one with a
 * trailing '/', which this strips on the way in - so the marker is a
 * flag here rather than part of the name, and nothing downstream has to
 * remember to trim it before building a path. */
static uint8_t is_dir[MAX_FILES];
static int file_count;
static int selected = -1;
static int scroll_top;
static const char *status_text = "";

/* M53: which directory this window is showing. There is no working
 * directory in this OS, so this is the app's own state and every path it
 * hands a syscall is built absolute from it. */
static char cwd[PATH_MAX_LEN] = PATH_HOME;

/* cwd + '/' + name, into out (PATH_MAX_LEN bytes). Returns 0, or -1 if
 * it would not fit - refused rather than truncated, since a truncated
 * path names a different file. */
static int path_in_cwd(const char *name, char *out) {
    int n = 0;
    for (const char *s = cwd; *s; s++) {
        if (n >= PATH_MAX_LEN - 2) {
            return -1;
        }
        out[n++] = *s;
    }
    if (n == 0 || out[n - 1] != '/') {
        out[n++] = '/';
    }
    for (const char *s = name; *s; s++) {
        if (n >= PATH_MAX_LEN - 1) {
            return -1;
        }
        out[n++] = *s;
    }
    out[n] = '\0';
    return 0;
}

/* ---- M56: rename, delete, copy ---------------------------------------
 *
 * The write half of this filesystem stopped at "create or overwrite a
 * whole file" (SYS_writefile, M33), which is how a filesystem that had
 * never removed anything ended up with the boot self-tests' own fixtures
 * on it forever. SYS_unlink and SYS_rename close that; these three keys
 * are what makes them reachable.
 *
 * Delete goes behind a confirm, and nothing else does. There is no trash
 * and inventing one would be scope this does not need - which makes the
 * confirm the only thing standing between a keystroke and a file that is
 * gone, so it is the one operation that gets one. Rename and copy are
 * both recoverable by doing them again.
 *
 * Same centered-box prompt shape text_editor.c has had since M36, in the
 * same two flavors: one that takes typing and one that takes y/n. */
#define PROMPT_W 260
#define PROMPT_H 72
#define PROMPT_MAX_LEN 27 /* LEANFS_MAX_NAME - a name this window cannot express is one it should not offer to create */
#define PROMPT_BG      0x00243040u
#define PROMPT_BORDER  0x004C6699u
#define PROMPT_TEXT    0x00E8E8E8u
#define PROMPT_INPUT_BG 0x00141820u

/* How large a file this window will copy in one go. Not leanfs's own
 * 72 KiB cap: a copy buffer is static storage in every process that has
 * one, and the same 16 KiB text_editor.c settled on covers everything a
 * person keeps in /home. A file larger than this is refused out loud
 * rather than silently truncated - a half-copied file is worse than no
 * copy at all. */
#define FM_COPY_MAX 16384

typedef enum {
    FM_PROMPT_NONE = 0,
    FM_PROMPT_RENAME,
    FM_PROMPT_COPY,
    FM_PROMPT_CONFIRM_DELETE,
} fm_prompt_t;

static fm_prompt_t prompt_kind;
static char prompt_buf[PROMPT_MAX_LEN + 1];
static int prompt_len;

/* M49: drag state. A press on a row arms it; the drag only actually
 * begins once the pointer has moved DRAG_THRESHOLD away, so a click and a
 * drag stay distinguishable - without that every selection click would
 * announce a drag the moment the pointer twitched. */
#define DRAG_THRESHOLD 8
static int drag_armed_row = -1;
static int32_t drag_press_x, drag_press_y;
static int dragging;

static void refresh_list(void) {
    static char buf[LIST_BUF_SIZE];
    file_count = 0;

    /* M53: ".." first, and always at row 0, so leaving a directory is in
     * the same place every time rather than wherever it happened to sort.
     * Not present in the root, which has nowhere to go. */
    if (!(cwd[0] == '/' && cwd[1] == '\0')) {
        names[0][0] = '.';
        names[0][1] = '.';
        names[0][2] = '\0';
        is_dir[0] = 1;
        file_count = 1;
    }

    long n = sys_listdir(cwd, buf, sizeof(buf));
    if (n > 0) {
        if (n > (long)sizeof(buf)) {
            n = (long)sizeof(buf);
        }
        int col = 0;
        for (long i = 0; i < n && file_count < MAX_FILES; i++) {
            char c = buf[i];
            if (c == '\n') {
                /* A trailing '/' is SYS_listdir saying "this one is a
                 * directory" - taken off the name and kept as a flag. */
                int dir = (col > 0 && names[file_count][col - 1] == '/');
                if (dir) {
                    col--;
                }
                names[file_count][col] = '\0';
                is_dir[file_count] = (uint8_t)dir;
                file_count++;
                col = 0;
            } else if (col < MAX_NAME_LEN - 1) {
                names[file_count][col++] = c;
            }
        }
    }
    if (selected >= file_count) {
        selected = file_count - 1;
    }
}

static void clamp_scroll(void) {
    if (selected < 0) {
        return;
    }
    if (selected < scroll_top) {
        scroll_top = selected;
    }
    if (selected >= scroll_top + ROWS_VISIBLE) {
        scroll_top = selected - ROWS_VISIBLE + 1;
    }
}

/* M53: drops the last component of cwd. Purely textual - see this
 * file's header on why that is the honest place for it. */
static void go_up(void) {
    int n = 0;
    while (cwd[n]) {
        n++;
    }
    while (n > 0 && cwd[n - 1] != '/') {
        n--;
    }
    if (n > 1) {
        n--; /* drop the separator too, unless it is the root's own */
    }
    cwd[n ? n : 1] = '\0';
    cwd[0] = '/';
}

static void open_selected(void) {
    if (selected < 0 || selected >= file_count) {
        return;
    }
    if (is_dir[selected]) {
        /* M53: entering a directory, not launching anything. */
        if (names[selected][0] == '.' && names[selected][1] == '.') {
            go_up();
        } else {
            char next[PATH_MAX_LEN];
            if (path_in_cwd(names[selected], next) != 0) {
                status_text = "Path too long.";
                return;
            }
            for (int i = 0; i < PATH_MAX_LEN; i++) {
                cwd[i] = next[i];
            }
        }
        selected = -1;
        scroll_top = 0;
        status_text = "";
        refresh_list();
        return;
    }
    char full[PATH_MAX_LEN];
    if (path_in_cwd(names[selected], full) != 0) {
        status_text = "Path too long.";
        return;
    }
    /* M48: this used to throw the result away, so a file that couldn't be
     * opened looked exactly like a double-click that didn't register. */
    long rc = sys_spawn(PATH_BIN_DIR "text_editor", full);
    status_text = rc < 0 ? spawn_error_message(rc) : "";
}

/* The selected row's full path, or -1 if there is no ordinary file
 * selected. ".." and directories are excluded from all three operations:
 * this filesystem has no rmdir and a directory rename would work but a
 * directory *copy* would not, and offering two of three would be worse
 * than offering none. */
static int selected_file_path(char *out) {
    if (selected < 0 || selected >= file_count || is_dir[selected]) {
        return -1;
    }
    return path_in_cwd(names[selected], out);
}

static void prompt_open(fm_prompt_t kind) {
    char scratch[PATH_MAX_LEN];
    if (selected_file_path(scratch) != 0) {
        status_text = "Select a file first.";
        return;
    }
    prompt_kind = kind;
    prompt_len = 0;
    if (kind != FM_PROMPT_CONFIRM_DELETE) {
        /* Pre-filled with the current name, which is what a rename
         * usually starts from and what a copy usually differs from by a
         * character or two. */
        for (int i = 0; names[selected][i] && i < PROMPT_MAX_LEN; i++) {
            prompt_buf[prompt_len++] = names[selected][i];
        }
    }
    prompt_buf[prompt_len] = '\0';
}

static void prompt_confirm(void) {
    char from[PATH_MAX_LEN];
    if (selected_file_path(from) != 0) {
        prompt_kind = FM_PROMPT_NONE;
        return;
    }
    if (prompt_kind == FM_PROMPT_CONFIRM_DELETE) {
        status_text = sys_unlink(from) == 0 ? "Deleted." : "Could not delete that.";
    } else if (prompt_len > 0) {
        prompt_buf[prompt_len] = '\0';
        char to[PATH_MAX_LEN];
        if (path_in_cwd(prompt_buf, to) != 0) {
            status_text = "Name too long.";
            prompt_kind = FM_PROMPT_NONE;
            return;
        }
        if (prompt_kind == FM_PROMPT_RENAME) {
            status_text = sys_rename(from, to) == 0 ? "Renamed." : "Could not rename that.";
        } else {
            /* Copy is a read and a write, not a filesystem operation -
             * leanfs has no notion of one, and a whole-file read/write
             * pair is exactly what this OS's file API offers. Bounded by
             * the same buffer every other whole-file caller here uses. */
            static char copy_buf[FM_COPY_MAX];
            long n = sys_readfile(from, copy_buf, sizeof(copy_buf));
            if (n < 0) {
                status_text = "Could not read that file.";
            } else if (n > (long)sizeof(copy_buf)) {
                status_text = "Too large to copy.";
            } else {
                status_text = sys_writefile(to, copy_buf, (size_t)n) == 0 ? "Copied." : "Could not write the copy.";
            }
        }
    }
    prompt_kind = FM_PROMPT_NONE;
    refresh_list();
}

static void redraw(wm_window_t *win) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);
    gfx_fill_rect(&win->gfx, 0, 0, LIST_W, HEADER_H, HEADER_COLOR);
    /* M53: the path bar. Right-aligned when it is too long for the
     * header, so the part you are actually in stays visible - the tail of
     * a path says where you are, the head only says how you got there. */
    {
        int32_t max_chars = (LIST_W - 12) / FONT_WIDTH;
        const char *shown = cwd;
        int len = 0;
        while (cwd[len]) {
            len++;
        }
        if (len > max_chars) {
            shown = cwd + (len - max_chars);
        }
        gfx_draw_text(&win->gfx, 6, 4, shown, LABEL_COLOR);
    }

    for (int row = 0; row < ROWS_VISIBLE; row++) {
        int i = scroll_top + row;
        if (i >= file_count) {
            break;
        }
        int32_t y = HEADER_H + row * ROW_H;
        if (i == selected) {
            gfx_fill_rect(&win->gfx, 0, y, LIST_W, ROW_H, SELECT_COLOR);
        }
        gfx_draw_text(&win->gfx, 6, y + 2, names[i], TEXT_COLOR);
    }

    /* M37: the on-screen position/extent indicator this list previously
     * had none of - it already scrolled (Up/Down, or clicking a row near
     * an edge), there was just no visual cue there was more above/below. */
    gfx_draw_scrollbar(&win->gfx, LIST_W, HEADER_H, SCROLLBAR_W, LIST_H,
                        file_count, ROWS_VISIBLE, scroll_top,
                        SCROLLBAR_TRACK, SCROLLBAR_THUMB);

    gfx_fill_rect(&win->gfx, 0, WIN_H - STATUS_H, WIN_W, STATUS_H, STATUS_BG);
    gfx_draw_text(&win->gfx, 6, WIN_H - STATUS_H + 2,
                  status_text[0] ? status_text : "R rename  C copy  Del delete",
                  status_text[0] ? STATUS_ERR_FG : LABEL_COLOR);

    /* M56: the prompt, drawn last so it sits over the list it is about -
     * this app owns its whole window buffer and there is no
     * compositor-level popup surface to put it in (the same note M35 left
     * on text_editor.c's own dialog). */
    if (prompt_kind != FM_PROMPT_NONE) {
        int32_t x = (WIN_W - PROMPT_W) / 2;
        int32_t y = (WIN_H - PROMPT_H) / 2;
        gfx_fill_rect_rounded(&win->gfx, x, y, PROMPT_W, PROMPT_H, PROMPT_BG);
        gfx_draw_rect_rounded(&win->gfx, x, y, PROMPT_W, PROMPT_H, PROMPT_BORDER);
        if (prompt_kind == FM_PROMPT_CONFIRM_DELETE) {
            gfx_draw_text(&win->gfx, x + GFX_PAD, y + 8, "Delete this file?", PROMPT_TEXT);
            gfx_draw_text(&win->gfx, x + GFX_PAD, y + 32, "Y = delete   any key = cancel", PROMPT_TEXT);
        } else {
            gfx_draw_text(&win->gfx, x + GFX_PAD, y + 8,
                           prompt_kind == FM_PROMPT_RENAME ? "Rename to:" : "Copy to:", PROMPT_TEXT);
            gfx_fill_rect_rounded(&win->gfx, x + GFX_PAD, y + 28, PROMPT_W - 2 * GFX_PAD, FONT_HEIGHT + 4, PROMPT_INPUT_BG);
            char buf[PROMPT_MAX_LEN + 1];
            memcpy(buf, prompt_buf, (size_t)prompt_len);
            buf[prompt_len] = '\0';
            gfx_draw_text(&win->gfx, x + GFX_PAD + 4, y + 30, buf, PROMPT_TEXT);
            gfx_fill_rect(&win->gfx, x + GFX_PAD + 4 + prompt_len * FONT_WIDTH, y + 30, 2, FONT_HEIGHT, PROMPT_TEXT);
        }
    }
}

int main(void) {
    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Files", &win) != 0) {
        sys_exit(1);
    }

    refresh_list();
    redraw(&win);

    long last_click_ms = -1;
    int last_click_row = -1;
    long next_refresh = sys_uptime_ms() + 1000;
    static const long REFRESH_MS = 1000;

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE) {
                changed = 1; /* M55: a replacement compositor handed this client a blank buffer - see WM_EVENT_EXPOSE */
            } else if (ev.type == WM_EVENT_KEY && prompt_kind != FM_PROMPT_NONE) {
                /* M56: an open prompt owns every keystroke until it is
                 * answered - the same "in-progress interaction takes over
                 * the input stream" shape text_editor.c's own prompt has
                 * had since M36, and compositor.c's drag state machine
                 * since M31. */
                changed = 1;
                if (prompt_kind == FM_PROMPT_CONFIRM_DELETE) {
                    if (ev.ch == 'y' || ev.ch == 'Y') {
                        prompt_confirm();
                    } else {
                        prompt_kind = FM_PROMPT_NONE;
                    }
                } else if (ev.ch == '\n' || ev.ch == '\r') {
                    prompt_confirm();
                } else if (ev.ch == 0x1B) {
                    prompt_kind = FM_PROMPT_NONE;
                } else if (ev.ch == '\b' || ev.ch == 0x7F) {
                    if (prompt_len > 0) {
                        prompt_len--;
                    }
                } else if (ev.ch >= 0x20 && ev.ch < 0x7F && prompt_len < PROMPT_MAX_LEN) {
                    prompt_buf[prompt_len++] = ev.ch;
                }
            } else if (ev.type == WM_EVENT_KEY) {
                /* M56: three single keys rather than a menu. This window
                 * has no menu bar to hang them off, and a modifier chord
                 * would collide with the window-manager ones the
                 * compositor swallows before a client ever sees them
                 * (system_api/include/shortcuts.h). */
                if (ev.ch == 'r' || ev.ch == 'R') {
                    prompt_open(FM_PROMPT_RENAME);
                    changed = 1;
                } else if (ev.ch == 'c' || ev.ch == 'C') {
                    prompt_open(FM_PROMPT_COPY);
                    changed = 1;
                } else if (ev.ch == 0x7F || ev.ch == '\b') {
                    prompt_open(FM_PROMPT_CONFIRM_DELETE);
                    changed = 1;
                } else if (ev.ch == KBD_KEY_UP && selected > 0) {
                    selected--;
                    clamp_scroll();
                    changed = 1;
                } else if (ev.ch == KBD_KEY_DOWN && selected + 1 < file_count) {
                    selected++;
                    clamp_scroll();
                    changed = 1;
                } else if (ev.ch == '\n' || ev.ch == '\r') {
                    open_selected();
                }
            } else if (ev.type == WM_EVENT_MOUSE_MOVE) {
                /* M49: this window keeps receiving motion even once the
                 * pointer has left it (the compositor routes by focus and
                 * hands over out-of-bounds coordinates), which is exactly
                 * what makes dragging a file *out* of it possible. */
                if (drag_armed_row >= 0 && !dragging && (ev.buttons & 1)) {
                    int32_t dx = ev.x - drag_press_x;
                    int32_t dy = ev.y - drag_press_y;
                    if (dx < 0) {
                        dx = -dx;
                    }
                    if (dy < 0) {
                        dy = -dy;
                    }
                    if (dx + dy >= DRAG_THRESHOLD) {
                        dragging = 1;
                        /* M53: the payload is a full path now - whatever
                         * receives the drop has no idea which directory
                         * this window is showing, and a bare name only
                         * ever resolved because the namespace was flat. */
                        {
                            char full[PATH_MAX_LEN];
                            if (path_in_cwd(names[drag_armed_row], full) == 0) {
                                wm_drag_begin(full);
                            }
                        }
                        status_text = "";
                        changed = 1;
                    }
                }
                if (!(ev.buttons & 1)) {
                    drag_armed_row = -1;
                    dragging = 0;
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && !(ev.buttons & 1)) {
                drag_armed_row = -1;
                dragging = 0;
            } else if (ev.type == WM_EVENT_MOUSE_WHEEL) {
                /* M49: one row per detent - the same unit Up/Down move,
                 * so the wheel and the keyboard agree about what a step
                 * is. Scrolls the view without moving the selection: a
                 * wheel is a look-around gesture, and dragging the
                 * selection with it would make "scroll down to see, then
                 * press Enter" open the wrong file. */
                int max_top = file_count - ROWS_VISIBLE;
                if (max_top < 0) {
                    max_top = 0;
                }
                int want = scroll_top + ev.wheel;
                if (want < 0) {
                    want = 0;
                }
                if (want > max_top) {
                    want = max_top;
                }
                if (want != scroll_top) {
                    scroll_top = want;
                    changed = 1;
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                if (ev.y >= HEADER_H && ev.y < HEADER_H + LIST_H) {
                    int row = scroll_top + (ev.y - HEADER_H) / ROW_H;
                    if (row < file_count) {
                        selected = row;
                        drag_armed_row = row; /* M49: a drag may be starting - see DRAG_THRESHOLD */
                        drag_press_x = ev.x;
                        drag_press_y = ev.y;
                        changed = 1;
                        long now = sys_uptime_ms();
                        if (last_click_row == row && last_click_ms >= 0 && now - last_click_ms <= DOUBLE_CLICK_MS) {
                            open_selected();
                            last_click_row = -1;
                            last_click_ms = -1;
                        } else {
                            last_click_row = row;
                            last_click_ms = now;
                        }
                    }
                }
            }
        }

        long now = sys_uptime_ms();
        if (now >= next_refresh) {
            refresh_list();
            next_refresh = now + REFRESH_MS;
            changed = 1;
        }

        if (changed) {
            redraw(&win);
        }
    }
}
