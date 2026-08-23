/* user_space/bin/text_editor.c
 *
 * M33: the missing "edit a file without a host toolchain" piece - a
 * real (if deliberately small) multi-line text editor, same "own the
 * pixels, own the input" shape as every other WM client here (gui_
 * terminal.c especially - the grid/viewport rendering below is the same
 * idea, just persistent and cursor-addressable instead of scrolling
 * output away).
 *
 * Editing model, and why it's this shape: this kernel's keyboard driver
 * (kernel/drivers/keyboard.c) decodes exactly four extended scancodes -
 * the arrow keys, M33's own addition - and nothing else extended (no
 * Home/End/Delete/PageUp). Left/Right/Up/Down move the cursor anywhere
 * in the already-loaded text, and typing/Backspace edit *at* that
 * position - real mid-file editing, not just appending. Enter is the one
 * real simplification: it always inserts a fresh empty line at the very
 * *end* of the file and moves the cursor there, regardless of the
 * cursor's current row - splitting the current line at the cursor would
 * need each line to be its own growable/shiftable slot in the lines[]
 * array rather than a fixed MAX_LINES table of fixed MAX_LINE_LEN
 * buffers, real complexity this milestone's scope doesn't need. Net
 * effect: you can fix a typo anywhere, but new lines only ever get added
 * at the bottom, then edited into place with Up/Down/Left/Right same as
 * anything else already there.
 *
 * Ctrl+S saves to the filename this process was spawned with (SYS_spawn's
 * single-string arg - "untitled" if launched with none). SYS_writefile is
 * this milestone's other new addition (system_api/include/syscall.h) -
 * the write half of SYS_readfile that had simply never been exposed to
 * user space before something needed to save a file back out.
 */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT */
#include "str.h"
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
#define MAX_LINES    600 /* 600 * 80 = 48000 bytes of line storage - comfortably within a user process's SYS_sbrk-backed heap */
#define EDITOR_MAX_FILE 16384
#define STATUS_ROWS 1
/* M35: one row reserved for the File menu bar, on top of the existing
 * status row at the bottom - see redraw()/menu_open below. */
#define MENU_ROWS 1
#define TEXT_ROWS (ROWS - STATUS_ROWS - MENU_ROWS)
#define CONTENT_Y0 (MENU_ROWS * FONT_HEIGHT)

#define FILE_MENU_X 4
#define FILE_MENU_ITEM_W 110
#define FILE_MENU_ITEM_H (FONT_HEIGHT + 4)
static const char *const FILE_MENU_ITEMS[] = {"Save", "Quit"};
#define FILE_MENU_COUNT ((int)(sizeof(FILE_MENU_ITEMS) / sizeof(FILE_MENU_ITEMS[0])))

static char lines[MAX_LINES][MAX_LINE_LEN];
static int line_len[MAX_LINES];
static int line_count = 1;

static int cur_row, cur_col;
static int scroll_top; /* index of the first line[] drawn in the text viewport */
static int dirty; /* unsaved changes since the last Ctrl+S */
static int menu_open; /* M35: File menu dropdown - toggled by clicking "File" in the menu bar */

static char filename[64];
static char status[COLS + 1];

static void load_file(const char *name) {
    static char file_buf[EDITOR_MAX_FILE];
    long n = sys_readfile(name, file_buf, sizeof(file_buf));
    if (n < 0) {
        return; /* doesn't exist yet - starts as one empty line, same as "new file" */
    }
    if (n > (long)sizeof(file_buf)) {
        n = (long)sizeof(file_buf);
    }
    line_count = 0;
    int col = 0;
    for (long i = 0; i < n && line_count < MAX_LINES; i++) {
        char c = file_buf[i];
        if (c == '\n') {
            line_len[line_count++] = col;
            col = 0;
        } else if (col < MAX_LINE_LEN - 1) {
            lines[line_count][col++] = c;
        }
    }
    if (line_count < MAX_LINES && (col > 0 || line_count == 0)) {
        line_len[line_count++] = col;
    }
    if (line_count == 0) {
        line_count = 1;
        line_len[0] = 0;
    }
}

static void save_file(void) {
    static char out_buf[EDITOR_MAX_FILE];
    size_t written = 0;
    for (int r = 0; r < line_count; r++) {
        int n = line_len[r];
        if (written + (size_t)n + 1 > sizeof(out_buf)) {
            break; /* file grew past what this editor can hold at all - silently stops rather than corrupting a partial write */
        }
        memcpy(out_buf + written, lines[r], (size_t)n);
        written += (size_t)n;
        out_buf[written++] = '\n';
    }
    int ok = sys_writefile(filename, out_buf, written) == 0;
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

static void insert_char(char c) {
    int *len = &line_len[cur_row];
    if (*len >= MAX_LINE_LEN - 1) {
        return; /* line's own fixed cap - silently refuses rather than corrupting adjacent memory */
    }
    for (int i = *len; i > cur_col; i--) {
        lines[cur_row][i] = lines[cur_row][i - 1];
    }
    lines[cur_row][cur_col] = c;
    (*len)++;
    cur_col++;
    dirty = 1;
}

static void backspace(void) {
    if (cur_col == 0) {
        return; /* no line-merge - see this file's own header comment on Enter's matching simplification */
    }
    int *len = &line_len[cur_row];
    for (int i = cur_col - 1; i < *len - 1; i++) {
        lines[cur_row][i] = lines[cur_row][i + 1];
    }
    (*len)--;
    cur_col--;
    dirty = 1;
}

static void new_line_at_end(void) {
    if (line_count >= MAX_LINES) {
        return;
    }
    line_len[line_count] = 0;
    line_count++;
    cur_row = line_count - 1;
    cur_col = 0;
    dirty = 1;
}

static void handle_char(char ch) {
    if (ch == '\n' || ch == '\r') {
        new_line_at_end();
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
        return; /* unrecognized control byte - nothing to redraw for */
    }
    clamp_cursor();
}

static void run_file_menu_item(int idx) {
    if (idx == 0) { /* Save */
        save_file();
    } else if (idx == 1) { /* Quit */
        sys_exit(0);
    }
}

static void redraw(wm_window_t *win) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);

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
        gfx_draw_text(&win->gfx, 0, CONTENT_Y0 + r * FONT_HEIGHT, row_buf, TEXT_COLOR);
    }
    gfx_fill_rect(&win->gfx, (cur_col) * FONT_WIDTH,
                  CONTENT_Y0 + (cur_row - scroll_top) * FONT_HEIGHT + FONT_HEIGHT - 2,
                  FONT_WIDTH, 2, CURSOR_COLOR);

    int status_y = CONTENT_Y0 + TEXT_ROWS * FONT_HEIGHT;
    gfx_fill_rect(&win->gfx, 0, status_y, WIN_W, FONT_HEIGHT, STATUS_BG);
    gfx_draw_text(&win->gfx, 4, status_y, status[0] ? status : "Ctrl+S to save", STATUS_COLOR);

    /* Dropdown drawn last so it overlays whatever content is underneath -
     * this app owns its whole window buffer, there's no compositor-level
     * popup surface to draw it into instead (see M35's milestones.md
     * note on that scope trim). */
    if (menu_open) {
        gfx_draw_menu(&win->gfx, FILE_MENU_X, CONTENT_Y0, FILE_MENU_ITEM_W, FILE_MENU_ITEM_H,
                      FILE_MENU_ITEMS, FILE_MENU_COUNT, -1,
                      MENU_BG, MENU_HOVER_BG, MENU_BORDER, MENU_TEXT);
    }
}

int main(const char *arg) {
    int i = 0;
    for (; arg && arg[i] && i < (int)sizeof(filename) - 1; i++) {
        filename[i] = arg[i];
    }
    filename[i] = '\0';
    if (filename[0] == '\0') {
        memcpy(filename, "untitled", sizeof("untitled"));
    }

    line_len[0] = 0;
    load_file(filename);
    clamp_cursor();
    status[0] = '\0';

    wm_window_t win;
    if (wm_connect(WIN_W, WIN_H, "Editor", &win) != 0) {
        sys_exit(1);
    }

    redraw(&win);

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                /* M35: the "File" label toggles the dropdown; any other
                 * click while it's open either picks an item or - same
                 * as a real menu - just dismisses it, consumed either
                 * way so it never also reaches handle_char/save_file. */
                if (gfx_point_in_rect(ev.x, ev.y, 0, 0, FILE_MENU_X + 4 * FONT_WIDTH + 8, CONTENT_Y0)) {
                    menu_open = !menu_open;
                } else if (menu_open) {
                    int idx = gfx_menu_hit_test(ev.x, ev.y, FILE_MENU_X, CONTENT_Y0,
                                                 FILE_MENU_ITEM_W, FILE_MENU_ITEM_H, FILE_MENU_COUNT);
                    menu_open = 0;
                    if (idx >= 0) {
                        run_file_menu_item(idx);
                    }
                }
                changed = 1;
            } else if (ev.type == WM_EVENT_KEY) {
                long mods = sys_kbd_modifiers();
                if ((mods & KBD_MOD_CTRL) && (ev.ch == 's' || ev.ch == 'S')) {
                    save_file();
                } else {
                    status[0] = '\0';
                    handle_char(ev.ch);
                }
                changed = 1;
            }
        }
        if (changed) {
            redraw(&win);
        }
    }
}
