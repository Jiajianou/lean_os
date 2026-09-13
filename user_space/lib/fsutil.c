#include "fsutil.h"

#include "os_time.h"
#include "paths.h"
#include "syscall_wrappers.h"

static int digits_into(uint32_t v, char *out) {
    char tmp[12];
    int t = 0;
    do {
        tmp[t++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v);
    for (int i = 0; i < t; i++) {
        out[i] = tmp[t - 1 - i];
    }
    return t;
}

void fsutil_format_size(uint32_t bytes, char *out) {
    static const char SUFFIX[] = " KMG";
    int unit = 0;
    uint32_t whole = bytes;
    uint32_t frac = 0;
    while (whole >= 1000u && unit < 3) {
        frac = ((whole % 1024u) * 10u) / 1024u;
        whole /= 1024u;
        unit++;
    }
    int n = digits_into(whole, out);
    if (unit > 0 && whole < 10) {
        out[n++] = '.';
        out[n++] = (char)('0' + frac);
    }
    if (unit > 0) {
        out[n++] = SUFFIX[unit];
    }
    out[n] = '\0';
}

void fsutil_format_exact(uint32_t n, char *out) {
    char plain[12];
    int len = digits_into(n, plain);
    int w = 0;
    for (int i = 0; i < len; i++) {
        if (i > 0 && ((len - i) % 3) == 0) {
            out[w++] = ',';
        }
        out[w++] = plain[i];
    }
    out[w] = '\0';
}

void fsutil_format_date(uint32_t mtime, char *out) {
    if (mtime == 0) {
        out[0] = '-';
        out[1] = '\0';
        return;
    }
    os_datetime_t t;
    os_civil_from_unix(mtime, &t);
    int n = 0;
    out[n++] = (char)('0' + t.month / 10);
    out[n++] = (char)('0' + t.month % 10);
    out[n++] = '-';
    out[n++] = (char)('0' + t.day / 10);
    out[n++] = (char)('0' + t.day % 10);
    out[n++] = ' ';
    out[n++] = (char)('0' + t.hour / 10);
    out[n++] = (char)('0' + t.hour % 10);
    out[n++] = ':';
    out[n++] = (char)('0' + t.minute / 10);
    out[n++] = (char)('0' + t.minute % 10);
    out[n] = '\0';
}

int fsutil_name_ok(const char *name) {
    if (!name || !name[0]) {
        return 0;
    }
    if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) {
        return 0;
    }
    int n = 0;
    for (const char *s = name; *s; s++, n++) {
        unsigned char c = (unsigned char)*s;
        if (c == '/' || c < 0x20 || c == 0x7F) {
            return 0;
        }
        if (n >= OS_NAME_MAX) {
            return 0;
        }
    }
    return 1;
}

static char walk_path[PATH_MAX_LEN];

static int walk_push(int len, const char *name) {
    int n = len;
    if (n > 0 && walk_path[n - 1] != '/') {
        if (n >= PATH_MAX_LEN - 1) {
            return -1;
        }
        walk_path[n++] = '/';
    }
    for (const char *s = name; *s; s++) {
        if (n >= PATH_MAX_LEN - 1) {
            return -1;
        }
        walk_path[n++] = *s;
    }
    walk_path[n] = '\0';
    return n;
}

static int is_dot_entry(const char *name) {
    return name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]));
}

static int count_at(int len, int depth, fsutil_tree_t *out) {
    if (depth >= FSUTIL_MAX_DEPTH) {
        out->deep = 1;
        return 0;
    }
    unsigned int cookie = 0;
    for (;;) {
        union { char bytes[FSUTIL_DIRENT_BUF]; uint64_t align; } buf;
        long n = sys_getdents(walk_path, &cookie, buf.bytes, sizeof(buf.bytes));
        if (n <= 0) {
            return n == 0 ? 0 : -1;
        }
        for (long off = 0; off + 8 <= n;) {
            const os_dirent_t *d = (const os_dirent_t *)(void *)(buf.bytes + off);
            if (d->reclen == 0 || off + d->reclen > n) {
                break;
            }
            off += d->reclen;
            if (is_dot_entry(d->name)) {
                continue;
            }
            out->entries++;
            int saved = len;
            int pushed = walk_push(len, d->name);
            if (pushed < 0) {
                continue;
            }
            if (d->type == OS_DT_DIR) {
                count_at(pushed, depth + 1, out);
            } else {
                os_stat_t st;
                if (sys_stat(walk_path, &st) == 0) {
                    out->bytes += st.size;
                }
            }
            walk_path[saved] = '\0';
        }
    }
}

int fsutil_count_tree(const char *path, fsutil_tree_t *out) {
    out->entries = 0;
    out->bytes = 0;
    out->deep = 0;
    walk_path[0] = '\0';
    int len = walk_push(0, path);
    if (len < 0) {
        return -1;
    }
    return count_at(len, 0, out);
}

static int remove_at(int len, int depth) {
    if (depth >= FSUTIL_MAX_DEPTH) {
        return -1;
    }
    for (;;) {
        unsigned int cookie = 0;
        union { char bytes[FSUTIL_DIRENT_BUF]; uint64_t align; } buf;
        long n = sys_getdents(walk_path, &cookie, buf.bytes, sizeof(buf.bytes));
        if (n <= 0) {
            break;
        }
        int removed = 0;
        for (long off = 0; off + 8 <= n;) {
            const os_dirent_t *d = (const os_dirent_t *)(void *)(buf.bytes + off);
            if (d->reclen == 0 || off + d->reclen > n) {
                break;
            }
            off += d->reclen;
            if (is_dot_entry(d->name)) {
                continue;
            }
            int saved = len;
            int pushed = walk_push(len, d->name);
            if (pushed < 0) {
                return -1;
            }
            int rc = (d->type == OS_DT_DIR) ? remove_at(pushed, depth + 1)
                                            : (int)sys_unlink(walk_path);
            walk_path[saved] = '\0';
            if (rc != 0) {
                return -1;
            }
            removed++;
        }
        if (!removed) {
            return -1;
        }
    }
    return (int)sys_rmdir(walk_path);
}

int fsutil_remove_tree(const char *path) {
    walk_path[0] = '\0';
    int len = walk_push(0, path);
    if (len < 0) {
        return -1;
    }
    return remove_at(len, 0);
}
