#include "paths.h"
#define LIST_FONT   ui_font_small
#define LIST_FONT_H UI_FONT_SMALL_HEIGHT
#include "str.h"
#include "fsutil.h"
#include "os_fs.h"
#include "spawn_error.h"
#include "recent.h"
#include "syscall_wrappers.h"
#include "wmclient.h"

#define WIN_W 340
#define WIN_H 360
#define ROW_H (LIST_FONT_H + 4)
#define HEADER_H 24
#define COLS_H 16
#define LIST_Y (HEADER_H + COLS_H)
#define SCROLLBAR_W 8
#define LIST_W (WIN_W - SCROLLBAR_W)

#define COL_NAME_X   6
#define COL_DATE_X   (LIST_W - 70)
#define COL_SIZE_R   (COL_DATE_X - 10)
#define COL_NAME_W   (COL_SIZE_R - 46 - COL_NAME_X)
#define STATUS_H (UI_FONT_UI_HEIGHT + 4)
#define LIST_H   (WIN_H - LIST_Y - STATUS_H)
#define ROWS_VISIBLE (LIST_H / ROW_H)

#define BG_COLOR       0x001C1C24u
#define HEADER_COLOR   0x00303850u
#define TEXT_COLOR     0x00D8D8D8u
#define SELECT_COLOR   0x004C6699u
#define LABEL_COLOR    0x0090A0C0u
#define DIR_MARK_COLOR 0x0078A8E0u
#define SCROLLBAR_TRACK 0x00141820u
#define SCROLLBAR_THUMB 0x00506080u
#define COLS_BG        0x00262C3Au
#define STATUS_BG      0x00141820u
#define STATUS_ERR_FG  0x00E08878u

#define MAX_FILES    512
#define MAX_NAME_LEN 256
#define LIST_BUF_SIZE 2048
#define DOUBLE_CLICK_MS 500

static char names[MAX_FILES][MAX_NAME_LEN];
static uint32_t sizes[MAX_FILES];
static uint32_t mtimes[MAX_FILES];
static uint8_t is_dir[MAX_FILES];
static int file_count;
static int selected = -1;
static int scroll_top;

typedef enum { SORT_NAME = 0, SORT_SIZE, SORT_DATE, SORT_COUNT } fm_sort_t;
static fm_sort_t sort_key = SORT_NAME;
static int sort_desc;
static const char *status_text = "";

static char cwd[PATH_MAX_LEN] = PATH_HOME;

#define RECENT_BTN_W 62
#define RECENT_BTN_X (LIST_W - RECENT_BTN_W - 6)
static int recent_mode;
static char recent_full[RECENT_MAX][PATH_MAX_LEN];
static char recent_return[PATH_MAX_LEN];

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

#define PROMPT_W 260
#define PROMPT_H 72
#define PROMPT_MAX_LEN 48
#define PROMPT_BG      0x00243040u
#define PROMPT_BORDER  0x004C6699u
#define PROMPT_TEXT    0x00E8E8E8u
#define PROMPT_INPUT_BG 0x00141820u

#define FM_COPY_CHUNK 4096

typedef enum {
    FM_PROMPT_NONE = 0,
    FM_PROMPT_RENAME,
    FM_PROMPT_COPY,
    FM_PROMPT_CONFIRM_DELETE,
    FM_PROMPT_NEW_FILE,
    FM_PROMPT_NEW_FOLDER,
    FM_PROMPT_INFO,
} fm_prompt_t;

static fm_prompt_t prompt_kind;
static char prompt_buf[PROMPT_MAX_LEN + 1];
static int prompt_len;

#define INFO_W 300
#define INFO_H 132

static os_stat_t info_stat;
static fsutil_tree_t info_tree;
static int info_is_dir;
static int info_tree_valid;
static char info_path[PATH_MAX_LEN];

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

static char status_buf[160];
static int status_error;

#define STATUS_MS 5000
static long status_expires_ms;

#define STATVFS_MS 5000
static long statvfs_next_ms;
static os_statvfs_t statvfs_cached;
static int statvfs_ok;

static void statvfs_invalidate(void) {
    statvfs_next_ms = 0;
}

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

static void status_say(const char *msg, int is_error) {
    status_text = msg;
    status_error = is_error;
    status_expires_ms = sys_uptime_ms() + STATUS_MS;
}

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
        return;
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

static void status_refresh(void) {
    if (status_expires_ms && sys_uptime_ms() < status_expires_ms) {
        return;
    }
    status_expires_ms = 0;
    status_describe();
}

#define DRAG_THRESHOLD 8
static int drag_armed_row = -1;
static int32_t drag_press_x, drag_press_y;
static int dragging;

static int sort_before(int a, int b) {
    if (is_dir[a] != is_dir[b]) {
        return is_dir[a];
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

    for (int i = 0; i < file_count; i++) {
        sizes[i] = 0;
        mtimes[i] = 0;
        char full[PATH_MAX_LEN];
        if (names[i][0] == '.' && names[i][1] == '.' && names[i][2] == '\0') {
            continue;
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

static void go_up(void) {
    int n = 0;
    while (cwd[n]) {
        n++;
    }
    while (n > 0 && cwd[n - 1] != '/') {
        n--;
    }
    if (n > 1) {
        n--;
    }
    cwd[n ? n : 1] = '\0';
    cwd[0] = '/';
}

static void leave_directory(void) {
    if (recent_mode) {
        recent_mode = 0;
        for (int i = 0; i < PATH_MAX_LEN; i++) {
            cwd[i] = recent_return[i];
        }
    } else if (cwd[0] == '/' && cwd[1] == '\0') {
        return;
    } else {
        go_up();
    }
    selected = -1;
    scroll_top = 0;
    status_expires_ms = 0;
    statvfs_invalidate();
    refresh_list();
}

static void open_selected(void) {
    if (selected < 0 || selected >= file_count) {
        return;
    }
    if (recent_mode) {
        if (is_dir[selected]) {
            leave_directory();
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
    long rc = sys_spawn(PATH_BIN_DIR "text_editor", full);
    if (rc < 0) {
        status_say(spawn_error_message(rc), 1);
    }
    if (rc >= 0) {
        recent_add(full);
    }
}

static int selected_file_path(char *out) {
    if (selected < 0 || selected >= file_count) {
        return -1;
    }
    if (names[selected][0] == '.' && names[selected][1] == '.' && names[selected][2] == '\0') {
        return -1;
    }
    if (recent_mode) {
        return -1;
    }
    return path_in_cwd(names[selected], out);
}

static void prompt_open_new(fm_prompt_t kind) {
    if (recent_mode) {
        status_say("Recent is a list, not a folder.", 1);
        return;
    }
    prompt_kind = kind;
    prompt_len = 0;
    prompt_buf[0] = '\0';
}

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
    if (kind == FM_PROMPT_CONFIRM_DELETE && selected >= 0 && is_dir[selected]) {
        info_tree_valid = (fsutil_count_tree(scratch, &info_tree) == 0);
    } else {
        info_tree_valid = 0;
    }
    prompt_kind = kind;
    prompt_len = 0;
    if (kind != FM_PROMPT_CONFIRM_DELETE) {
        for (int i = 0; names[selected][i] && i < PROMPT_MAX_LEN; i++) {
            prompt_buf[prompt_len++] = names[selected][i];
        }
    }
    prompt_buf[prompt_len] = '\0';
}

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
    if (sys_mkdir(full) != 0) {
        return "That name is already taken, or the disk is full.";
    }
    *failed = 0;
    return "Folder created.";
}

static void select_named(const char *name) {
    for (int i = 0; i < file_count; i++) {
        if (strcmp(names[i], name) == 0) {
            selected = i;
            clamp_scroll();
            return;
        }
    }
}

static const char *copy_file(const char *from, const char *to, int *failed) {
    *failed = 1;
    long src = sys_open(from, OPEN_READ);
    if (src < 0) {
        return "Could not read that file.";
    }
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
            break;
        }
        if (sys_write((int)dst, chunk, (size_t)n) != n) {
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
        if (is_dir[selected]) {
            int empty = info_tree_valid && info_tree.entries == 0;
            int ok = empty ? (sys_rmdir(from) == 0) : (fsutil_remove_tree(from) == 0);
            if (ok) {
                status_say("Deleted.", 0);
            } else {
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
    statvfs_invalidate();
    refresh_list();
}

static void redraw(wm_window_t *win) {
    gfx_fill_rect(&win->gfx, 0, 0, WIN_W, WIN_H, BG_COLOR);
    gfx_fill_rect(&win->gfx, 0, 0, LIST_W, HEADER_H, HEADER_COLOR);
    {
        const char *shown = recent_mode ? "Recent" : cwd;
        int32_t avail = RECENT_BTN_X - 12;
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

    {
        uint32_t btn_bg = recent_mode ? SELECT_COLOR : COLS_BG;
        gfx_fill_rect(&win->gfx, RECENT_BTN_X, 3, RECENT_BTN_W, HEADER_H - 6, btn_bg);
        gfx_draw_text(&win->gfx, RECENT_BTN_X + 7, 4, "Recent", LABEL_COLOR);
    }

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
        int32_t name_x = COL_NAME_X + gfx_char_advance(&LIST_FONT, UI_G_ARROW_RIGHT) + 3;
        if (is_dir[i]) {
            gfx_draw_text_font(&win->gfx, COL_NAME_X, y + 2, UI_S_ARROW_RIGHT, DIR_MARK_COLOR, &LIST_FONT, 0);
        }
        {
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

    gfx_draw_scrollbar(&win->gfx, LIST_W, LIST_Y, SCROLLBAR_W, LIST_H,
                        file_count, ROWS_VISIBLE, scroll_top,
                        SCROLLBAR_TRACK, SCROLLBAR_THUMB);

    gfx_fill_rect(&win->gfx, 0, WIN_H - STATUS_H, WIN_W, STATUS_H, STATUS_BG);
    gfx_draw_text(&win->gfx, 6, WIN_H - STATUS_H + 2, status_text,
                  status_error ? STATUS_ERR_FG : LABEL_COLOR);

    if (ctx_open) {
        gfx_draw_menu(&win->gfx, ctx_x, ctx_y, CTX_ITEM_W, CTX_ITEM_H,
                      CTX_ITEMS, CTX_COUNT, -1,
                      CTX_MENU_BG, CTX_MENU_HOVER, CTX_MENU_BORDER, CTX_MENU_TEXT);
    }

    if (prompt_kind == FM_PROMPT_INFO) {
        int32_t x = (WIN_W - INFO_W) / 2;
        int32_t y = (WIN_H - INFO_H) / 2;
        gfx_fill_rect_rounded(&win->gfx, x, y, INFO_W, INFO_H, PROMPT_BG);
        gfx_draw_rect_rounded(&win->gfx, x, y, INFO_W, INFO_H, PROMPT_BORDER);
        int32_t tx = x + GFX_PAD;
        int32_t ty = y + 8;
        int32_t line = UI_FONT_UI_HEIGHT + 4;
        int32_t avail = INFO_W - 2 * GFX_PAD;

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
    wm_present(&win);

    long last_click_ms = -1;
    int last_click_row = -1;
    long next_refresh = sys_uptime_ms() + 1000;
    static const long REFRESH_MS = 1000;

    for (;;) {
        int changed = 0;
        wm_event_t ev;
        while (wm_poll_event(&win, &ev)) {
            if (ev.type == WM_EVENT_EXPOSE || ev.type == WM_EVENT_DISPLAY_CHANGED) {
                changed = 1;
            } else if (prompt_kind != FM_PROMPT_NONE &&
                       (ev.type == WM_EVENT_MOUSE_BUTTON ||
                        ev.type == WM_EVENT_MOUSE_WHEEL ||
                        ev.type == WM_EVENT_MOUSE_MOVE)) {
                drag_armed_row = -1;
                dragging = 0;
            } else if (ev.type == WM_EVENT_KEY && prompt_kind != FM_PROMPT_NONE) {
                changed = 1;
                if (prompt_kind == FM_PROMPT_INFO) {
                    prompt_kind = FM_PROMPT_NONE;
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
                    leave_directory();
                    changed = 1;
                } else if (ev.ch == KBD_KEY_RIGHT || ev.ch == '\n' || ev.ch == '\r') {
                    open_selected();
                    changed = 1;
                }
            } else if (ev.type == WM_EVENT_MOUSE_MOVE) {
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
                if (ev.y < HEADER_H) {
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
                        drag_armed_row = row;
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
        wm_wait_ms(&win, NULL, 0, (int)(next_refresh - now));
    }
}
