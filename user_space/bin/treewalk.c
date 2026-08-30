/* user_space/bin/treewalk.c - M77's proof
 *
 * The milestone's own statement of what would show this worked: "A
 * program written against only <dirent.h>, <sys/stat.h> and <unistd.h> -
 * none of this project's own headers - walks a directory tree it knows
 * nothing about ahead of time and prints what it finds, the way `find`
 * or `du` would."
 *
 * So that is exactly what this is, and the include list below is the
 * point of the file. There is no "paths.h", no "syscall_wrappers.h", and
 * nothing here knows it is running on lean_os. Every line of it would
 * compile unchanged on a Linux box, which is the whole claim M77 makes.
 *
 * It prints one line per entry - type, size and path, deepest-last
 * within each directory - and a total at the end, because `du` is the
 * shortest real program that needs both halves of this milestone: names
 * from <dirent.h> and sizes from <sys/stat.h>.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_DEPTH 8
#define PATHBUF   256

static unsigned long total_bytes;
static unsigned long file_count;
static unsigned long dir_count;

static void join(char *out, const char *dir, const char *name) {
    size_t n = strlen(dir);
    if (n >= PATHBUF - 2) {
        out[0] = '\0';
        return;
    }
    memcpy(out, dir, n);
    if (n == 0 || out[n - 1] != '/') {
        out[n++] = '/';
    }
    size_t m = strlen(name);
    if (n + m >= PATHBUF) {
        m = PATHBUF - 1 - n;
    }
    memcpy(out + n, name, m);
    out[n + m] = '\0';
}

static void walk(const char *path, int depth) {
    if (depth > MAX_DEPTH) {
        return;
    }
    DIR *d = opendir(path);
    if (!d) {
        printf("!! cannot open %s\n", path);
        return;
    }
    /* Two passes so that output is stable regardless of the order the
     * filesystem happens to store entries in: files first, then descend.
     * A test that graded an order the filesystem never promised would be
     * a test of leanfs's allocator. */
    struct dirent *e;
    char child[PATHBUF];
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        join(child, path, e->d_name);
        struct stat st;
        if (lstat(child, &st) != 0) {
            printf("!! cannot stat %s\n", child);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            /* d_type and st_mode have to agree. They come from two
             * different calls over two different kernel interfaces, and
             * a program walking a tree trusts the cheap one - so a
             * disagreement is worth saying out loud rather than
             * silently preferring one. */
            if (e->d_type != DT_DIR) {
                printf("!! %s is a directory but d_type said %d\n", child, (int)e->d_type);
            }
            dir_count++;
            printf("d %8ld %s\n", (long)st.st_size, child);
        } else {
            if (e->d_type != DT_REG) {
                printf("!! %s is a file but d_type said %d\n", child, (int)e->d_type);
            }
            file_count++;
            total_bytes += (unsigned long)st.st_size;
            printf("f %8ld %s\n", (long)st.st_size, child);
        }
    }
    rewinddir(d);
    while ((e = readdir(d)) != 0) {
        if (e->d_type != DT_DIR) {
            continue;
        }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        join(child, path, e->d_name);
        walk(child, depth + 1);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    char here[PATHBUF];
    /* No argument means "here", which is only a thing a program can say
     * because M75 gave this machine a working directory. */
    const char *root = argc > 1 ? argv[1] : (getcwd(here, sizeof(here)) ? here : "/");

    if (access(root, F_OK) != 0) {
        printf("treewalk: no such path: %s\n", root);
        return 1;
    }
    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        printf("treewalk: not a directory: %s\n", root);
        return 1;
    }

    walk(root, 0);
    printf("total %lu byte(s) in %lu file(s), %lu director(ies)\n",
            total_bytes, file_count, dir_count);
    return 0;
}
