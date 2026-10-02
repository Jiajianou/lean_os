#include <signal.h>

#include "paths.h"
#include "font8x16.h"
#include "string_utilities.h"
#include "syscall_wrappers.h"
#include "window_manager_client.h"

#define COLS 70
#define ROWS 21
#define WIN_W (COLS * FONT_WIDTH)
#define WIN_H (ROWS * FONT_HEIGHT)

#define BG_COLOR     0x00101418u
#define TEXT_COLOR   0x00D0D0D0u
#define CURSOR_COLOR 0x00D0D0D0u

#define LINE_MAX (COLS - 4)
#define IO_BUFFER_SIZE 512
#define LIST_BUFFER 16384
#define LEANFS_NAME_MAX 256

static char grid[ROWS][COLS + 1];
static int32_t current_row, current_col;

#define SCROLLBACK_ROWS 200

static char scrollback[SCROLLBACK_ROWS][COLS + 1];
static int sb_start;
static int sb_count;
static int view_offset;

static void scrollback_push(const char *row) {
    int at = (sb_start + sb_count) % SCROLLBACK_ROWS;
    if (sb_count == SCROLLBACK_ROWS) {
        at = sb_start;
        sb_start = (sb_start + 1) % SCROLLBACK_ROWS;
    } else {
        sb_count++;
    }
    memcpy(scrollback[at], row, COLS);
    scrollback[at][COLS] = '\0';
}

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

static char line_buffer[LINE_MAX];
static int line_length;

static int running_pid = -1;
static int pipe_pid = -1;

static char term_cwd[PATH_MAX_LENGTH] = PATH_HOME;

static void sync_cwd(void) {
    char buffer[PATH_MAX_LENGTH];
    if (sys_getcwd(buffer, sizeof(buffer)) >= 0) {
        int i = 0;
        for (; buffer[i] && i < PATH_MAX_LENGTH - 1; i++) {
            term_cwd[i] = buffer[i];
        }
        term_cwd[i] = '\0';
    }
}

static int resolve_path(const char *name, char *out) {
    if (name[0] == '/') {
        int i = 0;
        for (; name[i] && i < PATH_MAX_LENGTH - 1; i++) {
            out[i] = name[i];
        }
        out[i] = '\0';
        return name[i] ? -1 : 0;
    }
    int n = 0;
    for (const char *s = term_cwd; *s; s++) {
        if (n >= PATH_MAX_LENGTH - 2) {
            return -1;
        }
        out[n++] = *s;
    }
    if (n == 0 || out[n - 1] != '/') {
        out[n++] = '/';
    }
    for (const char *s = name; *s; s++) {
        if (n >= PATH_MAX_LENGTH - 1) {
            return -1;
        }
        out[n++] = *s;
    }
    out[n] = '\0';
    return 0;
}
static int read_file_descriptor, write_file_descriptor;

static int sel_dragging, sel_active;
static int sel_anchor_row, sel_anchor_col, sel_end_row, sel_end_col;
#define SELECTION_COLOR 0x00355070u

static void grid_clear(void) {
    for (int r = 0; r < ROWS; r++) {
        memset(grid[r], ' ', COLS);
        grid[r][COLS] = '\0';
    }
    current_row = 0;
    current_col = 0;
    sb_start = 0;
    sb_count = 0;
    view_offset = 0;
}

static void grid_scroll(void) {
    scrollback_push(grid[0]);
    for (int r = 0; r < ROWS - 1; r++) {
        memcpy(grid[r], grid[r + 1], COLS);
    }
    memset(grid[ROWS - 1], ' ', COLS);
    current_row = ROWS - 1;
    if (view_offset > 0) {
        view_scroll(1);
    }
}

static void putc_term(char c) {
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        current_col = 0;
        current_row++;
    } else {
        grid[current_row][current_col] = c;
        current_col++;
        if (current_col >= COLS) {
            current_col = 0;
            current_row++;
        }
    }
    if (current_row >= ROWS) {
        grid_scroll();
    }
}

static void print_term(const char *s, size_t length) {
    for (size_t i = 0; i < length; i++) {
        putc_term(s[i]);
    }
}

static void print_string_term(const char *s) {
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

static void copy_selection_to_clipboard(void) {
    int sr, sc, er, ec;
    normalized_selection(&sr, &sc, &er, &ec);
    static char buffer[1024];
    size_t n = 0;
    for (int r = sr; r <= er && n < sizeof(buffer); r++) {
        int col_start = (r == sr) ? sc : 0;
        int col_end = (r == er) ? ec : COLS;
        const char *source = view_row(r);
        while (col_end > col_start && source[col_end - 1] == ' ') {
            col_end--;
        }
        for (int c = col_start; c < col_end && n < sizeof(buffer); c++) {
            buffer[n++] = source[c];
        }
        if (r != er && n < sizeof(buffer)) {
            buffer[n++] = '\n';
        }
    }
    sys_clipboard_set(buffer, n);
}

static void redraw(window_manager_window_t *win, int show_cursor) {
    graphics_fill_rect(&win->graphics, 0, 0, WIN_W, WIN_H, BG_COLOR);
    if (sel_active || sel_dragging) {
        int sr, sc, er, ec;
        normalized_selection(&sr, &sc, &er, &ec);
        for (int r = sr; r <= er; r++) {
            int col_start = (r == sr) ? sc : 0;
            int col_end = (r == er) ? ec : COLS;
            if (col_end <= col_start) {
                continue;
            }
            graphics_fill_rect(&win->graphics, col_start * FONT_WIDTH, r * FONT_HEIGHT,
                          (col_end - col_start) * FONT_WIDTH, FONT_HEIGHT, SELECTION_COLOR);
        }
    }
    for (int r = 0; r < ROWS; r++) {
        graphics_draw_text_mono(&win->graphics, 0, r * FONT_HEIGHT, view_row(r), TEXT_COLOR);
    }
    if (show_cursor && view_offset == 0) {
        graphics_fill_rect(&win->graphics, current_col * FONT_WIDTH, current_row * FONT_HEIGHT + FONT_HEIGHT - 2,
                      FONT_WIDTH, 2, CURSOR_COLOR);
    }
}

static long drain_output(void) {
    char buffer[IO_BUFFER_SIZE];
    long drained = 0;
    long avail = sys_pipe_poll(read_file_descriptor);
    while (avail > 0) {
        size_t n = (size_t)avail < IO_BUFFER_SIZE ? (size_t)avail : IO_BUFFER_SIZE;
        long got = sys_read(read_file_descriptor, buffer, n);
        if (got <= 0) {
            break;
        }
        print_term(buffer, (size_t)got);
        drained += got;
        avail = sys_pipe_poll(read_file_descriptor);
    }
    return drained;
}

static void start_prompt(void) {
    print_string_term("$ ");
}

#define MAX_ARGUMENTS 16

typedef struct {
    char *argv[MAX_ARGUMENTS + 1];
    int argc;
} command_t;

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
        char *destination = p;
        out[n++] = destination;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                while (*p && *p != quote) {
                    *destination++ = *p++;
                }
                if (!*p) {
                    return -1;
                }
                p++;
            } else {
                *destination++ = *p++;
            }
        }
        int at_end = (*p == '\0');
        p++;
        *destination = '\0';
        if (at_end) {
            break;
        }
    }
    return n;
}

static int resolve_program(const char *name, char *out) {
    for (const char *c = name; *c; c++) {
        if (*c == '/') {
            int i = 0;
            for (; name[i] && i < PATH_MAX_LENGTH - 1; i++) {
                out[i] = name[i];
            }
            out[i] = '\0';
            return name[i] ? -1 : 0;
        }
    }
    return path_join(out, PATH_BIN_DIRECTORY, name);
}

static void restore_std_file_descriptors(void) {
    sys_dup2(write_file_descriptor, 1);
}

static void report(const char *what, const char *why) {
    print_string_term(what);
    print_string_term(why);
}

static void run_line(void) {
    line_buffer[line_length] = '\0';
    putc_term('\n');

    if (line_length == 0) {
        start_prompt();
        return;
    }
    if (strcmp(line_buffer, "clear") == 0) {
        grid_clear();
        start_prompt();
        return;
    }

    char *tokens[MAX_ARGUMENTS * 2 + 8];
    int ntok = tokenize(line_buffer, tokens, (int)(sizeof(tokens) / sizeof(tokens[0])));
    if (ntok < 0) {
        report("", "unterminated quote\n");
        start_prompt();
        return;
    }
    if (ntok == 0) {
        start_prompt();
        return;
    }

    command_t left, right;
    left.argc = 0;
    right.argc = 0;
    command_t *into = &left;
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
        if (into->argc >= MAX_ARGUMENTS) {
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

    if (strcmp(left.argv[0], "cd") == 0) {
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
        print_string_term(term_cwd);
        putc_term('\n');
        start_prompt();
        return;
    }

    char left_path[PATH_MAX_LENGTH];
    char right_path[PATH_MAX_LENGTH];
    if (resolve_program(left.argv[0], left_path) != 0 ||
        (have_pipe && resolve_program(right.argv[0], right_path) != 0)) {
        report("", "name too long\n");
        start_prompt();
        return;
    }

    int out_file_descriptor = -1;
    if (redirect_to) {
        char full[PATH_MAX_LENGTH];
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
        out_file_descriptor = (int)fd;
    }

    int pipe_file_descriptors[2] = {-1, -1};
    if (have_pipe && sys_pipe(pipe_file_descriptors) != 0) {
        report("", "no pipe available\n");
        if (out_file_descriptor >= 0) {
            sys_close(out_file_descriptor);
        }
        start_prompt();
        return;
    }

    sys_dup2(have_pipe ? pipe_file_descriptors[1] : (out_file_descriptor >= 0 ? out_file_descriptor : write_file_descriptor), 1);
    long pid = sys_spawnv(left_path, (const char *const *)&left.argv[1]);
    if (pid < 0) {
        restore_std_file_descriptors();
        if (out_file_descriptor >= 0) {
            sys_close(out_file_descriptor);
        }
        if (have_pipe) {
            sys_close(pipe_file_descriptors[0]);
            sys_close(pipe_file_descriptors[1]);
        }
        report(left.argv[0], ": command not found\n");
        start_prompt();
        return;
    }
    running_pid = (int)pid;

    if (have_pipe) {
        restore_std_file_descriptors();
        sys_close(pipe_file_descriptors[1]);

        sys_dup2(pipe_file_descriptors[0], 0);
        sys_dup2(out_file_descriptor >= 0 ? out_file_descriptor : write_file_descriptor, 1);
        long pid2 = sys_spawnv(right_path, (const char *const *)&right.argv[1]);
        sys_close(pipe_file_descriptors[0]);
        sys_close(0);
        restore_std_file_descriptors();
        if (pid2 < 0) {
            report(right.argv[0], ": command not found\n");
        } else {
            pipe_pid = (int)pid2;
        }
    } else {
        restore_std_file_descriptors();
    }
    if (out_file_descriptor >= 0) {
        sys_close(out_file_descriptor);
    }
}

static int common_prefix_length(const char *a, const char *b) {
    int n = 0;
    while (a[n] && a[n] == b[n]) {
        n++;
    }
    return n;
}

static void complete_line(void) {
    int start = line_length;
    while (start > 0 && line_buffer[start - 1] != ' ') {
        start--;
    }
    int is_first = 1;
    for (int i = 0; i < start; i++) {
        if (line_buffer[i] != ' ') {
            is_first = 0;
            break;
        }
    }
    line_buffer[line_length] = '\0';
    const char *stem = line_buffer + start;
    int stem_length = line_length - start;

    char directory[PATH_MAX_LENGTH];
    const char *leaf = stem;
    if (is_first) {
        int i = 0;
        for (; PATH_BIN_DIRECTORY[i]; i++) {
            directory[i] = PATH_BIN_DIRECTORY[i];
        }
        directory[i] = '\0';
    } else {
        int last_slash = -1;
        for (int i = 0; i < stem_length; i++) {
            if (stem[i] == '/') {
                last_slash = i;
            }
        }
        if (last_slash < 0) {
            int i = 0;
            for (; term_cwd[i] && i < PATH_MAX_LENGTH - 1; i++) {
                directory[i] = term_cwd[i];
            }
            directory[i] = '\0';
        } else {
            int n = last_slash == 0 ? 1 : last_slash;
            for (int i = 0; i < n; i++) {
                directory[i] = stem[i];
            }
            directory[n] = '\0';
            leaf = stem + last_slash + 1;
        }
    }
    int leaf_length = (int)strlen(leaf);

    static char listing[LIST_BUFFER];
    long n = sys_listdir(directory, listing, sizeof(listing));
    if (n <= 0) {
        return;
    }
    if (n > (long)sizeof(listing)) {
        n = (long)sizeof(listing);
    }

    char best[LEANFS_NAME_MAX + 1];
    int best_length = 0;
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
        name[col] = '\0';
        int this_length = col;
        col = 0;
        if (this_length < leaf_length) {
            continue;
        }
        int same = 1;
        for (int k = 0; k < leaf_length; k++) {
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
            for (int k = 0; k <= this_length; k++) {
                best[k] = name[k];
            }
            best_length = this_length;
        } else {
            best_length = common_prefix_length(best, name);
            best[best_length] = '\0';
        }
    }
    if (matches == 0) {
        return;
    }

    for (int k = leaf_length; k < best_length && line_length < LINE_MAX - 1; k++) {
        line_buffer[line_length++] = best[k];
        putc_term(best[k]);
    }
    if (matches == 1 && best_length > 0 && best[best_length - 1] != '/' && line_length < LINE_MAX - 1) {
        line_buffer[line_length++] = ' ';
        putc_term(' ');
        return;
    }
    if (matches > 1) {
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
            int this_length = col;
            col = 0;
            if (this_length < leaf_length) {
                continue;
            }
            int same = 1;
            for (int k = 0; k < leaf_length; k++) {
                if (name[k] != leaf[k]) {
                    same = 0;
                    break;
                }
            }
            if (same) {
                print_string_term(name);
                putc_term(' ');
            }
        }
        putc_term('\n');
        start_prompt();
        line_buffer[line_length] = '\0';
        print_string_term(line_buffer);
    }
}

static void handle_key(char ch) {
    if (ch == '\t') {
        complete_line();
        return;
    }
    if (ch == '\n' || ch == '\r') {
        run_line();
        line_length = 0;
        return;
    }
    if (ch == '\b' || ch == 0x7F) {
        if (line_length > 0) {
            line_length--;
            current_col--;
            grid[current_row][current_col] = ' ';
        }
        return;
    }
    if (ch >= 0x20 && ch < 0x7F && line_length < LINE_MAX - 1) {
        line_buffer[line_length++] = ch;
        putc_term(ch);
    }
}

int main(void) {
    window_manager_window_t win;
    if (window_manager_connect(WIN_W, WIN_H, "Terminal", &win) != 0) {
        sys_exit(1);
    }
    sync_cwd();

    int pipe_file_descriptors[2];
    if (sys_pipe(pipe_file_descriptors) != 0) {
        sys_exit(1);
    }
    read_file_descriptor = pipe_file_descriptors[0];
    write_file_descriptor = pipe_file_descriptors[1];

    sys_close(0);

    grid_clear();
    print_string_term("lean_os terminal - a shell: ls, cat, cp, echo, cd, pwd, clear\n"
                    "quotes, > redirect, >> append, | pipe, Tab completes\n");
    start_prompt();
    redraw(&win, 1);
    window_manager_present(&win);

    for (;;) {
        int changed = 0;
        window_manager_event_t ev;
        while (window_manager_poll_event(&win, &ev)) {
            if (ev.type == WINDOW_MANAGER_EVENT_EXPOSE || ev.type == WINDOW_MANAGER_EVENT_DISPLAY_CHANGED) {
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_MOVE && sel_dragging) {
                pixel_to_cell(ev.x, ev.y, &sel_end_row, &sel_end_col);
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                sel_active = 0;
                sel_dragging = 1;
                pixel_to_cell(ev.x, ev.y, &sel_anchor_row, &sel_anchor_col);
                sel_end_row = sel_anchor_row;
                sel_end_col = sel_anchor_col;
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && !(ev.buttons & 1) && sel_dragging) {
                sel_dragging = 0;
                sel_active = (sel_anchor_row != sel_end_row || sel_anchor_col != sel_end_col);
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_WHEEL) {
                view_scroll(-ev.wheel);
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_KEY &&
                       (ev.ch == (char)KEYBOARD_KEY_PAGE_UP || ev.ch == (char)KEYBOARD_KEY_PAGE_DOWN)) {
                /* M216: a page of scrollback, as the wheel moves a few lines -
                   and, like the wheel, without throwing away what is typed. */
                view_scroll(ev.ch == (char)KEYBOARD_KEY_PAGE_UP ? ROWS - 1 : -(ROWS - 1));
                changed = 1;
            } else if (ev.type == WINDOW_MANAGER_EVENT_KEY && (running_pid >= 0 || pipe_pid >= 0)) {
                if ((ev.mods & KEYBOARD_MOD_CTRL) && (ev.ch == 'c' || ev.ch == 'C')) {
                    if (running_pid >= 0) {
                        sys_kill(running_pid, SIGINT);
                    }
                    if (pipe_pid >= 0) {
                        sys_kill(pipe_pid, SIGINT);
                    }
                    print_string_term("^C\n");
                    changed = 1;
                }
            } else if (ev.type == WINDOW_MANAGER_EVENT_KEY) {
                if (view_offset != 0) {
                    view_offset = 0;
                }
                long mods = ev.mods;
                if ((mods & KEYBOARD_MOD_CTRL) && (ev.ch == 'c' || ev.ch == 'C')) {
                    if (sel_active) {
                        copy_selection_to_clipboard();
                    } else {
                        sys_clipboard_set(line_buffer, (size_t)line_length);
                    }
                } else if ((mods & KEYBOARD_MOD_CTRL) && (ev.ch == 'v' || ev.ch == 'V')) {
                    char paste_buffer[LINE_MAX];
                    long n = sys_clipboard_get(paste_buffer, sizeof(paste_buffer));
                    if (n > (long)sizeof(paste_buffer)) {
                        n = (long)sizeof(paste_buffer);
                    }
                    for (long i = 0; i < n && line_length < LINE_MAX - 1; i++) {
                        line_buffer[line_length++] = paste_buffer[i];
                        putc_term(paste_buffer[i]);
                    }
                } else {
                    handle_key(ev.ch);
                }
                changed = 1;
            }
        }

        if (running_pid >= 0 || pipe_pid >= 0) {
            if (drain_output() > 0) {
                changed = 1;
            }
            if (running_pid >= 0 && sys_wait_nb(running_pid) != -2) {
                running_pid = -1;
                changed = 1;
            }
            if (pipe_pid >= 0 && sys_wait_nb(pipe_pid) != -2) {
                pipe_pid = -1;
                changed = 1;
            }
            if (running_pid < 0 && pipe_pid < 0) {
                drain_output();
                start_prompt();
                changed = 1;
            }
        }

        if (changed) {
            redraw(&win, running_pid < 0);
            window_manager_present(&win);
        }
        if (running_pid >= 0 || pipe_pid >= 0) {
            window_manager_wait_ms(&win, &read_file_descriptor, 1, 50);
        } else {
            window_manager_wait_ms(&win, NULL, 0, -1);
        }
    }
}
