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
 *
 * ---- M112: the half that was missing --------------------------------
 *
 * Every milestone up to here taught this window to *act on files that
 * already existed*: browse them (M53), rename, copy and delete them
 * (M56), sort them and see their sizes (M59), reopen them (M74). What it
 * could never do is make one. A file arrived on this disk from a text
 * editor's Save box, from a shell redirect, or from the boot self-tests,
 * and a folder arrived from `mkdir` in a terminal - so the Files app was
 * a viewer with an edit menu, and the first thing anybody tries in it
 * was the one thing it refused.
 *
 * So: New File and New Folder, a right-click menu that makes all six
 * operations visible instead of leaving three of them behind single
 * letters in a hint line, an info box with the *exact* byte count
 * (M59's size column rounds, and two files that both read "8.4K" are not
 * the same file), a status strip that describes what is selected or
 * summarises the folder and how much of the disk is left, and a delete
 * that will finally take a folder with things in it.
 *
 * That last one is the only place this milestone argues with an earlier
 * decision, so the argument is written down. SYS_rmdir takes empty
 * directories only and says why: "recursive delete is one keystroke away
 * from losing everything under a path, and this OS has no trash." The
 * kernel is still right and is unchanged. What changed is that a person
 * can now *create* folders here, and a window that will make a folder
 * but not remove one it filled is half a feature - so the recursion
 * lives in user space (user_space/lib/fsutil.c), behind a confirm that
 * counted the tree first and names the number. "Delete folder and 41
 * items?" is a question somebody can answer; "Delete this folder?" about
 * forty-one files is not.
 *
 * Everything here that can be wrong *quietly* moved into
 * user_space/lib/fsutil.c so the host tier can grade it - the size
 * formatting, the rule about what a new name may contain, and both tree
 * walks. See tests/test_fsutil.c.
 */
#include "paths.h" /* system_api/include/paths.h - M53: /bin is where programs live now */
/* M57: the file list is a dense list, which is what the 12-row face
 * exists for - the window shows six more names for the same height.
 * Everything else here (path bar, status line, prompts) is chrome and
 * stays on the 16-row UI face. */
#define LIST_FONT   ui_font_small
#define LIST_FONT_H UI_FONT_SMALL_HEIGHT
#include "str.h"
#include "fsutil.h" /* M112 - the formatting, the name rule and the two tree walks */
#include "os_fs.h"  /* system_api/include/os_fs.h - M88's os_statvfs_t, for "how much is left" */
#include "spawn_error.h" /* system_api/include/spawn_error.h - M48 */
#include "recent.h" /* M74 - the recently-opened list */
#include "syscall_wrappers.h"
#include "wmclient.h"

/* M59: 280 -> 340, for the size and date columns. Those columns are the
 * reason the CMOS clock belongs in the same milestone as descriptors: a
 * date column is the most visible thing a machine that knows the date can
 * do, and it is worthless if every row says the same zero. */
#define WIN_W 340
#define WIN_H 360
#define ROW_H (LIST_FONT_H + 4)
#define HEADER_H 24
/* M59: the clickable column headings, between the path bar and the list.
 * Sorting has to be reachable from somewhere, and the heading you sort by
 * is where every file manager puts it. */
#define COLS_H 16
#define LIST_Y (HEADER_H + COLS_H)
#define SCROLLBAR_W 8 /* M37: reserved strip along the right edge - see redraw()'s gfx_draw_scrollbar call */
#define LIST_W (WIN_W - SCROLLBAR_W)

/* Column geometry. The name column takes whatever is left; size is
 * right-aligned against the date column's left edge, because digits read
 * as a magnitude when their ones place lines up and as noise when it
 * does not. */
#define COL_NAME_X   6
#define COL_DATE_X   (LIST_W - 70)   /* "MM-DD HH:MM" in the 12-row face is 62px */
#define COL_SIZE_R   (COL_DATE_X - 10)
#define COL_NAME_W   (COL_SIZE_R - 46 - COL_NAME_X)
/* M48: a one-row status strip along the bottom. An error about the file
 * you just double-clicked belongs in the window you double-clicked it in
 * - a toast in the far corner of the screen is the wrong place for
 * something about what you are looking directly at. */
#define STATUS_H (UI_FONT_UI_HEIGHT + 4)
#define LIST_H   (WIN_H - LIST_Y - STATUS_H)
#define ROWS_VISIBLE (LIST_H / ROW_H)

#define BG_COLOR       0x001C1C24u
#define HEADER_COLOR   0x00303850u
#define TEXT_COLOR     0x00D8D8D8u
#define SELECT_COLOR   0x004C6699u
#define LABEL_COLOR    0x0090A0C0u
#define DIR_MARK_COLOR 0x0078A8E0u /* M57: the directory arrow, tinted so the mark reads as a kind rather than as part of the name */
#define SCROLLBAR_TRACK 0x00141820u
#define SCROLLBAR_THUMB 0x00506080u
#define COLS_BG        0x00262C3Au /* M59: the column-heading strip - darker than the path bar, lighter than the list, so it reads as a divider rather than as a second title */
#define STATUS_BG      0x00141820u
#define STATUS_ERR_FG  0x00E08878u

/* M81: 48 -> 512 and 32 -> 256.
 *
 * MAX_NAME_LEN was a correctness bug the moment leanfs's names went to
 * 255, not a cosmetic one: a name longer than 31 bytes was truncated on
 * the way in, and entry_path() builds the path it opens out of the
 * truncated copy - so double-clicking a long name would have opened
 * nothing, or something else. It is now leanfs's real cap plus a NUL.
 *
 * MAX_FILES is a window's capacity rather than a filesystem's, and it
 * stays that way - but 48 was chosen when the whole disk held 192 files
 * and a directory now holds thousands, so it was the smaller lie of the
 * two. 512 entries of 256 bytes is 128 KiB of this program's own .bss,
 * which is the price of not silently hiding files.
 *
 * Kept as constants here rather than including the kernel header, which
 * user_space builds cannot see. */
#define MAX_FILES    512
#define MAX_NAME_LEN 256
#define LIST_BUF_SIZE 2048
#define DOUBLE_CLICK_MS 500

static char names[MAX_FILES][MAX_NAME_LEN];
/* M59: what SYS_stat says about each row. Read once per refresh rather
 * than per redraw - a stat is a path resolution and a redraw happens
 * several times a second. */
static uint32_t sizes[MAX_FILES];
static uint32_t mtimes[MAX_FILES];
/* M53: whether names[i] is a directory. SYS_listdir marks one with a
 * trailing '/', which this strips on the way in - so the marker is a
 * flag here rather than part of the name, and nothing downstream has to
 * remember to trim it before building a path. */
static uint8_t is_dir[MAX_FILES];
static int file_count;
static int selected = -1;
static int scroll_top;

/* M59: how the list is ordered, and which way. Directories always sort
 * ahead of files whatever the key is - that is not a rule about the key,
 * it is what makes a list navigable, and every file manager does it. */
typedef enum { SORT_NAME = 0, SORT_SIZE, SORT_DATE, SORT_COUNT } fm_sort_t;
static fm_sort_t sort_key = SORT_NAME;
static int sort_desc; /* 0 = ascending */
static const char *status_text = "";

/* M53: which directory this window is showing. There is no working
 * directory in this OS, so this is the app's own state and every path it
 * hands a syscall is built absolute from it. */
static char cwd[PATH_MAX_LEN] = PATH_HOME;

/* ---- M74: recently opened ---------------------------------------------
 *
 * A second thing the list can be showing: not a directory, but the files
 * this machine was last asked to open (user_space/lib/recent.h). It is a
 * mode rather than a real directory because it is not one - the entries
 * come from several places at once and "up" from it means "back to where
 * I was", not "the parent of a path".
 *
 * Reached from a button in the header rather than from a row in the list,
 * and that is deliberate: a synthetic row would shift every real one down
 * by an index, which is a thing three interactive tests measure and, more
 * importantly, a thing a person's muscle memory measures. The header had
 * no click handler at all before this, so the button costs nothing that
 * was already there. */
#define RECENT_BTN_W 62
#define RECENT_BTN_X (LIST_W - RECENT_BTN_W - 6)
static int recent_mode;
static char recent_full[RECENT_MAX][PATH_MAX_LEN];
/* Where the list was before Recent was opened, so leaving it goes back
 * rather than to a fixed place. */
static char recent_return[PATH_MAX_LEN];

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
/* M81: this is a *window* limit and no longer a filesystem one, which is
 * a distinction worth keeping rather than quietly raising to 255.
 *
 * The original comment said "a name this window cannot express is one it
 * should not offer to create", and that reasoning is still exactly right
 * - the prompt field does not scroll, so a name longer than fits in it
 * could be typed and never read back. What changed is that 27 is no
 * longer also leanfs's cap: this dialog now refuses names the filesystem
 * would accept, and says so here rather than leaving a reader to assume
 * the two numbers are still the same one. Raising it means making the
 * field scroll, which is a UI change and not this milestone's. */
#define PROMPT_MAX_LEN 48
#define PROMPT_BG      0x00243040u
#define PROMPT_BORDER  0x004C6699u
#define PROMPT_TEXT    0x00E8E8E8u
#define PROMPT_INPUT_BG 0x00141820u

/* M59: how much of a file this window moves at a time. It used to be how
 * large a file it would copy *at all* - 16 KiB, because a copy was a
 * whole-file read into static storage and a file bigger than the buffer
 * was refused out loud. Descriptors make that limit meaningless: the
 * copy is a loop now, and the only thing this number decides is how many
 * times round it goes. 4 KiB is eight sectors, which is a whole ATA
 * transfer's worth without being a page of stack. */
#define FM_COPY_CHUNK 4096

typedef enum {
    FM_PROMPT_NONE = 0,
    FM_PROMPT_RENAME,
    FM_PROMPT_COPY,
    FM_PROMPT_CONFIRM_DELETE,
    /* M112. The first two take typing exactly as rename and copy do; the
     * third takes no input at all and is dismissed by anything, which is
     * a third flavour of the same box rather than a fourth kind of
     * dialog. */
    FM_PROMPT_NEW_FILE,
    FM_PROMPT_NEW_FOLDER,
    FM_PROMPT_INFO,
} fm_prompt_t;

static fm_prompt_t prompt_kind;
static char prompt_buf[PROMPT_MAX_LEN + 1];
static int prompt_len;

/* M112: the info box is taller and wider than the four-line prompt,
 * because it is five labelled facts rather than one question. */
#define INFO_W 300
#define INFO_H 132

/* What the info box and the delete confirm are about, read once when the
 * box opens rather than on every redraw: counting a tree is a directory
 * walk, and this window repaints several times a second. */
static os_stat_t info_stat;
static fsutil_tree_t info_tree;
static int info_is_dir;
static int info_tree_valid;
static char info_path[PATH_MAX_LEN];

/* ---- M112: the context menu ------------------------------------------
 *
 * Six operations, three of which used to be reachable only by knowing
 * that R, C and Delete did something - written in a hint line that this
 * milestone replaces with information about what is actually selected.
 * A right-click menu is where every file manager puts these, it is the
 * gesture people try first, and this project already has the widget:
 * gfx_draw_menu/gfx_menu_hit_test, which desktop_icons.c's own
 * right-click menu and text_editor.c's File menu both use.
 *
 * The keys stay, and the labels name them - a menu that hides the
 * shortcut is a menu that has to be opened forever. */
#define CTX_ITEM_W 124
#define CTX_ITEM_H (UI_FONT_UI_HEIGHT + 4)
#define CTX_MENU_BG     0x00243040u
#define CTX_MENU_HOVER  0x003A5A80u
#define CTX_MENU_BORDER 0x00506070u
#define CTX_MENU_TEXT   0x00FFFFFFu

typedef enum {
    CTX_NEW_FILE = 0,
    CTX_NEW_FOLDER,
    CTX_RENAME,
    CTX_COPY,
    CTX_DELETE,
    CTX_INFO,
    CTX_COUNT,
} fm_ctx_item_t;

/* "Bksp" and not "Del", because Delete is a key this machine's keyboard
 * driver does not decode at all - kernel/drivers/keyboard.c maps 0x0E to
 * '\b' and has no entry for the keypad's delete scancode. The status
 * line this menu replaces said "Del delete" for fifty-six milestones
 * about a key that never sent anything. A label naming a key that does
 * nothing is worse than no label. */
static const char *const CTX_ITEMS[CTX_COUNT] = {
    "New File (N)",
    "New Folder (F)",
    "Rename (R)",
    "Copy (C)",
    "Delete (Bksp)",
    "Get Info (I)",
};

static int ctx_open;
static int32_t ctx_x, ctx_y;

/* M112: the status strip says what is selected, or what the folder holds
 * and how much of the disk is left. Built into a buffer rather than
 * pointed at a literal, so status_text stays one type - and status_error
 * decides the colour, which used to be "is there any message at all" and
 * therefore drew "Copied." in the same alarmed red as "Could not delete
 * that." */
static char status_buf[160];
static int status_error;

/* How long a message about something that just happened stays before the
 * strip goes back to describing what is there. Five seconds is long
 * enough to read a failure and short enough that "Copied." is not still
 * on screen describing a window you have since navigated away from -
 * which is the state the old code left it in permanently. */
#define STATUS_MS 5000
static long status_expires_ms;

/* M112: how often "how much is left" is actually asked of the kernel.
 *
 * SYS_statvfs is not a cheap read of a counter: leanfs_free_blocks walks
 * the whole allocation bitmap and leanfs_free_inodes walks the whole
 * inode table, both under the filesystem lock. That is fine once, and it
 * is not fine once a second per open Files window for as long as the
 * desktop is up - M50's soak leaves exactly that running for three
 * minutes.
 *
 * Five seconds, invalidated by every write this window makes, so the
 * number is exact right after you create or delete something and at
 * worst five seconds stale when somebody else does. The listing still
 * re-reads every second; free space changes more slowly than a directory
 * does. */
#define STATVFS_MS 5000
static long statvfs_next_ms;
static os_statvfs_t statvfs_cached;
static int statvfs_ok;

static void statvfs_invalidate(void) {
    statvfs_next_ms = 0;
}

/* Appends `t` to `out` at offset n, stopping at cap-1 and always leaving
 * a NUL. Returns the new offset. Every status line below is built out of
 * these, which is what keeps a name of any length from running off the
 * end of the buffer - the truncation is at the buffer, not at a call
 * site that has to remember. */
static int sb_puts(char *out, int n, int cap, const char *t) {
    while (*t && n < cap - 1) {
        out[n++] = *t++;
    }
    out[n] = '\0';
    return n;
}

static int sb_num(char *out, int n, int cap, uint32_t v) {
    char tmp[FSUTIL_EXACT_MAX];
    fsutil_format_exact(v, tmp);
    return sb_puts(out, n, cap, tmp);
}

/* Something happened. Shown until STATUS_MS is up, then the strip goes
 * back to status_describe(). is_error picks the colour, which is a
 * distinction the strip did not previously make. */
static void status_say(const char *msg, int is_error) {
    status_text = msg;
    status_error = is_error;
    status_expires_ms = sys_uptime_ms() + STATUS_MS;
}

/* Nothing has happened, so describe what is there: the selected row in
 * full, or the folder and what is left of the disk.
 *
 * The exact byte count is the point of the first branch. M59's size
 * column rounds to three significant figures because that is what a
 * column is for, which means it cannot answer "is this the same file as
 * that one" - and that is the question somebody is asking when they look
 * at a size at all.
 *
 * The second branch reports both of this filesystem's ceilings, not just
 * free space. os_fs.h says why in as many words: LEANFS_MAX_INODES is
 * fixed at format time, so a tree of small files exhausts inodes with
 * most of the disk still free, and a status line watching only megabytes
 * would go on saying there was room right up until a New File failed. */
static void status_describe(void) {
    int n = 0;
    status_buf[0] = '\0';
    status_error = 0;
    status_text = status_buf;

    int is_up_row = (selected >= 0 && selected < file_count &&
                     names[selected][0] == '.' && names[selected][1] == '.' &&
                     names[selected][2] == '\0');
    if (selected >= 0 && selected < file_count && !is_up_row) {
        char cell[FSUTIL_EXACT_MAX];
        n = sb_puts(status_buf, n, (int)sizeof(status_buf), names[selected]);
        n = sb_puts(status_buf, n, (int)sizeof(status_buf), "   ");
        if (is_dir[selected]) {
            n = sb_puts(status_buf, n, (int)sizeof(status_buf), "folder");
        } else {
            n = sb_num(status_buf, n, (int)sizeof(status_buf), sizes[selected]);
            n = sb_puts(status_buf, n, (int)sizeof(status_buf), " bytes");
        }
        n = sb_puts(status_buf, n, (int)sizeof(status_buf), "   ");
        fsutil_format_date(mtimes[selected], cell);
        sb_puts(status_buf, n, (int)sizeof(status_buf), cell);
        return;
    }

    /* No selection: the folder itself. ".." is this window's own row and
     * is not one of the things the folder holds, so it is not counted. */
    int items = file_count;
    uint32_t here = 0;
    for (int i = 0; i < file_count; i++) {
        if (names[i][0] == '.' && names[i][1] == '.' && names[i][2] == '\0') {
            items--;
            continue;
        }
        if (!is_dir[i]) {
            here += sizes[i];
        }
    }
    n = sb_num(status_buf, n, (int)sizeof(status_buf), (uint32_t)items);
    n = sb_puts(status_buf, n, (int)sizeof(status_buf), items == 1 ? " item" : " items");
    if (recent_mode) {
        return; /* a recents list spans directories; "how full is it" is not a question about one */
    }
    {
        char cell[FSUTIL_SIZE_MAX];
        fsutil_format_size(here, cell);
        n = sb_puts(status_buf, n, (int)sizeof(status_buf), ", ");
        n = sb_puts(status_buf, n, (int)sizeof(status_buf), cell);
    }
    long now = sys_uptime_ms();
    if (statvfs_next_ms == 0 || now >= statvfs_next_ms) {
        statvfs_ok = (sys_statvfs(cwd, &statvfs_cached) == 0);
        statvfs_next_ms = now + STATVFS_MS;
    }
    if (statvfs_ok) {
        const os_statvfs_t vfs = statvfs_cached;
        /* Clamped at what a uint32_t size can express, which is also what
         * fsutil_format_size takes. A 2 GiB image cannot reach it; a
         * larger one would, and a floor is a better answer there than a
         * number that has wrapped. */
        uint64_t freeb = (uint64_t)vfs.free_blocks * (uint64_t)vfs.block_size;
        if (freeb > 0xFFFFFFFFu) {
            freeb = 0xFFFFFFFFu;
        }
        char cell[FSUTIL_SIZE_MAX];
        fsutil_format_size((uint32_t)freeb, cell);
        n = sb_puts(status_buf, n, (int)sizeof(status_buf), "   ");
        n = sb_puts(status_buf, n, (int)sizeof(status_buf), cell);
        n = sb_puts(status_buf, n, (int)sizeof(status_buf), " free, ");
        n = sb_num(status_buf, n, (int)sizeof(status_buf), vfs.free_inodes);
        sb_puts(status_buf, n, (int)sizeof(status_buf), " files left");
    }
}

/* Called wherever the thing being described changed. A message that is
 * still inside its STATUS_MS wins - somebody who just pressed Delete
 * wants to read what happened, not a fresh description of the folder it
 * happened in. */
static void status_refresh(void) {
    if (status_expires_ms && sys_uptime_ms() < status_expires_ms) {
        return;
    }
    status_expires_ms = 0;
    status_describe();
}

/* M49: drag state. A press on a row arms it; the drag only actually
 * begins once the pointer has moved DRAG_THRESHOLD away, so a click and a
 * drag stay distinguishable - without that every selection click would
 * announce a drag the moment the pointer twitched. */
#define DRAG_THRESHOLD 8
static int drag_armed_row = -1;
static int32_t drag_press_x, drag_press_y;
static int dragging;

/* M112: fsutil_format_size and fsutil_format_date used to be static
 * functions here. They moved to user_space/lib/fsutil.c unchanged in
 * behaviour, for the reason its header gives - a size column that says
 * "8.4M" when the file is 8.4K is wrong in a way no boot can see, and
 * the host tier can compare it against a written-down answer in
 * microseconds. Everything below still calls them the same number of
 * times from the same places. */

/* M59: an insertion sort over the parallel arrays. Insertion rather than
 * anything cleverer for the honest reason: MAX_FILES is small, the list is
 * re-sorted only when it is re-read, and forty-eight elements is the size
 * at which a simpler algorithm is also the faster one.
 *
 * Row 0 is ".." when there is one and stays there - it is this window's
 * own invention rather than a directory entry, and "go up" belongs in the
 * same place every time rather than wherever it happens to sort. */
static int sort_before(int a, int b) {
    if (is_dir[a] != is_dir[b]) {
        return is_dir[a]; /* directories first, whatever the key */
    }
    long cmp;
    if (sort_key == SORT_SIZE) {
        cmp = (long)sizes[a] - (long)sizes[b];
    } else if (sort_key == SORT_DATE) {
        cmp = (long)mtimes[a] - (long)mtimes[b];
    } else {
        cmp = 0;
    }
    if (cmp == 0) {
        /* Name is both the default key and the tie-break for the other
         * two, so a list of same-sized files still has a stable, readable
         * order rather than whatever the directory happened to hold. */
        cmp = strcmp(names[a], names[b]);
    }
    return sort_desc ? cmp > 0 : cmp < 0;
}

static void swap_rows(int a, int b) {
    char nm[MAX_NAME_LEN];
    memcpy(nm, names[a], MAX_NAME_LEN);
    memcpy(names[a], names[b], MAX_NAME_LEN);
    memcpy(names[b], nm, MAX_NAME_LEN);
    uint8_t d = is_dir[a]; is_dir[a] = is_dir[b]; is_dir[b] = d;
    uint32_t sz = sizes[a]; sizes[a] = sizes[b]; sizes[b] = sz;
    uint32_t mt = mtimes[a]; mtimes[a] = mtimes[b]; mtimes[b] = mt;
}

static void sort_list(void) {
    int first = 0;
    if (file_count > 0 && names[0][0] == '.' && names[0][1] == '.' && names[0][2] == '\0') {
        first = 1;
    }
    for (int i = first + 1; i < file_count; i++) {
        for (int j = i; j > first && sort_before(j, j - 1); j--) {
            swap_rows(j, j - 1);
        }
    }
}

static void rebuild_list(void) {
    static char buf[LIST_BUF_SIZE];
    file_count = 0;

    /* M74: the recents view. Row 0 is ".." exactly as it is in a real
     * directory, so leaving is in the same place with the same gesture -
     * and deliberately NOT sorted, because the order *is* the
     * information: newest first is the whole point of a recents list, and
     * a sort by name would throw away the only thing it knows. */
    if (recent_mode) {
        names[0][0] = '.';
        names[0][1] = '.';
        names[0][2] = '\0';
        is_dir[0] = 1;
        sizes[0] = 0;
        mtimes[0] = 0;
        file_count = 1;
        int got = recent_load(recent_full, RECENT_MAX);
        for (int i = 0; i < got && file_count < MAX_FILES; i++) {
            const char *base = recent_full[i];
            for (const char *c = recent_full[i]; *c; c++) {
                if (*c == '/') {
                    base = c + 1;
                }
            }
            int col = 0;
            for (; base[col] && col < MAX_NAME_LEN - 1; col++) {
                names[file_count][col] = base[col];
            }
            names[file_count][col] = '\0';
            is_dir[file_count] = 0;
            os_stat_t st;
            sizes[file_count] = 0;
            mtimes[file_count] = 0;
            if (sys_stat(recent_full[i], &st) == 0) {
                sizes[file_count] = st.size;
                mtimes[file_count] = st.mtime;
            }
            file_count++;
        }
        if (selected >= file_count) {
            selected = file_count - 1;
        }
        return;
    }

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

    /* M59: one stat per row, here rather than in redraw - a stat is a
     * path resolution and this window redraws several times a second. */
    for (int i = 0; i < file_count; i++) {
        sizes[i] = 0;
        mtimes[i] = 0;
        char full[PATH_MAX_LEN];
        if (names[i][0] == '.' && names[i][1] == '.' && names[i][2] == '\0') {
            continue; /* ".." is this window's own invention, not an entry */
        }
        os_stat_t st;
        if (path_in_cwd(names[i], full) == 0 && sys_stat(full, &st) == 0) {
            sizes[i] = st.size;
            mtimes[i] = st.mtime;
        }
    }
    sort_list();

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

/* M112: re-read the directory, and keep the selection on the *file* it
 * was on rather than on the row number.
 *
 * That distinction was a real bug and not a refinement. This window
 * re-lists once a second, and `selected` was an index into a list that
 * had just been rebuilt - so a file appearing or vanishing anywhere
 * above the selection (a second Files window copying something, an
 * editor saving, the sort order changing) silently moved the selection
 * to a different file. The next Delete then asked its confirm about the
 * row you were looking at and removed the one the index had drifted
 * onto. Nothing on screen showed the moment it happened.
 *
 * The fix is to carry the name across the rebuild, which is also what
 * makes "create a folder and it is selected" and "sort by size and keep
 * looking at the same file" work. A name that is no longer there leaves
 * nothing selected, which is the truthful answer. */
static void refresh_list(void) {
    char keep[MAX_NAME_LEN];
    int had = (selected >= 0 && selected < file_count);
    if (had) {
        memcpy(keep, names[selected], MAX_NAME_LEN);
    }
    rebuild_list();
    if (had) {
        selected = -1;
        for (int i = 0; i < file_count; i++) {
            if (strcmp(names[i], keep) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected >= file_count) {
        selected = file_count - 1;
    }
    clamp_scroll();
    status_refresh();
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

/* M112: leaving where you are, whether that is a directory or the
 * recents view. Both used to be reachable only by selecting row 0 and
 * pressing Enter; this is what the Left arrow does, and what the ".."
 * row goes through so the two cannot drift apart. */
static void leave_directory(void) {
    if (recent_mode) {
        recent_mode = 0;
        for (int i = 0; i < PATH_MAX_LEN; i++) {
            cwd[i] = recent_return[i];
        }
    } else if (cwd[0] == '/' && cwd[1] == '\0') {
        return; /* the root has nowhere to go */
    } else {
        go_up();
    }
    selected = -1;
    scroll_top = 0;
    status_expires_ms = 0;
    statvfs_invalidate(); /* a different path can be a different mount - SYS_statvfs refuses /proc */
    refresh_list();
}


static void open_selected(void) {
    if (selected < 0 || selected >= file_count) {
        return;
    }
    /* M74: in the recents view every row but ".." is a file, and its path
     * is the one recent_load handed back rather than anything built from
     * a directory this window is standing in. */
    if (recent_mode) {
        if (is_dir[selected]) {
            leave_directory(); /* M112: the one ".." in the recents view, through the one path that leaves */
            return;
        }
        long rc = sys_spawn(PATH_BIN_DIR "text_editor", recent_full[selected - 1]);
        if (rc < 0) {
            status_say(spawn_error_message(rc), 1);
        }
        if (rc >= 0) {
            recent_add(recent_full[selected - 1]);
        }
        return;
    }
    if (is_dir[selected]) {
        /* M53: entering a directory, not launching anything. */
        if (names[selected][0] == '.' && names[selected][1] == '.') {
            leave_directory();
            return;
        }
        char next[PATH_MAX_LEN];
        if (path_in_cwd(names[selected], next) != 0) {
            status_say("Path too long.", 1);
            return;
        }
        for (int i = 0; i < PATH_MAX_LEN; i++) {
            cwd[i] = next[i];
        }
        selected = -1;
        scroll_top = 0;
        status_expires_ms = 0;
        statvfs_invalidate();
        refresh_list();
        return;
    }
    char full[PATH_MAX_LEN];
    if (path_in_cwd(names[selected], full) != 0) {
        status_say("Path too long.", 1);
        return;
    }
    /* M48: this used to throw the result away, so a file that couldn't be
     * opened looked exactly like a double-click that didn't register. */
    long rc = sys_spawn(PATH_BIN_DIR "text_editor", full);
    if (rc < 0) {
        status_say(spawn_error_message(rc), 1);
    }
    if (rc >= 0) {
        /* M74: recorded by whichever program did the opening, not by the
         * editor alone - the whole value of a recents list is that every
         * route into a file feeds it. The editor records it too, and
         * recent_add moves rather than duplicates, so the two agreeing is
         * free. */
        recent_add(full);
    }
}

/* The selected row's full path, or -1 if there is no ordinary file
 * selected. ".." and directories are excluded from all three operations:
 * this filesystem has no rmdir and a directory rename would work but a
 * directory *copy* would not, and offering two of three would be worse
 * than offering none. */
static int selected_file_path(char *out) {
    if (selected < 0 || selected >= file_count) {
        return -1;
    }
    /* M59: a directory is a legal target for two of the three operations
     * now. Rename always was (it moves a record, not data) and delete
     * became one when SYS_rmdir arrived; copy still is not, because a
     * recursive copy is a different operation with its own failure modes
     * and nothing has asked for one. ".." is this window's own invention
     * and is never a target for anything. */
    if (names[selected][0] == '.' && names[selected][1] == '.' && names[selected][2] == '\0') {
        return -1;
    }
    /* M74: the recents view is a list of files from all over the
     * filesystem, so nothing here is "in" the directory this window would
     * otherwise be showing - and rename, copy and delete all build their
     * target from that directory. Refused rather than made to work: a
     * recents list is for reopening things, and a delete that appeared to
     * act on the row you were looking at while actually naming a
     * different file would be the worst possible outcome. The row can
     * still be opened, which is what it is for. */
    if (recent_mode) {
        return -1;
    }
    return path_in_cwd(names[selected], out);
}

/* M112: the two boxes that need no selection. New File and New Folder
 * act on the directory this window is showing, so a window listing the
 * recents view - which is not a directory - has nowhere to put one.
 * Refused there for exactly the reason rename, copy and delete are. */
static void prompt_open_new(fm_prompt_t kind) {
    if (recent_mode) {
        status_say("Recent is a list, not a folder.", 1);
        return;
    }
    prompt_kind = kind;
    prompt_len = 0;
    prompt_buf[0] = '\0';
}

/* M112: what SYS_stat and a tree walk say about the selected row, read
 * once here rather than on every redraw - counting a folder is a
 * directory walk and this window repaints several times a second. */
static void prompt_open_info(void) {
    if (selected_file_path(info_path) != 0) {
        status_say("Select a file first.", 1);
        return;
    }
    info_is_dir = selected >= 0 && is_dir[selected];
    info_tree_valid = 0;
    if (sys_stat(info_path, &info_stat) != 0) {
        status_say("Could not read that.", 1);
        return;
    }
    if (info_is_dir) {
        info_tree_valid = (fsutil_count_tree(info_path, &info_tree) == 0);
    }
    prompt_kind = FM_PROMPT_INFO;
}

static void prompt_open(fm_prompt_t kind) {
    char scratch[PATH_MAX_LEN];
    if (selected_file_path(scratch) != 0) {
        status_say("Select a file first.", 1);
        return;
    }
    if (kind == FM_PROMPT_COPY && selected >= 0 && is_dir[selected]) {
        status_say("Folders cannot be copied.", 1);
        return;
    }
    /* M112: a delete confirm has to say what it is about to destroy, and
     * for a folder that means counting first. A confirm that asks the
     * same question about an empty folder and about one holding a
     * hundred files is a confirm that has stopped being one. */
    if (kind == FM_PROMPT_CONFIRM_DELETE && selected >= 0 && is_dir[selected]) {
        info_tree_valid = (fsutil_count_tree(scratch, &info_tree) == 0);
    } else {
        info_tree_valid = 0;
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

/* M112: creating an empty file.
 *
 * OPEN_EXCL rather than a stat followed by a create, and that is the
 * whole reason M87 added the flag: "check then create is a race and this
 * is not - the check and the create happen inside one critical section
 * of the filesystem lock". Without it, a New File box racing anything
 * else that writes (a text editor saving, a second Files window) could
 * truncate a file somebody was in the middle of. Truncating a file
 * because you asked for a new one is the worst thing this window could
 * do, and it would have been one flag away.
 *
 * Returns the message to show, and whether it is a failure - which is
 * how every operation in this window already answers. */
static const char *create_file(const char *name, int *failed) {
    *failed = 1;
    if (!fsutil_name_ok(name)) {
        return "That name cannot be used.";
    }
    char full[PATH_MAX_LEN];
    if (path_in_cwd(name, full) != 0) {
        return "Name too long.";
    }
    long fd = sys_open(full, OPEN_WRITE | OPEN_CREATE | OPEN_EXCL);
    if (fd < 0) {
        /* Deliberately not two messages. The interesting failures here
         * are "that name is taken" and "the disk or its inodes are
         * full", and this window shows the second in its status strip
         * all the time - so the one worth naming is the first, and it is
         * the one somebody can act on. */
        return "That name is already taken, or the disk is full.";
    }
    sys_close((int)fd);
    *failed = 0;
    return "Created.";
}

static const char *create_folder(const char *name, int *failed) {
    *failed = 1;
    if (!fsutil_name_ok(name)) {
        return "That name cannot be used.";
    }
    char full[PATH_MAX_LEN];
    if (path_in_cwd(name, full) != 0) {
        return "Name too long.";
    }
    /* SYS_mkdir is already an error rather than a no-op when the path is
     * taken, and its own comment says why: so that "I made this" and
     * "this was already here" cannot be confused by a caller that cares.
     * This one cares - it selects what it made. */
    if (sys_mkdir(full) != 0) {
        return "That name is already taken, or the disk is full.";
    }
    *failed = 0;
    return "Folder created.";
}

/* Puts the selection on `name` if the current listing has it. Called
 * after a create so the new row is selected and can be opened, renamed
 * or deleted without hunting for it - which is what every file manager
 * does and what makes "New Folder, type, Enter, Enter" work. */
static void select_named(const char *name) {
    for (int i = 0; i < file_count; i++) {
        if (strcmp(names[i], name) == 0) {
            selected = i;
            clamp_scroll();
            return;
        }
    }
}

/* M59: a copy is a loop over two descriptors now, not one whole-file
 * read. Copy is still not a filesystem operation - leanfs has no notion
 * of one - but it is no longer bounded by how much of a file this process
 * can hold at once, which is what it was apologising for since M56.
 *
 * Returns the status line to show, which is also how it reports failure:
 * this window has one status strip and every other operation here already
 * answers the same way. */
static const char *copy_file(const char *from, const char *to, int *failed) {
    *failed = 1;
    long src = sys_open(from, OPEN_READ);
    if (src < 0) {
        return "Could not read that file.";
    }
    /* M112: OPEN_EXCL, for the same reason create_file uses it - "Copy
     * to:" pre-fills the box with the source's own name, so the one
     * keystroke between opening it and pressing Enter used to be a
     * whole-file truncation of something that already existed. It
     * refuses now and says so. */
    long dst = sys_open(to, OPEN_WRITE | OPEN_CREATE | OPEN_EXCL);
    if (dst < 0) {
        sys_close((int)src);
        return "That name is already taken, or the disk is full.";
    }
    static char chunk[FM_COPY_CHUNK];
    const char *result = "Copied.";
    *failed = 0;
    for (;;) {
        long n = sys_read((int)src, chunk, sizeof(chunk));
        if (n < 0) {
            result = "Could not read that file.";
            *failed = 1;
            break;
        }
        if (n == 0) {
            break; /* end of file - the whole thing is across */
        }
        if (sys_write((int)dst, chunk, (size_t)n) != n) {
            /* A partial write is a full disk. The destination is left as
             * far as it got rather than removed: this window has no undo
             * and deleting a file on the user's behalf because a copy of
             * it failed is a worse outcome than leaving a short one they
             * can see and delete. */
            result = "Ran out of space part way through.";
            *failed = 1;
            break;
        }
    }
    sys_close((int)src);
    sys_close((int)dst);
    return result;
}

static void prompt_confirm(void) {
    /* M112: the two create boxes act on the directory rather than on a
     * row, so they run before the "what is selected" question is even
     * asked - a New File box with nothing selected is the ordinary case,
     * not an error. */
    if (prompt_kind == FM_PROMPT_NEW_FILE || prompt_kind == FM_PROMPT_NEW_FOLDER) {
        prompt_buf[prompt_len] = '\0';
        if (prompt_len > 0) {
            int failed = 1;
            const char *msg = (prompt_kind == FM_PROMPT_NEW_FILE)
                                  ? create_file(prompt_buf, &failed)
                                  : create_folder(prompt_buf, &failed);
            status_say(msg, failed);
            prompt_kind = FM_PROMPT_NONE;
            statvfs_invalidate();
            refresh_list();
            if (!failed) {
                select_named(prompt_buf);
            }
            return;
        }
        prompt_kind = FM_PROMPT_NONE;
        return;
    }

    char from[PATH_MAX_LEN];
    if (selected_file_path(from) != 0) {
        prompt_kind = FM_PROMPT_NONE;
        return;
    }
    if (prompt_kind == FM_PROMPT_CONFIRM_DELETE) {
        /* M59: a directory too, now that SYS_rmdir exists - a file
         * manager that can delete a file but not the folder it sits in is
         * visibly half-finished.
         *
         * M112: and one with things in it. SYS_rmdir still takes empty
         * directories only and the kernel is still right about why; what
         * makes the recursion acceptable here is the confirm that
         * precedes it, which counted the tree and named the number. The
         * empty case still goes through SYS_rmdir directly rather than
         * through the walk - not for speed, but because "remove this one
         * empty directory" and "remove everything under this path" are
         * different operations and the code should say which one it is
         * doing. */
        if (is_dir[selected]) {
            int empty = info_tree_valid && info_tree.entries == 0;
            int ok = empty ? (sys_rmdir(from) == 0) : (fsutil_remove_tree(from) == 0);
            if (ok) {
                status_say("Deleted.", 0);
            } else {
                /* A failed tree delete has usually removed part of the
                 * tree, which is why this says what it says rather than
                 * "nothing happened" - and why refresh_list runs below
                 * whatever the outcome was. */
                status_say("Could not delete all of that.", 1);
            }
        } else {
            if (sys_unlink(from) == 0) {
                status_say("Deleted.", 0);
            } else {
                status_say("Could not delete that.", 1);
            }
        }
    } else if (prompt_len > 0) {
        prompt_buf[prompt_len] = '\0';
        /* M112: the same name rule the create boxes use. A rename to
         * "../elsewhere" would have moved the file out of the folder
         * this window is showing, which is a move dressed as a rename -
         * and SYS_rename would have carried it out. */
        if (!fsutil_name_ok(prompt_buf)) {
            status_say("That name cannot be used.", 1);
            prompt_kind = FM_PROMPT_NONE;
            return;
        }
        char to[PATH_MAX_LEN];
        if (path_in_cwd(prompt_buf, to) != 0) {
            status_say("Name too long.", 1);
            prompt_kind = FM_PROMPT_NONE;
            return;
        }
        if (prompt_kind == FM_PROMPT_RENAME) {
            if (sys_rename(from, to) == 0) {
                status_say("Renamed.", 0);
            } else {
                status_say("Could not rename that.", 1);
            }
        } else {
            int failed = 0;
            const char *msg = copy_file(from, to, &failed);
            status_say(msg, failed);
        }
    }
    prompt_kind = FM_PROMPT_NONE;
    statvfs_invalidate(); /* M112: this window just wrote - the free-space number is stale */
    refresh_list();
}

static void redraw(wm_window_t *win) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);
    gfx_fill_rect(&win->gfx, 0, 0, LIST_W, HEADER_H, HEADER_COLOR);
    /* M53: the path bar. Right-aligned when it is too long for the
     * header, so the part you are actually in stays visible - the tail of
     * a path says where you are, the head only says how you got there. */
    {
        /* M57: what fits is now measured, not divided - with a
         * proportional advance, "how many characters" is a property of
         * *which* characters. Trimming the head is the same walk from
         * the other end: drop leading characters until the rest fits. */
        const char *shown = recent_mode ? "Recent" : cwd;
        int32_t avail = RECENT_BTN_X - 12; /* M74: leave the header's Recent button room */
        if (gfx_text_width(gfx_ui_font(), shown) > avail) {
            avail -= gfx_char_advance(gfx_ui_font(), UI_G_ELLIPSIS);
            while (*shown && gfx_text_width(gfx_ui_font(), shown) > avail) {
                shown++;
            }
            gfx_draw_text(&win->gfx, 6, 4, UI_S_ELLIPSIS, LABEL_COLOR);
            gfx_draw_text(&win->gfx, 6 + gfx_char_advance(gfx_ui_font(), UI_G_ELLIPSIS), 4,
                          shown, LABEL_COLOR);
        } else {
            gfx_draw_text(&win->gfx, 6, 4, shown, LABEL_COLOR);
        }
    }

    /* M74: the Recent button, in the header's own right-hand end. Lit
     * while the recents view is showing, so it reads as a place you are
     * rather than only as a place you can go. */
    {
        uint32_t btn_bg = recent_mode ? SELECT_COLOR : COLS_BG;
        gfx_fill_rect(&win->gfx, RECENT_BTN_X, 3, RECENT_BTN_W, HEADER_H - 6, btn_bg);
        gfx_draw_text(&win->gfx, RECENT_BTN_X + 7, 4, "Recent", LABEL_COLOR);
    }

    /* M59: the column headings, and the sort control. The heading you
     * sort by is where every file manager puts it, and the arrow glyph
     * (M57) is what says which way - a highlight alone leaves "sorted by
     * size" and "sorted by size, backwards" looking identical. */
    gfx_fill_rect(&win->gfx, 0, HEADER_H, LIST_W, COLS_H, COLS_BG);
    {
        static const char *const HEADINGS[SORT_COUNT] = {"Name", "Size", "Modified"};
        const int32_t xs[SORT_COUNT] = {COL_NAME_X, COL_SIZE_R - 34, COL_DATE_X};
        for (int k = 0; k < SORT_COUNT; k++) {
            uint32_t fg = (k == (int)sort_key) ? TEXT_COLOR : LABEL_COLOR;
            gfx_draw_text_font(&win->gfx, xs[k], HEADER_H + 2, HEADINGS[k], fg, &LIST_FONT, 0);
            if (k == (int)sort_key) {
                int32_t w = gfx_text_width(&LIST_FONT, HEADINGS[k]);
                gfx_draw_char_font(&win->gfx, xs[k] + w + 2, HEADER_H + 2,
                                   sort_desc ? UI_G_ARROW_DOWN : UI_G_ARROW_UP,
                                   fg, &LIST_FONT, 0);
            }
        }
    }

    for (int row = 0; row < ROWS_VISIBLE; row++) {
        int i = scroll_top + row;
        if (i >= file_count) {
            break;
        }
        int32_t y = LIST_Y + row * ROW_H;
        if (i == selected) {
            gfx_fill_rect(&win->gfx, 0, y, LIST_W, ROW_H, SELECT_COLOR);
        }
        /* M57: a directory is marked with the arrow glyph. Until this
         * milestone the only thing that distinguished one from a file
         * was what happened when you double-clicked it. */
        int32_t name_x = COL_NAME_X + gfx_char_advance(&LIST_FONT, UI_G_ARROW_RIGHT) + 3;
        if (is_dir[i]) {
            gfx_draw_text_font(&win->gfx, COL_NAME_X, y + 2, UI_S_ARROW_RIGHT, DIR_MARK_COLOR, &LIST_FONT, 0);
        }
        {
            /* Truncated with the ellipsis glyph rather than run into the
             * size column - a name that overlaps a number reads as
             * neither. */
            char shown_name[MAX_NAME_LEN + 2];
            int32_t avail = COL_NAME_W;
            if (gfx_text_width(&LIST_FONT, names[i]) > avail) {
                int32_t fit = gfx_text_fit(&LIST_FONT, names[i],
                                            avail - gfx_char_advance(&LIST_FONT, UI_G_ELLIPSIS));
                memcpy(shown_name, names[i], (size_t)fit);
                shown_name[fit] = UI_G_ELLIPSIS;
                shown_name[fit + 1] = '\0';
            } else {
                memcpy(shown_name, names[i], MAX_NAME_LEN);
                shown_name[MAX_NAME_LEN] = '\0';
            }
            gfx_draw_text_font(&win->gfx, name_x, y + 2, shown_name, TEXT_COLOR, &LIST_FONT, 0);
        }

        char cell[20];
        if (!is_dir[i]) {
            fsutil_format_size(sizes[i], cell);
            gfx_draw_text_font(&win->gfx, COL_SIZE_R - gfx_text_width(&LIST_FONT, cell), y + 2,
                                cell, LABEL_COLOR, &LIST_FONT, 0);
        }
        fsutil_format_date(mtimes[i], cell);
        gfx_draw_text_font(&win->gfx, COL_DATE_X, y + 2, cell, LABEL_COLOR, &LIST_FONT, 0);
    }

    /* M37: the on-screen position/extent indicator this list previously
     * had none of - it already scrolled (Up/Down, or clicking a row near
     * an edge), there was just no visual cue there was more above/below. */
    gfx_draw_scrollbar(&win->gfx, LIST_W, LIST_Y, SCROLLBAR_W, LIST_H,
                        file_count, ROWS_VISIBLE, scroll_top,
                        SCROLLBAR_TRACK, SCROLLBAR_THUMB);

    /* M112: the strip used to hold a hint line - "R rename  C copy  Del
     * delete" - which is the wrong thing to spend the only row of
     * writing in this window on, now that a right-click menu names all
     * six operations and their keys. It describes what is selected, or
     * what the folder holds and what is left of the disk. */
    gfx_fill_rect(&win->gfx, 0, WIN_H - STATUS_H, WIN_W, STATUS_H, STATUS_BG);
    gfx_draw_text(&win->gfx, 6, WIN_H - STATUS_H + 2, status_text,
                  status_error ? STATUS_ERR_FG : LABEL_COLOR);

    /* M56: the prompt, drawn last so it sits over the list it is about -
     * this app owns its whole window buffer and there is no
     * compositor-level popup surface to put it in (the same note M35 left
     * on text_editor.c's own dialog). */
    /* M112: the right-click menu, over the list and under the prompt -
     * a menu item can open a prompt, so the prompt has to be the thing
     * on top when both are somehow up. */
    if (ctx_open) {
        gfx_draw_menu(&win->gfx, ctx_x, ctx_y, CTX_ITEM_W, CTX_ITEM_H,
                      CTX_ITEMS, CTX_COUNT, -1,
                      CTX_MENU_BG, CTX_MENU_HOVER, CTX_MENU_BORDER, CTX_MENU_TEXT);
    }

    if (prompt_kind == FM_PROMPT_INFO) {
        /* M112: five facts about one thing, in its own taller box. The
         * exact byte count is the one that could not be read anywhere
         * else in this window - the size column rounds, by design. */
        int32_t x = (WIN_W - INFO_W) / 2;
        int32_t y = (WIN_H - INFO_H) / 2;
        gfx_fill_rect_rounded(&win->gfx, x, y, INFO_W, INFO_H, PROMPT_BG);
        gfx_draw_rect_rounded(&win->gfx, x, y, INFO_W, INFO_H, PROMPT_BORDER);
        int32_t tx = x + GFX_PAD;
        int32_t ty = y + 8;
        int32_t line = UI_FONT_UI_HEIGHT + 4;
        int32_t avail = INFO_W - 2 * GFX_PAD;

        /* The path, trimmed from the head exactly as the path bar is -
         * the tail says which file this is, the head only says how you
         * got to it. */
        {
            const char *shown = info_path;
            if (gfx_text_width(gfx_ui_font(), shown) > avail) {
                int32_t room = avail - gfx_char_advance(gfx_ui_font(), UI_G_ELLIPSIS);
                while (*shown && gfx_text_width(gfx_ui_font(), shown) > room) {
                    shown++;
                }
                gfx_draw_text(&win->gfx, tx, ty, UI_S_ELLIPSIS, PROMPT_TEXT);
                gfx_draw_text(&win->gfx, tx + gfx_char_advance(gfx_ui_font(), UI_G_ELLIPSIS), ty,
                              shown, PROMPT_TEXT);
            } else {
                gfx_draw_text(&win->gfx, tx, ty, shown, PROMPT_TEXT);
            }
        }
        ty += line;
        gfx_draw_text(&win->gfx, tx, ty, info_is_dir ? "Folder" : "File", LABEL_COLOR);
        ty += line;

        char cell[FSUTIL_EXACT_MAX];
        char row[80];
        int n = 0;
        if (info_is_dir) {
            if (info_tree_valid) {
                n = sb_num(row, 0, (int)sizeof(row), info_tree.entries);
                n = sb_puts(row, n, (int)sizeof(row), info_tree.entries == 1 ? " item, " : " items, ");
                fsutil_format_exact(info_tree.bytes, cell);
                n = sb_puts(row, n, (int)sizeof(row), cell);
                sb_puts(row, n, (int)sizeof(row), " bytes");
            } else {
                sb_puts(row, 0, (int)sizeof(row), "could not be counted");
            }
        } else {
            fsutil_format_exact(info_stat.size, cell);
            n = sb_puts(row, 0, (int)sizeof(row), cell);
            sb_puts(row, n, (int)sizeof(row), " bytes");
        }
        gfx_draw_text(&win->gfx, tx, ty, row, PROMPT_TEXT);
        ty += line;

        /* A tree the walk could not finish is said out loud rather than
         * rounded off, because the number above it is then a floor. */
        if (info_is_dir && info_tree_valid && info_tree.deep) {
            gfx_draw_text(&win->gfx, tx, ty, "(deeper than this can count)", STATUS_ERR_FG);
        } else {
            fsutil_format_date(info_stat.mtime, cell);
            n = sb_puts(row, 0, (int)sizeof(row), "Modified  ");
            sb_puts(row, n, (int)sizeof(row), cell);
            gfx_draw_text(&win->gfx, tx, ty, row, LABEL_COLOR);
        }
        ty += line;
        gfx_draw_text(&win->gfx, tx, ty, "any key closes", LABEL_COLOR);
    } else if (prompt_kind != FM_PROMPT_NONE) {
        int32_t x = (WIN_W - PROMPT_W) / 2;
        int32_t y = (WIN_H - PROMPT_H) / 2;
        gfx_fill_rect_rounded(&win->gfx, x, y, PROMPT_W, PROMPT_H, PROMPT_BG);
        gfx_draw_rect_rounded(&win->gfx, x, y, PROMPT_W, PROMPT_H, PROMPT_BORDER);
        if (prompt_kind == FM_PROMPT_CONFIRM_DELETE) {
            /* M112: the question names what is about to go. "Delete this
             * folder?" asked about a folder holding forty-one files is
             * not a confirm - it is a button somebody presses. */
            const char *question = "Delete this file?";
            char counted[64];
            if (selected >= 0 && selected < file_count && is_dir[selected]) {
                if (info_tree_valid && info_tree.entries > 0) {
                    int n = sb_puts(counted, 0, (int)sizeof(counted), "Delete folder and ");
                    n = sb_num(counted, n, (int)sizeof(counted), info_tree.entries);
                    sb_puts(counted, n, (int)sizeof(counted),
                            info_tree.entries == 1 ? " item inside?" : " items inside?");
                    question = counted;
                } else {
                    question = "Delete this empty folder?";
                }
            }
            gfx_draw_text(&win->gfx, x + GFX_PAD, y + 8, question, PROMPT_TEXT);
            gfx_draw_text(&win->gfx, x + GFX_PAD, y + 32, "Y = delete   any key = cancel", PROMPT_TEXT);
        } else {
            static const char *const TITLES[] = {
                "", "Rename to:", "Copy to:", "", "New file named:", "New folder named:",
            };
            gfx_draw_text(&win->gfx, x + GFX_PAD, y + 8, TITLES[prompt_kind], PROMPT_TEXT);
            gfx_fill_rect_rounded(&win->gfx, x + GFX_PAD, y + 28, PROMPT_W - 2 * GFX_PAD, UI_FONT_UI_HEIGHT + 4, PROMPT_INPUT_BG);
            char buf[PROMPT_MAX_LEN + 1];
            memcpy(buf, prompt_buf, (size_t)prompt_len);
            buf[prompt_len] = '\0';
            gfx_draw_text(&win->gfx, x + GFX_PAD + 4, y + 30, buf, PROMPT_TEXT);
            gfx_fill_rect(&win->gfx, x + GFX_PAD + 4 + gfx_text_width(gfx_ui_font(), buf), y + 30,
                          2, (int32_t)gfx_ui_font()->height, PROMPT_TEXT);
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
    wm_present(&win); /* M117: the first frame, like every other one */

    long last_click_ms = -1;
    int last_click_row = -1;
    long next_refresh = sys_uptime_ms() + 1000;
    static const long REFRESH_MS = 1000;

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                changed = 1; /* M55: a replacement compositor handed this client a blank buffer - see WM_EVENT_EXPOSE */
            } else if (prompt_kind != FM_PROMPT_NONE &&
                       (ev.type == WM_EVENT_MOUSE_BUTTON ||
                        ev.type == WM_EVENT_MOUSE_WHEEL ||
                        ev.type == WM_EVENT_MOUSE_MOVE)) {
                /* M112: an open prompt owns the mouse too, not only the
                 * keyboard.
                 *
                 * The comment below has said "an open prompt owns every
                 * keystroke" since M56 and the mouse was never included,
                 * which was a real bug rather than an omission: every
                 * prompt acts on "the selection", and a click on another
                 * row while a Rename box was open moved it. Enter then
                 * renamed the row that had been clicked, using the name
                 * prefilled from the row that had not. Nothing on screen
                 * said which file the box was about, because the box
                 * does not name it.
                 *
                 * An armed drag is cancelled rather than left armed: the
                 * press that armed it is now the press that is being
                 * ignored, and a drag that began after the prompt closed
                 * would be one nobody started. */
                drag_armed_row = -1;
                dragging = 0;
            } else if (ev.type == WM_EVENT_KEY && prompt_kind != FM_PROMPT_NONE) {
                /* M56: an open prompt owns every keystroke until it is
                 * answered - the same "in-progress interaction takes over
                 * the input stream" shape text_editor.c's own prompt has
                 * had since M36, and compositor.c's drag state machine
                 * since M31. */
                changed = 1;
                if (prompt_kind == FM_PROMPT_INFO) {
                    prompt_kind = FM_PROMPT_NONE; /* M112: it asks nothing, so anything dismisses it */
                } else if (prompt_kind == FM_PROMPT_CONFIRM_DELETE) {
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
                /* M56: single keys rather than a menu bar. This window
                 * has none to hang them off, and a modifier chord would
                 * collide with the window-manager ones the compositor
                 * swallows before a client ever sees them
                 * (system_api/include/shortcuts.h).
                 *
                 * M112: six of them now, and they are written on the
                 * right-click menu's own labels - a shortcut nothing
                 * names is a shortcut only its author has. Any key also
                 * closes that menu, which is what every menu anywhere
                 * does and what stops one being left open over the list. */
                ctx_open = 0;
                if (ev.ch == 'r' || ev.ch == 'R') {
                    prompt_open(FM_PROMPT_RENAME);
                    changed = 1;
                } else if (ev.ch == 'c' || ev.ch == 'C') {
                    prompt_open(FM_PROMPT_COPY);
                    changed = 1;
                } else if (ev.ch == 'n' || ev.ch == 'N') {
                    prompt_open_new(FM_PROMPT_NEW_FILE);
                    changed = 1;
                } else if (ev.ch == 'f' || ev.ch == 'F') {
                    prompt_open_new(FM_PROMPT_NEW_FOLDER);
                    changed = 1;
                } else if (ev.ch == 'i' || ev.ch == 'I') {
                    prompt_open_info();
                    changed = 1;
                } else if (ev.ch == 0x7F || ev.ch == '\b') {
                    prompt_open(FM_PROMPT_CONFIRM_DELETE);
                    changed = 1;
                } else if (ev.ch == KBD_KEY_UP && selected > 0) {
                    selected--;
                    clamp_scroll();
                    status_refresh();
                    changed = 1;
                } else if (ev.ch == KBD_KEY_DOWN && selected + 1 < file_count) {
                    selected++;
                    clamp_scroll();
                    status_refresh();
                    changed = 1;
                } else if (ev.ch == KBD_KEY_LEFT) {
                    /* M112: the gesture every file browser has and this
                     * one did not - leaving a directory took selecting
                     * row 0 and pressing Enter, which is two keys for
                     * the thing done most often. */
                    leave_directory();
                    changed = 1;
                } else if (ev.ch == KBD_KEY_RIGHT || ev.ch == '\n' || ev.ch == '\r') {
                    open_selected();
                    changed = 1;
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
                        status_expires_ms = 0;
                        status_refresh();
                        changed = 1;
                    }
                }
                if (!(ev.buttons & 1)) {
                    drag_armed_row = -1;
                    dragging = 0;
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 2)) {
                /* M112: right-click, the same shape desktop_icons.c's own
                 * context menu has had since M35 - always (re)opens at
                 * the new point rather than toggling, so a second
                 * right-click somewhere else moves the menu instead of
                 * dismissing it.
                 *
                 * A right-click on a row selects it first. That is what
                 * every file manager does and it is not a nicety: the
                 * five item-shaped entries below all act on "the
                 * selection", so a menu opened over one row while a
                 * different one was selected would offer to delete
                 * something the pointer is not on. */
                if (ev.y >= LIST_Y && ev.y < LIST_Y + LIST_H) {
                    int row = scroll_top + (ev.y - LIST_Y) / ROW_H;
                    if (row < file_count) {
                        selected = row;
                        status_refresh();
                    }
                }
                ctx_open = 1;
                ctx_x = ev.x;
                ctx_y = ev.y;
                int32_t max_x = LIST_W - CTX_ITEM_W;
                int32_t max_y = (WIN_H - STATUS_H) - CTX_ITEM_H * CTX_COUNT;
                if (ctx_x > max_x) {
                    ctx_x = max_x;
                }
                if (ctx_y > max_y) {
                    ctx_y = max_y;
                }
                if (ctx_x < 0) {
                    ctx_x = 0;
                }
                if (ctx_y < 0) {
                    ctx_y = 0;
                }
                changed = 1;
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
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1) && ctx_open) {
                /* M112: a left-click while the menu is open is consumed
                 * by it - picking an item or dismissing it - and never
                 * also falls through to the list underneath. Same
                 * contract desktop_icons.c's menu has, and the reason a
                 * click that closes a menu does not move the selection. */
                int idx = gfx_menu_hit_test(ev.x, ev.y, ctx_x, ctx_y,
                                             CTX_ITEM_W, CTX_ITEM_H, CTX_COUNT);
                ctx_open = 0;
                changed = 1;
                if (idx == CTX_NEW_FILE) {
                    prompt_open_new(FM_PROMPT_NEW_FILE);
                } else if (idx == CTX_NEW_FOLDER) {
                    prompt_open_new(FM_PROMPT_NEW_FOLDER);
                } else if (idx == CTX_RENAME) {
                    prompt_open(FM_PROMPT_RENAME);
                } else if (idx == CTX_COPY) {
                    prompt_open(FM_PROMPT_COPY);
                } else if (idx == CTX_DELETE) {
                    prompt_open(FM_PROMPT_CONFIRM_DELETE);
                } else if (idx == CTX_INFO) {
                    prompt_open_info();
                }
            } else if (ev.type == WM_EVENT_MOUSE_BUTTON && (ev.buttons & 1)) {
                /* M59: a click on a column heading sorts by it; a second
                 * click on the same one reverses it. Same convention as
                 * every file list anywhere, and the reason the arrow
                 * glyph is drawn there. */
                if (ev.y < HEADER_H) {
                    /* M74: the header's only clickable thing. Anywhere
                     * else in it is the path bar, which is text. */
                    if (ev.x >= RECENT_BTN_X && ev.x < RECENT_BTN_X + RECENT_BTN_W) {
                        if (!recent_mode) {
                            for (int i = 0; i < PATH_MAX_LEN; i++) {
                                recent_return[i] = cwd[i];
                            }
                            recent_mode = 1;
                        } else {
                            recent_mode = 0;
                            for (int i = 0; i < PATH_MAX_LEN; i++) {
                                cwd[i] = recent_return[i];
                            }
                        }
                        selected = -1;
                        scroll_top = 0;
                        status_expires_ms = 0;
                        refresh_list();
                        changed = 1;
                    }
                } else if (ev.y >= HEADER_H && ev.y < LIST_Y) {
                    fm_sort_t want = SORT_NAME;
                    if (ev.x >= COL_DATE_X) {
                        want = SORT_DATE;
                    } else if (ev.x >= COL_SIZE_R - 34) {
                        want = SORT_SIZE;
                    }
                    if (want == sort_key) {
                        sort_desc = !sort_desc;
                    } else {
                        sort_key = want;
                        sort_desc = 0;
                    }
                    sort_list();
                    changed = 1;
                } else if (ev.y >= LIST_Y && ev.y < LIST_Y + LIST_H) {
                    int row = scroll_top + (ev.y - LIST_Y) / ROW_H;
                    if (row < file_count) {
                        selected = row;
                        drag_armed_row = row; /* M49: a drag may be starting - see DRAG_THRESHOLD */
                        drag_press_x = ev.x;
                        drag_press_y = ev.y;
                        status_refresh();
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
            wm_present(&win);
        }
        wm_wait_ms(&win, NULL, 0, (int)(next_refresh - now)); /* M117: block until an event, a deadline, or the liveness cap - see wm_wait_ms */
    }
}
