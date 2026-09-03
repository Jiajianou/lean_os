#!/usr/bin/env bash
# tools/stdio-test.sh - M98: the FILE layer, off the machine.
#
# printf-test.sh grades the format engine by agreement with the host;
# this grades the stream machinery - ungetc, the read functions that
# must honour it, and the position functions that must account for it -
# against C99 directly, because "what does the host's FILE do" is not
# observable through a FILE built over lean_os syscalls. The oracle here
# is the standard's own guarantees, written as assertions.
#
# Why it exists: the machine's own `as` could not assemble a comment.
# gas's first act on every input file is getc, getc, ungetc('#') - it
# reads two characters and pushes back a DIFFERENT one, which C
# explicitly permits (one character of pushback is guaranteed). This
# libc's ungetc was lseek(pos-1), which hands back whatever byte is on
# disk, not what the caller pushed. The '#' became a space, line one of
# tests/binutils/hello.s became code, and the [m98] boot test caught it
# at the first `nm`. The fake syscall layer below is what lets that
# five-minute boot failure be a five-millisecond host test.
#
# Compiles user_space/libc/src/stdio.c for the host with every exported
# symbol renamed lean_* (printf-test.sh's isolation, same rename list,
# same stray-symbol check) over an in-memory file.
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

long sys_open(const char *p, unsigned f) { (void)p; (void)f; file_pos = 0; return 100; }
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
long sys_write(int fd, const void *b, unsigned long n) { (void)fd; (void)b; return (long)n; }
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
void fake_file(const char *data);

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

    if (failures) {
        printf("stdio-test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("stdio-test: the FILE layer honours ungetc everywhere a reader looks\n");
    return 0;
}
EOF

# The same rename list printf-test.sh uses, checked the same way.
RENAMES=""
for s in __assert_fail __fpending clearerr dprintf fclose fdopen feof \
         ferror fflush fgetc fgetpos fgets fileno fopen fprintf fputc \
         fputs fread freopen fseek fsetpos ftell fwrite getc getchar \
         getdelim getline perror printf putc putchar puts remove rename \
         rewind setbuf setvbuf snprintf sprintf stderr stdin stdout \
         tmpfile ungetc vdprintf vfprintf vprintf vsnprintf vsprintf; do
  RENAMES="$RENAMES -D$s=lean_$s"
done

if ! $HOSTCC -std=c11 -O1 -g -c -o "$BUILD/stdio-engine.o" \
     user_space/libc/src/stdio.c \
     -I user_space/libc/include -I user_space/lib -I system_api/include \
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
