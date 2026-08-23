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
 *
 * M35/M36 added a File menu (New/Save/Save As/Quit) and the two prompts
 * "Save As" and "Quit"/titlebar-close needed once there was a real way to
 * lose unsaved work: a filename input (there was previously no way to
 * save under a different name at all) and a discard-confirm, both drawn
 * as a small modal-to-this-window overlay rather than a new compositor-
 * level popup surface - see milestones.md's M35/M36 entries for why. The
 * titlebar close button reaching this editor's own confirm prompt instead
 * of an unconditional SIGTERM needed one new opt-in protocol field
 * (wm_create_request_t.confirm_close, system_api/include/wm.h) - every
 * other GUI client in this project still closes the old way, unchanged.
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
/* M41: this window no longer draws a menu row of its own. M35 put a
 * "File" label and its dropdown inside this window because there was
 * nowhere else to put them; there is now - the shared top bar
 * (menu_bar.c) shows whichever app is focused. Keeping both would be
 * exactly the "second copy living in two places" this milestone existed
 * to remove, so the row is gone and the reclaimed height goes back to
 * the text. What's left here is the *definition* of the menu (FILE_MENU
 * below, declared once at startup) and the handling of a pick coming
 * back as WM_EVENT_MENU_COMMAND - this app still owns both ends; only
 * the drawing moved. */
#define TEXT_ROWS (ROWS - STATUS_ROWS)
#define CONTENT_Y0 0

static const char *const FILE_MENU_ITEMS[] = {"New", "Save", "Save As", "Quit"};
#define FILE_MENU_COUNT ((int)(sizeof(FILE_MENU_ITEMS) / sizeof(FILE_MENU_ITEMS[0])))

/* M36: modal-to-this-window-only prompts (see this file's own note in
 * redraw()/main() and milestones.md's M36 entry for why this stays
 * client-side rather than a compositor-level modal). Both share one
 * centered box; only PROMPT_SAVE_AS's contents accept typed input. */
typedef enum { PROMPT_NONE = 0, PROMPT_SAVE_AS, PROMPT_CONFIRM_DISCARD } prompt_kind_t;
typedef enum { PENDING_NONE = 0, PENDING_NEW, PENDING_QUIT } pending_action_t;

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
static int scroll_top; /* index of the first line[] drawn in the text viewport */
static int dirty; /* unsaved changes since the last Ctrl+S */

static char filename[64];
static char status[COLS + 1];

static prompt_kind_t prompt_kind;
static pending_action_t pending_action; /* what to do once a PROMPT_CONFIRM_DISCARD is answered "yes" */
static char prompt_buf[PROMPT_MAX_LEN + 1];
static int prompt_len;

/* M37: click-drag text selection - the piece M32's own header comment on
 * gui_terminal.c's Ctrl+C flagged as missing ("there's no text selection
 * UI to copy from") ever since. sel_dragging is true only while the
 * mouse button is physically held; sel_active survives the button
 * release so the highlight (and a later Ctrl+C) still has something to
 * act on until the next click or keystroke clears it. */
static int sel_dragging, sel_active;
static int sel_anchor_row, sel_anchor_col, sel_end_row, sel_end_col;
#define SELECTION_COLOR 0x00355070u

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
    memcpy(filename, "untitled", sizeof("untitled"));
    dirty = 0;
    status[0] = '\0';
    clear_selection(); /* M37: the old selection's row indices may no longer even exist in the fresh, 1-line buffer */
}

/* M36: the exit code deliberately isn't 0 - M29's reap_dead_clients only
 * ever reclaims a window slot on a *nonzero* SYS_exit (a clean exit(0) is
 * treated as "still meant to be showing something," see wm_demo's own
 * self-test) - so a plain sys_exit(0) here would leave a stale, unclosable
 * window on screen despite the process actually being gone. Any nonzero
 * code reclaims it identically (SYS_task_alive's own doc comment already
 * lumps "any other non-zero SYS_exit" in with a real crash for exactly
 * this reason) - 1 is just this app's own convention for "closed on
 * purpose," not a magic value the kernel treats specially. */
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
        save_file();
    }
    prompt_kind = PROMPT_NONE;
}

/* Shared by the File menu's New/Quit items and WM_EVENT_CLOSE_REQUEST
 * (the titlebar close button, once this window opted into confirm_close -
 * see wm_connect_confirm_close) - same "one real path, not near-copies"
 * shape as compositor.c's own apply_window_action. */
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
    }
}

static void run_file_menu_item(int idx) {
    if (idx == 0) { /* New */
        request_action(PENDING_NEW);
    } else if (idx == 1) { /* Save */
        save_file();
    } else if (idx == 2) { /* Save As */
        begin_save_as();
    } else if (idx == 3) { /* Quit */
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
                continue; /* an empty span on this row (e.g. selection starts at EOL) - nothing to shade */
            }
            int32_t y = CONTENT_Y0 + (r - scroll_top) * FONT_HEIGHT;
            gfx_fill_rect(&win->gfx, col_start * FONT_WIDTH, y, (col_end - col_start) * FONT_WIDTH, FONT_HEIGHT, SELECTION_COLOR);
        }
    }

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

    if (prompt_kind != PROMPT_NONE) {
        int32_t x = (WIN_W - PROMPT_W) / 2;
        int32_t y = (WIN_H - PROMPT_H) / 2;
        gfx_fill_rect(&win->gfx, x, y, PROMPT_W, PROMPT_H, PROMPT_BG);
        gfx_draw_rect(&win->gfx, x, y, PROMPT_W, PROMPT_H, PROMPT_BORDER);
        if (prompt_kind == PROMPT_SAVE_AS) {
            gfx_draw_text(&win->gfx, x + 8, y + 6, "Save as (Enter=save, click=cancel):", PROMPT_TEXT);
            gfx_fill_rect(&win->gfx, x + 8, y + 26, PROMPT_W - 16, FONT_HEIGHT + 4, PROMPT_INPUT_BG);
            char buf[PROMPT_MAX_LEN + 1];
            memcpy(buf, prompt_buf, (size_t)prompt_len);
            buf[prompt_len] = '\0';
            gfx_draw_text(&win->gfx, x + 12, y + 28, buf, PROMPT_TEXT);
            gfx_fill_rect(&win->gfx, x + 12 + prompt_len * FONT_WIDTH, y + 28, 2, FONT_HEIGHT, CURSOR_COLOR);
        } else { /* PROMPT_CONFIRM_DISCARD */
            gfx_draw_text(&win->gfx, x + 8, y + 6, "Discard unsaved changes?", PROMPT_TEXT);
            gfx_draw_text(&win->gfx, x + 8, y + 30, "Y = discard      N / click = cancel", PROMPT_TEXT);
        }
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
    if (wm_connect_confirm_close(WIN_W, WIN_H, "Editor", &win) != 0) {
        sys_exit(1);
    }

    /* M41: hand the shared top bar this app's menu, once. The bar draws
     * the labels; picking one comes back as WM_EVENT_MENU_COMMAND below
     * and runs through the exact same run_file_menu_item this window's
     * own dropdown used to call. */
    {
        wm_menu_set_t declared;
        memset(&declared, 0, sizeof(declared));
        declared.menu_count = 1;
        strlcpy(declared.menus[0].title, "File", WM_MENU_TITLE_MAX);
        declared.menus[0].item_count = FILE_MENU_COUNT;
        for (int i = 0; i < FILE_MENU_COUNT; i++) {
            strlcpy(declared.menus[0].items[i], FILE_MENU_ITEMS[i], WM_MENU_ITEM_MAX);
        }
        wm_declare_menus(&win, &declared);
    }

    redraw(&win);

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            /* M36: an open prompt owns every event until answered - same
             * "in-progress interaction takes over the input stream"
             * shape as compositor.c's own drag state machine (M31), just
             * scoped to this one window instead of the whole desktop. */
            if (prompt_kind == PROMPT_SAVE_AS) {
                if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                    prompt_kind = PROMPT_NONE; /* click anywhere cancels */
                } else if (ev.type == WM_EVENT_KEY) {
                    if (ev.ch == '\n' || ev.ch == '\r') {
                        confirm_save_as();
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
                    confirm_discard(0); /* click cancels, same as N */
                } else if (ev.type == WM_EVENT_KEY && (ev.ch == 'y' || ev.ch == 'Y')) {
                    confirm_discard(1);
                } else if (ev.type == WM_EVENT_KEY && (ev.ch == 'n' || ev.ch == 'N')) {
                    confirm_discard(0);
                }
                changed = 1;
                continue;
            }

            if (ev.type == WM_EVENT_CLOSE_REQUEST) {
                request_action(PENDING_QUIT);
            } else if (ev.type == WM_EVENT_MENU_COMMAND) {
                /* M41: a pick from the shared top bar, arriving as
                 * (menu index, item index) into the set declared at
                 * startup - see wm.h's WM_EVENT_MENU_COMMAND. Only one
                 * menu is declared, so ev.x is always 0; ev.y indexes
                 * FILE_MENU_ITEMS, and runs through the exact same
                 * function this window's own dropdown used to call. */
                if (ev.x == 0) {
                    run_file_menu_item(ev.y);
                }
            } else if (ev.type == WM_EVENT_MOUSE_MOVE && sel_dragging) {
                pixel_to_grid(ev.x, ev.y, &sel_end_row, &sel_end_col);
                cur_row = sel_end_row;
                cur_col = sel_end_col;
                clamp_cursor();
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                if (ev.y >= CONTENT_Y0 && ev.y < CONTENT_Y0 + TEXT_ROWS * FONT_HEIGHT) {
                    /* M37: a plain click (no drag) just moves the cursor
                     * there, same as a real editor - sel_active only
                     * turns on at button-up if the drag actually covered
                     * more than one grid cell. */
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
                long mods = sys_kbd_modifiers();
                if ((mods & KBD_MOD_CTRL) && (ev.ch == 'c' || ev.ch == 'C')) {
                    if (sel_active) {
                        copy_selection_to_clipboard();
                    }
                } else if ((mods & KBD_MOD_CTRL) && (ev.ch == 's' || ev.ch == 'S')) {
                    save_file();
                } else {
                    status[0] = '\0';
                    clear_selection();
                    handle_char(ev.ch);
                }
            }
            changed = 1;
        }
        if (changed) {
            redraw(&win);
        }
    }
}
