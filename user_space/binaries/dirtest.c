#include "fsutil.h"
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

static int gone(const char *path) {
    os_stat_t st;
    return sys_stat(path, &st) != 0;
}

int main(void) {
    fsutil_remove_tree(ROOT);
    fsutil_remove_tree(NEIGHBOUR);

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

    fsutil_tree_t t;
    if (fsutil_count_tree(ROOT, &t) != 0) {
        return E_COUNT;
    }
    if (t.entries != (uint32_t)(6 + WIDE_FILES) ||
        t.bytes != (uint32_t)(3 + 10 + 20 + WIDE_FILES * WIDE_BYTES) || t.deep) {
        return E_COUNT_WRONG;
    }

    if (sys_rmdir(ROOT) == 0) {
        return E_RMDIR_TOOK_A_FULL_ONE;
    }

    if (fsutil_remove_tree(ROOT) != 0) {
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
    fsutil_remove_tree(NEIGHBOUR);

    if (fsutil_name_ok("../elsewhere") || fsutil_name_ok("a/b") ||
        fsutil_name_ok("..") || fsutil_name_ok("") || !fsutil_name_ok("ordinary.txt")) {
        return E_NAME_RULE;
    }
    return E_OK;
}
