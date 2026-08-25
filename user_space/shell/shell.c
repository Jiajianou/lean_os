/* user_space/shell/shell.c - M13's minimal shell. Reads a line, spawns
 * the first word as a program (SYS_spawn) with everything after the
 * first space as its single string "arg" (lean_os's whole argv
 * mechanism - see kernel/proc/proc.c), and waits for it before prompting
 * again. Line editing (echo, backspace) lives here in user space, not in
 * the kernel's SYS_read - that's a real tty's job in a real OS, and this
 * is the simplest thing that gives it to us without one.
 */
#include "str.h"
#include "paths.h" /* system_api/include/paths.h - M53: /bin is this shell's whole search path */
#include "syscall_wrappers.h"

#define LINE_MAX 128

static int read_line(char *buf) {
    int len = 0;
    for (;;) {
        char c;
        long n = sys_read(0, &c, 1);
        if (n <= 0) {
            continue;
        }
        if (c == '\n' || c == '\r') {
            sys_write(1, "\n", 1);
            buf[len] = '\0';
            return len;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                len--;
                sys_write(1, "\b \b", 3); /* move back, erase, move back again */
            }
            continue;
        }
        if (len < LINE_MAX - 1) {
            buf[len++] = c;
            sys_write(1, &c, 1); /* echo */
        }
    }
}

int main(int argc, char **argv) {
    /* M60: argv[0] is this program's own path; argv[1] is the first thing
     * the caller had to say. `arg` keeps the name the body already uses,
     * and is the empty string when there was nothing - which is exactly
     * what the single-string mechanism this replaced handed over. */
    const char *arg = argc > 1 ? argv[1] : "";
    (void)arg;
    char line[LINE_MAX];

    for (;;) {
        const char prompt[] = "$ ";
        sys_write(1, prompt, sizeof(prompt) - 1);

        int len = read_line(line);
        if (len == 0) {
            continue;
        }
        if (strcmp(line, "exit") == 0) {
            sys_exit(0);
        }

        /* Split at the first space: line becomes the program name, and
         * whatever follows (or "" if there's no space) becomes its arg. */
        char *space = line;
        while (*space && *space != ' ') {
            space++;
        }
        char *prog_arg = "";
        if (*space == ' ') {
            *space = '\0';
            prog_arg = space + 1;
        }

        /* M53: a command with no '/' in it is looked up in /bin - the
         * smallest thing that deserves to be called a search path, and
         * the one this OS needs now that a bare name is no longer a
         * location. Anything containing a '/' is taken as the path it
         * plainly is, so "/tmp/m33test" still reaches exactly that. */
        char resolved[PATH_MAX_LEN];
        const char *target = line;
        int has_slash = 0;
        for (const char *c = line; *c; c++) {
            if (*c == '/') {
                has_slash = 1;
            }
        }
        if (!has_slash) {
            if (path_join(resolved, PATH_BIN_DIR, line) != 0) {
                sys_write(1, line, strlen(line));
                const char toolong[] = ": name too long\n";
                sys_write(1, toolong, sizeof(toolong) - 1);
                continue;
            }
            target = resolved;
        }

        long pid = sys_spawn(target, prog_arg);
        if (pid < 0) {
            sys_write(1, line, strlen(line));
            const char msg[] = ": command not found\n";
            sys_write(1, msg, sizeof(msg) - 1);
            continue;
        }
        sys_wait(pid);
    }

    return 0;
}
