#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."

HOSTCC="${HOSTCC:-cc}"
BUILD=build
mkdir -p "$BUILD"

FAKES="$BUILD/stdio-fakes.c"
cat > "$FAKES" <<'EOF'
/* One in-memory file, served through the sys_* surface stdio.c reads.
 * fd 100 is the file; writes to any fd are swallowed and counted. */
#include <stddef.h>
#include <string.h>

static const char *file_data;
static long file_len, file_pos;

void fake_file(const char *data) {
    file_data = data;
    file_len = (long)strlen(data);
    file_pos = 0;
}

/* M99: an open that can fail, so the errno contract can be graded. */
static int open_fails;
void fake_fail_open(int on) { open_fails = on; }
long sys_open(const char *p, unsigned f) {
    (void)p; (void)f;
    if (open_fails) return -1;
    file_pos = 0;
    return 100;
}
long sys_close(int fd) { (void)fd; return 0; }
long sys_read(int fd, void *b, unsigned long n) {
    if (fd != 100) return -1;
    long left = file_len - file_pos;
    long take = (long)n < left ? (long)n : left;
    if (take <= 0) return 0;
    memcpy(b, file_data + file_pos, (size_t)take);
    file_pos += take;
    return take;
}
/* M98: writes are recorded rather than swallowed, because the buffer
 * this libc grew is graded on two things a counter cannot see - WHAT
 * arrived and HOW MANY calls it took. */
static char written[8192];
static long written_len;
static int write_calls;

void fake_write_reset(void) { written_len = 0; write_calls = 0; written[0] = 0; }
const char *fake_written(void) { written[written_len] = 0; return written; }
long fake_written_len(void) { return written_len; }
int fake_write_calls(void) { return write_calls; }

long sys_write(int fd, const void *b, unsigned long n) {
    (void)fd;
    write_calls++;
    if (written_len + (long)n < (long)sizeof(written) - 1) {
        memcpy(written + written_len, b, n);
        written_len += (long)n;
    }
    return (long)n;
}
long sys_lseek(int fd, long off, int whence) {
    if (fd != 100) return -1;
    long target = whence == 0 ? off : whence == 1 ? file_pos + off : file_len + off;
    if (target < 0) return -1;
    file_pos = target;
    return target;
}
long sys_unlink(const char *p) { (void)p; return -1; }
long sys_rename(const char *a, const char *b) { (void)a; (void)b; return -1; }
long sys_getpid(void) { return 1; }
/* M98: perror reads errno through this libc's own accessor and names
 * it with this libc's strerror; the host build supplies both here. */
static int fake_errno;
int *__errno_location(void) { return &fake_errno; }
char *strerror(int e) { (void)e; return "error"; }
int fake_get_errno(void) { return fake_errno; }
void fake_set_errno(int e) { fake_errno = e; }
/* M99: fopen infers its errno through this, which lives in unistd.c and
 * is not part of this harness. ENOENT, so the driver can check that
 * fopen SET errno - the contract CPython's startup depends on - without
 * this harness having to reimplement the inference itself. */
int __lean_path_errno(const char *p, int creating) {
    (void)p; (void)creating;
    return 2; /* ENOENT */
}
EOF

DRIVER="$BUILD/stdio-driver.c"
cat > "$DRIVER" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct lean_FILE lean_FILE;
lean_FILE *lean_fopen(const char *path, const char *mode);
int lean_fclose(lean_FILE *f);
int lean_fgetc(lean_FILE *f);
char *lean_fgets(char *buf, int n, lean_FILE *f);
int lean_ungetc(int c, lean_FILE *f);
unsigned long lean_fread(void *buf, unsigned long sz, unsigned long n, lean_FILE *f);
int lean_fseek(lean_FILE *f, long off, int whence);
long lean_ftell(lean_FILE *f);
long lean_getdelim(char **line, size_t *n, int delim, lean_FILE *f);
int lean_feof(lean_FILE *f);
unsigned long lean_fwrite(const void *b, unsigned long sz, unsigned long n, lean_FILE *f);
int lean_fputc(int c, lean_FILE *f);
int lean_fputs(const char *s, lean_FILE *f);
int lean_fflush(lean_FILE *f);
int lean_fprintf(lean_FILE *f, const char *fmt, ...);
int lean_setvbuf(lean_FILE *f, char *buf, int mode, size_t size);
unsigned long lean___fpending(lean_FILE *f);
void fake_file(const char *data);
void fake_fail_open(int on);
int fake_get_errno(void);
void fake_set_errno(int e);
void fake_write_reset(void);
const char *fake_written(void);
long fake_written_len(void);
int fake_write_calls(void);

static int failures;
#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL: %s (%s)\n", what, #cond); failures++; } \
} while (0)

int main(void) {
    /* 1. gas's opening move: read two characters, push back a DIFFERENT
     * one. The pushed-back character - not the byte on disk - must be
     * what the next read returns, and the reader that gets it is fread,
     * not the fgetc that did the pushing. */
    fake_file("# hello\nmov\n");
    lean_FILE *f = lean_fopen("x", "r");
    CHECK(f != 0, "fopen");
    CHECK(lean_fgetc(f) == '#', "first getc");
    CHECK(lean_fgetc(f) == ' ', "second getc");
    CHECK(lean_ungetc('#', f) == '#', "ungetc a different char than on disk");
    char buf[64];
    memset(buf, 0, sizeof buf);
    CHECK(lean_fread(buf, 1, 6, f) == 6, "fread across the pushback");
    CHECK(memcmp(buf, "#hello", 6) == 0, "fread yields pushback then stream");
    lean_fclose(f);

    /* 2. One character of pushback is guaranteed; a second is refused,
     * not silently dropped. */
    fake_file("ab");
    f = lean_fopen("x", "r");
    CHECK(lean_fgetc(f) == 'a', "getc before double unget");
    CHECK(lean_ungetc('x', f) == 'x', "first ungetc");
    CHECK(lean_ungetc('y', f) == EOF, "second ungetc refused");
    CHECK(lean_fgetc(f) == 'x', "the one buffered char comes back");
    CHECK(lean_fgetc(f) == 'b', "then the stream continues");
    lean_fclose(f);

    /* 3. ftell counts the pushed-back character as unread, and a
     * relative fseek lands where the caller's arithmetic says. */
    fake_file("abcdef");
    f = lean_fopen("x", "r");
    lean_fgetc(f); lean_fgetc(f);              /* pos 2 */
    CHECK(lean_ftell(f) == 2, "ftell before ungetc");
    lean_ungetc('X', f);
    CHECK(lean_ftell(f) == 1, "ftell counts pushback as unread");
    CHECK(lean_fseek(f, 0, 1 /*SEEK_CUR*/) == 0, "relative seek with pushback");
    CHECK(lean_fgetc(f) == 'b', "seek discarded pushback, landed right");
    lean_fclose(f);

    /* 4. fgets and getdelim honour the pushback like any reader. */
    fake_file("line one\nline two\n");
    f = lean_fopen("x", "r");
    lean_fgetc(f);
    lean_ungetc('L', f);
    CHECK(lean_fgets(buf, sizeof buf, f) != 0, "fgets after ungetc");
    CHECK(strcmp(buf, "Line one\n") == 0, "fgets starts with the pushback");
    char *line = 0; size_t cap = 0;
    lean_fgetc(f); /* consume the 'l' this pushback replaces */
    lean_ungetc('L', f);
    CHECK(lean_getdelim(&line, &cap, '\n', f) == 9, "getdelim after ungetc");
    CHECK(strcmp(line, "Line two\n") == 0, "getdelim starts with the pushback");
    free(line);
    lean_fclose(f);

    /* 5. ungetc at end of file un-sets EOF, and EOF comes back after. */
    fake_file("z");
    f = lean_fopen("x", "r");
    CHECK(lean_fgetc(f) == 'z', "last char");
    CHECK(lean_fgetc(f) == EOF, "then EOF");
    CHECK(lean_feof(f) == 1, "feof set");
    CHECK(lean_ungetc('q', f) == 'q', "ungetc after EOF");
    CHECK(lean_feof(f) == 0, "feof cleared by ungetc");
    CHECK(lean_fgetc(f) == 'q', "pushback readable at EOF");
    CHECK(lean_fgetc(f) == EOF, "EOF again after it");
    lean_fclose(f);

    /* ---- M98: the write buffer ------------------------------------
     *
     * The measurement that asked for it: one C++ compile on the machine
     * made 199,385 `write` syscalls, 89% of every syscall it made, to
     * produce a 1.5 MB file - about seven bytes a call, because there
     * was no buffer at all. These checks are what a buffer has to get
     * right, and each one is a way of losing somebody's bytes. */

    /* 6. Many small writes become few syscalls, and every byte arrives
     * in order. */
    fake_file("");
    fake_write_reset();
    f = lean_fopen("x", "w");
    CHECK(f != 0, "fopen for writing");
    for (int i = 0; i < 500; i++) {
        lean_fputc('a' + (i % 26), f);
    }
    CHECK(fake_write_calls() == 0, "500 fputc calls are not 500 syscalls");
    CHECK(lean___fpending(f) == 500, "the pending count is what is buffered");
    lean_fclose(f);
    CHECK(fake_write_calls() == 1, "fclose flushed once");
    CHECK(fake_written_len() == 500, "every byte arrived");
    CHECK(fake_written()[0] == 'a' && fake_written()[25] == 'z' &&
          fake_written()[26] == 'a', "in order");

    /* 7. fflush is what a caller uses when order matters, and it empties
     * the buffer rather than merely claiming to. */
    fake_write_reset();
    f = lean_fopen("x", "w");
    lean_fputs("half", f);
    CHECK(fake_write_calls() == 0, "buffered so far");
    CHECK(lean_fflush(f) == 0, "fflush");
    CHECK(fake_write_calls() == 1, "fflush wrote");
    CHECK(strcmp(fake_written(), "half") == 0, "and wrote the right bytes");
    CHECK(lean___fpending(f) == 0, "nothing left pending");
    lean_fclose(f);
    CHECK(fake_write_calls() == 1, "an empty buffer does not write again");

    /* 8. A write larger than the buffer goes straight through, in one
     * call, after whatever was pending - which is the ordering rule that
     * makes a mixed program's output make sense. */
    fake_write_reset();
    f = lean_fopen("x", "w");
    lean_fputs("first", f);
    static char big[4096];
    memset(big, 'B', sizeof big);
    CHECK(lean_fwrite(big, 1, sizeof big, f) == sizeof big, "big fwrite");
    CHECK(fake_write_calls() == 2, "pending flushed, then written through");
    CHECK(fake_written_len() == 5 + (long)sizeof big, "both arrived");
    CHECK(memcmp(fake_written(), "firstBBB", 8) == 0, "in that order");
    lean_fclose(f);

    /* 9. Line buffering: a newline is what makes a prompt appear before
     * the read that follows it. */
    fake_write_reset();
    f = lean_fopen("x", "w");
    CHECK(lean_setvbuf(f, 0, 1 /*_IOLBF*/, 1024) == 0, "setvbuf line mode");
    lean_fputs("no newline yet", f);
    CHECK(fake_write_calls() == 0, "still buffered");
    lean_fputc('\n', f);
    CHECK(fake_write_calls() == 1, "the newline flushed it");
    lean_fclose(f);

    /* 10. Unbuffered means unbuffered, which is what stderr is. */
    fake_write_reset();
    f = lean_fopen("x", "w");
    CHECK(lean_setvbuf(f, 0, 2 /*_IONBF*/, 0) == 0, "setvbuf unbuffered");
    lean_fputs("a", f);
    lean_fputs("b", f);
    CHECK(fake_write_calls() == 2, "each write went out on its own");
    lean_fclose(f);

    /* 11. M99: a failed fopen SETS ERRNO. Three ways to fail, and all
     * three used to return NULL over whatever errno happened to hold.
     *
     * This is control flow rather than diagnostics, which is why it is
     * a check rather than a nicety: CPython's startup opens a
     * `pyvenv.cfg` that is not there and catches FileNotFoundError to
     * mean "not a virtual environment". The C side builds that
     * exception FROM ERRNO, so errno 0 produced a bare OSError, the
     * except clause did not catch it, and the interpreter died before
     * running a line of anything. The ordinary case was the fatal one.
     *
     * errno is set to a sentinel first, so this cannot pass because
     * something earlier left the right value lying around. */
    fake_set_errno(0);
    fake_fail_open(1);
    CHECK(lean_fopen("nope", "r") == 0, "fopen of a missing file fails");
    CHECK(fake_get_errno() != 0, "and says why - errno is not left at 0");
    fake_fail_open(0);

    fake_set_errno(0);
    CHECK(lean_fopen("x", "z") == 0, "a mode string with no r, w or a fails");
    CHECK(fake_get_errno() == 22 /*EINVAL*/, "and that failure is EINVAL");

    if (failures) {
        printf("stdio-test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("stdio-test: the FILE layer honours ungetc everywhere a reader "
           "looks, buffers its writes without losing one, and says why "
           "when it cannot open a file\n");
    return 0;
}
EOF

RENAMES=""
for s in __assert_fail __fpending __lean_stdio_flush_all \
         clearerr dprintf fclose fdopen feof \
         ferror fflush fgetc fgetpos fgets fileno fopen fprintf fputc \
         fputs fread freopen fseek fsetpos ftell fwrite getc getchar \
         getdelim getline perror printf putc putchar puts remove rename \
         rewind setbuf setvbuf snprintf sprintf stderr stdin stdout \
         tmpfile ungetc vdprintf vfprintf vprintf vsnprintf vsprintf \
         asprintf vasprintf fgetwc ungetwc; do
  RENAMES="$RENAMES -D$s=lean_$s"
done

if ! $HOSTCC -std=c11 -O1 -g -c -o "$BUILD/stdio-engine.o" \
     user_space/libc/src/stdio.c \
     -I user_space/libc/include -I user_space/library -I system_api/include \
     $RENAMES 2>"$BUILD/stdio-engine.log"; then
  echo "stdio-test: could not compile stdio.c for the host:" >&2
  tail -20 "$BUILD/stdio-engine.log" >&2
  exit 1
fi
STRAY=$(nm -gU "$BUILD/stdio-engine.o" | awk '{print $3}' | sed 's/^_//' \
        | grep -v '^lean_' || true)
if [ -n "$STRAY" ]; then
  echo "stdio-test: stdio.c exports symbols this script does not rename:" >&2
  echo "$STRAY" >&2
  exit 1
fi

BIN="$BUILD/stdio-test"
if ! $HOSTCC -std=c11 -O1 -g -o "$BIN" "$DRIVER" "$BUILD/stdio-engine.o" \
     "$FAKES" 2>"$BUILD/stdio-link.log"; then
  echo "stdio-test: link failed:" >&2
  tail -20 "$BUILD/stdio-link.log" >&2
  exit 1
fi

"$BIN"
