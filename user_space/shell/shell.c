/* user_space/shell/shell.c - M13's minimal shell. Reads a line, spawns
 * the first word as a program (SYS_spawn) with everything after the
 * first space as its single string "arg" (lean_os's whole argv
 * mechanism - see kernel/proc/proc.c), and waits for it before prompting
 * again. Line editing (echo, backspace) lives here in user space, not in
 * the kernel's SYS_read - that's a real tty's job in a real OS, and this
 * is the simplest thing that gives it to us without one.
 */
#include "str.h"
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

int main(const char *arg) {
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

        long pid = sys_spawn(line, prog_arg);
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
