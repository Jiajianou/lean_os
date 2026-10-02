#include "file_browser.h"

#include <string.h>

#include "file_system_utilities.h"
#include "syscall_wrappers.h"

#define FILE_BROWSER_COPY_CHUNK 16384
#define FILE_BROWSER_DIRENT_BUFFER 2048
#define FILE_BROWSER_SNIFF_BYTES 64
#define FILE_BROWSER_NAME_ATTEMPTS 999

const char *file_browser_error_message(int error) {
    switch (error) {
    case FILE_BROWSER_OK:
        return "Done.";
    case FILE_BROWSER_ERROR_TOO_LONG:
        return "That path is too long.";
    case FILE_BROWSER_ERROR_INTO_ITSELF:
        return "A folder cannot go inside itself.";
    case FILE_BROWSER_ERROR_EXISTS:
        return "Something with that name is already there.";
    case FILE_BROWSER_ERROR_READ:
        return "Could not read it.";
    case FILE_BROWSER_ERROR_WRITE:
        return "Could not write it - the disk may be full.";
    case FILE_BROWSER_ERROR_NOT_FOUND:
        return "It is not there any more.";
    case FILE_BROWSER_ERROR_IN_TRASH:
        return "It is already in the Trash.";
    case FILE_BROWSER_ERROR_NO_ORIGIN:
        return "The Trash does not know where that came from.";
    case FILE_BROWSER_ERROR_ORIGIN_GONE:
        return "The folder it came from is gone.";
    case FILE_BROWSER_ERROR_TOO_DEEP:
        return "Those folders are nested too deeply.";
    case FILE_BROWSER_ERROR_RENAME:
        return "Could not move it.";
    case FILE_BROWSER_ERROR_DELETE:
        return "Could not delete all of it.";
    case FILE_BROWSER_ERROR_NAME:
        return "That name cannot be used.";
    case FILE_BROWSER_ERROR_SAME:
        return "It is already there.";
    default:
        return "Something went wrong.";
    }
}

int file_browser_join(const char *directory, const char *name, char *out, size_t capacity) {
    size_t n = 0;
    for (const char *s = directory; *s; s++) {
        if (n + 1 >= capacity) {
            return FILE_BROWSER_ERROR_TOO_LONG;
        }
        out[n++] = *s;
    }
    if (n == 0 || out[n - 1] != '/') {
        if (n + 1 >= capacity) {
            return FILE_BROWSER_ERROR_TOO_LONG;
        }
        out[n++] = '/';
    }
    for (const char *s = name; *s; s++) {
        if (n + 1 >= capacity) {
            return FILE_BROWSER_ERROR_TOO_LONG;
        }
        out[n++] = *s;
    }
    out[n] = '\0';
    return FILE_BROWSER_OK;
}

const char *file_browser_basename(const char *path) {
    const char *base = path;
    for (const char *s = path; *s; s++) {
        if (*s == '/' && s[1]) {
            base = s + 1;
        }
    }
    return base;
}

int file_browser_parent(const char *path, char *out, size_t capacity) {
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') {
        length--;
    }
    while (length > 0 && path[length - 1] != '/') {
        length--;
    }
    while (length > 1 && path[length - 1] == '/') {
        length--;
    }
    if (length == 0) {
        length = 1;
    }
    if (length + 1 > capacity) {
        return FILE_BROWSER_ERROR_TOO_LONG;
    }
    memcpy(out, path, length);
    out[length] = '\0';
    if (out[0] != '/') {
        out[0] = '/';
        out[1] = '\0';
    }
    return FILE_BROWSER_OK;
}

int file_browser_is_inside(const char *path, const char *ancestor) {
    size_t n = strlen(ancestor);
    while (n > 1 && ancestor[n - 1] == '/') {
        n--;
    }
    if (n == 1 && ancestor[0] == '/') {
        return path[0] == '/';
    }
    if (strncmp(path, ancestor, n) != 0) {
        return 0;
    }
    return path[n] == '\0' || path[n] == '/';
}

static int lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int is_digit(int c) {
    return c >= '0' && c <= '9';
}

int file_browser_compare_names(const char *a, const char *b) {
    while (*a && *b) {
        if (is_digit((unsigned char)*a) && is_digit((unsigned char)*b)) {
            while (*a == '0' && is_digit((unsigned char)a[1])) {
                a++;
            }
            while (*b == '0' && is_digit((unsigned char)b[1])) {
                b++;
            }
            size_t run_a = 0;
            size_t run_b = 0;
            while (is_digit((unsigned char)a[run_a])) {
                run_a++;
            }
            while (is_digit((unsigned char)b[run_b])) {
                run_b++;
            }
            if (run_a != run_b) {
                return run_a < run_b ? -1 : 1;
            }
            for (size_t i = 0; i < run_a; i++) {
                if (a[i] != b[i]) {
                    return a[i] < b[i] ? -1 : 1;
                }
            }
            a += run_a;
            b += run_b;
            continue;
        }
        int ca = lower((unsigned char)*a);
        int cb = lower((unsigned char)*b);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        a++;
        b++;
    }
    if (*a || *b) {
        return *a ? 1 : -1;
    }
    return 0;
}

int file_browser_name_matches(const char *name, const char *query) {
    if (!query[0]) {
        return 0;
    }
    for (const char *start = name; *start; start++) {
        const char *n = start;
        const char *q = query;
        while (*n && *q && lower((unsigned char)*n) == lower((unsigned char)*q)) {
            n++;
            q++;
        }
        if (!*q) {
            return 1;
        }
    }
    return 0;
}

int file_browser_looks_like_text(const unsigned char *bytes, size_t length) {
    size_t control = 0;
    size_t i = 0;
    while (i < length) {
        unsigned char c = bytes[i];
        if (c == 0) {
            return 0;
        }
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != 0x1B) {
            control++;
            i++;
            continue;
        }
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t follow = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
        if (follow == 0) {
            return 0;
        }
        for (size_t k = 1; k <= follow; k++) {
            if (i + k >= length) {
                return 1;
            }
            if ((bytes[i + k] & 0xC0) != 0x80) {
                return 0;
            }
        }
        i += follow + 1;
    }
    return control * 50 <= length;
}

typedef struct {
    const char *extension;
    const char *kind_name;
    uint8_t kind;
} file_browser_extension_t;

static const file_browser_extension_t EXTENSIONS[] = {
    {"txt", "Plain Text", FILE_BROWSER_KIND_TEXT},
    {"md", "Markdown", FILE_BROWSER_KIND_TEXT},
    {"conf", "Configuration", FILE_BROWSER_KIND_TEXT},
    {"cfg", "Configuration", FILE_BROWSER_KIND_TEXT},
    {"ini", "Configuration", FILE_BROWSER_KIND_TEXT},
    {"log", "Log", FILE_BROWSER_KIND_TEXT},
    {"csv", "Spreadsheet Text", FILE_BROWSER_KIND_TEXT},
    {"c", "C Source", FILE_BROWSER_KIND_SOURCE},
    {"h", "C Header", FILE_BROWSER_KIND_SOURCE},
    {"cc", "C++ Source", FILE_BROWSER_KIND_SOURCE},
    {"cpp", "C++ Source", FILE_BROWSER_KIND_SOURCE},
    {"hpp", "C++ Header", FILE_BROWSER_KIND_SOURCE},
    {"asm", "Assembly", FILE_BROWSER_KIND_SOURCE},
    {"s", "Assembly", FILE_BROWSER_KIND_SOURCE},
    {"py", "Python Script", FILE_BROWSER_KIND_SOURCE},
    {"sh", "Shell Script", FILE_BROWSER_KIND_SOURCE},
    {"rs", "Rust Source", FILE_BROWSER_KIND_SOURCE},
    {"js", "JavaScript", FILE_BROWSER_KIND_SOURCE},
    {"json", "JSON", FILE_BROWSER_KIND_SOURCE},
    {"css", "Stylesheet", FILE_BROWSER_KIND_SOURCE},
    {"xml", "XML", FILE_BROWSER_KIND_SOURCE},
    {"mk", "Makefile", FILE_BROWSER_KIND_SOURCE},
    {"html", "Web Page", FILE_BROWSER_KIND_DOCUMENT},
    {"htm", "Web Page", FILE_BROWSER_KIND_DOCUMENT},
    {"pdf", "PDF Document", FILE_BROWSER_KIND_DOCUMENT},
    {"png", "PNG Image", FILE_BROWSER_KIND_IMAGE},
    {"bmp", "BMP Image", FILE_BROWSER_KIND_IMAGE},
    {"jpg", "JPEG Image", FILE_BROWSER_KIND_IMAGE},
    {"jpeg", "JPEG Image", FILE_BROWSER_KIND_IMAGE},
    {"gif", "GIF Image", FILE_BROWSER_KIND_IMAGE},
    {"icon", "Icon", FILE_BROWSER_KIND_IMAGE},
    {"tar", "Tar Archive", FILE_BROWSER_KIND_ARCHIVE},
    {"gz", "Gzip Archive", FILE_BROWSER_KIND_ARCHIVE},
    {"tgz", "Gzip Archive", FILE_BROWSER_KIND_ARCHIVE},
    {"bz2", "Bzip2 Archive", FILE_BROWSER_KIND_ARCHIVE},
    {"zip", "Zip Archive", FILE_BROWSER_KIND_ARCHIVE},
    {"osp", "Package", FILE_BROWSER_KIND_ARCHIVE},
    {"ttf", "Font", FILE_BROWSER_KIND_FONT},
    {"otf", "Font", FILE_BROWSER_KIND_FONT},
    {"a", "Static Library", FILE_BROWSER_KIND_DATA},
    {"o", "Object File", FILE_BROWSER_KIND_DATA},
    {"so", "Shared Library", FILE_BROWSER_KIND_PROGRAM},
};

#define EXTENSION_COUNT ((int)(sizeof(EXTENSIONS) / sizeof(EXTENSIONS[0])))

static const char *extension_of(const char *name) {
    const char *dot = 0;
    for (const char *s = name; *s; s++) {
        if (*s == '.') {
            dot = s;
        }
    }
    if (!dot || dot == name || !dot[1]) {
        return 0;
    }
    return dot + 1;
}

static int same_ignoring_case(const char *a, const char *b) {
    while (*a && *b) {
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int has_magic(const unsigned char *head, size_t length, const char *magic, size_t magic_length) {
    return length >= magic_length && memcmp(head, magic, magic_length) == 0;
}

file_browser_kind_t file_browser_classify(const char *name, int is_directory, int is_link,
                                          const unsigned char *head, size_t head_length,
                                          const char **kind_name) {
    const char *unused;
    if (!kind_name) {
        kind_name = &unused;
    }
    if (is_directory) {
        *kind_name = is_link ? "Folder Alias" : "Folder";
        return FILE_BROWSER_KIND_FOLDER;
    }
    if (head && has_magic(head, head_length, "\x7f" "ELF", 4)) {
        *kind_name = "Program";
        return FILE_BROWSER_KIND_PROGRAM;
    }
    if (head && has_magic(head, head_length, "\x89PNG\r\n\x1a\n", 8)) {
        *kind_name = "PNG Image";
        return FILE_BROWSER_KIND_IMAGE;
    }
    const char *extension = extension_of(name);
    if (extension) {
        for (int i = 0; i < EXTENSION_COUNT; i++) {
            if (same_ignoring_case(extension, EXTENSIONS[i].extension)) {
                *kind_name = EXTENSIONS[i].kind_name;
                return (file_browser_kind_t)EXTENSIONS[i].kind;
            }
        }
    }
    if (is_link) {
        *kind_name = "Alias";
        return FILE_BROWSER_KIND_LINK;
    }
    if (head && has_magic(head, head_length, "#!", 2)) {
        *kind_name = "Script";
        return FILE_BROWSER_KIND_SOURCE;
    }
    if (head && head_length == 0) {
        *kind_name = "Empty File";
        return FILE_BROWSER_KIND_TEXT;
    }
    if (head && file_browser_looks_like_text(head, head_length)) {
        *kind_name = "Plain Text";
        return FILE_BROWSER_KIND_TEXT;
    }
    *kind_name = head ? "Data" : "Document";
    return FILE_BROWSER_KIND_DATA;
}

file_browser_preview_t file_browser_preview_kind(const unsigned char *head, size_t head_length) {
    if (has_magic(head, head_length, "\x89PNG\r\n\x1a\n", 8)) {
        return FILE_BROWSER_PREVIEW_IMAGE;
    }
    if (head_length >= 26 && head[0] == 'B' && head[1] == 'M') {
        return FILE_BROWSER_PREVIEW_IMAGE;
    }
    if (file_browser_looks_like_text(head, head_length)) {
        return FILE_BROWSER_PREVIEW_TEXT;
    }
    return FILE_BROWSER_PREVIEW_BYTES;
}

int file_browser_exists(const char *path) {
    os_stat_t status;
    return sys_lstat(path, &status) == 0;
}

static size_t stem_length_of(const char *name) {
    const char *extension = extension_of(name);
    return extension ? (size_t)(extension - 1 - name) : strlen(name);
}

static int compose_name(char *out, size_t capacity, const char *name, const char *suffix,
                        int number) {
    char digits[12];
    int digit_count = 0;
    for (int v = number; number >= 2 && v > 0; v /= 10) {
        digits[digit_count++] = (char)('0' + v % 10);
    }
    size_t stem_length = stem_length_of(name);
    size_t total = strlen(name);
    size_t needed = total + strlen(suffix) + (size_t)digit_count + 2;
    if (needed > capacity || needed > FILE_BROWSER_NAME_MAX) {
        return FILE_BROWSER_ERROR_TOO_LONG;
    }
    size_t n = stem_length;
    memcpy(out, name, stem_length);
    for (const char *s = suffix; *s; s++) {
        out[n++] = *s;
    }
    if (digit_count > 0) {
        out[n++] = ' ';
        while (digit_count > 0) {
            out[n++] = digits[--digit_count];
        }
    }
    memcpy(out + n, name + stem_length, total - stem_length);
    n += total - stem_length;
    out[n] = '\0';
    return FILE_BROWSER_OK;
}

static int first_free(const char *directory, const char *name, const char *suffix, char *out,
                      size_t capacity) {
    char candidate[FILE_BROWSER_NAME_MAX];
    char full[PATH_MAX_LENGTH];
    for (int number = 1; number <= FILE_BROWSER_NAME_ATTEMPTS; number++) {
        int rc = compose_name(candidate, sizeof(candidate), name, suffix, number);
        if (rc != FILE_BROWSER_OK) {
            return rc;
        }
        if (file_browser_join(directory, candidate, full, sizeof(full)) != FILE_BROWSER_OK) {
            return FILE_BROWSER_ERROR_TOO_LONG;
        }
        if (!file_browser_exists(full)) {
            size_t length = strlen(candidate) + 1;
            if (length > capacity) {
                return FILE_BROWSER_ERROR_TOO_LONG;
            }
            memcpy(out, candidate, length);
            return FILE_BROWSER_OK;
        }
    }
    return FILE_BROWSER_ERROR_EXISTS;
}

int file_browser_available_name(const char *directory, const char *wanted, char *out,
                                size_t capacity) {
    return first_free(directory, wanted, "", out, capacity);
}

int file_browser_copy_name(const char *directory, const char *source_name, char *out,
                           size_t capacity) {
    char full[PATH_MAX_LENGTH];
    if (file_browser_join(directory, source_name, full, sizeof(full)) != FILE_BROWSER_OK) {
        return FILE_BROWSER_ERROR_TOO_LONG;
    }
    if (!file_browser_exists(full)) {
        size_t length = strlen(source_name) + 1;
        if (length > capacity) {
            return FILE_BROWSER_ERROR_TOO_LONG;
        }
        memcpy(out, source_name, length);
        return FILE_BROWSER_OK;
    }
    return first_free(directory, source_name, " copy", out, capacity);
}

static int is_dot_entry(const char *name) {
    return name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]));
}

static long sniff(const char *path, unsigned char *head, size_t capacity) {
    long fd = sys_open(path, OPEN_READ);
    if (fd < 0) {
        return -1;
    }
    long n = sys_read((int)fd, head, capacity);
    sys_close((int)fd);
    return n;
}

int file_browser_needs_sniff(const file_browser_entry_t *entry) {
    return !entry->is_directory && !extension_of(entry->name);
}

void file_browser_sniff_entry(const char *path, file_browser_entry_t *entry) {
    unsigned char head[FILE_BROWSER_SNIFF_BYTES];
    long got = file_browser_needs_sniff(entry) ? sniff(path, head, sizeof(head)) : -1;
    entry->kind = (uint8_t)file_browser_classify(entry->name, entry->is_directory, entry->is_link,
                                                 got >= 0 ? head : 0, got >= 0 ? (size_t)got : 0,
                                                 &entry->kind_name);
}

int file_browser_list(const char *directory, int flags, file_browser_entry_t *out, int maximum) {
    int include_hidden = (flags & FILE_BROWSER_LIST_HIDDEN) != 0;
    unsigned int cookie = 0;
    int count = 0;
    char full[PATH_MAX_LENGTH];
    for (;;) {
        union { char bytes[FILE_BROWSER_DIRENT_BUFFER]; uint64_t align; } buffer;
        long n = sys_getdents(directory, &cookie, buffer.bytes, sizeof(buffer.bytes));
        if (n < 0) {
            return count > 0 ? count : -1;
        }
        if (n == 0) {
            return count;
        }
        for (long offset = 0; offset + 8 <= n;) {
            const os_dirent_t *d = (const os_dirent_t *)(void *)(buffer.bytes + offset);
            if (d->reclen == 0 || offset + d->reclen > n) {
                break;
            }
            offset += d->reclen;
            if (is_dot_entry(d->name) || (!include_hidden && d->name[0] == '.')) {
                continue;
            }
            if (count >= maximum) {
                return count;
            }
            file_browser_entry_t *e = &out[count];
            size_t length = strlen(d->name);
            if (length >= sizeof(e->name)) {
                continue;
            }
            memcpy(e->name, d->name, length + 1);
            e->size = 0;
            e->mtime = 0;
            e->is_link = d->type == OS_DT_LNK;
            e->is_directory = d->type == OS_DT_DIRECTORY;
            os_stat_t status;
            if (file_browser_join(directory, d->name, full, sizeof(full)) == FILE_BROWSER_OK &&
                sys_stat(full, &status) == 0) {
                e->size = status.size;
                e->mtime = status.mtime;
                e->is_directory = status.is_directory;
            }
            if (flags & FILE_BROWSER_LIST_SNIFF) {
                file_browser_sniff_entry(full, e);
            } else {
                e->kind = (uint8_t)file_browser_classify(e->name, e->is_directory, e->is_link, 0, 0,
                                                         &e->kind_name);
            }
            count++;
        }
    }
}

static int sort_compare(const file_browser_entry_t *a, const file_browser_entry_t *b,
                        file_browser_sort_t key, int descending) {
    if (a->is_directory != b->is_directory) {
        return a->is_directory ? -1 : 1;
    }
    long difference = 0;
    if (key == FILE_BROWSER_SORT_SIZE) {
        difference = a->size == b->size ? 0 : (a->size < b->size ? -1 : 1);
    } else if (key == FILE_BROWSER_SORT_DATE) {
        difference = a->mtime == b->mtime ? 0 : (a->mtime < b->mtime ? -1 : 1);
    } else if (key == FILE_BROWSER_SORT_KIND && a->kind_name && b->kind_name) {
        difference = file_browser_compare_names(a->kind_name, b->kind_name);
    }
    if (difference == 0) {
        difference = file_browser_compare_names(a->name, b->name);
    }
    if (difference == 0) {
        difference = strcmp(a->name, b->name);
    }
    return (int)(descending ? -difference : difference);
}

void file_browser_sort(const file_browser_entry_t *entries, int *order, int count,
                       file_browser_sort_t key, int descending) {
    for (int i = 0; i < count; i++) {
        order[i] = i;
    }
    for (int gap = count / 2; gap > 0; gap = gap == 2 ? 1 : gap * 5 / 11) {
        for (int i = gap; i < count; i++) {
            int moving = order[i];
            int j = i;
            while (j >= gap &&
                   sort_compare(&entries[order[j - gap]], &entries[moving], key, descending) > 0) {
                order[j] = order[j - gap];
                j -= gap;
            }
            order[j] = moving;
        }
    }
}

static int copy_file_contents(const char *from, const char *to) {
    long source = sys_open(from, OPEN_READ);
    if (source < 0) {
        return FILE_BROWSER_ERROR_READ;
    }
    long destination = sys_open(to, OPEN_WRITE | OPEN_CREATE | OPEN_EXCL);
    if (destination < 0) {
        sys_close((int)source);
        return file_browser_exists(to) ? FILE_BROWSER_ERROR_EXISTS : FILE_BROWSER_ERROR_WRITE;
    }
    static unsigned char chunk[FILE_BROWSER_COPY_CHUNK];
    int result = FILE_BROWSER_OK;
    for (;;) {
        long n = sys_read((int)source, chunk, sizeof(chunk));
        if (n < 0) {
            result = FILE_BROWSER_ERROR_READ;
            break;
        }
        if (n == 0) {
            break;
        }
        if (sys_write((int)destination, chunk, (size_t)n) != n) {
            result = FILE_BROWSER_ERROR_WRITE;
            break;
        }
    }
    sys_close((int)source);
    sys_close((int)destination);
    if (result != FILE_BROWSER_OK) {
        sys_unlink(to);
    }
    return result;
}

static char copy_from_path[PATH_MAX_LENGTH];
static char copy_to_path[PATH_MAX_LENGTH];

static int path_push(char *path, size_t length, const char *name) {
    size_t n = length;
    if (n > 0 && path[n - 1] != '/') {
        if (n + 1 >= PATH_MAX_LENGTH) {
            return -1;
        }
        path[n++] = '/';
    }
    for (const char *s = name; *s; s++) {
        if (n + 1 >= PATH_MAX_LENGTH) {
            return -1;
        }
        path[n++] = *s;
    }
    path[n] = '\0';
    return (int)n;
}

static int copy_entry(size_t from_length, size_t to_length, int depth);

static int copy_directory(size_t from_length, size_t to_length, int depth) {
    if (depth >= FILE_BROWSER_DEPTH_MAX) {
        return FILE_BROWSER_ERROR_TOO_DEEP;
    }
    if (sys_mkdir(copy_to_path) != 0) {
        return file_browser_exists(copy_to_path) ? FILE_BROWSER_ERROR_EXISTS : FILE_BROWSER_ERROR_WRITE;
    }
    unsigned int cookie = 0;
    for (;;) {
        union { char bytes[FILE_BROWSER_DIRENT_BUFFER]; uint64_t align; } buffer;
        long n = sys_getdents(copy_from_path, &cookie, buffer.bytes, sizeof(buffer.bytes));
        if (n < 0) {
            return FILE_BROWSER_ERROR_READ;
        }
        if (n == 0) {
            return FILE_BROWSER_OK;
        }
        for (long offset = 0; offset + 8 <= n;) {
            const os_dirent_t *d = (const os_dirent_t *)(void *)(buffer.bytes + offset);
            if (d->reclen == 0 || offset + d->reclen > n) {
                break;
            }
            offset += d->reclen;
            if (is_dot_entry(d->name)) {
                continue;
            }
            int from_pushed = path_push(copy_from_path, from_length, d->name);
            int to_pushed = path_push(copy_to_path, to_length, d->name);
            int rc = (from_pushed < 0 || to_pushed < 0)
                         ? FILE_BROWSER_ERROR_TOO_LONG
                         : copy_entry((size_t)from_pushed, (size_t)to_pushed, depth + 1);
            copy_from_path[from_length] = '\0';
            copy_to_path[to_length] = '\0';
            if (rc != FILE_BROWSER_OK) {
                return rc;
            }
        }
    }
}

static int copy_entry(size_t from_length, size_t to_length, int depth) {
    os_stat_t status;
    if (sys_lstat(copy_from_path, &status) != 0) {
        return FILE_BROWSER_ERROR_NOT_FOUND;
    }
    if (status.is_link) {
        char target[PATH_MAX_LENGTH];
        long n = sys_readlink(copy_from_path, target, sizeof(target) - 1);
        if (n < 0) {
            return FILE_BROWSER_ERROR_READ;
        }
        target[n] = '\0';
        return sys_symlink(target, copy_to_path) == 0 ? FILE_BROWSER_OK : FILE_BROWSER_ERROR_WRITE;
    }
    if (status.is_directory) {
        return copy_directory(from_length, to_length, depth);
    }
    return copy_file_contents(copy_from_path, copy_to_path);
}

int file_browser_copy(const char *from, const char *to) {
    if (file_browser_is_inside(to, from)) {
        return FILE_BROWSER_ERROR_INTO_ITSELF;
    }
    if (file_browser_exists(to)) {
        return FILE_BROWSER_ERROR_EXISTS;
    }
    size_t from_length = strlen(from);
    size_t to_length = strlen(to);
    if (from_length >= PATH_MAX_LENGTH || to_length >= PATH_MAX_LENGTH) {
        return FILE_BROWSER_ERROR_TOO_LONG;
    }
    memcpy(copy_from_path, from, from_length + 1);
    memcpy(copy_to_path, to, to_length + 1);
    int rc = copy_entry(from_length, to_length, 0);
    if (rc != FILE_BROWSER_OK && rc != FILE_BROWSER_ERROR_EXISTS) {
        file_browser_delete(to);
    }
    return rc;
}

int file_browser_copy_into(const char *from, const char *directory, char *made, size_t capacity) {
    if (file_browser_is_inside(directory, from)) {
        return FILE_BROWSER_ERROR_INTO_ITSELF;
    }
    char name[FILE_BROWSER_NAME_MAX];
    int rc = file_browser_copy_name(directory, file_browser_basename(from), name, sizeof(name));
    if (rc != FILE_BROWSER_OK) {
        return rc;
    }
    char to[PATH_MAX_LENGTH];
    if (file_browser_join(directory, name, to, sizeof(to)) != FILE_BROWSER_OK) {
        return FILE_BROWSER_ERROR_TOO_LONG;
    }
    rc = file_browser_copy(from, to);
    if (rc == FILE_BROWSER_OK && made) {
        if (strlen(to) + 1 > capacity) {
            return FILE_BROWSER_ERROR_TOO_LONG;
        }
        memcpy(made, to, strlen(to) + 1);
    }
    return rc;
}

int file_browser_move_into(const char *from, const char *directory, char *moved, size_t capacity) {
    char parent[PATH_MAX_LENGTH];
    if (file_browser_parent(from, parent, sizeof(parent)) != FILE_BROWSER_OK) {
        return FILE_BROWSER_ERROR_TOO_LONG;
    }
    if (strcmp(parent, directory) == 0) {
        return FILE_BROWSER_ERROR_SAME;
    }
    if (file_browser_is_inside(directory, from)) {
        return FILE_BROWSER_ERROR_INTO_ITSELF;
    }
    char to[PATH_MAX_LENGTH];
    if (file_browser_join(directory, file_browser_basename(from), to, sizeof(to)) != FILE_BROWSER_OK) {
        return FILE_BROWSER_ERROR_TOO_LONG;
    }
    if (file_browser_exists(to)) {
        return FILE_BROWSER_ERROR_EXISTS;
    }
    if (!file_browser_exists(from)) {
        return FILE_BROWSER_ERROR_NOT_FOUND;
    }
    if (sys_rename(from, to) != 0) {
        return FILE_BROWSER_ERROR_RENAME;
    }
    if (moved) {
        if (strlen(to) + 1 > capacity) {
            return FILE_BROWSER_ERROR_TOO_LONG;
        }
        memcpy(moved, to, strlen(to) + 1);
    }
    return FILE_BROWSER_OK;
}

static int origin_record_path(const char *trashed_name, char *out, size_t capacity) {
    return file_browser_join(FILE_BROWSER_TRASH_ORIGINS, trashed_name, out, capacity);
}

static int in_trash(const char *path) {
    char parent[PATH_MAX_LENGTH];
    return file_browser_parent(path, parent, sizeof(parent)) == FILE_BROWSER_OK &&
           strcmp(parent, FILE_BROWSER_TRASH) == 0;
}

int file_browser_delete(const char *path) {
    os_stat_t status;
    if (sys_lstat(path, &status) != 0) {
        return FILE_BROWSER_ERROR_NOT_FOUND;
    }
    int ok = (status.is_directory && !status.is_link)
                 ? file_system_utilities_remove_tree(path) == 0
                 : sys_unlink(path) == 0;
    if (!ok) {
        return FILE_BROWSER_ERROR_DELETE;
    }
    if (in_trash(path)) {
        char record[PATH_MAX_LENGTH];
        if (origin_record_path(file_browser_basename(path), record, sizeof(record)) == FILE_BROWSER_OK) {
            sys_unlink(record);
        }
    }
    return FILE_BROWSER_OK;
}

int file_browser_trash(const char *path, char *trashed, size_t capacity) {
    if (file_browser_is_inside(path, FILE_BROWSER_TRASH)) {
        return FILE_BROWSER_ERROR_IN_TRASH;
    }
    if (file_browser_is_inside(FILE_BROWSER_TRASH, path)) {
        return FILE_BROWSER_ERROR_INTO_ITSELF;
    }
    if (!file_browser_exists(path)) {
        return FILE_BROWSER_ERROR_NOT_FOUND;
    }
    sys_mkdir(FILE_BROWSER_TRASH);
    sys_mkdir(FILE_BROWSER_TRASH_ORIGINS);
    char name[FILE_BROWSER_NAME_MAX];
    const char *wanted = file_browser_basename(path);
    if (strcmp(wanted, FILE_BROWSER_ORIGINS_NAME) == 0) {
        wanted = "origins";
    }
    int rc = file_browser_available_name(FILE_BROWSER_TRASH, wanted, name, sizeof(name));
    if (rc != FILE_BROWSER_OK) {
        return rc;
    }
    char destination[PATH_MAX_LENGTH];
    char record[PATH_MAX_LENGTH];
    if (file_browser_join(FILE_BROWSER_TRASH, name, destination, sizeof(destination)) != FILE_BROWSER_OK ||
        origin_record_path(name, record, sizeof(record)) != FILE_BROWSER_OK) {
        return FILE_BROWSER_ERROR_TOO_LONG;
    }
    sys_unlink(record);
    long fd = sys_open(record, OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE);
    if (fd < 0) {
        return FILE_BROWSER_ERROR_WRITE;
    }
    long length = (long)strlen(path);
    long wrote = sys_write((int)fd, path, (size_t)length);
    sys_close((int)fd);
    if (wrote != length) {
        sys_unlink(record);
        return FILE_BROWSER_ERROR_WRITE;
    }
    if (sys_rename(path, destination) != 0) {
        sys_unlink(record);
        return FILE_BROWSER_ERROR_RENAME;
    }
    if (trashed) {
        if (strlen(destination) + 1 > capacity) {
            return FILE_BROWSER_ERROR_TOO_LONG;
        }
        memcpy(trashed, destination, strlen(destination) + 1);
    }
    return FILE_BROWSER_OK;
}

int file_browser_trash_origin(const char *trashed_path, char *out, size_t capacity) {
    char record[PATH_MAX_LENGTH];
    if (!in_trash(trashed_path) ||
        origin_record_path(file_browser_basename(trashed_path), record, sizeof(record)) != FILE_BROWSER_OK) {
        return FILE_BROWSER_ERROR_NO_ORIGIN;
    }
    long fd = sys_open(record, OPEN_READ);
    if (fd < 0) {
        return FILE_BROWSER_ERROR_NO_ORIGIN;
    }
    long n = sys_read((int)fd, out, capacity - 1);
    sys_close((int)fd);
    if (n <= 0 || out[0] != '/') {
        return FILE_BROWSER_ERROR_NO_ORIGIN;
    }
    out[n] = '\0';
    return FILE_BROWSER_OK;
}

int file_browser_put_back(const char *trashed_path, char *restored, size_t capacity) {
    char origin[PATH_MAX_LENGTH];
    int rc = file_browser_trash_origin(trashed_path, origin, sizeof(origin));
    if (rc != FILE_BROWSER_OK) {
        return rc;
    }
    char parent[PATH_MAX_LENGTH];
    if (file_browser_parent(origin, parent, sizeof(parent)) != FILE_BROWSER_OK) {
        return FILE_BROWSER_ERROR_TOO_LONG;
    }
    os_stat_t status;
    if (sys_stat(parent, &status) != 0 || !status.is_directory) {
        return FILE_BROWSER_ERROR_ORIGIN_GONE;
    }
    char name[FILE_BROWSER_NAME_MAX];
    rc = file_browser_available_name(parent, file_browser_basename(origin), name, sizeof(name));
    if (rc != FILE_BROWSER_OK) {
        return rc;
    }
    char destination[PATH_MAX_LENGTH];
    if (file_browser_join(parent, name, destination, sizeof(destination)) != FILE_BROWSER_OK) {
        return FILE_BROWSER_ERROR_TOO_LONG;
    }
    if (sys_rename(trashed_path, destination) != 0) {
        return FILE_BROWSER_ERROR_RENAME;
    }
    char record[PATH_MAX_LENGTH];
    if (origin_record_path(file_browser_basename(trashed_path), record, sizeof(record)) == FILE_BROWSER_OK) {
        sys_unlink(record);
    }
    if (restored) {
        if (strlen(destination) + 1 > capacity) {
            return FILE_BROWSER_ERROR_TOO_LONG;
        }
        memcpy(restored, destination, strlen(destination) + 1);
    }
    return FILE_BROWSER_OK;
}

int file_browser_empty_trash(void) {
    int removed = 0;
    for (;;) {
        unsigned int cookie = 0;
        union { char bytes[FILE_BROWSER_DIRENT_BUFFER]; uint64_t align; } buffer;
        long n = sys_getdents(FILE_BROWSER_TRASH, &cookie, buffer.bytes, sizeof(buffer.bytes));
        if (n <= 0) {
            break;
        }
        int removed_this_pass = 0;
        for (long offset = 0; offset + 8 <= n;) {
            const os_dirent_t *d = (const os_dirent_t *)(void *)(buffer.bytes + offset);
            if (d->reclen == 0 || offset + d->reclen > n) {
                break;
            }
            offset += d->reclen;
            if (is_dot_entry(d->name) || strcmp(d->name, FILE_BROWSER_ORIGINS_NAME) == 0) {
                continue;
            }
            char full[PATH_MAX_LENGTH];
            if (file_browser_join(FILE_BROWSER_TRASH, d->name, full, sizeof(full)) != FILE_BROWSER_OK) {
                continue;
            }
            if (file_browser_delete(full) == FILE_BROWSER_OK) {
                removed++;
                removed_this_pass++;
            }
        }
        if (!removed_this_pass) {
            break;
        }
    }
    file_system_utilities_remove_tree(FILE_BROWSER_TRASH_ORIGINS);
    return removed;
}

typedef struct {
    const char *query;
    int include_hidden;
    long budget;
    long visited;
    file_browser_found_t found;
    void *context;
    int stopped;
} search_state_t;

static char search_path[PATH_MAX_LENGTH];

static void search_at(size_t length, int depth, search_state_t *state) {
    if (depth >= FILE_BROWSER_DEPTH_MAX) {
        return;
    }
    unsigned int cookie = 0;
    for (;;) {
        union { char bytes[FILE_BROWSER_DIRENT_BUFFER]; uint64_t align; } buffer;
        long n = sys_getdents(search_path, &cookie, buffer.bytes, sizeof(buffer.bytes));
        if (n <= 0) {
            return;
        }
        for (long offset = 0; offset + 8 <= n;) {
            const os_dirent_t *d = (const os_dirent_t *)(void *)(buffer.bytes + offset);
            if (d->reclen == 0 || offset + d->reclen > n) {
                break;
            }
            offset += d->reclen;
            if (is_dot_entry(d->name) || (!state->include_hidden && d->name[0] == '.')) {
                continue;
            }
            if (state->visited >= state->budget) {
                state->stopped = 1;
                return;
            }
            state->visited++;
            int pushed = path_push(search_path, length, d->name);
            if (pushed < 0) {
                continue;
            }
            if (file_browser_name_matches(d->name, state->query)) {
                os_stat_t status;
                if (sys_stat(search_path, &status) == 0 &&
                    state->found(state->context, search_path, &status) != 0) {
                    state->stopped = 1;
                }
            }
            if (!state->stopped && d->type == OS_DT_DIRECTORY &&
                strcmp(search_path, PATH_PROCESS) != 0 && strcmp(search_path, PATH_DEV) != 0) {
                search_at((size_t)pushed, depth + 1, state);
            }
            search_path[length] = '\0';
            if (state->stopped) {
                return;
            }
        }
    }
}

long file_browser_search(const char *root, const char *query, int include_hidden,
                         long visit_budget, file_browser_found_t found, void *context) {
    size_t length = strlen(root);
    if (length >= PATH_MAX_LENGTH || !query[0]) {
        return 0;
    }
    memcpy(search_path, root, length + 1);
    search_state_t state = {query, include_hidden, visit_budget, 0, found, context, 0};
    search_at(length, 0, &state);
    return state.stopped && state.visited >= visit_budget ? -state.visited : state.visited;
}
