#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."

HOSTCC="${HOSTCC:-cc}"
BUILD=build
FILTER="${1:-}"
mkdir -p "$BUILD"

DRIVER="$BUILD/printf-driver.c"
cat > "$DRIVER" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_OURS
int lean_snprintf(char *dst, size_t cap, const char *fmt, ...);
#define SNPRINTF lean_snprintf
#else
#define SNPRINTF snprintf
#endif

static int arg_len(const char *fmt) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (*p=='-'||*p=='0'||*p=='+'||*p==' '||*p=='#') p++;
        while ((*p >= '0' && *p <= '9') || *p == '*' || *p == '.') p++;
        if (*p == 'h') { return p[1] == 'h' ? 2 : 1; }
        if (*p == 'l') { return p[1] == 'l' ? 4 : 3; }
        if (*p == 'z') return 5;
        return 0;
    }
    return 0;
}

int main(void) {
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *fmt = line;
        char *kind = strchr(fmt, '\t');
        if (!kind) continue;
        *kind++ = 0;
        char *val = strchr(kind, '\t');
        if (val) *val++ = 0; else val = kind + strlen(kind);

        char vbuf[2048];
        size_t ii = 0;
        for (char *s = val; *s && ii < sizeof(vbuf)-1; s++) {
            if (s[0] == '\\' && s[1] == 'n') { vbuf[ii++] = '\n'; s++; }
            else if (s[0] == '\\' && s[1] == 't') { vbuf[ii++] = '\t'; s++; }
            else vbuf[ii++] = *s;
        }
        vbuf[ii] = 0;

        char out[2048];
        memset(out, 0, sizeof out);
        int rc = -999;
        int len = arg_len(fmt);
        switch (kind[0]) {
        case 'n': rc = SNPRINTF(out, sizeof out, fmt); break;
        case 'd': {
            long long v = strtoll(vbuf, 0, 0);
            if (len == 3)      rc = SNPRINTF(out, sizeof out, fmt, (long)v);
            else if (len == 4) rc = SNPRINTF(out, sizeof out, fmt, v);
            else if (len == 5) rc = SNPRINTF(out, sizeof out, fmt, (size_t)v);
            else               rc = SNPRINTF(out, sizeof out, fmt, (int)v);
            break;
        }
        case 'u': {
            unsigned long long v = strtoull(vbuf, 0, 0);
            if (len == 3)      rc = SNPRINTF(out, sizeof out, fmt, (unsigned long)v);
            else if (len == 4) rc = SNPRINTF(out, sizeof out, fmt, v);
            else if (len == 5) rc = SNPRINTF(out, sizeof out, fmt, (size_t)v);
            else               rc = SNPRINTF(out, sizeof out, fmt, (unsigned)v);
            break;
        }
        case 'f': rc = SNPRINTF(out, sizeof out, fmt, strtod(vbuf, 0)); break;
        case 's': rc = SNPRINTF(out, sizeof out, fmt, vbuf); break;
        case 'c': rc = SNPRINTF(out, sizeof out, fmt, (int)vbuf[0]); break;
        case 'w': {
            char *colon = strchr(vbuf, ':');
            int w = atoi(vbuf);
            long long v = colon ? strtoll(colon + 1, 0, 0) : 0;
            rc = SNPRINTF(out, sizeof out, fmt, w, (int)v);
            break;
        }
        default: continue;
        }
        printf("rc=%d [%s]\n", rc, out);
    }
    return 0;
}
EOF

FAKES="$BUILD/printf-fakes.c"
cat > "$FAKES" <<'EOF'
#include <stddef.h>
long sys_open(const char *p, unsigned f) { (void)p; (void)f; return -1; }
long sys_close(int fd) { (void)fd; return -1; }
long sys_read(int fd, void *b, unsigned long n) { (void)fd; (void)b; (void)n; return -1; }
long sys_write(int fd, const void *b, unsigned long n) { (void)fd; (void)b; (void)n; return -1; }
long sys_lseek(int fd, long o, int w) { (void)fd; (void)o; (void)w; return -1; }
long sys_unlink(const char *p) { (void)p; return -1; }
long sys_rename(const char *a, const char *b) { (void)a; (void)b; return -1; }
long sys_getpid(void) { return 1; }
static int fake_errno;
int *__errno_location(void) { return &fake_errno; }
char *strerror(int e) { (void)e; return "error"; }
int __lean_path_errno(const char *p, int creating) {
    (void)p; (void)creating;
    return 2;
}
EOF

OURS="$BUILD/printf-ours"
THEIRS="$BUILD/printf-theirs"

RENAMES=""
for s in __assert_fail __fpending __lean_stdio_flush_all \
         clearerr dprintf fclose fdopen feof \
         ferror fflush fgetc fgetpos fgets fileno fopen fprintf fputc \
         fputs fread freopen fseek fseeko fsetpos ftell ftello fwrite getc getchar \
         getdelim getline perror printf putc putchar puts remove rename \
         rewind setbuf setvbuf snprintf sprintf stderr stdin stdout \
         tmpfile ungetc vdprintf vfprintf vprintf vsnprintf vsprintf \
         asprintf vasprintf fgetwc ungetwc; do
  RENAMES="$RENAMES -D$s=lean_$s"
done

if ! $HOSTCC -std=c11 -O1 -g -c -o "$BUILD/printf-engine.o" \
     user_space/libc/src/stdio.c \
     -I user_space/libc/include -I user_space/library -I system_api/include \
     $RENAMES 2>"$BUILD/printf-engine.log"; then
  echo "printf-test: could not compile stdio.c for the host:" >&2
  tail -20 "$BUILD/printf-engine.log" >&2
  exit 1
fi

STRAY=$(nm -gU "$BUILD/printf-engine.o" | awk '{print $3}' | sed 's/^_//' \
        | grep -v '^lean_' || true)
if [ -n "$STRAY" ]; then
  echo "printf-test: stdio.c exports symbols this script does not rename:" >&2
  echo "$STRAY" >&2
  echo "printf-test: add them to RENAMES - an unrenamed one shadows the host's." >&2
  exit 1
fi

if ! $HOSTCC -std=c11 -O1 -g -DUSE_OURS -o "$OURS" "$DRIVER" \
     "$BUILD/printf-engine.o" "$FAKES" 2>"$BUILD/printf-ours.log"; then
  echo "printf-test: could not link the lean_os printf driver:" >&2
  tail -20 "$BUILD/printf-ours.log" >&2
  exit 1
fi
if ! $HOSTCC -std=c11 -O1 -o "$THEIRS" "$DRIVER" 2>"$BUILD/printf-theirs.log"; then
  echo "printf-test: could not build the host printf driver:" >&2
  tail -20 "$BUILD/printf-theirs.log" >&2
  exit 1
fi

CASES=tests/printf/cases.tsv
[ -f "$CASES" ] || { echo "printf-test: no $CASES" >&2; exit 1; }

INPUT="$BUILD/printf-cases.txt"
if [ -n "$FILTER" ]; then
  grep -v '^#' "$CASES" | grep -F -- "$FILTER" > "$INPUT" || true
else
  grep -v '^#' "$CASES" > "$INPUT"
fi

A="$BUILD/printf-ours.out"
B="$BUILD/printf-theirs.out"
"$OURS"  < "$INPUT" > "$A"
"$THEIRS" < "$INPUT" > "$B"

TOTAL=$(grep -c . "$INPUT" || true)
if ! cmp -s "$A" "$B"; then
  echo "printf-test: DISAGREEMENT with the host's snprintf:" >&2
  paste "$INPUT" "$A" "$B" | awk -F'\t' '
    $(NF-1) != $NF {
      printf "  format %-24s ours=%s theirs=%s\n", $1, $(NF-1), $NF
    }' >&2
  DIFFS=$(paste "$A" "$B" | awk -F'\t' '$1 != $2' | wc -l | tr -d ' ')
  echo "printf-test: $DIFFS of $TOTAL cases disagree" >&2
  exit 1
fi

echo "printf-test: $TOTAL cases, byte-identical with the host's snprintf"
