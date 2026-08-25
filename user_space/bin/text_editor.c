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
 * M35/M36 added a File menu (New/Save/Save As/Quit) - drawn in this
 * window's own top row (M41 briefly moved it to a shared screen-top bar;
 * M42 brought it back, see MENU_ROWS below) - and the two prompts
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
#include "paths.h" /* system_api/include/paths.h - M53: /home is where a new document goes */
#include "font8x16.h" /* FONT_WIDTH/FONT_HEIGHT - the editor's column arithmetic is a fixed cell by definition (M57) */
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
#define STATUS_ROWS 1
/* M35: one row reserved for the File menu bar, on top of the existing
 * status row at the bottom - see redraw()/menu_open below.
 *
 * M41 moved this row out to a shared, screen-top menu bar that drew the
 * labels for whichever app was focused, over a three-pipe protocol.
 * M42 brought it back: this project follows the Windows convention, where
 * an app's menus live in its own window. That makes the whole
 * cross-process round trip unnecessary - the menu is drawn by the process
 * that owns it, hit-tested by the same gfx_draw_menu/gfx_menu_hit_test
 * pair desktop_icons.c's context menu uses, and "what does Save As mean"
 * never has to leave this file. */
#define MENU_ROWS 1
#define TEXT_ROWS (ROWS - STATUS_ROWS - MENU_ROWS)
#define CONTENT_Y0 (MENU_ROWS * FONT_HEIGHT)

#define FILE_MENU_X 4
#define FILE_MENU_ITEM_W 110
#define FILE_MENU_ITEM_H (UI_FONT_UI_HEIGHT + 4)
/* M57: "Save As" opens a dialog and now says so with the ellipsis glyph -
 * the oldest menu convention there is, and one this project could not
 * write down until the font had the character. */
static const char *const FILE_MENU_ITEMS[] = {"New", "Save", "Save As" UI_S_ELLIPSIS, "Quit"};
#define FILE_MENU_COUNT ((int)(sizeof(FILE_MENU_ITEMS) / sizeof(FILE_MENU_ITEMS[0])))

/* M36: modal-to-this-window-only prompts (see this file's own note in
 * redraw()/main() and milestones.md's M36 entry for why this stays
 * client-side rather than a compositor-level modal). Both share one
 * centered box; only PROMPT_SAVE_AS's contents accept typed input. */
/* M60: PROMPT_FIND joins them - the most-reached-for editor feature that
 * is not typing, and the same typed-input prompt shape Save As already
 * uses rather than a second kind of dialog. */
typedef enum { PROMPT_NONE = 0, PROMPT_SAVE_AS, PROMPT_CONFIRM_DISCARD, PROMPT_FIND } prompt_kind_t;
/* M49: PENDING_DROP joins the two the File menu already had - a file
 * dragged onto this window is one more thing that replaces the buffer,
 * so it goes through the same confirm-discard prompt rather than round
 * an existing guard. Which file is in pending_drop, below. */
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
static int scroll_top; /* index of the first line[] drawn in the text viewport */
static int dirty; /* unsaved changes since the last Ctrl+S */
/* M60: this file has more lines than this editor can hold, so what is on
 * screen is a prefix of it. A plain save over the original is refused
 * while this is set - writing back 600 lines of a thousand-line file is
 * how a person loses the other four hundred. */
static int truncated;
static int menu_open; /* M35: File menu dropdown - toggled by clicking "File" in the menu row */

/* M53: a path, not a name - PATH_MAX_LEN so it can hold anything the
 * resolver will accept. It was 64 when a filename was at most 28
 * characters and there was nowhere for it to live but the root. */
static char filename[PATH_MAX_LEN];
/* M49: the file a pending PENDING_DROP will open once the discard prompt
 * is answered. Its own buffer rather than filename's, so declining the
 * prompt leaves the current file's name untouched. */
static char pending_drop[64];
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

/* M60: streamed through a descriptor rather than read whole.
 *
 * This used to `sys_readfile` into a 16 KiB buffer, which is why the
 * editor's real limit was neither MAX_LINES nor MAX_LINE_LEN but "how
 * much of a file fits in one whole-file read" - and why a file larger
 * than that opened silently truncated. The byte cap is gone: what is
 * left is the line cap, which is a property of what this editor can hold
 * rather than of how it reads.
 *
 * A file with more lines than MAX_LINES still cannot be *held*, and that
 * is said out loud rather than hidden - `truncated` refuses a plain save
 * over the original afterwards, because writing back 600 lines of a
 * thousand-line file is how a person loses the other four hundred. */
static void load_file(const char *name) {
    long fd = sys_open(name, OPEN_READ);
    if (fd < 0) {
        return; /* doesn't exist yet - starts as one empty line, same as "new file" */
    }
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
    /* M60: a file this editor could only hold a prefix of is not one it
     * may write back over. Save As is still offered, and is the honest
     * way to keep what is on screen without destroying what is not. */
    if (truncated) {
        memcpy(status, "File is longer than this editor holds - use Save As.",
               sizeof("File is longer than this editor holds - use Save As."));
        return;
    }
    /* M60: streamed out a line at a time, so what this can save is what
     * it can hold rather than what fits in a second whole-file buffer. */
    long fd = sys_open(filename, OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE);
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

/* ---- M56: undo -------------------------------------------------------
 *
 * The single most-missed thing in any editor, and this one had a
 * clipboard before it had this - so a paste was unrecoverable.
 *
 * A bounded ring of edit *records*, not snapshots. A snapshot-per-
 * keystroke editor is one that stops working on a large file: this
 * buffer is 600 x 80 characters, so even ten levels of undo would be
 * half a megabyte of copies, and the thing being copied barely changes
 * between them. A record is six bytes.
 *
 * There are exactly three mutations in this file, which is what makes
 * records practical here at all: insert one character, delete one
 * character, and append an empty line. Enter deliberately does not split
 * a line and Backspace deliberately does not merge one (see this file's
 * header), so there is no structural edit more complicated than "the
 * buffer grew by one empty row" to undo.
 *
 * `group` is what makes a paste undo as one action rather than as
 * eighty. Every user-visible action bumps the counter; undo pops records
 * until the group changes. Typing gets a group per keystroke, which is
 * the behavior of every editor that does not try to be clever about
 * coalescing runs - and being clever about it is how undo starts
 * surprising people. */
#define UNDO_MAX 512

typedef enum {
    EDIT_INSERT = 0, /* a character was inserted at (row, col) - undo deletes it */
    EDIT_DELETE = 1, /* `ch` was deleted from (row, col) - undo puts it back */
    /* M60: EDIT_NEWLINE is gone with the primitive that produced it.
     * Enter appended an empty line at the *end* of the buffer regardless
     * of the cursor - it split nothing, which is why undoing it was just
     * "drop the last line". Enter splits now, so every newline this
     * editor makes is an EDIT_SPLIT. The ring lives only in memory, so
     * there is no old record of the retired kind anywhere to honour. */
    /* M60: the two structural edits M56 deferred, and said why: they are
     * the only edits that change how many lines there are, and undo has
     * to invert them. That was the right call when undo did not exist; it
     * exists now and works, which turns the argument around - these are
     * the two edits that make it an editor.
     *
     * Each inverse is performed through the same primitive as the edit
     * (split_line / join_line), under M56's own rule, so undo cannot
     * drift from what it is undoing. */
    EDIT_SPLIT = 3,  /* line `row` was split at `col` - undo joins them back */
    EDIT_JOIN = 4,   /* line `row + 1` was appended onto `row` at `col` - undo splits at (row, col) */
} edit_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t group;
    int16_t row, col;
    char ch;
} edit_t;

static edit_t undo_ring[UNDO_MAX];
static int undo_count;   /* records behind `head` that can still be undone */
static int undo_head;    /* where the next record goes */
/* M60: records *ahead* of head that have been undone and can be redone.
 * The ring already held what redo needs; what was missing was a cursor
 * into it and the rule that a fresh edit discards the forward half. That
 * rule is one line in undo_record, and it is what keeps redo from
 * replaying an edit against a buffer that has since moved on. */
static int redo_count;
static uint8_t undo_group;
/* Set while undo or redo is replaying, so the edits they perform are not
 * themselves recorded - which would make undo a loop rather than a
 * history. */
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
    /* M60: a fresh edit discards the forward half. Anything that was
     * undone described a buffer this edit has just diverged from, and
     * replaying it would apply a change at coordinates that no longer
     * mean what they did. */
    redo_count = 0;
    /* Full: the oldest record is simply overwritten. Bounded history is
     * the trade this design makes on purpose - an editor that can undo
     * forever is one that can run out of memory while you type. */
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
        return; /* line's own fixed cap - silently refuses rather than corrupting adjacent memory */
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

/* M60: split line `row` at `col` - everything from `col` onward becomes a
 * new line below it. The primitive, with no cursor movement and no undo
 * record of its own, because undo performs it too (as the inverse of a
 * join) and a primitive that recorded itself would make undo a loop. */
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

/* The inverse: append line `row + 1` onto the end of `row`. Returns 0 if
 * the joined line would not fit, which is what keeps Backspace from
 * silently losing the tail of a long line. */
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

/* M60: Enter, at last. It appended an empty line at the end of the buffer
 * regardless of where the cursor was - which is what "you cannot split a
 * line" looks like from the inside, and the first thing anyone typing a
 * paragraph discovers. */
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
        /* M60: and the other half - Backspace at column 0 joins this line
         * onto the one above, landing the cursor exactly where the join
         * happened, which is where the text you just merged now starts. */
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
    memcpy(filename, PATH_HOME_DIR "untitled", sizeof(PATH_HOME_DIR "untitled"));
    dirty = 0;
    undo_reset(); /* M56: the history described a buffer that no longer exists */
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
        /* M60: Save As under a new name is exactly the escape hatch a
         * truncated buffer needs, so the flag clears here - what gets
         * written is the whole of *this* file, whatever it was a prefix
         * of before. */
        truncated = 0;
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
    } else if (action == PENDING_DROP) {
        load_file(pending_drop);
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

/* M56: replays the most recent group of edits backwards. Each record's
 * inverse is one of the same three mutations, performed through the same
 * primitives - so undo cannot drift away from what it is undoing, which
 * is the failure mode of every hand-written inverse. */
/* ---- M60: find and find-next ----------------------------------------
 *
 * Searches forward from just after the cursor and wraps to the top, which
 * is what makes repeated find-next walk every occurrence and stop where
 * it started rather than at the end of the file. Case-sensitive and plain
 * substring: this editor has no notion of a word and inventing one would
 * be guessing at what somebody meant. */
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
    /* From one past the cursor, so find-next moves off the match it is
     * standing on rather than finding it again forever. */
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

/* One record, undone. Every inverse is performed through the same
 * primitive as the edit it reverses, which is what keeps undo from
 * drifting away from what it is undoing - M56's rule, and the reason the
 * two structural kinds M60 added needed no new machinery, only their own
 * two lines here. */
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

/* And the same record, re-done - the edit itself rather than its
 * inverse, through the same primitives again. */
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
            redo_count++; /* the record stays in the ring, ahead of head, for redo */
        }
        apply_inverse(&e);
    }
    undo_replaying = 0;
    clamp_cursor();
    dirty = 1;
}

/* M60: redo. The ring already held what this needs - the records are
 * still there, ahead of head; what was missing was a cursor into it.
 * Symmetric with undo down to the group rule, so one Ctrl+Y puts back
 * exactly what one Ctrl+Z took away. */
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

/* M56: the other half of the clipboard this editor has had since M32.
 * Copy existed; paste did not, which made "the clipboard" a one-way
 * street between this window and gui_terminal. One undo group for the
 * whole thing - eighty separate undos for one Ctrl+V would be a worse
 * feature than none. */
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
            /* M60: a pasted newline splits too, so pasting two lines into
             * the middle of a third does what it looks like it should. */
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
                continue; /* an empty span on this row (e.g. selection starts at EOL) - nothing to shade */
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

    /* Dropdown drawn last so it overlays whatever content is underneath -
     * this app owns its whole window buffer, there's no compositor-level
     * popup surface to draw it into instead (see M35's milestones.md
     * note on that scope trim, and M42's on why the menu came back here
     * rather than staying in a shared bar). */
    if (menu_open) {
        gfx_draw_menu(&win->gfx, FILE_MENU_X, CONTENT_Y0, FILE_MENU_ITEM_W, FILE_MENU_ITEM_H,
                      FILE_MENU_ITEMS, FILE_MENU_COUNT, -1,
                      MENU_BG, MENU_HOVER_BG, MENU_BORDER, MENU_TEXT);
    }

    if (prompt_kind != PROMPT_NONE) {
        int32_t x = (WIN_W - PROMPT_W) / 2;
        int32_t y = (WIN_H - PROMPT_H) / 2;
        /* M44: same corner radius as every other rounded surface on this
         * desktop (gfx.h's GFX_CORNER_R) - a dialog was the last flat-
         * cornered box left once the taskbar, the launcher and the icons
         * were done. */
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
        } else { /* PROMPT_CONFIRM_DISCARD */
            gfx_draw_text(&win->gfx, x + GFX_PAD, y + 8, "Discard unsaved changes?", PROMPT_TEXT);
            gfx_draw_text(&win->gfx, x + GFX_PAD, y + 32, "Y = discard      N / click = cancel", PROMPT_TEXT);
        }
    }
}

int main(int argc, char **argv) {
    /* M60: argv[0] is this program's own path; argv[1] is the first thing
     * the caller had to say. `arg` keeps the name the body already uses,
     * and is the empty string when there was nothing - which is exactly
     * what the single-string mechanism this replaced handed over. */
    const char *arg = argc > 1 ? argv[1] : "";
    int i = 0;
    for (; arg && arg[i] && i < (int)sizeof(filename) - 1; i++) {
        filename[i] = arg[i];
    }
    filename[i] = '\0';
    if (filename[0] == '\0') {
        /* M53: a new document belongs in /home, which is where the file
         * manager opens and where a person would look for it. */
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

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                changed = 1; /* M55: a replacement compositor handed this client a blank buffer - see WM_EVENT_EXPOSE */
                continue;
            }
            /* M36: an open prompt owns every event until answered - same
             * "in-progress interaction takes over the input stream"
             * shape as compositor.c's own drag state machine (M31), just
             * scoped to this one window instead of the whole desktop. */
            if (prompt_kind == PROMPT_SAVE_AS || prompt_kind == PROMPT_FIND) {
                if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                    prompt_kind = PROMPT_NONE; /* click anywhere cancels */
                } else if (ev.type == WM_EVENT_KEY) {
                    if (ev.ch == '\n' || ev.ch == '\r') {
                        if (prompt_kind == PROMPT_FIND) {
                            prompt_buf[prompt_len] = '\0';
                            memcpy(find_needle, prompt_buf, (size_t)prompt_len + 1);
                            prompt_kind = PROMPT_NONE;
                            /* From the cursor itself, so the first Enter
                             * finds the nearest match rather than
                             * skipping one. */
                            if (!find_from(cur_row, cur_col, find_needle)) {
                                find_next(); /* not found from here - say so, having wrapped */
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
                    confirm_discard(0); /* click cancels, same as N */
                } else if (ev.type == WM_EVENT_KEY && (ev.ch == 'y' || ev.ch == 'Y')) {
                    confirm_discard(1);
                } else if (ev.type == WM_EVENT_KEY && (ev.ch == 'n' || ev.ch == 'N')) {
                    confirm_discard(0);
                }
                changed = 1;
                continue;
            }

            if (ev.type == WM_EVENT_DROP) {
                /* M49: a file dragged onto this window opens it here -
                 * the one case drag and drop was scoped to. Routed
                 * through request_action's existing confirm-discard path
                 * rather than loading straight over the top, because
                 * losing unsaved work to a mis-drop is exactly the
                 * accident M36's prompt exists to prevent. */
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
            } else if (ev.type == WM_EVENT_MOUSE_WHEEL) {
                /* M49: one text row per detent. Scrolls the viewport
                 * without moving the caret - clamp_scroll (which the
                 * caret's own movement goes through) would immediately
                 * drag the view back if it did, so this deliberately
                 * writes scroll_top directly and lets the next arrow key
                 * or click re-anchor it. */
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
                /* M35: the "File" label toggles the dropdown; any other
                 * click while it's open either picks an item or - same
                 * as a real menu - just dismisses it, consumed either
                 * way so it never also reaches handle_char/save_file. */
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
                    /* M56: one group per keystroke - see UNDO_MAX's note
                     * on why this deliberately does not coalesce runs. */
                    undo_begin_group();
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
