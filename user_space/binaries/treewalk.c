#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_DEPTH 8
#define PATHBUF   256

static unsigned long total_bytes;
static unsigned long file_count;
static unsigned long directory_count;

static void join(char *out, const char *directory, const char *name) {
    size_t n = strlen(directory);
    if (n >= PATHBUF - 2) {
        out[0] = '\0';
        return;
    }
    memcpy(out, directory, n);
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
            if (e->d_type != DT_DIR) {
                printf("!! %s is a directory but d_type said %d\n", child, (int)e->d_type);
            }
            directory_count++;
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
            total_bytes, file_count, directory_count);
    return 0;
}
