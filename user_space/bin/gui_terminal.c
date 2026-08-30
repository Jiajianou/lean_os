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
#include <signal.h> /* SIGINT - M76's Ctrl+C */

#include "paths.h" /* system_api/include/paths.h - M53: /bin is this terminal's search path too */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT - the terminal grid is a fixed cell by definition (M57) */
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
/* M60: how much of a directory listing tab completion reads at once.
 * /bin holds every program this OS ships and is by far the longest thing
 * this will ever list. */
#define LIST_BUF 2048
/* leanfs's own LEANFS_MAX_NAME (27) plus the '/' SYS_listdir appends to a
 * directory and a NUL - that header is not visible to user_space builds,
 * which is why file_manager.c keeps its own copy of the same number. */
#define LEANFS_NAME_MAX 28

static char grid[ROWS][COLS + 1];
static int32_t cur_row, cur_col;

/* ---- M56: scrollback -------------------------------------------------
 *
 * grid_scroll used to discard the top line outright, which is why M49
 * gave this window a wheel that had nothing to reveal - and why a command
 * whose output was longer than the window was a command whose output you
 * could not read. `ls /bin` alone is more lines than this grid has rows.
 *
 * The retained lines live in a ring rather than a shifted array: at 200
 * rows of 70 characters, shifting on every scrolled line would be 14 KiB
 * of memcpy per line of output, which is exactly the case this is for.
 *
 * `view_offset` is how many lines back the window is showing, 0 being
 * live. The visible rows are drawn from one combined sequence -
 * scrollback first, then the live grid - so nothing in the drawing or the
 * selection code has to know which half a row came from (see view_row). */
#define SCROLLBACK_ROWS 200

static char scrollback[SCROLLBACK_ROWS][COLS + 1];
static int sb_start;  /* ring head: index of the oldest retained line */
static int sb_count;  /* how many are retained, <= SCROLLBACK_ROWS */
static int view_offset;

static void scrollback_push(const char *row) {
    int at = (sb_start + sb_count) % SCROLLBACK_ROWS;
    if (sb_count == SCROLLBACK_ROWS) {
        /* Full: the oldest line falls off, which is what makes this
         * bounded rather than a slow leak of the machine's memory. */
        at = sb_start;
        sb_start = (sb_start + 1) % SCROLLBACK_ROWS;
    } else {
        sb_count++;
    }
    memcpy(scrollback[at], row, COLS);
    scrollback[at][COLS] = '\0';
}

/* Row `r` of the visible window, given the current view offset. The
 * combined sequence is scrollback[0..sb_count) then grid[0..ROWS), and
 * the window shows its last ROWS entries shifted back by view_offset. */
static const char *view_row(int r) {
    int total = sb_count + ROWS;
    int i = total - ROWS - view_offset + r;
    if (i < 0) {
        i = 0;
    }
    if (i >= total) {
        i = total - 1;
    }
    return i < sb_count ? scrollback[i] : grid[i - sb_count];
}

static void view_scroll(int lines) {
    int want = view_offset + lines;
    if (want < 0) {
        want = 0;
    }
    if (want > sb_count) {
        want = sb_count;
    }
    view_offset = want;
}

static char line_buf[LINE_MAX];
static int line_len;

static int running_pid = -1;
/* M60: the right-hand side of a pipe. Two children instead of one, and
 * the prompt does not come back until both are gone - a prompt printed
 * while `cat` is still writing would interleave its output with whatever
 * gets typed next. */
static int pipe_pid = -1;

/* M60: a working directory, which this terminal has never had. It is what
 * `cd` changes, what tab completion completes against, and what a
 * relative path is resolved from.
 *
 * M75: it is a *cache* of the kernel's now, not the only copy. There is a
 * real per-process working directory (SYS_chdir/SYS_getcwd), so `cd`
 * changes that and re-reads it here - which is what makes `cd /tmp` and
 * then running a program actually run that program in /tmp, instead of
 * only affecting paths this terminal happened to build itself. The cache
 * exists because tab completion and the prompt want the string on every
 * keystroke and a syscall per keystroke would be a syscall per keystroke.
 * sync_cwd() is the one place it is refilled. */
static char term_cwd[PATH_MAX_LEN] = PATH_HOME;

static void sync_cwd(void) {
    char buf[PATH_MAX_LEN];
    if (sys_getcwd(buf, sizeof(buf)) >= 0) {
        int i = 0;
        for (; buf[i] && i < PATH_MAX_LEN - 1; i++) {
            term_cwd[i] = buf[i];
        }
        term_cwd[i] = '\0';
    }
}

/* A user-typed path made absolute. Absolute stays absolute; anything else
 * is joined onto the working directory. The rule is one sentence and has
 * no heuristic in it - a token that reaches here is already known to be a
 * path (a redirect target, a `cd` argument), not a word that might be
 * one. */
static int resolve_path(const char *name, char *out) {
    if (name[0] == '/') {
        int i = 0;
        for (; name[i] && i < PATH_MAX_LEN - 1; i++) {
            out[i] = name[i];
        }
        out[i] = '\0';
        return name[i] ? -1 : 0;
    }
    /* path_join concatenates without a separator (its callers all pass a
     * directory constant that already ends in '/'), so the separator is
     * this function's job - term_cwd is "/home", not "/home/". */
    int n = 0;
    for (const char *s = term_cwd; *s; s++) {
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
    /* M56: `clear` clears the history too. A screen that looks empty
     * while a wheel still reveals what was there is not cleared. */
    sb_start = 0;
    sb_count = 0;
    view_offset = 0;
}

static void grid_scroll(void) {
    scrollback_push(grid[0]); /* M56: kept, not discarded */
    for (int r = 0; r < ROWS - 1; r++) {
        memcpy(grid[r], grid[r + 1], COLS);
    }
    memset(grid[ROWS - 1], ' ', COLS);
    cur_row = ROWS - 1;
    /* M56: a reader scrolled back stays on the *same content* while new
     * output arrives, rather than watching it slide upward - which is
     * what every terminal with a scrollback does, and the only behavior
     * that makes reading old output while a command is still running
     * possible at all. Clamped by view_scroll once the ring is full. */
    if (view_offset > 0) {
        view_scroll(1);
    }
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
        const char *src = view_row(r); /* M56: selection reads what is on screen, which may be scrollback */
        while (col_end > col_start && src[col_end - 1] == ' ') {
            col_end--;
        }
        for (int c = col_start; c < col_end && n < sizeof(buf); c++) {
            buf[n++] = src[c];
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
        gfx_draw_text_mono(&win->gfx, 0, r * FONT_HEIGHT, view_row(r), TEXT_COLOR);
    }
    /* M56: the cursor belongs to the live grid, so it is only drawn when
     * the live grid is what is on screen. A blinking cursor sitting in
     * the middle of old output would be claiming you could type there. */
    if (show_cursor && view_offset == 0) {
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

/* ---- M60: a command line ----------------------------------------------
 *
 * This split on the first space, which is all `SYS_spawn(path, arg)`
 * could carry: one program, one argument. `cp a b` was not a command this
 * terminal could express, and that was a property of the *kernel* ABI,
 * not of the parser - which is why M60 had to make argv real first.
 *
 * What is here now: multiple arguments, quoted arguments containing
 * spaces, `>` and `>>` redirection, and one `|`. Deliberately not
 * globbing, not `&&`, not variables and not a job table - each is a shell
 * feature with its own failure modes and none of them is what was
 * missing. What was missing was being able to say two words.
 */
#define MAX_ARGS 16

typedef struct {
    char *argv[MAX_ARGS + 1];
    int argc;
} cmd_t;

/* Splits `line` in place into whitespace-separated tokens, honouring
 * single and double quotes so a filename with a space in it is one
 * argument. Returns the token count, or -1 for an unterminated quote -
 * which is refused rather than guessed at, because guessing turns
 * `rm "my file` into two arguments neither of which was meant. */
static int tokenize(char *line, char **out, int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        char *dst = p;      /* quotes are removed in place, so the token is written back over itself */
        out[n++] = dst;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                while (*p && *p != quote) {
                    *dst++ = *p++;
                }
                if (!*p) {
                    return -1; /* unterminated */
                }
                p++; /* the closing quote */
            } else {
                *dst++ = *p++;
            }
        }
        int at_end = (*p == '\0');
        p++;
        *dst = '\0';
        if (at_end) {
            break;
        }
    }
    return n;
}

/* M53's /bin lookup, unchanged in rule: a name with no '/' in it is
 * looked for in /bin, anything else is taken as the path it is. */
static int resolve_program(const char *name, char *out) {
    for (const char *c = name; *c; c++) {
        if (*c == '/') {
            int i = 0;
            for (; name[i] && i < PATH_MAX_LEN - 1; i++) {
                out[i] = name[i];
            }
            out[i] = '\0';
            return name[i] ? -1 : 0;
        }
    }
    return path_join(out, PATH_BIN_DIR, name);
}

/* Everything the terminal has to put back after a command has been
 * launched with its descriptors pointed somewhere else. fd 1 always goes
 * back to this window's own output pipe; fd 0 goes back to the keyboard,
 * which is what FD_NONE means for a fresh process here (this window never
 * reads fd 0 itself - it gets keys as window events). */
static void restore_std_fds(void) {
    sys_dup2(write_fd, 1);
}

static void report(const char *what, const char *why) {
    print_str_term(what);
    print_str_term(why);
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

    char *tokens[MAX_ARGS * 2 + 8];
    int ntok = tokenize(line_buf, tokens, (int)(sizeof(tokens) / sizeof(tokens[0])));
    if (ntok < 0) {
        report("", "unterminated quote\n");
        start_prompt();
        return;
    }
    if (ntok == 0) {
        start_prompt();
        return;
    }

    /* One pass over the tokens, splitting them into up to two commands
     * and pulling out the redirection target. Each of `>`, `>>` and `|`
     * may appear once; a second one is an error rather than a silent
     * last-one-wins, because "which of the two did it use" is not a
     * question a person should have to ask. */
    cmd_t left, right;
    left.argc = 0;
    right.argc = 0;
    cmd_t *into = &left;
    const char *redirect_to = 0;
    int append = 0;
    int have_pipe = 0;
    for (int i = 0; i < ntok; i++) {
        const char *t = tokens[i];
        if (strcmp(t, "|") == 0) {
            if (have_pipe || left.argc == 0) {
                report("", "syntax error near |\n");
                start_prompt();
                return;
            }
            have_pipe = 1;
            into = &right;
            continue;
        }
        if (strcmp(t, ">") == 0 || strcmp(t, ">>") == 0) {
            if (redirect_to || i + 1 >= ntok) {
                report("", "syntax error near >\n");
                start_prompt();
                return;
            }
            append = (t[1] == '>');
            redirect_to = tokens[++i];
            continue;
        }
        if (into->argc >= MAX_ARGS) {
            report("", "too many arguments\n");
            start_prompt();
            return;
        }
        into->argv[into->argc++] = tokens[i];
    }
    left.argv[left.argc] = 0;
    right.argv[right.argc] = 0;
    if (left.argc == 0 || (have_pipe && right.argc == 0)) {
        report("", "syntax error\n");
        start_prompt();
        return;
    }

    /* M60: the two builtins a working directory needs. They are builtins
     * for the reason `cd` is a builtin everywhere: a child process
     * changing its own idea of where it is would change nothing about
     * this one. */
    if (strcmp(left.argv[0], "cd") == 0) {
        /* M75: one syscall, and the ".." special case is gone with it -
         * the kernel resolves "." and ".." now (syscall.c's
         * path_normalize), so the textual climb this used to do by hand
         * would be a second implementation of something that has one. */
        const char *where = left.argc > 1 ? left.argv[1] : PATH_HOME;
        if (sys_chdir(where) != 0) {
            report(where, ": not a directory\n");
            start_prompt();
            return;
        }
        sync_cwd();
        start_prompt();
        return;
    }
    if (strcmp(left.argv[0], "pwd") == 0) {
        print_str_term(term_cwd);
        putc_term('\n');
        start_prompt();
        return;
    }

    char left_path[PATH_MAX_LEN];
    char right_path[PATH_MAX_LEN];
    if (resolve_program(left.argv[0], left_path) != 0 ||
        (have_pipe && resolve_program(right.argv[0], right_path) != 0)) {
        report("", "name too long\n");
        start_prompt();
        return;
    }

    /* The redirection target is opened *before* anything is spawned, so a
     * path that cannot be written is reported as itself rather than as a
     * command that ran and produced nothing. */
    int out_fd = -1;
    if (redirect_to) {
        char full[PATH_MAX_LEN];
        if (resolve_path(redirect_to, full) != 0) {
            report(redirect_to, ": name too long\n");
            start_prompt();
            return;
        }
        uint32_t flags = OPEN_WRITE | OPEN_CREATE | (append ? OPEN_APPEND : OPEN_TRUNCATE);
        long fd = sys_open(full, flags);
        if (fd < 0) {
            report(redirect_to, ": cannot write there\n");
            start_prompt();
            return;
        }
        out_fd = (int)fd;
    }

    int pipe_fds[2] = {-1, -1};
    if (have_pipe && sys_pipe(pipe_fds) != 0) {
        report("", "no pipe available\n");
        if (out_fd >= 0) {
            sys_close(out_fd);
        }
        start_prompt();
        return;
    }

    /* The left-hand command. Its stdout is the pipe if there is one, the
     * redirect if there is one, and this window otherwise. */
    sys_dup2(have_pipe ? pipe_fds[1] : (out_fd >= 0 ? out_fd : write_fd), 1);
    long pid = sys_spawnv(left_path, (const char *const *)&left.argv[1]);
    if (pid < 0) {
        restore_std_fds();
        if (out_fd >= 0) {
            sys_close(out_fd);
        }
        if (have_pipe) {
            sys_close(pipe_fds[0]);
            sys_close(pipe_fds[1]);
        }
        report(left.argv[0], ": command not found\n");
        start_prompt();
        return;
    }
    running_pid = (int)pid;

    if (have_pipe) {
        /* This is the step a pipe does not work without, and it is not
         * bookkeeping: the write end has to be closed *here*, in the
         * terminal, so the left-hand command is the only process left
         * holding one. Otherwise nothing ever brings the writer count to
         * zero, the right-hand command never sees EOF, and `ls | cat`
         * hangs forever. M59's pipe refcount is what makes closing it
         * mean something. */
        restore_std_fds();
        sys_close(pipe_fds[1]);

        sys_dup2(pipe_fds[0], 0);
        sys_dup2(out_fd >= 0 ? out_fd : write_fd, 1);
        long pid2 = sys_spawnv(right_path, (const char *const *)&right.argv[1]);
        sys_close(pipe_fds[0]);
        /* fd 0 back to the keyboard: FD_NONE is what a fresh process here
         * has, and this window reads keys as window events rather than
         * from fd 0. */
        sys_close(0);
        restore_std_fds();
        if (pid2 < 0) {
            report(right.argv[0], ": command not found\n");
        } else {
            pipe_pid = (int)pid2;
        }
    } else {
        restore_std_fds();
    }
    if (out_fd >= 0) {
        sys_close(out_fd);
    }
}

/* ---- M60: tab completion ---------------------------------------------
 *
 * Small, and the thing that makes a terminal feel like one. The rule is
 * the one every shell uses and is worth stating because it is the whole
 * design: the *first* token completes against /bin, because that is what
 * a first token is - a program; every other token completes against the
 * working directory, because that is what an argument usually is.
 *
 * One match completes it. Several print the shared prefix and then the
 * candidates, which is the behaviour that lets you keep typing rather
 * than guess. None does nothing at all - a terminal that beeps at you for
 * a name that does not exist is not telling you anything you did not
 * already know.
 */
static int common_prefix_len(const char *a, const char *b) {
    int n = 0;
    while (a[n] && a[n] == b[n]) {
        n++;
    }
    return n;
}

static void complete_line(void) {
    /* Where the token under the cursor starts. Quotes are deliberately
     * not honoured here: completing inside a quoted string would have to
     * decide where to put the closing quote, and the token this is
     * about - a path - is exactly the kind that rarely has a space. */
    int start = line_len;
    while (start > 0 && line_buf[start - 1] != ' ') {
        start--;
    }
    int is_first = 1;
    for (int i = 0; i < start; i++) {
        if (line_buf[i] != ' ') {
            is_first = 0;
            break;
        }
    }
    line_buf[line_len] = '\0';
    const char *stem = line_buf + start;
    int stem_len = line_len - start;

    /* The directory being completed against, and the part of the stem
     * that is a name rather than a path. `cat /bi<Tab>` completes the
     * last component against "/", not against the working directory. */
    char dir[PATH_MAX_LEN];
    const char *leaf = stem;
    if (is_first) {
        int i = 0;
        for (; PATH_BIN_DIR[i]; i++) {
            dir[i] = PATH_BIN_DIR[i];
        }
        dir[i] = '\0';
    } else {
        int last_slash = -1;
        for (int i = 0; i < stem_len; i++) {
            if (stem[i] == '/') {
                last_slash = i;
            }
        }
        if (last_slash < 0) {
            int i = 0;
            for (; term_cwd[i] && i < PATH_MAX_LEN - 1; i++) {
                dir[i] = term_cwd[i];
            }
            dir[i] = '\0';
        } else {
            int n = last_slash == 0 ? 1 : last_slash;
            for (int i = 0; i < n; i++) {
                dir[i] = stem[i];
            }
            dir[n] = '\0';
            leaf = stem + last_slash + 1;
        }
    }
    int leaf_len = (int)strlen(leaf);

    static char listing[LIST_BUF];
    long n = sys_listdir(dir, listing, sizeof(listing));
    if (n <= 0) {
        return;
    }
    if (n > (long)sizeof(listing)) {
        n = (long)sizeof(listing);
    }

    char best[LEANFS_NAME_MAX + 1];
    int best_len = 0;
    int matches = 0;
    char name[LEANFS_NAME_MAX + 2];
    int col = 0;
    for (long i = 0; i < n; i++) {
        if (listing[i] != '\n') {
            if (col < (int)sizeof(name) - 1) {
                name[col++] = listing[i];
            }
            continue;
        }
        /* SYS_listdir marks a directory with a trailing '/', which is
         * kept: completing to "docs/" and letting the next Tab descend is
         * exactly what it is for. */
        name[col] = '\0';
        int this_len = col;
        col = 0;
        if (this_len < leaf_len) {
            continue;
        }
        int same = 1;
        for (int k = 0; k < leaf_len; k++) {
            if (name[k] != leaf[k]) {
                same = 0;
                break;
            }
        }
        if (!same) {
            continue;
        }
        matches++;
        if (matches == 1) {
            for (int k = 0; k <= this_len; k++) {
                best[k] = name[k];
            }
            best_len = this_len;
        } else {
            best_len = common_prefix_len(best, name);
            best[best_len] = '\0';
        }
    }
    if (matches == 0) {
        return;
    }

    /* Type the shared prefix in, exactly as if it had been typed - which
     * keeps the grid, the cursor and line_buf in step without this
     * function knowing anything about any of the three. */
    for (int k = leaf_len; k < best_len && line_len < LINE_MAX - 1; k++) {
        line_buf[line_len++] = best[k];
        putc_term(best[k]);
    }
    if (matches == 1 && best_len > 0 && best[best_len - 1] != '/' && line_len < LINE_MAX - 1) {
        line_buf[line_len++] = ' ';
        putc_term(' ');
        return;
    }
    if (matches > 1) {
        /* Several - show them, then reprint the prompt and the line so
         * the cursor ends up back where it was. */
        putc_term('\n');
        col = 0;
        for (long i = 0; i < n; i++) {
            if (listing[i] != '\n') {
                if (col < (int)sizeof(name) - 1) {
                    name[col++] = listing[i];
                }
                continue;
            }
            name[col] = '\0';
            int this_len = col;
            col = 0;
            if (this_len < leaf_len) {
                continue;
            }
            int same = 1;
            for (int k = 0; k < leaf_len; k++) {
                if (name[k] != leaf[k]) {
                    same = 0;
                    break;
                }
            }
            if (same) {
                print_str_term(name);
                putc_term(' ');
            }
        }
        putc_term('\n');
        start_prompt();
        line_buf[line_len] = '\0';
        print_str_term(line_buf);
    }
}

static void handle_key(char ch) {
    if (ch == '\t') {
        complete_line();
        return;
    }
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
    sync_cwd(); /* M75: start where whoever launched this terminal was */

    int pipe_fds[2];
    if (sys_pipe(pipe_fds) != 0) {
        sys_exit(1);
    }
    read_fd = pipe_fds[0];
    write_fd = pipe_fds[1];

    /* M60: this window has no standard input to give away, and now that a
     * command can be written to read one, saying so matters. Keys reach
     * this process as window events, not on fd 0 - a child that read fd 0
     * would be pulling keystrokes out of the same kernel ring the
     * compositor drains, which is a wedged desktop rather than a wedged
     * command. Closing it means a child inherits nothing there and a read
     * fails immediately, which is exactly what "no input" should look
     * like. A pipe puts a real read end back for the one command that
     * has one. */
    sys_close(0);

    grid_clear();
    print_str_term("lean_os terminal - a shell: ls, cat, cp, echo, cd, pwd, clear\n"
                    "quotes, > redirect, >> append, | pipe, Tab completes\n");
    start_prompt();
    redraw(&win, 1);

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                changed = 1; /* M55: a replacement compositor handed this client a blank buffer - see WM_EVENT_EXPOSE */
            } else if (ev.type == WM_EVENT_MOUSE_MOVE && sel_dragging) {
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
            } else if (ev.type == WM_EVENT_MOUSE_WHEEL) {
                /* M56: the wheel M49 gave this window finally has
                 * something to reveal. One line per detent, matching the
                 * file manager's own one-row-per-detent rule so a wheel
                 * means the same thing everywhere. Negative is up, which
                 * is *back* through history - hence the negation. */
                view_scroll(-ev.wheel);
                changed = 1;
            } else if (ev.type == WM_EVENT_KEY && (running_pid >= 0 || pipe_pid >= 0)) {
                /* ---- M76: Ctrl+C while a program is running ----------
                 *
                 * Every keystroke that arrived while a child held this
                 * terminal used to be dropped on the floor - there was
                 * nothing a terminal could do with one, because the only
                 * signals this kernel had were the two that kill and
                 * neither was reachable from a keystroke. So a program
                 * that would not finish had to be force-quit from the
                 * taskbar, which is the desktop equivalent of pulling
                 * the plug.
                 *
                 * Ctrl+C is an interrupt here and a copy at the prompt,
                 * which is the split every terminal emulator makes and
                 * for the same reason: while a program is running, the
                 * keyboard belongs to it. What SIGINT then *does* is the
                 * program's business - the default action ends it with
                 * 130, and a program that installed a handler gets to
                 * decide. That difference is the whole milestone.
                 *
                 * Both halves of a pipeline are signalled, because both
                 * are the foreground job and stopping one would leave the
                 * other blocked on a pipe nobody will ever fill. */
                if ((ev.mods & KBD_MOD_CTRL) && (ev.ch == 'c' || ev.ch == 'C')) {
                    if (running_pid >= 0) {
                        sys_kill(running_pid, SIGINT);
                    }
                    if (pipe_pid >= 0) {
                        sys_kill(pipe_pid, SIGINT);
                    }
                    print_str_term("^C\n");
                    changed = 1;
                }
                /* Anything else typed while a child holds the terminal is
                 * still dropped, exactly as it always was - there is no
                 * standard input to hand it to (see main's sys_close(0)). */
            } else if (ev.type == WM_EVENT_KEY) {
                /* M56: typing snaps back to the live grid. A keystroke
                 * that appeared somewhere off screen would be the worst
                 * possible way to find out you were still scrolled up. */
                if (view_offset != 0) {
                    view_offset = 0;
                }
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
                /* M-fix: from the event, not from the kernel's global
                 * "most recently read" state - see wm_event_t.mods. The
                 * keystroke this handler is holding was decoded before
                 * the compositor forwarded it, so asking now is asking
                 * about somebody else's key. */
                long mods = ev.mods;
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

        if (running_pid >= 0 || pipe_pid >= 0) {
            drain_output();
            /* M60: both halves of a pipe, and the prompt waits for both.
             * They are polled rather than waited on for the reason this
             * loop has always polled: pipe_write blocks a child once its
             * 1 KiB buffer fills, so a blocking wait here - which never
             * drains the output pipe - would deadlock on any command
             * whose output outgrew it. */
            if (running_pid >= 0 && sys_wait_nb(running_pid) != -2) {
                running_pid = -1;
            }
            if (pipe_pid >= 0 && sys_wait_nb(pipe_pid) != -2) {
                pipe_pid = -1;
            }
            if (running_pid < 0 && pipe_pid < 0) {
                drain_output(); /* one last catch-up read after both are confirmed dead */
                start_prompt();
            }
            changed = 1;
        }

        if (changed) {
            redraw(&win, running_pid < 0);
        }
    }
}
