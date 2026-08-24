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
                  status_text[0] ? status_text : "Enter or double-click to open",
                  status_text[0] ? STATUS_ERR_FG : LABEL_COLOR);
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
            if (ev.type == WM_EVENT_KEY) {
                if (ev.ch == KBD_KEY_UP && selected > 0) {
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
