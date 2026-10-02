#include "file_browser.h"
#include "file_system_utilities.h"
#include "paths.h"
#include "syscall_wrappers.h"

#define E_OK          0
#define E_MKDIR       2
#define E_CREATE      3
#define E_COUNT       4
#define E_COUNT_WRONG 5
#define E_RMDIR_TOOK_A_FULL_ONE 6
#define E_REMOVE      7
#define E_STILL_THERE 8
#define E_TOOK_A_NEIGHBOUR 9
#define E_NAME_RULE   10
#define E_COPY        11
#define E_COPY_WRONG  12
#define E_INTO_ITSELF 13
#define E_TRASH       14
#define E_PUT_BACK    15
#define E_SEARCH      16
#define E_KERNEL_LOOP 17
#define E_MOVE        18

#define ROOT     "/home/m112tree"
#define NEIGHBOUR "/home/m112keep"

#define WIDE_FILES 12
#define WIDE_BYTES 7

static int make_file(const char *path, int bytes) {
    long fd = sys_open(path, OPEN_WRITE | OPEN_CREATE | OPEN_EXCL);
    if (fd < 0) {
        return -1;
    }
    static const char FILL[64] = {0};
    long n = sys_write((int)fd, FILL, (size_t)bytes);
    sys_close((int)fd);
    return n == bytes ? 0 : -1;
}

static int gone(const char *path);

static int same_bytes(const char *a, const char *b) {
    static char left[256];
    static char right[256];
    long n = sys_readfile(a, left, sizeof(left));
    long m = sys_readfile(b, right, sizeof(right));
    if (n < 0 || n != m) {
        return 0;
    }
    for (long i = 0; i < n; i++) {
        if (left[i] != right[i]) {
            return 0;
        }
    }
    return 1;
}

typedef struct {
    int found;
    int wrong;
} search_tally_t;

static int tally(void *context, const char *path, const os_stat_t *status) {
    (void)status;
    search_tally_t *t = (search_tally_t *)context;
    if (file_browser_is_inside(path, "/home/m210tree")) {
        t->found++;
    } else if (file_browser_is_inside(path, "/home")) {
        t->wrong++;
    }
    return 0;
}

#define FINDER "/home/m210tree"
#define FINDER_COPY "/home/m210copy"

static int finder_checks(void) {
    file_system_utilities_remove_tree(FINDER);
    file_system_utilities_remove_tree(FINDER_COPY);
    if (sys_mkdir(FINDER) != 0 || sys_mkdir(FINDER "/inner") != 0 ||
        sys_writefile(FINDER "/needle-one.txt", "first needle", 12) != 0 ||
        sys_writefile(FINDER "/inner/Needle-two", "second needle, deeper", 21) != 0) {
        return E_MKDIR;
    }
    if (file_browser_copy(FINDER, FINDER_COPY) != FILE_BROWSER_OK) {
        return E_COPY;
    }
    if (!same_bytes(FINDER "/needle-one.txt", FINDER_COPY "/needle-one.txt") ||
        !same_bytes(FINDER "/inner/Needle-two", FINDER_COPY "/inner/Needle-two")) {
        return E_COPY_WRONG;
    }
    if (file_browser_copy(FINDER, FINDER "/inner/again") != FILE_BROWSER_ERROR_INTO_ITSELF ||
        file_browser_move_into(FINDER, FINDER "/inner", 0, 0) != FILE_BROWSER_ERROR_INTO_ITSELF) {
        return E_INTO_ITSELF;
    }
    if (sys_rename(FINDER, FINDER "/inner/escaped") == 0) {
        return E_KERNEL_LOOP;
    }
    char moved[256];
    if (file_browser_move_into(FINDER_COPY, FINDER "/inner", moved, sizeof(moved)) != FILE_BROWSER_OK ||
        !same_bytes(FINDER "/needle-one.txt", FINDER "/inner/m210copy/needle-one.txt")) {
        return E_MOVE;
    }

    char trashed[256];
    if (file_browser_trash(FINDER "/needle-one.txt", trashed, sizeof(trashed)) != FILE_BROWSER_OK ||
        !gone(FINDER "/needle-one.txt") || gone(trashed)) {
        return E_TRASH;
    }
    char restored[256];
    if (file_browser_put_back(trashed, restored, sizeof(restored)) != FILE_BROWSER_OK ||
        gone(FINDER "/needle-one.txt") || !gone(trashed)) {
        return E_PUT_BACK;
    }

    search_tally_t t = {0, 0};
    file_browser_search("/home", "NEEDLE", 0, 100000, tally, &t);
    if (t.found != 4 || t.wrong != 0) {
        return E_SEARCH;
    }

    file_system_utilities_remove_tree(FINDER);
    return E_OK;
}

static int gone(const char *path) {
    os_stat_t st;
    return sys_stat(path, &st) != 0;
}

int main(void) {
    file_system_utilities_remove_tree(ROOT);
    file_system_utilities_remove_tree(NEIGHBOUR);

    if (sys_mkdir(ROOT) != 0 || sys_mkdir(ROOT "/mid") != 0 ||
        sys_mkdir(ROOT "/mid/deep") != 0 || sys_mkdir(ROOT "/wide") != 0) {
        return E_MKDIR;
    }
    if (sys_mkdir(NEIGHBOUR) != 0 || make_file(NEIGHBOUR "/keep", 5) != 0) {
        return E_MKDIR;
    }
    if (make_file(ROOT "/top", 3) != 0 || make_file(ROOT "/mid/a", 10) != 0 ||
        make_file(ROOT "/mid/deep/b", 20) != 0) {
        return E_CREATE;
    }
    for (int i = 0; i < WIDE_FILES; i++) {
        char path[64];
        int n = 0;
        for (const char *s = ROOT "/wide/file"; *s; s++) {
            path[n++] = *s;
        }
        path[n++] = (char)('0' + i / 10);
        path[n++] = (char)('0' + i % 10);
        path[n] = '\0';
        if (make_file(path, WIDE_BYTES) != 0) {
            return E_CREATE;
        }
    }

    file_system_utilities_tree_t t;
    if (file_system_utilities_count_tree(ROOT, &t) != 0) {
        return E_COUNT;
    }
    if (t.entries != (uint32_t)(6 + WIDE_FILES) ||
        t.bytes != (uint32_t)(3 + 10 + 20 + WIDE_FILES * WIDE_BYTES) || t.deep) {
        return E_COUNT_WRONG;
    }

    if (sys_rmdir(ROOT) == 0) {
        return E_RMDIR_TOOK_A_FULL_ONE;
    }

    if (file_system_utilities_remove_tree(ROOT) != 0) {
        return E_REMOVE;
    }
    if (!gone(ROOT) || !gone(ROOT "/mid") || !gone(ROOT "/mid/deep") ||
        !gone(ROOT "/mid/deep/b") || !gone(ROOT "/wide") || !gone(ROOT "/top")) {
        return E_STILL_THERE;
    }

    os_stat_t st;
    if (sys_stat(NEIGHBOUR, &st) != 0 || sys_stat(NEIGHBOUR "/keep", &st) != 0 ||
        st.size != 5) {
        return E_TOOK_A_NEIGHBOUR;
    }
    file_system_utilities_remove_tree(NEIGHBOUR);

    if (file_system_utilities_name_ok("../elsewhere") || file_system_utilities_name_ok("a/b") ||
        file_system_utilities_name_ok("..") || file_system_utilities_name_ok("") || !file_system_utilities_name_ok("ordinary.txt")) {
        return E_NAME_RULE;
    }
    return finder_checks();
}
