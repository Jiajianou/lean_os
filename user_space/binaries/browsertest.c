#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "syscall_wrappers.h"

#define DIRECTORY_PATH  "/tmp/m100bt"
#define FILE_PATH "/tmp/m100bt/positional"

static int fail(int code) {
    return code;
}

static int check_positional(void) {
    (void)sys_mkdir(DIRECTORY_PATH);
    int fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return fail(2);
    }
    static const char base[] = "0123456789ABCDEF";
    if (write(fd, base, 16) != 16) {
        return fail(3);
    }
    if (lseek(fd, 0, SEEK_CUR) != 16) {
        return fail(4);
    }

    char buffer[8];
    memset(buffer, 0, sizeof(buffer));
    if (pread(fd, buffer, 4, 4) != 4) {
        return fail(5);
    }
    if (memcmp(buffer, "4567", 4) != 0) {
        return fail(6);
    }
    if (lseek(fd, 0, SEEK_CUR) != 16) {
        return fail(7);
    }

    if (pwrite(fd, "xy", 2, 10) != 2) {
        return fail(8);
    }
    if (lseek(fd, 0, SEEK_CUR) != 16) {
        return fail(9);
    }
    memset(buffer, 0, sizeof(buffer));
    if (pread(fd, buffer, 4, 8) != 4) {
        return fail(10);
    }
    if (memcmp(buffer, "89xy", 4) != 0) {
        return fail(11);
    }
    if (lseek(fd, 2, SEEK_SET) != 2) {
        return fail(12);
    }
    memset(buffer, 0, sizeof(buffer));
    if (read(fd, buffer, 3) != 3 || memcmp(buffer, "234", 3) != 0) {
        return fail(13);
    }
    if (lseek(fd, 0, SEEK_CUR) != 5) {
        return fail(14);
    }

    if (pread(fd, buffer, 4, 1000) != 0) {
        return fail(15);
    }
    if (pread(fd, buffer, 4, -1) != -1) {
        return fail(16);
    }
    close(fd);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return fail(17);
    }
    errno = 0;
    if (pread(pipefd[0], buffer, 4, 0) != -1 || errno != ESPIPE) {
        return fail(18);
    }
    close(pipefd[0]);
    close(pipefd[1]);
    return 0;
}

static int only_dat(const struct dirent *e) {
    size_t n = strlen(e->d_name);
    return n > 4 && strcmp(e->d_name + n - 4, ".dat") == 0;
}

static int check_scandir(void) {
    static const char *const names[] = {"delta.dat", "alpha.dat",
                                        "charlie.dat", "bravo.dat"};
    for (int i = 0; i < 4; i++) {
        char path[128];
        strcpy(path, DIRECTORY_PATH "/");
        strcat(path, names[i]);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            return fail(20);
        }
        close(fd);
    }
    int fd = open(DIRECTORY_PATH "/ignore.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return fail(21);
    }
    close(fd);

    struct dirent **list = NULL;
    int n = scandir(DIRECTORY_PATH, &list, only_dat, alphasort);
    if (n != 4) {
        return fail(22);
    }
    static const char *const sorted[] = {"alpha.dat", "bravo.dat",
                                         "charlie.dat", "delta.dat"};
    for (int i = 0; i < 4; i++) {
        if (strcmp(list[i]->d_name, sorted[i]) != 0) {
            return fail(23);
        }
    }
    for (int i = 0; i < n; i++) {
        free(list[i]);
    }
    free(list);

    list = NULL;
    if (scandir(DIRECTORY_PATH "/alpha.dat", &list, NULL, alphasort) != -1) {
        return fail(24);
    }
    if (scandir("/no/such/place", &list, NULL, alphasort) != -1) {
        return fail(25);
    }
    return 0;
}

static int convert(const char *to, const char *from, const void *in,
                   size_t inlen, char *out, size_t outmax, size_t *outlen) {
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) {
        return -1;
    }
    char *ip = (char *)in, *op = out;
    size_t il = inlen, ol = outmax;
    size_t rc = iconv(cd, &ip, &il, &op, &ol);
    iconv_close(cd);
    if (rc == (size_t)-1 || il != 0) {
        return -1;
    }
    *outlen = outmax - ol;
    return 0;
}

static int check_iconv(void) {
    char out[64];
    size_t n;

    static const unsigned char cp1252[] = {0x93, 'h', 'i', 0x94};
    if (convert("UTF-8", "windows-1252", cp1252, 4, out, sizeof(out), &n) != 0) {
        return fail(30);
    }
    static const unsigned char want[] = {0xE2, 0x80, 0x9C, 'h', 'i',
                                         0xE2, 0x80, 0x9D};
    if (n != sizeof(want) || memcmp(out, want, n) != 0) {
        return fail(31);
    }

    static const unsigned char utf8_ae[] = {0xC3, 0xA9};
    if (convert("ISO-8859-1", "UTF-8", utf8_ae, 2, out, sizeof(out), &n) != 0) {
        return fail(32);
    }
    if (n != 1 || (unsigned char)out[0] != 0xE9) {
        return fail(33);
    }

    static const unsigned char utf8_cjk[] = {0xE6, 0x97, 0xA5};
    if (convert("ISO-8859-1", "UTF-8", utf8_cjk, 3, out, sizeof(out), &n) == 0) {
        return fail(34);
    }

    static const unsigned char overlong[] = {0xC0, 0xAF};
    if (convert("UTF-8", "UTF-8", overlong, 2, out, sizeof(out), &n) == 0) {
        return fail(35);
    }

    static const unsigned char utf8_emoji[] = {0xF0, 0x9F, 0x98, 0x80};
    if (convert("UTF-16LE", "UTF-8", utf8_emoji, 4, out, sizeof(out), &n) != 0) {
        return fail(36);
    }
    static const unsigned char want16[] = {0x3D, 0xD8, 0x00, 0xDE};
    if (n != 4 || memcmp(out, want16, 4) != 0) {
        return fail(37);
    }

    if (iconv_open("UTF-8", "SHIFT_JIS") != (iconv_t)-1) {
        return fail(38);
    }
    return 0;
}

static int check_lround(void) {
    if (lround(0.5) != 1 || lround(-0.5) != -1) {
        return fail(40);
    }
    if (lround(1.5) != 2 || lround(2.5) != 3) {
        return fail(41);
    }
    if (lround(-2.5) != -3) {
        return fail(42);
    }
    if (lround(0.49999999999999994) != 0) {
        return fail(43);
    }
    if (lroundf(2.5f) != 3 || lroundf(-0.5f) != -1) {
        return fail(44);
    }
    if (llround(1e15 + 0.5) != 1000000000000001LL) {
        return fail(45);
    }
    if (lround(1e300) != (-9223372036854775807L - 1)) {
        return fail(46);
    }
    return 0;
}

int main(void) {
    int rc;
    if ((rc = check_positional()) != 0) {
        return rc;
    }
    if ((rc = check_scandir()) != 0) {
        return rc;
    }
    if ((rc = check_iconv()) != 0) {
        return rc;
    }
    if ((rc = check_lround()) != 0) {
        return rc;
    }
    return 0;
}
