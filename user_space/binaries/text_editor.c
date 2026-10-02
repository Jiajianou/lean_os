#include "paths.h"
#include "font8x16.h"
#include "string_utilities.h"
#include "recent.h"
#include "syscall_wrappers.h"
#include "window_manager_client.h"

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

#define MAX_LINE_LENGTH (COLS)
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
#define PROMPT_MAX_LENGTH  48

static char lines[MAX_LINES][MAX_LINE_LENGTH];
static int line_length[MAX_LINES];
static int line_count = 1;

static int current_row, current_col;
static int scroll_top;
static int dirty;
static int truncated;
static int menu_open;

static char filename[PATH_MAX_LENGTH];
static char pending_drop[64];
static char status[COLS + 1];

static prompt_kind_t prompt_kind;
static pending_action_t pending_action;
static char prompt_buffer[PROMPT_MAX_LENGTH + 1];
static int prompt_length;

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
                line_length[line_count++] = col;
                col = 0;
            } else if (col < MAX_LINE_LENGTH - 1) {
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
        line_length[line_count++] = col;
    }
    if (line_count == 0) {
        line_count = 1;
        line_length[0] = 0;
    }
}

static void save_file(void) {
    if (truncated) {
        memcpy(status, "File is longer than this editor holds - use Save As.",
               sizeof("File is longer than this editor holds - use Save As."));
        return;
    }
    char tmpname[PATH_MAX_LENGTH];
    int t = 0;
    for (; filename[t] && t < PATH_MAX_LENGTH - 8; t++) {
        tmpname[t] = filename[t];
    }
    static const char suffix[] = ".tmp~";
    for (int k = 0; suffix[k] && t < PATH_MAX_LENGTH - 1; k++, t++) {
        tmpname[t] = suffix[k];
    }
    tmpname[t] = '\0';

    long fd = sys_open(tmpname, OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE);
    int ok = fd >= 0;
    for (int r = 0; ok && r < line_count; r++) {
        int n = line_length[r];
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
    if (current_row < 0) {
        current_row = 0;
    }
    if (current_row >= line_count) {
        current_row = line_count - 1;
    }
    if (current_col < 0) {
        current_col = 0;
    }
    if (current_col > line_length[current_row]) {
        current_col = line_length[current_row];
    }
    if (current_row < scroll_top) {
        scroll_top = current_row;
    }
    if (current_row >= scroll_top + TEXT_ROWS) {
        scroll_top = current_row - TEXT_ROWS + 1;
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
    int *length = &line_length[current_row];
    if (*length >= MAX_LINE_LENGTH - 1) {
        return;
    }
    for (int i = *length; i > current_col; i--) {
        lines[current_row][i] = lines[current_row][i - 1];
    }
    lines[current_row][current_col] = c;
    (*length)++;
    undo_record(EDIT_INSERT, current_row, current_col, c);
    current_col++;
    dirty = 1;
}

static int split_line(int row, int col) {
    if (line_count >= MAX_LINES) {
        return 0;
    }
    for (int r = line_count; r > row + 1; r--) {
        memcpy(lines[r], lines[r - 1], (size_t)line_length[r - 1]);
        line_length[r] = line_length[r - 1];
    }
    line_count++;
    int tail = line_length[row] - col;
    memcpy(lines[row + 1], lines[row] + col, (size_t)tail);
    line_length[row + 1] = tail;
    line_length[row] = col;
    return 1;
}

static int join_line(int row) {
    if (row + 1 >= line_count) {
        return 0;
    }
    if (line_length[row] + line_length[row + 1] > MAX_LINE_LENGTH - 1) {
        return 0;
    }
    memcpy(lines[row] + line_length[row], lines[row + 1], (size_t)line_length[row + 1]);
    line_length[row] += line_length[row + 1];
    for (int r = row + 1; r < line_count - 1; r++) {
        memcpy(lines[r], lines[r + 1], (size_t)line_length[r + 1]);
        line_length[r] = line_length[r + 1];
    }
    line_count--;
    return 1;
}

static void split_at_cursor(void) {
    if (!split_line(current_row, current_col)) {
        return;
    }
    undo_record(EDIT_SPLIT, current_row, current_col, 0);
    current_row++;
    current_col = 0;
    dirty = 1;
}

static void backspace(void) {
    if (current_col == 0) {
        if (current_row == 0) {
            return;
        }
        int at = line_length[current_row - 1];
        if (!join_line(current_row - 1)) {
            return;
        }
        undo_record(EDIT_JOIN, current_row - 1, at, 0);
        current_row--;
        current_col = at;
        dirty = 1;
        return;
    }
    int *length = &line_length[current_row];
    undo_record(EDIT_DELETE, current_row, current_col - 1, lines[current_row][current_col - 1]);
    for (int i = current_col - 1; i < *length - 1; i++) {
        lines[current_row][i] = lines[current_row][i + 1];
    }
    (*length)--;
    current_col--;
    dirty = 1;
}

/* M216: Delete takes the character after the cursor - at the end of a
   line, the line break - which is backspace from one place to the right,
   recorded for undo exactly as that backspace would be. */
static void delete_forward(void) {
    if (current_col < line_length[current_row]) {
        current_col++;
        backspace();
    } else if (current_row + 1 < line_count) {
        current_row++;
        current_col = 0;
        backspace();
    }
}

static void handle_char(char ch) {
    if (ch == '\n' || ch == '\r') {
        split_at_cursor();
    } else if (ch == '\b' || ch == 0x7F) {
        backspace();
    } else if (ch == KEYBOARD_KEY_DELETE) {
        delete_forward();
    } else if (ch == KEYBOARD_KEY_HOME) {
        current_col = 0;
    } else if (ch == KEYBOARD_KEY_END) {
        current_col = line_length[current_row];
    } else if (ch == KEYBOARD_KEY_PAGE_UP) {
        current_row -= TEXT_ROWS;
    } else if (ch == KEYBOARD_KEY_PAGE_DOWN) {
        current_row += TEXT_ROWS;
    } else if (ch == KEYBOARD_KEY_UP) {
        current_row--;
    } else if (ch == KEYBOARD_KEY_DOWN) {
        current_row++;
    } else if (ch == KEYBOARD_KEY_LEFT) {
        current_col--;
    } else if (ch == KEYBOARD_KEY_RIGHT) {
        current_col++;
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
    line_length[0] = 0;
    current_row = 0;
    current_col = 0;
    scroll_top = 0;
    memcpy(filename, PATH_HOME_DIRECTORY "untitled", sizeof(PATH_HOME_DIRECTORY "untitled"));
    dirty = 0;
    undo_reset();
    status[0] = '\0';
    clear_selection();
}

static window_manager_window_t win;

static void quit_now(void) {
    window_manager_quit(&win, 0);
}

static void begin_save_as(void) {
    prompt_kind = PROMPT_SAVE_AS;
    prompt_length = 0;
    for (; filename[prompt_length] && prompt_length < PROMPT_MAX_LENGTH; prompt_length++) {
        prompt_buffer[prompt_length] = filename[prompt_length];
    }
}

static void confirm_save_as(void) {
    prompt_buffer[prompt_length] = '\0';
    if (prompt_length > 0) {
        int i = 0;
        for (; prompt_buffer[i] && i < (int)sizeof(filename) - 1; i++) {
            filename[i] = prompt_buffer[i];
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
    if (col > line_length[row]) {
        col = line_length[row];
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

static char find_needle[PROMPT_MAX_LENGTH + 1];

static int line_has_at(int row, int col, const char *needle) {
    int n = 0;
    while (needle[n]) {
        if (col + n >= line_length[row] || lines[row][col + n] != needle[n]) {
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
        for (int c = start; c < line_length[r]; c++) {
            if (line_has_at(r, c, needle)) {
                current_row = r;
                current_col = c;
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
    int from_row = current_row;
    int from_col = current_col + 1;
    if (from_col > line_length[current_row]) {
        from_col = 0;
        from_row = (current_row + 1) % line_count;
    }
    if (find_from(from_row, from_col, find_needle)) {
        clamp_cursor();
        status[0] = '\0';
    } else {
        int i = 0;
        static const char message[] = "Not found: ";
        for (; message[i]; i++) {
            status[i] = message[i];
        }
        for (int j = 0; find_needle[j] && i < COLS - 1; j++, i++) {
            status[i] = find_needle[j];
        }
        status[i] = '\0';
    }
}

static void apply_inverse(const edit_t *e) {
    if (e->kind == EDIT_INSERT) {
        current_row = e->row;
        current_col = e->col + 1;
        clamp_cursor();
        backspace();
    } else if (e->kind == EDIT_DELETE) {
        current_row = e->row;
        current_col = e->col;
        clamp_cursor();
        insert_char(e->ch);
    } else if (e->kind == EDIT_SPLIT) {
        join_line(e->row);
        current_row = e->row;
        current_col = e->col;
    } else if (e->kind == EDIT_JOIN) {
        split_line(e->row, e->col);
        current_row = e->row + 1;
        current_col = 0;
    }
}

static void apply_forward(const edit_t *e) {
    if (e->kind == EDIT_INSERT) {
        current_row = e->row;
        current_col = e->col;
        clamp_cursor();
        insert_char(e->ch);
    } else if (e->kind == EDIT_DELETE) {
        current_row = e->row;
        current_col = e->col + 1;
        clamp_cursor();
        backspace();
    } else if (e->kind == EDIT_SPLIT) {
        split_line(e->row, e->col);
        current_row = e->row + 1;
        current_col = 0;
    } else if (e->kind == EDIT_JOIN) {
        join_line(e->row);
        current_row = e->row;
        current_col = e->col;
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
    static char buffer[1024];
    long n = sys_clipboard_get(buffer, sizeof(buffer));
    if (n <= 0) {
        return;
    }
    if (n > (long)sizeof(buffer)) {
        n = (long)sizeof(buffer);
    }
    undo_begin_group();
    for (long i = 0; i < n; i++) {
        if (buffer[i] == '\n' || buffer[i] == '\r') {
            split_at_cursor();
        } else if (buffer[i] >= 0x20 && buffer[i] < 0x7F) {
            insert_char(buffer[i]);
        }
    }
    clamp_cursor();
}

static void copy_selection_to_clipboard(void) {
    int sr, sc, er, ec;
    normalized_selection(&sr, &sc, &er, &ec);
    static char buffer[1024];
    size_t n = 0;
    for (int r = sr; r <= er && n < sizeof(buffer); r++) {
        int col_start = (r == sr) ? sc : 0;
        int col_end = (r == er) ? ec : line_length[r];
        for (int c = col_start; c < col_end && n < sizeof(buffer); c++) {
            buffer[n++] = lines[r][c];
        }
        if (r != er && n < sizeof(buffer)) {
            buffer[n++] = '\n';
        }
    }
    sys_clipboard_set(buffer, n);
}

static void redraw(window_manager_window_t *win) {
    graphics_fill_rect(&win->graphics, 0, 0, WIN_W, WIN_H, BG_COLOR);

    if (sel_active || sel_dragging) {
        int sr, sc, er, ec;
        normalized_selection(&sr, &sc, &er, &ec);
        for (int r = sr; r <= er; r++) {
            if (r < scroll_top || r >= scroll_top + TEXT_ROWS) {
                continue;
            }
            int col_start = (r == sr) ? sc : 0;
            int col_end = (r == er) ? ec : line_length[r];
            if (col_end <= col_start) {
                continue;
            }
            int32_t y = CONTENT_Y0 + (r - scroll_top) * FONT_HEIGHT;
            graphics_fill_rect(&win->graphics, col_start * FONT_WIDTH, y, (col_end - col_start) * FONT_WIDTH, FONT_HEIGHT, SELECTION_COLOR);
        }
    }

    graphics_fill_rect(&win->graphics, 0, 0, WIN_W, CONTENT_Y0, MENU_BG);
    graphics_draw_text(&win->graphics, FILE_MENU_X, 2, "File", MENU_TEXT);

    for (int r = 0; r < TEXT_ROWS; r++) {
        int source = scroll_top + r;
        if (source >= line_count) {
            break;
        }
        char row_buffer[MAX_LINE_LENGTH + 1];
        memcpy(row_buffer, lines[source], (size_t)line_length[source]);
        row_buffer[line_length[source]] = '\0';
        graphics_draw_text_mono(&win->graphics, 0, CONTENT_Y0 + r * FONT_HEIGHT, row_buffer, TEXT_COLOR);
    }
    graphics_fill_rect(&win->graphics, (current_col) * FONT_WIDTH,
                  CONTENT_Y0 + (current_row - scroll_top) * FONT_HEIGHT + FONT_HEIGHT - 2,
                  FONT_WIDTH, 2, CURSOR_COLOR);

    int status_y = CONTENT_Y0 + TEXT_ROWS * FONT_HEIGHT;
    graphics_fill_rect(&win->graphics, 0, status_y, WIN_W, FONT_HEIGHT, STATUS_BG);
    graphics_draw_text(&win->graphics, 4, status_y,
                  status[0] ? status
                            : (truncated ? "Showing the first part of a longer file"
                                         : "Ctrl+S save  Ctrl+Z undo  Ctrl+Y redo  Ctrl+F find  Ctrl+G next"),
                  STATUS_COLOR);

    if (menu_open) {
        graphics_draw_menu(&win->graphics, FILE_MENU_X, CONTENT_Y0, FILE_MENU_ITEM_W, FILE_MENU_ITEM_H,
                      FILE_MENU_ITEMS, FILE_MENU_COUNT, -1,
                      MENU_BG, MENU_HOVER_BG, MENU_BORDER, MENU_TEXT);
    }

    if (prompt_kind != PROMPT_NONE) {
        int32_t x = (WIN_W - PROMPT_W) / 2;
        int32_t y = (WIN_H - PROMPT_H) / 2;
        graphics_fill_rect_rounded(&win->graphics, x, y, PROMPT_W, PROMPT_H, PROMPT_BG);
        graphics_draw_rect_rounded(&win->graphics, x, y, PROMPT_W, PROMPT_H, PROMPT_BORDER);
        if (prompt_kind == PROMPT_SAVE_AS || prompt_kind == PROMPT_FIND) {
            graphics_draw_text(&win->graphics, x + GRAPHICS_PAD, y + 8,
                          prompt_kind == PROMPT_FIND ? "Find (Enter=search, click=cancel):"
                                                     : "Save as (Enter=save, click=cancel):",
                          PROMPT_TEXT);
            graphics_fill_rect_rounded(&win->graphics, x + GRAPHICS_PAD, y + 28, PROMPT_W - 2 * GRAPHICS_PAD, UI_FONT_UI_HEIGHT + 4, PROMPT_INPUT_BG);
            char buffer[PROMPT_MAX_LENGTH + 1];
            memcpy(buffer, prompt_buffer, (size_t)prompt_length);
            buffer[prompt_length] = '\0';
            graphics_draw_text(&win->graphics, x + GRAPHICS_PAD + 4, y + 30, buffer, PROMPT_TEXT);
            graphics_fill_rect(&win->graphics, x + GRAPHICS_PAD + 4 + graphics_text_width(graphics_ui_font(), buffer), y + 30,
                          2, (int32_t)graphics_ui_font()->height, CURSOR_COLOR);
        } else {
            graphics_draw_text(&win->graphics, x + GRAPHICS_PAD, y + 8, "Discard unsaved changes?", PROMPT_TEXT);
            graphics_draw_text(&win->graphics, x + GRAPHICS_PAD, y + 32, "Y = discard      N / click = cancel", PROMPT_TEXT);
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
        memcpy(filename, PATH_HOME_DIRECTORY "untitled", sizeof(PATH_HOME_DIRECTORY "untitled"));
    }

    line_length[0] = 0;
    load_file(filename);
    clamp_cursor();
    status[0] = '\0';

    if (window_manager_connect_confirm_close(WIN_W, WIN_H, "Editor", &win) != 0) {
        sys_exit(1);
    }

    redraw(&win);
    window_manager_present(&win);

    for (;;) {
        int changed = 0;
        window_manager_event_t ev;
        while (window_manager_poll_event(&win, &ev)) {
            if (ev.type == WINDOW_MANAGER_EVENT_EXPOSE || ev.type == WINDOW_MANAGER_EVENT_DISPLAY_CHANGED) {
                changed = 1;
                continue;
            }
            if (prompt_kind == PROMPT_SAVE_AS || prompt_kind == PROMPT_FIND) {
                if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                    prompt_kind = PROMPT_NONE;
                } else if (ev.type == WINDOW_MANAGER_EVENT_KEY) {
                    if (ev.ch == '\n' || ev.ch == '\r') {
                        if (prompt_kind == PROMPT_FIND) {
                            prompt_buffer[prompt_length] = '\0';
                            memcpy(find_needle, prompt_buffer, (size_t)prompt_length + 1);
                            prompt_kind = PROMPT_NONE;
                            if (!find_from(current_row, current_col, find_needle)) {
                                find_next();
                            }
                            clamp_cursor();
                        } else {
                            confirm_save_as();
                        }
                    } else if (ev.ch == '\b' || ev.ch == 0x7F) {
                        if (prompt_length > 0) {
                            prompt_length--;
                        }
                    } else if (ev.ch >= 0x20 && ev.ch < 0x7F && prompt_length < PROMPT_MAX_LENGTH) {
                        prompt_buffer[prompt_length++] = ev.ch;
                    }
                }
                changed = 1;
                continue;
            }
            if (prompt_kind == PROMPT_CONFIRM_DISCARD) {
                if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                    confirm_discard(0);
                } else if (ev.type == WINDOW_MANAGER_EVENT_KEY && (ev.ch == 'y' || ev.ch == 'Y')) {
                    confirm_discard(1);
                } else if (ev.type == WINDOW_MANAGER_EVENT_KEY && (ev.ch == 'n' || ev.ch == 'N')) {
                    confirm_discard(0);
                }
                changed = 1;
                continue;
            }

            if (ev.type == WINDOW_MANAGER_EVENT_DROP) {
                char dropped[sizeof(pending_drop)];
                if (window_manager_drag_payload(dropped, sizeof(dropped)) == 0 && dropped[0]) {
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
            } else if (ev.type == WINDOW_MANAGER_EVENT_CLOSE_REQUEST) {
                request_action(PENDING_QUIT);
            } else if (ev.type == WINDOW_MANAGER_EVENT_QUERY_SHUTDOWN) {
                if (dirty) {
                    window_manager_veto_shutdown(win.window_id);
                }
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_WHEEL) {
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
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_MOVE && sel_dragging) {
                pixel_to_grid(ev.x, ev.y, &sel_end_row, &sel_end_col);
                current_row = sel_end_row;
                current_col = sel_end_col;
                clamp_cursor();
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                if (graphics_point_in_rect(ev.x, ev.y, 0, 0,
                                      FILE_MENU_X + graphics_text_width(graphics_ui_font(), "File") + 8, CONTENT_Y0)) {
                    menu_open = !menu_open;
                } else if (menu_open) {
                    int idx = graphics_menu_hit_test(ev.x, ev.y, FILE_MENU_X, CONTENT_Y0,
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
                    current_row = sel_anchor_row;
                    current_col = sel_anchor_col;
                    clamp_cursor();
                }
            } else if (ev.type == WINDOW_MANAGER_EVENT_MOUSE_BUTTON && !(ev.buttons & 1) && sel_dragging) {
                sel_dragging = 0;
                sel_active = (sel_anchor_row != sel_end_row || sel_anchor_col != sel_end_col);
            } else if (ev.type == WINDOW_MANAGER_EVENT_KEY) {
                long mods = ev.mods;
                if ((mods & KEYBOARD_MOD_CTRL) && (ev.ch == 'c' || ev.ch == 'C')) {
                    if (sel_active) {
                        copy_selection_to_clipboard();
                    }
                } else if ((mods & KEYBOARD_MOD_CTRL) && (ev.ch == 'v' || ev.ch == 'V')) {
                    status[0] = '\0';
                    clear_selection();
                    paste_from_clipboard();
                } else if ((mods & KEYBOARD_MOD_CTRL) && (ev.ch == 'z' || ev.ch == 'Z')) {
                    status[0] = '\0';
                    clear_selection();
                    undo_last_group();
                } else if ((mods & KEYBOARD_MOD_CTRL) && (ev.ch == 'y' || ev.ch == 'Y')) {
                    status[0] = '\0';
                    clear_selection();
                    redo_next_group();
                } else if ((mods & KEYBOARD_MOD_CTRL) && (ev.ch == 'f' || ev.ch == 'F')) {
                    prompt_kind = PROMPT_FIND;
                    prompt_length = 0;
                    for (; find_needle[prompt_length] && prompt_length < PROMPT_MAX_LENGTH; prompt_length++) {
                        prompt_buffer[prompt_length] = find_needle[prompt_length];
                    }
                    prompt_buffer[prompt_length] = '\0';
                } else if ((mods & KEYBOARD_MOD_CTRL) && (ev.ch == 'g' || ev.ch == 'G')) {
                    clear_selection();
                    find_next();
                } else if ((mods & KEYBOARD_MOD_CTRL) && (ev.ch == 's' || ev.ch == 'S')) {
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
            window_manager_present(&win);
        }
        window_manager_wait_ms(&win, NULL, 0, -1);
    }
}
