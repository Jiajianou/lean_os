#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define FILE_BYTES  (48 * 1024)
#define FILES       12

static char buf[FILE_BYTES];
static char back[FILE_BYTES];

static unsigned char byte_for(int id, int file, int off) {
    return (unsigned char)(id * 71 + file * 13 + off * 7 + 29);
}

static void fill(int id, int file) {
    for (int i = 0; i < FILE_BYTES; i++) {
        buf[i] = (char)byte_for(id, file, i);
    }
}

static int write_all(const char *path, const char *src, int len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }
    int done = 0;
    while (done < len) {
        int n = (int)write(fd, src + done, (size_t)(len - done));
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += n;
    }
    fsync(fd);
    close(fd);
    return 0;
}

static int read_all(const char *path, char *dst, int len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    int done = 0;
    while (done < len) {
        int n = (int)read(fd, dst + done, (size_t)(len - done));
        if (n <= 0) {
            break;
        }
        done += n;
    }
    close(fd);
    return done;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return 2;
    }
    int id = atoi(argv[1]);

    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/w%d", id);
    mkdir(dir, 0755);
    struct stat st;
    if (stat(dir, &st) != 0) {
        return 3;
    }

    char temporary[96], final[96];

    for (int f = 0; f < FILES; f++) {
        fill(id, f);
        snprintf(temporary, sizeof(temporary), "%s/t%d", dir, f);
        snprintf(final, sizeof(final), "%s/f%d", dir, f);

        if (write_all(temporary, buf, FILE_BYTES) != 0) {
            return 5;
        }
        unlink(final);
        if (rename(temporary, final) != 0) {
            return 7;
        }

        memset(back, 0, sizeof(back));
        if (read_all(final, back, FILE_BYTES) != FILE_BYTES) {
            return 6;
        }
        if (memcmp(buf, back, FILE_BYTES) != 0) {
            return 6;
        }
    }

    for (int f = 0; f < FILES; f += 2) {
        snprintf(final, sizeof(final), "%s/f%d", dir, f);
        if (unlink(final) != 0) {
            return 8;
        }
        if (access(final, F_OK) == 0) {
            return 8;
        }
    }

    for (int f = 1; f < FILES; f += 2) {
        fill(id, f);
        snprintf(final, sizeof(final), "%s/f%d", dir, f);
        memset(back, 0, sizeof(back));
        int n = read_all(final, back, FILE_BYTES);
        if (n != FILE_BYTES) {
            return 9;
        }
        if (memcmp(buf, back, FILE_BYTES) != 0) {
            return 6;
        }
    }

    printf("fswriter %d: %d files of %d bytes written, half deleted, "
           "survivors verified\n", id, FILES, FILE_BYTES);
    return 0;
}
