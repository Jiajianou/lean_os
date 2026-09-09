/* user_space/lib/fsutil.c - M112. See fsutil.h for why this is a file. */
#include "fsutil.h"

#include "os_time.h" /* system_api/include/os_time.h - os_civil_from_unix */
#include "paths.h"   /* system_api/include/paths.h - PATH_MAX_LEN */
#include "syscall_wrappers.h"

/* Digits of `v` into `out`, most significant first. Returns how many
 * were written. Shared by both number formatters below, which differ
 * only in what they do around the digits. */
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
    /* 1000 rather than 1024 as the threshold, deliberately: the column
     * is four characters wide before a suffix, so a number that would
     * print as "1023" is still one this column can show, and rolling
     * over at 1000 keeps every cell at most five glyphs. The divisor
     * stays 1024 because that is what a K is here. */
    while (whole >= 1000u && unit < 3) {
        frac = ((whole % 1024u) * 10u) / 1024u;
        whole /= 1024u;
        unit++;
    }
    int n = digits_into(whole, out);
    /* One decimal only below ten, where it is the difference between
     * "1K" and "1.9K" - a 90% error. At 10 and above the same digit is
     * worth under 10% and costs a character the column would rather
     * spend on the name. */
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
        /* A separator goes before every digit whose distance from the
         * end is a multiple of three, except the first digit - which is
         * what stops "100" becoming ",100". */
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
        return 0; /* "." and ".." name a directory this window already has a row for */
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

/* ---- the two tree walks ---------------------------------------------
 *
 * One mutable path buffer, appended to on the way down and truncated on
 * the way back up, rather than a PATH_MAX_LEN copy per level: four
 * kilobytes times sixteen levels is a quarter of the stack a process
 * starts with (USER_STACK_PAGES, kernel/proc/proc.h), and this is a
 * program that is also holding 128 KiB of file list.
 *
 * File scope rather than passed through, because both walks want it and
 * neither is re-entrant in any case - nothing here has a second thread.
 */
static char walk_path[PATH_MAX_LEN];

/* Appends "/name" to walk_path, returning the length to restore
 * afterwards, or -1 if it would not fit - refused rather than
 * truncated, since a truncated path names a different file. */
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

/* Whether a dirent is one of the two entries a walk must never follow.
 * leanfs does not store them (see leanfs.c's resolve(), which refuses
 * ".." in a path outright), so this guards against a filesystem that
 * one day does rather than against one that does - which is exactly the
 * kind of check that costs one line now and a recursive delete of a
 * parent directory later. */
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
        /* Eight-byte aligned on purpose: os_dirent_t's reclen is a
         * multiple of 8 so that every record after the first is
         * aligned, which only holds if the buffer itself is - and a
         * bare char array is aligned by luck, not by the language. */
        union { char bytes[FSUTIL_DIRENT_BUF]; uint64_t align; } buf;
        long n = sys_getdents(walk_path, &cookie, buf.bytes, sizeof(buf.bytes));
        if (n <= 0) {
            /* 0 is the end of the directory and -1 is a directory that
             * could not be read. Both stop this level; only the first
             * call's -1 is reported to the caller, and that is done by
             * fsutil_count_tree rather than here. */
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
                continue; /* a path this deep cannot be named, so it cannot be counted */
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
    /* Re-read from the start of the directory on every pass rather than
     * carrying a cookie across the deletes. A cookie is a position in a
     * directory that is being emptied underneath it, and what a stale
     * one skips is a file that then keeps its parent alive - so the
     * rmdir at the end fails and the whole delete reports failure with
     * most of the tree gone. Each pass removes everything one buffer
     * holds, so a directory of any size finishes in as many passes as it
     * has batches. */
    for (;;) {
        unsigned int cookie = 0;
        /* Eight-byte aligned on purpose: os_dirent_t's reclen is a
         * multiple of 8 so that every record after the first is
         * aligned, which only holds if the buffer itself is - and a
         * bare char array is aligned by luck, not by the language. */
        union { char bytes[FSUTIL_DIRENT_BUF]; uint64_t align; } buf;
        long n = sys_getdents(walk_path, &cookie, buf.bytes, sizeof(buf.bytes));
        if (n <= 0) {
            break; /* empty, or unreadable - the rmdir below decides which */
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
            /* A pass that read entries and removed none would loop
             * forever. Nothing known produces it - every entry is
             * either a dot entry, a file or a directory - but a walk
             * whose termination depends on that staying true is one bad
             * dirent away from hanging the file manager. */
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
