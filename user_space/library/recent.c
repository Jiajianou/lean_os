#include "recent.h"

#include "string_utilities.h"
#include "syscall_wrappers.h"

#define RECENT_BUF ((PATH_MAX_LEN + 1) * RECENT_MAX)

static int copy_path(char *dst, const char *src) {
    int n = 0;
    for (; src[n] && n < PATH_MAX_LEN - 1; n++) {
        dst[n] = src[n];
    }
    dst[n] = '\0';
    return n;
}

static int read_raw(char out[][PATH_MAX_LEN], int max) {
    static char buf[RECENT_BUF];
    long n = sys_readfile(RECENT_PATH, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    int count = 0;
    int col = 0;
    for (long i = 0; i <= n && count < max; i++) {
        char c = (i < n) ? buf[i] : '\n';
        if (c == '\n' || c == '\r') {
            if (col > 0) {
                out[count][col] = '\0';
                count++;
            }
            col = 0;
            continue;
        }
        if (col < PATH_MAX_LEN - 1) {
            out[count][col++] = c;
        }
    }
    return count;
}

void recent_add(const char *path) {
    if (!path || !path[0] || path[0] != '/') {
        return;
    }
    if (strlen(path) >= PATH_MAX_LEN) {
        return;
    }
    static char list[RECENT_MAX][PATH_MAX_LEN];
    int count = read_raw(list, RECENT_MAX);

    if (count > 0 && strcmp(list[0], path) == 0) {
        return;
    }

    static char merged[RECENT_MAX][PATH_MAX_LEN];
    copy_path(merged[0], path);
    int m = 1;
    for (int i = 0; i < count && m < RECENT_MAX; i++) {
        if (strcmp(list[i], path) == 0) {
            continue;
        }
        copy_path(merged[m++], list[i]);
    }

    static char out[RECENT_BUF];
    int n = 0;
    for (int i = 0; i < m; i++) {
        for (int j = 0; merged[i][j] && n < (int)sizeof(out) - 1; j++) {
            out[n++] = merged[i][j];
        }
        if (n < (int)sizeof(out)) {
            out[n++] = '\n';
        }
    }
    sys_writefile(RECENT_PATH, out, (size_t)n);
}

int recent_load(char out[][PATH_MAX_LEN], int max) {
    static char raw[RECENT_MAX][PATH_MAX_LEN];
    int count = read_raw(raw, RECENT_MAX);
    int kept = 0;
    for (int i = 0; i < count && kept < max; i++) {
        os_stat_t st;
        if (sys_stat(raw[i], &st) != 0 || st.is_dir) {
            continue;
        }
        copy_path(out[kept], raw[i]);
        kept++;
    }
    return kept;
}
