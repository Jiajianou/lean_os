/* user_space/bin/gui_terminal.c
 *
 * A real interactive shell inside its own window - the desktop icon's
 * double-click target (desktop_icons.c). This is *not* user_space/shell
 * (that reads fd 0/sys_write(1,...) straight from the global keyboard/
 * console the compositor already owns exclusively, wm.h's own M20/M22
 * comments note the two paths conflict) - it's a from-scratch line editor
 * over WM_EVENT_KEY events plus its own text-grid renderer (gfx.h), the
 * same "own the pixels, own the input" shape every other WM client here
 * uses (gui_clock.c, gui_paint.c).
 *
 * The one new piece this needs that no earlier GUI app did: seeing a
 * spawned child's stdout. SYS_pipe alone only ever hands back *new* fd
 * numbers, so this dup2s a pipe's write end onto its own fd 1 exactly
 * once at startup - every command spawned afterward inherits that same
 * fd 1 (SYS_spawn copies the whole fd table), so its output lands in the
 * pipe this process reads back and renders, never the global console.
 * Reused across every command rather than a fresh pipe per command:
 * there's no SYS_close in this project (system_api/include/syscall.h's
 * SYS_dup2 comment), so a fresh pipe every time would eventually exhaust
 * MAX_FDS (sched.h) in a long session.
 *
 * Running a command is synchronous from this process's own point of view
 * (SYS_wait_nb polled once per loop iteration, not SYS_wait) specifically
 * so the window keeps redrawing and draining the output pipe while the
 * child runs - pipe_write (kernel/ipc/pipe.c) blocks the child once its
 * 1 KiB buffer fills, so a real SYS_wait here (which never reads the
 * pipe) would deadlock the moment a command's output exceeded that.
 */
#include "paths.h" /* system_api/include/paths.h - M53: /bin is this terminal's search path too */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "str.h"
#include "syscall_wrappers.h"
#include "wmclient.h"

#define COLS 70
#define ROWS 21
#define WIN_W (COLS * FONT_WIDTH)
#define WIN_H (ROWS * FONT_HEIGHT)

#define BG_COLOR     0x00101418u
#define TEXT_COLOR   0x00D0D0D0u
#define CURSOR_COLOR 0x00D0D0D0u

#define LINE_MAX (COLS - 4) /* leaves room for the "$ " prompt without ever wrapping a typed line onto a second row */
#define IO_BUF_SIZE 512

static char grid[ROWS][COLS + 1];
static int32_t cur_row, cur_col;

static char line_buf[LINE_MAX];
static int line_len;

static int running_pid = -1;
static int read_fd, write_fd;

/* M37: click-drag text selection over the output grid - the gap M32's own
 * header comment on Ctrl+C flagged ("there's no text selection UI to copy
 * from"). Same dragging/active split as text_editor.c's own selection. */
static int sel_dragging, sel_active;
static int sel_anchor_row, sel_anchor_col, sel_end_row, sel_end_col;
#define SELECTION_COLOR 0x00355070u

static void grid_clear(void) {
    for (int r = 0; r < ROWS; r++) {
        memset(grid[r], ' ', COLS);
        grid[r][COLS] = '\0';
    }
    cur_row = 0;
    cur_col = 0;
}

static void grid_scroll(void) {
    for (int r = 0; r < ROWS - 1; r++) {
        memcpy(grid[r], grid[r + 1], COLS);
    }
    memset(grid[ROWS - 1], ' ', COLS);
    cur_row = ROWS - 1;
}

static void putc_term(char c) {
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        cur_col = 0;
        cur_row++;
    } else {
        grid[cur_row][cur_col] = c;
        cur_col++;
        if (cur_col >= COLS) {
            cur_col = 0;
            cur_row++;
        }
    }
    if (cur_row >= ROWS) {
        grid_scroll();
    }
}

static void print_term(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        putc_term(s[i]);
    }
}

static void print_str_term(const char *s) {
    print_term(s, strlen(s));
}

static void pixel_to_cell(int32_t px, int32_t py, int *out_row, int *out_col) {
    int row = (int)py / FONT_HEIGHT;
    if (row < 0) {
        row = 0;
    }
    if (row >= ROWS) {
        row = ROWS - 1;
    }
    int col = (int)px / FONT_WIDTH;
    if (col < 0) {
        col = 0;
    }
    if (col > COLS) {
        col = COLS;
    }
    *out_row = row;
    *out_col = col;
}

static void normalized_selection(int *sr, int *sc, int *er, int *ec) {
    if (sel_anchor_row < sel_end_row || (sel_anchor_row == sel_end_row && sel_anchor_col <= sel_end_col)) {
        *sr = sel_anchor_row;
        *sc = sel_anchor_col;
        *er = sel_end_row;
        *ec = sel_end_col;
    } else {
        *sr = sel_end_row;
        *sc = sel_end_col;
        *er = sel_anchor_row;
        *ec = sel_anchor_col;
    }
}

/* Trims trailing spaces off each row's contribution - grid[] is always
 * space-padded to COLS (grid_clear/grid_scroll), so copying a row-
 * spanning selection verbatim would paste a mostly-blank COLS-wide line
 * for every row it covers. */
static void copy_selection_to_clipboard(void) {
    int sr, sc, er, ec;
    normalized_selection(&sr, &sc, &er, &ec);
    static char buf[1024];
    size_t n = 0;
    for (int r = sr; r <= er && n < sizeof(buf); r++) {
        int col_start = (r == sr) ? sc : 0;
        int col_end = (r == er) ? ec : COLS;
        while (col_end > col_start && grid[r][col_end - 1] == ' ') {
            col_end--;
        }
        for (int c = col_start; c < col_end && n < sizeof(buf); c++) {
            buf[n++] = grid[r][c];
        }
        if (r != er && n < sizeof(buf)) {
            buf[n++] = '\n';
        }
    }
    sys_clipboard_set(buf, n);
}

static void redraw(wm_window_t *win, int show_cursor) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);
    if (sel_active || sel_dragging) {
        int sr, sc, er, ec;
        normalized_selection(&sr, &sc, &er, &ec);
        for (int r = sr; r <= er; r++) {
            int col_start = (r == sr) ? sc : 0;
            int col_end = (r == er) ? ec : COLS;
            if (col_end <= col_start) {
                continue;
            }
            gfx_fill_rect(&win->gfx, col_start * FONT_WIDTH, r * FONT_HEIGHT,
                          (col_end - col_start) * FONT_WIDTH, FONT_HEIGHT, SELECTION_COLOR);
        }
    }
    for (int r = 0; r < ROWS; r++) {
        gfx_draw_text(&win->gfx, 0, r * FONT_HEIGHT, grid[r], TEXT_COLOR);
    }
    if (show_cursor) {
        gfx_fill_rect(&win->gfx, cur_col * FONT_WIDTH, cur_row * FONT_HEIGHT + FONT_HEIGHT - 2,
                      FONT_WIDTH, 2, CURSOR_COLOR);
    }
}

static void drain_output(void) {
    char buf[IO_BUF_SIZE];
    long avail = sys_pipe_poll(read_fd);
    while (avail > 0) {
        size_t n = (size_t)avail < IO_BUF_SIZE ? (size_t)avail : IO_BUF_SIZE;
        long got = sys_read(read_fd, buf, n);
        if (got <= 0) {
            break;
        }
        print_term(buf, (size_t)got);
        avail = sys_pipe_poll(read_fd);
    }
}

static void start_prompt(void) {
    print_str_term("$ ");
}

static void run_line(void) {
    line_buf[line_len] = '\0';
    putc_term('\n');

    if (line_len == 0) {
        start_prompt();
        return;
    }
    if (strcmp(line_buf, "clear") == 0) {
        grid_clear();
        start_prompt();
        return;
    }

    char *space = line_buf;
    while (*space && *space != ' ') {
        space++;
    }
    char *prog_arg = "";
    if (*space == ' ') {
        *space = '\0';
        prog_arg = space + 1;
    }

    /* M53: same /bin lookup the text shell does, and for the same reason -
     * see user_space/shell/shell.c. Anything with a '/' in it is taken as
     * the path it is. */
    char resolved[PATH_MAX_LEN];
    const char *target = line_buf;
    int has_slash = 0;
    for (const char *c = line_buf; *c; c++) {
        if (*c == '/') {
            has_slash = 1;
        }
    }
    if (!has_slash) {
        if (path_join(resolved, PATH_BIN_DIR, line_buf) != 0) {
            print_str_term(line_buf);
            print_str_term(": name too long\n");
            start_prompt();
            return;
        }
        target = resolved;
    }

    sys_dup2(write_fd, 1);
    long pid = sys_spawn(target, prog_arg);
    if (pid < 0) {
        print_str_term(line_buf);
        print_str_term(": command not found\n");
        start_prompt();
        return;
    }
    running_pid = (int)pid;
}

static void handle_key(char ch) {
    if (ch == '\n' || ch == '\r') {
        run_line();
        line_len = 0;
        return;
    }
    if (ch == '\b' || ch == 0x7F) {
        if (line_len > 0) {
            line_len--;
            cur_col--;
            grid[cur_row][cur_col] = ' ';
        }
        return;
    }
    if (ch >= 0x20 && ch < 0x7F && line_len < LINE_MAX - 1) {
        line_buf[line_len++] = ch;
        putc_term(ch);
    }
}

int main(void) {
    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Terminal", &win) != 0) {
        sys_exit(1);
    }

    int pipe_fds[2];
    if (sys_pipe(pipe_fds) != 0) {
        sys_exit(1);
    }
    read_fd = pipe_fds[0];
    write_fd = pipe_fds[1];

    grid_clear();
    print_str_term("lean_os terminal - type a program name (ls, cat <f>, echo ..., clear)\n");
    start_prompt();
    redraw(&win, 1);

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_MOUSE_MOVE && sel_dragging) {
                pixel_to_cell(ev.x, ev.y, &sel_end_row, &sel_end_col);
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                sel_active = 0;
                sel_dragging = 1;
                pixel_to_cell(ev.x, ev.y, &sel_anchor_row, &sel_anchor_col);
                sel_end_row = sel_anchor_row;
                sel_end_col = sel_anchor_col;
                changed = 1;
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && !(ev.buttons & 1) && sel_dragging) {
                sel_dragging = 0;
                sel_active = (sel_anchor_row != sel_end_row || sel_anchor_col != sel_end_col);
                changed = 1;
            } else if (ev.type == WM_EVENT_KEY && running_pid < 0) {
                /* M32: Ctrl+C/V is checked before ordinary line-editing -
                 * the decoded character alone ('c'/'v') can't tell a
                 * chord from a plain keypress, so SYS_kbd_modifiers'
                 * *current* held state is what actually distinguishes
                 * them (see its own doc comment for why that's "good
                 * enough" rather than exact: modifier and character
                 * arrive as two separate reads of two separate pieces of
                 * live/buffered state, not one atomic event, but Ctrl is
                 * physically held for the whole chord in practice).
                 * M37: Ctrl+C now prefers a real selection (sel_active)
                 * over the old "copy the whole current input line"
                 * stand-in, which only still fires when nothing's
                 * selected - the exact gap M32's own comment here used to
                 * flag. Paste still inserts at the end of the input line,
                 * silently dropping whatever wouldn't fit - same
                 * truncation behavior ordinary typing already has. */
                long mods = sys_kbd_modifiers();
                if ((mods & KBD_MOD_CTRL) && (ev.ch == 'c' || ev.ch == 'C')) {
                    if (sel_active) {
                        copy_selection_to_clipboard();
                    } else {
                        sys_clipboard_set(line_buf, (size_t)line_len);
                    }
                } else if ((mods & KBD_MOD_CTRL) && (ev.ch == 'v' || ev.ch == 'V')) {
                    char paste_buf[LINE_MAX];
                    long n = sys_clipboard_get(paste_buf, sizeof(paste_buf));
                    if (n > (long)sizeof(paste_buf)) {
                        n = (long)sizeof(paste_buf);
                    }
                    for (long i = 0; i < n && line_len < LINE_MAX - 1; i++) {
                        line_buf[line_len++] = paste_buf[i];
                        putc_term(paste_buf[i]);
                    }
                } else {
                    handle_key(ev.ch);
                }
                changed = 1;
            }
        }

        if (running_pid >= 0) {
            drain_output();
            long rc = sys_wait_nb(running_pid);
            if (rc != -2) {
                drain_output(); /* one last catch-up read after the child is confirmed dead */
                running_pid = -1;
                start_prompt();
            }
            changed = 1;
        }

        if (changed) {
            redraw(&win, running_pid < 0);
        }
    }
}
