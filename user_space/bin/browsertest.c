/* user_space/bin/browsertest.c - M100: the four things the browser port
 * added to this system, graded on the machine that has them.
 *
 * Porting NetSurf named five gaps in this OS (see tools/build-netsurf.sh
 * for the list and for what named each one). Four of them are new kernel
 * or libc surface, and this is where they are proved:
 *
 *   SYS_pread / SYS_pwrite   positional, and - the whole point - leaving
 *                            the descriptor's own offset alone
 *   scandir / alphasort      a whole directory, filtered and sorted
 *   iconv                    charset conversion, on this machine rather
 *                            than on the host that graded the tables
 *   lround family            the four integer spellings of round()
 *
 * ---- why this exists beside the instruments that already grade them --
 *
 * tools/iconv-test.sh compares 2.58 million conversions against the
 * host's iconv, and tools/math-test.sh grades every lround against the
 * host's libm. Both are far harder tests than anything below, and
 * neither can answer the question this one asks: whether the code that
 * agreed with the host, compiled for THIS target and linked against
 * THIS libc, still does. A differential test on the host grades the
 * algorithm; this grades the machine.
 *
 * pread and pwrite get the opposite treatment - the host tier cannot
 * reach them at all, because what they are for is a real file on a real
 * filesystem with a real shared offset behind it.
 *
 * Exit codes are distinct per failure and the kernel prints the one it
 * got - see kernel.c's [m100h] block.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "syscall_wrappers.h"

#define DIR_PATH  "/tmp/m100bt"
#define FILE_PATH "/tmp/m100bt/positional"

static int fail(int code) {
    return code;
}

/* ---- 1. pread/pwrite ------------------------------------------------- */
static int check_positional(void) {
    (void)sys_mkdir(DIR_PATH); /* may already exist from a previous boot */
    int fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return fail(2);
    }
    static const char base[] = "0123456789ABCDEF";
    if (write(fd, base, 16) != 16) {
        return fail(3);
    }
    /* The descriptor's position is now 16. Everything below depends on
     * that number NOT changing. */
    if (lseek(fd, 0, SEEK_CUR) != 16) {
        return fail(4);
    }

    char buf[8];
    memset(buf, 0, sizeof(buf));
    if (pread(fd, buf, 4, 4) != 4) {
        return fail(5);
    }
    if (memcmp(buf, "4567", 4) != 0) {
        return fail(6);
    }
    /* The point of the whole call: reading from position 4 did not move
     * the descriptor to 8. A pread built out of lseek/read/lseek would
     * pass every check above and fail this one under a second thread. */
    if (lseek(fd, 0, SEEK_CUR) != 16) {
        return fail(7);
    }

    if (pwrite(fd, "xy", 2, 10) != 2) {
        return fail(8);
    }
    if (lseek(fd, 0, SEEK_CUR) != 16) {
        return fail(9);
    }
    /* And the write landed where it was asked to, not at the position. */
    memset(buf, 0, sizeof(buf));
    if (pread(fd, buf, 4, 8) != 4) {
        return fail(10);
    }
    if (memcmp(buf, "89xy", 4) != 0) {
        return fail(11);
    }
    /* An ordinary read still starts from the position it always did, so
     * the two calls have not been quietly merged. */
    if (lseek(fd, 2, SEEK_SET) != 2) {
        return fail(12);
    }
    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 3) != 3 || memcmp(buf, "234", 3) != 0) {
        return fail(13);
    }
    if (lseek(fd, 0, SEEK_CUR) != 5) {
        return fail(14);
    }

    /* Past the end reads nothing rather than failing. */
    if (pread(fd, buf, 4, 1000) != 0) {
        return fail(15);
    }
    /* A negative offset is refused rather than wrapped. */
    if (pread(fd, buf, 4, -1) != -1) {
        return fail(16);
    }
    close(fd);

    /* Refused on something with no position, with ESPIPE - which is the
     * failure libnsutils tests for by name. */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return fail(17);
    }
    errno = 0;
    if (pread(pipefd[0], buf, 4, 0) != -1 || errno != ESPIPE) {
        return fail(18);
    }
    close(pipefd[0]);
    close(pipefd[1]);
    return 0;
}

/* ---- 2. scandir/alphasort -------------------------------------------- */
static int only_dat(const struct dirent *e) {
    size_t n = strlen(e->d_name);
    return n > 4 && strcmp(e->d_name + n - 4, ".dat") == 0;
}

static int check_scandir(void) {
    /* Deliberately created out of order, so a sort that did nothing
     * would be visible. */
    static const char *const names[] = {"delta.dat", "alpha.dat",
                                        "charlie.dat", "bravo.dat"};
    for (int i = 0; i < 4; i++) {
        char path[128];
        strcpy(path, DIR_PATH "/");
        strcat(path, names[i]);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            return fail(20);
        }
        close(fd);
    }
    /* And one the filter must exclude. */
    int fd = open(DIR_PATH "/ignore.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return fail(21);
    }
    close(fd);

    struct dirent **list = NULL;
    int n = scandir(DIR_PATH, &list, only_dat, alphasort);
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
    /* The caller owns every one of these; freeing them is part of the
     * contract and a leak here would show up in Q9's audit. */
    for (int i = 0; i < n; i++) {
        free(list[i]);
    }
    free(list);

    /* A directory that is not one is a failure, not an empty listing. */
    list = NULL;
    if (scandir(DIR_PATH "/alpha.dat", &list, NULL, alphasort) != -1) {
        return fail(24);
    }
    if (scandir("/no/such/place", &list, NULL, alphasort) != -1) {
        return fail(25);
    }
    return 0;
}

/* ---- 3. iconv --------------------------------------------------------- */
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

    /* windows-1252 0x93/0x94 are the curly quotes that are NOT in
     * ISO-8859-1, which is the single most common way a real page is
     * mislabelled - and the reason a browser needs this table rather
     * than the identity map. */
    static const unsigned char cp1252[] = {0x93, 'h', 'i', 0x94};
    if (convert("UTF-8", "windows-1252", cp1252, 4, out, sizeof(out), &n) != 0) {
        return fail(30);
    }
    /* U+201C hi U+201D */
    static const unsigned char want[] = {0xE2, 0x80, 0x9C, 'h', 'i',
                                         0xE2, 0x80, 0x9D};
    if (n != sizeof(want) || memcmp(out, want, n) != 0) {
        return fail(31);
    }

    /* Round trip through a charset that has the character. */
    static const unsigned char utf8_ae[] = {0xC3, 0xA9}; /* U+00E9 e-acute */
    if (convert("ISO-8859-1", "UTF-8", utf8_ae, 2, out, sizeof(out), &n) != 0) {
        return fail(32);
    }
    if (n != 1 || (unsigned char)out[0] != 0xE9) {
        return fail(33);
    }

    /* A character the target charset does not have is REFUSED, not
     * approximated. This is the clause macOS's own iconv does not
     * honour (it transliterates) and the reason tools/iconv-test.sh is
     * built the way it is. */
    static const unsigned char utf8_cjk[] = {0xE6, 0x97, 0xA5}; /* U+65E5 */
    if (convert("ISO-8859-1", "UTF-8", utf8_cjk, 3, out, sizeof(out), &n) == 0) {
        return fail(34);
    }

    /* An invalid UTF-8 sequence is refused. An overlong '/' above all -
     * a browser is handed these on purpose. */
    static const unsigned char overlong[] = {0xC0, 0xAF}; /* overlong '/' */
    if (convert("UTF-8", "UTF-8", overlong, 2, out, sizeof(out), &n) == 0) {
        return fail(35);
    }

    /* UTF-16LE out, including a character above the BMP so the surrogate
     * pair path runs. U+1F600. */
    static const unsigned char utf8_emoji[] = {0xF0, 0x9F, 0x98, 0x80};
    if (convert("UTF-16LE", "UTF-8", utf8_emoji, 4, out, sizeof(out), &n) != 0) {
        return fail(36);
    }
    static const unsigned char want16[] = {0x3D, 0xD8, 0x00, 0xDE};
    if (n != 4 || memcmp(out, want16, 4) != 0) {
        return fail(37);
    }

    /* A charset this libc does not have is refused by name rather than
     * silently treated as ASCII. */
    if (iconv_open("UTF-8", "SHIFT_JIS") != (iconv_t)-1) {
        return fail(38);
    }
    return 0;
}

/* ---- 4. the lround family -------------------------------------------- */
static int check_lround(void) {
    /* Away from zero on a tie, which is what distinguishes these from a
     * cast and from nearest-even. */
    if (lround(0.5) != 1 || lround(-0.5) != -1) {
        return fail(40);
    }
    if (lround(1.5) != 2 || lround(2.5) != 3) {
        return fail(41); /* 3, not 2 - round() is not banker's rounding */
    }
    if (lround(-2.5) != -3) {
        return fail(42);
    }
    /* The input round() itself was fixed for in M99: the double just
     * below a half, which floor(x + 0.5) gets wrong. */
    if (lround(0.49999999999999994) != 0) {
        return fail(43);
    }
    if (lroundf(2.5f) != 3 || lroundf(-0.5f) != -1) {
        return fail(44);
    }
    if (llround(1e15 + 0.5) != 1000000000000001LL) {
        return fail(45);
    }
    /* Out of range gives LONG_MIN rather than trapping or wrapping. */
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
