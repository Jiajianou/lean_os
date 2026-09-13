#include "paths.h"
#include "font8x16.h"
#include "str.h"
#include "recent.h"
#include "syscall_wrappers.h"
#include "wmclient.h"

#define COLS 80
#define ROWS 24
#define WIN_W (COLS * FONT_WIDTH)
#define WIN_H (ROWS * FONT_HEIGHT)

#define BG_COLOR      0x00141414u
#define TEXT_COLOR    0x00E0E0E0u
#define CURSOR_COLOR  0x00E0E0E0u
#define STATUS_BG     0x00303030u
#define STATUS_COLOR  0x00FFFF88u
#define MENU_BG       0x00242424u
#define MENU_HOVER_BG 0x003A5A80u
#define MENU_BORDER   0x00484848u
#define MENU_TEXT     0x00E0E0E0u

#define MAX_LINE_LEN (COLS)
#define MAX_LINES    600
#define STATUS_ROWS 1
#define MENU_ROWS 1
#define TEXT_ROWS (ROWS - STATUS_ROWS - MENU_ROWS)
#define CONTENT_Y0 (MENU_ROWS * FONT_HEIGHT)

#define FILE_MENU_X 4
#define FILE_MENU_ITEM_W 110
#define FILE_MENU_ITEM_H (UI_FONT_UI_HEIGHT + 4)
static const char *const FILE_MENU_ITEMS[] = {"New", "Save", "Save As" UI_S_ELLIPSIS, "Quit"};
#define FILE_MENU_COUNT ((int)(sizeof(FILE_MENU_ITEMS) / sizeof(FILE_MENU_ITEMS[0])))

typedef enum { PROMPT_NONE = 0, PROMPT_SAVE_AS, PROMPT_CONFIRM_DISCARD, PROMPT_FIND } prompt_kind_t;
typedef enum { PENDING_NONE = 0, PENDING_NEW, PENDING_QUIT, PENDING_DROP } pending_action_t;

#define PROMPT_W 360
#define PROMPT_H 72
#define PROMPT_BG       0x00202020u
#define PROMPT_BORDER   0x00606060u
#define PROMPT_TEXT     0x00E0E0E0u
#define PROMPT_INPUT_BG 0x00101010u
#define PROMPT_MAX_LEN  48

static char lines[MAX_LINES][MAX_LINE_LEN];
static int line_len[MAX_LINES];
static int line_count = 1;

static int cur_row, cur_col;
static int scroll_top;
static int dirty;
static int truncated;
static int menu_open;

static char filename[PATH_MAX_LEN];
static char pending_drop[64];
static char status[COLS + 1];

static prompt_kind_t prompt_kind;
static pending_action_t pending_action;
static char prompt_buf[PROMPT_MAX_LEN + 1];
static int prompt_len;

static int sel_dragging, sel_active;
static int sel_anchor_row, sel_anchor_col, sel_end_row, sel_end_col;
#define SELECTION_COLOR 0x00355070u

static void load_file(const char *name) {
    long fd = sys_open(name, OPEN_READ);
    if (fd < 0) {
        return;
    }
    recent_add(name);
    line_count = 0;
    truncated = 0;
    int col = 0;
    char chunk[512];
    for (;;) {
        long got = sys_read((int)fd, chunk, sizeof(chunk));
        if (got <= 0) {
            break;
        }
        for (long i = 0; i < got; i++) {
            char c = chunk[i];
            if (c == '\n') {
                if (line_count >= MAX_LINES) {
                    truncated = 1;
                    break;
                }
                line_len[line_count++] = col;
                col = 0;
            } else if (col < MAX_LINE_LEN - 1) {
                if (line_count >= MAX_LINES) {
                    truncated = 1;
                    break;
                }
                lines[line_count][col++] = c;
            }
        }
        if (truncated) {
            break;
        }
    }
    sys_close((int)fd);
    if (!truncated && line_count < MAX_LINES && (col > 0 || line_count == 0)) {
        line_len[line_count++] = col;
    }
    if (line_count == 0) {
        line_count = 1;
        line_len[0] = 0;
    }
}

static void save_file(void) {
    if (truncated) {
        memcpy(status, "File is longer than this editor holds - use Save As.",
               sizeof("File is longer than this editor holds - use Save As."));
        return;
    }
    char tmpname[PATH_MAX_LEN];
    int t = 0;
    for (; filename[t] && t < PATH_MAX_LEN - 8; t++) {
        tmpname[t] = filename[t];
    }
    static const char suffix[] = ".tmp~";
    for (int k = 0; suffix[k] && t < PATH_MAX_LEN - 1; k++, t++) {
        tmpname[t] = suffix[k];
    }
    tmpname[t] = '\0';

    long fd = sys_open(tmpname, OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE);
    int ok = fd >= 0;
    for (int r = 0; ok && r < line_count; r++) {
        int n = line_len[r];
        if (sys_write((int)fd, lines[r], (size_t)n) != n ||
            sys_write((int)fd, "\n", 1) != 1) {
            ok = 0;
        }
    }
    if (fd >= 0) {
        sys_close((int)fd);
    }
    if (ok) {
        ok = (sys_rename_replace(tmpname, filename) == 0);
    }
    if (!ok) {
        sys_unlink(tmpname);
    }
    int i = 0;
    const char *prefix = ok ? "saved " : "SAVE FAILED ";
    for (; prefix[i]; i++) {
        status[i] = prefix[i];
    }
    for (int j = 0; filename[j] && i < COLS; j++, i++) {
        status[i] = filename[j];
    }
    status[i] = '\0';
    if (ok) {
        dirty = 0;
    }
}

static void clamp_cursor(void) {
    if (cur_row < 0) {
        cur_row = 0;
    }
    if (cur_row >= line_count) {
        cur_row = line_count - 1;
    }
    if (cur_col < 0) {
        cur_col = 0;
    }
    if (cur_col > line_len[cur_row]) {
        cur_col = line_len[cur_row];
    }
    if (cur_row < scroll_top) {
        scroll_top = cur_row;
    }
    if (cur_row >= scroll_top + TEXT_ROWS) {
        scroll_top = cur_row - TEXT_ROWS + 1;
    }
}

#define UNDO_MAX 512

typedef enum {
    EDIT_INSERT = 0,
    EDIT_DELETE = 1,
    EDIT_SPLIT = 3,
    EDIT_JOIN = 4,
} edit_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t group;
    int16_t row, col;
    char ch;
} edit_t;

static edit_t undo_ring[UNDO_MAX];
static int undo_count;
static int undo_head;
static int redo_count;
static uint8_t undo_group;
static int undo_replaying;

static void undo_begin_group(void) {
    undo_group++;
}

static void undo_record(edit_kind_t kind, int row, int col, char ch) {
    if (undo_replaying) {
        return;
    }
    edit_t *e = &undo_ring[undo_head];
    e->kind = (uint8_t)kind;
    e->group = undo_group;
    e->row = (int16_t)row;
    e->col = (int16_t)col;
    e->ch = ch;
    undo_head = (undo_head + 1) % UNDO_MAX;
    if (undo_count < UNDO_MAX) {
        undo_count++;
    }
    redo_count = 0;
}

static void undo_reset(void) {
    undo_count = 0;
    redo_count = 0;
    undo_head = 0;
    undo_group = 0;
}

static void insert_char(char c) {
    int *len = &line_len[cur_row];
    if (*len >= MAX_LINE_LEN - 1) {
        return;
    }
    for (int i = *len; i > cur_col; i--) {
        lines[cur_row][i] = lines[cur_row][i - 1];
    }
    lines[cur_row][cur_col] = c;
    (*len)++;
    undo_record(EDIT_INSERT, cur_row, cur_col, c);
    cur_col++;
    dirty = 1;
}

static int split_line(int row, int col) {
    if (line_count >= MAX_LINES) {
        return 0;
    }
    for (int r = line_count; r > row + 1; r--) {
        memcpy(lines[r], lines[r - 1], (size_t)line_len[r - 1]);
        line_len[r] = line_len[r - 1];
    }
    line_count++;
    int tail = line_len[row] - col;
    memcpy(lines[row + 1], lines[row] + col, (size_t)tail);
    line_len[row + 1] = tail;
    line_len[row] = col;
    return 1;
}

static int join_line(int row) {
    if (row + 1 >= line_count) {
        return 0;
    }
    if (line_len[row] + line_len[row + 1] > MAX_LINE_LEN - 1) {
        return 0;
    }
    memcpy(lines[row] + line_len[row], lines[row + 1], (size_t)line_len[row + 1]);
    line_len[row] += line_len[row + 1];
    for (int r = row + 1; r < line_count - 1; r++) {
        memcpy(lines[r], lines[r + 1], (size_t)line_len[r + 1]);
        line_len[r] = line_len[r + 1];
    }
    line_count--;
    return 1;
}

static void split_at_cursor(void) {
    if (!split_line(cur_row, cur_col)) {
        return;
    }
    undo_record(EDIT_SPLIT, cur_row, cur_col, 0);
    cur_row++;
    cur_col = 0;
    dirty = 1;
}

static void backspace(void) {
    if (cur_col == 0) {
        if (cur_row == 0) {
            return;
        }
        int at = line_len[cur_row - 1];
        if (!join_line(cur_row - 1)) {
            return;
        }
        undo_record(EDIT_JOIN, cur_row - 1, at, 0);
        cur_row--;
        cur_col = at;
        dirty = 1;
        return;
    }
    int *len = &line_len[cur_row];
    undo_record(EDIT_DELETE, cur_row, cur_col - 1, lines[cur_row][cur_col - 1]);
    for (int i = cur_col - 1; i < *len - 1; i++) {
        lines[cur_row][i] = lines[cur_row][i + 1];
    }
    (*len)--;
    cur_col--;
    dirty = 1;
}

static void handle_char(char ch) {
    if (ch == '\n' || ch == '\r') {
        split_at_cursor();
    } else if (ch == '\b' || ch == 0x7F) {
        backspace();
    } else if (ch == KBD_KEY_UP) {
        cur_row--;
    } else if (ch == KBD_KEY_DOWN) {
        cur_row++;
    } else if (ch == KBD_KEY_LEFT) {
        cur_col--;
    } else if (ch == KBD_KEY_RIGHT) {
        cur_col++;
    } else if (ch >= 0x20 && ch < 0x7F) {
        insert_char(ch);
    } else {
        return;
    }
    clamp_cursor();
}

static void clear_selection(void) {
    sel_active = 0;
    sel_dragging = 0;
}

static void reset_to_new_file(void) {
    line_count = 1;
    line_len[0] = 0;
    cur_row = 0;
    cur_col = 0;
    scroll_top = 0;
    memcpy(filename, PATH_HOME_DIR "untitled", sizeof(PATH_HOME_DIR "untitled"));
    dirty = 0;
    undo_reset();
    status[0] = '\0';
    clear_selection();
}

static void quit_now(void) {
    sys_exit(1);
}

static void begin_save_as(void) {
    prompt_kind = PROMPT_SAVE_AS;
    prompt_len = 0;
    for (; filename[prompt_len] && prompt_len < PROMPT_MAX_LEN; prompt_len++) {
        prompt_buf[prompt_len] = filename[prompt_len];
    }
}

static void confirm_save_as(void) {
    prompt_buf[prompt_len] = '\0';
    if (prompt_len > 0) {
        int i = 0;
        for (; prompt_buf[i] && i < (int)sizeof(filename) - 1; i++) {
            filename[i] = prompt_buf[i];
        }
        filename[i] = '\0';
        truncated = 0;
        save_file();
    }
    prompt_kind = PROMPT_NONE;
}

static void request_action(pending_action_t action) {
    if (!dirty) {
        if (action == PENDING_NEW) {
            reset_to_new_file();
        } else {
            quit_now();
        }
        return;
    }
    prompt_kind = PROMPT_CONFIRM_DISCARD;
    pending_action = action;
}

static void confirm_discard(int discard) {
    prompt_kind = PROMPT_NONE;
    pending_action_t action = pending_action;
    pending_action = PENDING_NONE;
    if (!discard) {
        return;
    }
    if (action == PENDING_NEW) {
        reset_to_new_file();
    } else if (action == PENDING_QUIT) {
        quit_now();
    } else if (action == PENDING_DROP) {
        load_file(pending_drop);
    }
}

static void run_file_menu_item(int idx) {
    if (idx == 0) {
        request_action(PENDING_NEW);
    } else if (idx == 1) {
        save_file();
    } else if (idx == 2) {
        begin_save_as();
    } else if (idx == 3) {
        request_action(PENDING_QUIT);
    }
}

static void pixel_to_grid(int32_t px, int32_t py, int *out_row, int *out_col) {
    int row = scroll_top + (int)(py - CONTENT_Y0) / FONT_HEIGHT;
    if (row < 0) {
        row = 0;
    }
    if (row >= line_count) {
        row = line_count - 1;
    }
    int col = (int)px / FONT_WIDTH;
    if (col < 0) {
        col = 0;
    }
    if (col > line_len[row]) {
        col = line_len[row];
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

static char find_needle[PROMPT_MAX_LEN + 1];

static int line_has_at(int row, int col, const char *needle) {
    int n = 0;
    while (needle[n]) {
        if (col + n >= line_len[row] || lines[row][col + n] != needle[n]) {
            return 0;
        }
        n++;
    }
    return 1;
}

static int find_from(int row, int col, const char *needle) {
    if (!needle[0] || line_count == 0) {
        return 0;
    }
    int total = line_count;
    for (int step = 0; step <= total; step++) {
        int r = (row + step) % total;
        int start = (step == 0) ? col : 0;
        for (int c = start; c < line_len[r]; c++) {
            if (line_has_at(r, c, needle)) {
                cur_row = r;
                cur_col = c;
                return 1;
            }
        }
    }
    return 0;
}

static void find_next(void) {
    if (!find_needle[0]) {
        memcpy(status, "Nothing to find - Ctrl+F first.", sizeof("Nothing to find - Ctrl+F first."));
        return;
    }
    int from_row = cur_row;
    int from_col = cur_col + 1;
    if (from_col > line_len[cur_row]) {
        from_col = 0;
        from_row = (cur_row + 1) % line_count;
    }
    if (find_from(from_row, from_col, find_needle)) {
        clamp_cursor();
        status[0] = '\0';
    } else {
        int i = 0;
        static const char msg[] = "Not found: ";
        for (; msg[i]; i++) {
            status[i] = msg[i];
        }
        for (int j = 0; find_needle[j] && i < COLS - 1; j++, i++) {
            status[i] = find_needle[j];
        }
        status[i] = '\0';
    }
}

static void apply_inverse(const edit_t *e) {
    if (e->kind == EDIT_INSERT) {
        cur_row = e->row;
        cur_col = e->col + 1;
        clamp_cursor();
        backspace();
    } else if (e->kind == EDIT_DELETE) {
        cur_row = e->row;
        cur_col = e->col;
        clamp_cursor();
        insert_char(e->ch);
    } else if (e->kind == EDIT_SPLIT) {
        join_line(e->row);
        cur_row = e->row;
        cur_col = e->col;
    } else if (e->kind == EDIT_JOIN) {
        split_line(e->row, e->col);
        cur_row = e->row + 1;
        cur_col = 0;
    }
}

static void apply_forward(const edit_t *e) {
    if (e->kind == EDIT_INSERT) {
        cur_row = e->row;
        cur_col = e->col;
        clamp_cursor();
        insert_char(e->ch);
    } else if (e->kind == EDIT_DELETE) {
        cur_row = e->row;
        cur_col = e->col + 1;
        clamp_cursor();
        backspace();
    } else if (e->kind == EDIT_SPLIT) {
        split_line(e->row, e->col);
        cur_row = e->row + 1;
        cur_col = 0;
    } else if (e->kind == EDIT_JOIN) {
        join_line(e->row);
        cur_row = e->row;
        cur_col = e->col;
    }
}

static void undo_last_group(void) {
    if (undo_count == 0) {
        memcpy(status, "Nothing to undo.", sizeof("Nothing to undo."));
        return;
    }
    int last = (undo_head - 1 + UNDO_MAX) % UNDO_MAX;
    uint8_t group = undo_ring[last].group;
    undo_replaying = 1;
    while (undo_count > 0) {
        int at = (undo_head - 1 + UNDO_MAX) % UNDO_MAX;
        if (undo_ring[at].group != group) {
            break;
        }
        edit_t e = undo_ring[at];
        undo_head = at;
        undo_count--;
        if (redo_count < UNDO_MAX) {
            redo_count++;
        }
        apply_inverse(&e);
    }
    undo_replaying = 0;
    clamp_cursor();
    dirty = 1;
}

static void redo_next_group(void) {
    if (redo_count == 0) {
        memcpy(status, "Nothing to redo.", sizeof("Nothing to redo."));
        return;
    }
    uint8_t group = undo_ring[undo_head].group;
    undo_replaying = 1;
    while (redo_count > 0 && undo_ring[undo_head].group == group) {
        edit_t e = undo_ring[undo_head];
        undo_head = (undo_head + 1) % UNDO_MAX;
        redo_count--;
        if (undo_count < UNDO_MAX) {
            undo_count++;
        }
        apply_forward(&e);
    }
    undo_replaying = 0;
    clamp_cursor();
    dirty = 1;
}

static void paste_from_clipboard(void) {
    static char buf[1024];
    long n = sys_clipboard_get(buf, sizeof(buf));
    if (n <= 0) {
        return;
    }
    if (n > (long)sizeof(buf)) {
        n = (long)sizeof(buf);
    }
    undo_begin_group();
    for (long i = 0; i < n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            split_at_cursor();
        } else if (buf[i] >= 0x20 && buf[i] < 0x7F) {
            insert_char(buf[i]);
        }
    }
    clamp_cursor();
}

static void copy_selection_to_clipboard(void) {
    int sr, sc, er, ec;
    normalized_selection(&sr, &sc, &er, &ec);
    static char buf[1024];
    size_t n = 0;
    for (int r = sr; r <= er && n < sizeof(buf); r++) {
        int col_start = (r == sr) ? sc : 0;
        int col_end = (r == er) ? ec : line_len[r];
        for (int c = col_start; c < col_end && n < sizeof(buf); c++) {
            buf[n++] = lines[r][c];
        }
        if (r != er && n < sizeof(buf)) {
            buf[n++] = '\n';
        }
    }
    sys_clipboard_set(buf, n);
}

static void redraw(wm_window_t *win) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);

    if (sel_active || sel_dragging) {
        int sr, sc, er, ec;
        normalized_selection(&sr, &sc, &er, &ec);
        for (int r = sr; r <= er; r++) {
            if (r < scroll_top || r >= scroll_top + TEXT_ROWS) {
                continue;
            }
            int col_start = (r == sr) ? sc : 0;
            int col_end = (r == er) ? ec : line_len[r];
            if (col_end <= col_start) {
                continue;
            }
            int32_t y = CONTENT_Y0 + (r - scroll_top) * FONT_HEIGHT;
            gfx_fill_rect(&win->gfx, col_start * FONT_WIDTH, y, (col_end - col_start) * FONT_WIDTH, FONT_HEIGHT, SELECTION_COLOR);
        }
    }

    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, CONTENT_Y0, MENU_BG);
    gfx_draw_text(&win->gfx, FILE_MENU_X, 2, "File", MENU_TEXT);

    for (int r = 0; r < TEXT_ROWS; r++) {
        int src = scroll_top + r;
        if (src >= line_count) {
            break;
        }
        char row_buf[MAX_LINE_LEN + 1];
        memcpy(row_buf, lines[src], (size_t)line_len[src]);
        row_buf[line_len[src]] = '\0';
        gfx_draw_text_mono(&win->gfx, 0, CONTENT_Y0 + r * FONT_HEIGHT, row_buf, TEXT_COLOR);
    }
    gfx_fill_rect(&win->gfx, (cur_col) * FONT_WIDTH,
                  CONTENT_Y0 + (cur_row - scroll_top) * FONT_HEIGHT + FONT_HEIGHT - 2,
                  FONT_WIDTH, 2, CURSOR_COLOR);

    int status_y = CONTENT_Y0 + TEXT_ROWS * FONT_HEIGHT;
    gfx_fill_rect(&win->gfx, 0, status_y, WIN_W, FONT_HEIGHT, STATUS_BG);
    gfx_draw_text(&win->gfx, 4, status_y,
                  status[0] ? status
                            : (truncated ? "Showing the first part of a longer file"
                                         : "Ctrl+S save  Ctrl+Z undo  Ctrl+Y redo  Ctrl+F find  Ctrl+G next"),
                  STATUS_COLOR);

    if (menu_open) {
        gfx_draw_menu(&win->gfx, FILE_MENU_X, CONTENT_Y0, FILE_MENU_ITEM_W, FILE_MENU_ITEM_H,
                      FILE_MENU_ITEMS, FILE_MENU_COUNT, -1,
                      MENU_BG, MENU_HOVER_BG, MENU_BORDER, MENU_TEXT);
    }

    if (prompt_kind != PROMPT_NONE) {
        int32_t x = (WIN_W - PROMPT_W) / 2;
        int32_t y = (WIN_H - PROMPT_H) / 2;
        gfx_fill_rect_rounded(&win->gfx, x, y, PROMPT_W, PROMPT_H, PROMPT_BG);
        gfx_draw_rect_rounded(&win->gfx, x, y, PROMPT_W, PROMPT_H, PROMPT_BORDER);
        if (prompt_kind == PROMPT_SAVE_AS || prompt_kind == PROMPT_FIND) {
            gfx_draw_text(&win->gfx, x + GFX_PAD, y + 8,
                          prompt_kind == PROMPT_FIND ? "Find (Enter=search, click=cancel):"
                                                     : "Save as (Enter=save, click=cancel):",
                          PROMPT_TEXT);
            gfx_fill_rect_rounded(&win->gfx, x + GFX_PAD, y + 28, PROMPT_W - 2 * GFX_PAD, UI_FONT_UI_HEIGHT + 4, PROMPT_INPUT_BG);
            char buf[PROMPT_MAX_LEN + 1];
            memcpy(buf, prompt_buf, (size_t)prompt_len);
            buf[prompt_len] = '\0';
            gfx_draw_text(&win->gfx, x + GFX_PAD + 4, y + 30, buf, PROMPT_TEXT);
            gfx_fill_rect(&win->gfx, x + GFX_PAD + 4 + gfx_text_width(gfx_ui_font(), buf), y + 30,
                          2, (int32_t)gfx_ui_font()->height, CURSOR_COLOR);
        } else {
            gfx_draw_text(&win->gfx, x + GFX_PAD, y + 8, "Discard unsaved changes?", PROMPT_TEXT);
            gfx_draw_text(&win->gfx, x + GFX_PAD, y + 32, "Y = discard      N / click = cancel", PROMPT_TEXT);
        }
    }
}

int main(int argc, char **argv) {
    const char *arg = argc > 1 ? argv[1] : "";
    int i = 0;
    for (; arg && arg[i] && i < (int)sizeof(filename) - 1; i++) {
        filename[i] = arg[i];
    }
    filename[i] = '\0';
    if (filename[0] == '\0') {
        memcpy(filename, PATH_HOME_DIR "untitled", sizeof(PATH_HOME_DIR "untitled"));
    }

    line_len[0] = 0;
    load_file(filename);
    clamp_cursor();
    status[0] = '\0';

    wm_window_t win;
    if (wm_connect_confirm_close(WIN_W, WIN_H, "Editor", &win) != 0) {
        sys_exit(1);
    }

    redraw(&win);
    wm_present(&win);

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                changed = 1;
                continue;
            }
            if (prompt_kind == PROMPT_SAVE_AS || prompt_kind == PROMPT_FIND) {
                if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                    prompt_kind = PROMPT_NONE;
                } else if (ev.type == WM_EVENT_KEY) {
                    if (ev.ch == '\n' || ev.ch == '\r') {
                        if (prompt_kind == PROMPT_FIND) {
                            prompt_buf[prompt_len] = '\0';
                            memcpy(find_needle, prompt_buf, (size_t)prompt_len + 1);
                            prompt_kind = PROMPT_NONE;
                            if (!find_from(cur_row, cur_col, find_needle)) {
                                find_next();
                            }
                            clamp_cursor();
                        } else {
                            confirm_save_as();
                        }
                    } else if (ev.ch == '\b' || ev.ch == 0x7F) {
                        if (prompt_len > 0) {
                            prompt_len--;
                        }
                    } else if (ev.ch >= 0x20 && ev.ch < 0x7F && prompt_len < PROMPT_MAX_LEN) {
                        prompt_buf[prompt_len++] = ev.ch;
                    }
                }
                changed = 1;
                continue;
            }
            if (prompt_kind == PROMPT_CONFIRM_DISCARD) {
                if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                    confirm_discard(0);
                } else if (ev.type == WM_EVENT_KEY && (ev.ch == 'y' || ev.ch == 'Y')) {
                    confirm_discard(1);
                } else if (ev.type == WM_EVENT_KEY && (ev.ch == 'n' || ev.ch == 'N')) {
                    confirm_discard(0);
                }
                changed = 1;
                continue;
            }

            if (ev.type == WM_EVENT_DROP) {
                char dropped[sizeof(pending_drop)];
                if (wm_drag_payload(dropped, sizeof(dropped)) == 0 && dropped[0]) {
                    if (dirty) {
                        int i = 0;
                        for (; dropped[i] && i < (int)sizeof(pending_drop) - 1; i++) {
                            pending_drop[i] = dropped[i];
                        }
                        pending_drop[i] = '\0';
                        prompt_kind = PROMPT_CONFIRM_DISCARD;
                        pending_action = PENDING_DROP;
                    } else {
                        load_file(dropped);
                    }
                }
            } else if (ev.type == WM_EVENT_CLOSE_REQUEST) {
                request_action(PENDING_QUIT);
            } else if (ev.type == WM_EVENT_QUERY_SHUTDOWN) {
                if (dirty) {
                    wm_veto_shutdown(win.window_id);
                }
            } else if (ev.type == WM_EVENT_MOUSE_WHEEL) {
                int max_top = line_count - TEXT_ROWS;
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
                scroll_top = want;
            } else if (ev.type == WM_EVENT_MOUSE_MOVE && sel_dragging) {
                pixel_to_grid(ev.x, ev.y, &sel_end_row, &sel_end_col);
                cur_row = sel_end_row;
                cur_col = sel_end_col;
                clamp_cursor();
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                if (gfx_point_in_rect(ev.x, ev.y, 0, 0,
                                      FILE_MENU_X + gfx_text_width(gfx_ui_font(), "File") + 8, CONTENT_Y0)) {
                    menu_open = !menu_open;
                } else if (menu_open) {
                    int idx = gfx_menu_hit_test(ev.x, ev.y, FILE_MENU_X, CONTENT_Y0,
                                                 FILE_MENU_ITEM_W, FILE_MENU_ITEM_H, FILE_MENU_COUNT);
                    menu_open = 0;
                    if (idx >= 0) {
                        run_file_menu_item(idx);
                    }
                } else if (ev.y >= CONTENT_Y0 && ev.y < CONTENT_Y0 + TEXT_ROWS * FONT_HEIGHT) {
                    sel_active = 0;
                    sel_dragging = 1;
                    pixel_to_grid(ev.x, ev.y, &sel_anchor_row, &sel_anchor_col);
                    sel_end_row = sel_anchor_row;
                    sel_end_col = sel_anchor_col;
                    cur_row = sel_anchor_row;
                    cur_col = sel_anchor_col;
                    clamp_cursor();
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && !(ev.buttons & 1) && sel_dragging) {
                sel_dragging = 0;
                sel_active = (sel_anchor_row != sel_end_row || sel_anchor_col != sel_end_col);
            } else if (ev.type == WM_EVENT_KEY) {
                long mods = ev.mods;
                if ((mods & KBD_MOD_CTRL) && (ev.ch == 'c' || ev.ch == 'C')) {
                    if (sel_active) {
                        copy_selection_to_clipboard();
                    }
                } else if ((mods & KBD_MOD_CTRL) && (ev.ch == 'v' || ev.ch == 'V')) {
                    status[0] = '\0';
                    clear_selection();
                    paste_from_clipboard();
                } else if ((mods & KBD_MOD_CTRL) && (ev.ch == 'z' || ev.ch == 'Z')) {
                    status[0] = '\0';
                    clear_selection();
                    undo_last_group();
                } else if ((mods & KBD_MOD_CTRL) && (ev.ch == 'y' || ev.ch == 'Y')) {
                    status[0] = '\0';
                    clear_selection();
                    redo_next_group();
                } else if ((mods & KBD_MOD_CTRL) && (ev.ch == 'f' || ev.ch == 'F')) {
                    prompt_kind = PROMPT_FIND;
                    prompt_len = 0;
                    for (; find_needle[prompt_len] && prompt_len < PROMPT_MAX_LEN; prompt_len++) {
                        prompt_buf[prompt_len] = find_needle[prompt_len];
                    }
                    prompt_buf[prompt_len] = '\0';
                } else if ((mods & KBD_MOD_CTRL) && (ev.ch == 'g' || ev.ch == 'G')) {
                    clear_selection();
                    find_next();
                } else if ((mods & KBD_MOD_CTRL) && (ev.ch == 's' || ev.ch == 'S')) {
                    save_file();
                } else {
                    status[0] = '\0';
                    clear_selection();
                    undo_begin_group();
                    handle_char(ev.ch);
                }
            }
            changed = 1;
        }
        if (changed) {
            redraw(&win);
            wm_present(&win);
        }
        wm_wait_ms(&win, NULL, 0, -1);
    }
}
