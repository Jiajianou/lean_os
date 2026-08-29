/* user_space/shell/shell.c - /bin/sh
 *
 * M72 rewrote this. What was here was M13's shell: 109 lines that read a
 * line, split it at the first space, and spawned the first word with
 * everything after it as one string. No quoting, no exit status, no
 * variables, no way to run a file. The *other* shell - the one the README
 * describes, with arguments, quoting, redirects, a pipe and tab
 * completion - lived inside user_space/bin/gui_terminal.c, four hundred
 * lines deep in a program that draws a window. So this project had two
 * shells and neither was one: the capable one could not be spawned, could
 * not read a file, and could not be the thing that runs when something
 * else needs a command run.
 *
 * This is the real one. It reads commands from a file when given a path,
 * and from stdin otherwise, which is the whole difference between a
 * prompt and a script - and is why `#!` in kernel/proc/proc.c now works.
 *
 * What it does:
 *   - words, with '...' and "..." quoting
 *   - `;`, `&&` and `||`, with a real exit status behind them
 *   - `$?`, `$NAME`, and `NAME=value` assignment
 *   - `>`, `>>` redirects
 *   - `*` and `?` globbing, expanded here against SYS_listdir before the
 *     spawn - the Unix rule, and the reason every program on this machine
 *     gets globbing without knowing about it
 *   - `#` comments, so a script can explain itself
 *   - builtins: cd, pwd, exit, echo, set, unset
 *
 * What it deliberately does not do, each because it is a real feature
 * with a real cost and none of them is what a one-person desktop is
 * missing: job control, `&`, subshells, functions, `|` (gui_terminal has
 * one and this does not - see the milestone notes), and an *exported*
 * environment, which this kernel has no concept of at all: SYS_spawn
 * carries an argv and nothing else. Variables here are shell-local, and
 * saying so beats pretending a child can see them.
 */
#include <string.h>

#include "paths.h"
#include "syscall_wrappers.h"

#define LINE_MAX    512
#define MAX_WORDS   32
#define MAX_VARS    24
#define VAR_NAME_MAX 24
#define VAR_VAL_MAX  128

/* Bounded copy/append. This project's libc has strcpy/strcat and no
 * strlcpy/strlcat, and a shell is exactly the program where an unbounded
 * copy of user-supplied text is a bad idea. Two small helpers beat
 * adding to a library every program links for one caller. */
static void cpy(char *dst, const char *src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void cat(char *dst, const char *src, int cap) {
    int n = (int)strlen(dst);
    int i = 0;
    for (; src[i] && n + i < cap - 1; i++) {
        dst[n + i] = src[i];
    }
    dst[n + i] = '\0';
}

static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int last_status;   /* $? */
static int should_exit;
static int exit_code;

/* ---- variables --------------------------------------------------------
 *
 * Shell-local by necessity rather than by choice: this kernel's SYS_spawn
 * takes a path and an argv and there is nowhere to put an environment.
 * Making them local and documenting it is the honest version; quietly
 * setting them and having children not see them would be the other kind.
 */
static struct {
    char name[VAR_NAME_MAX];
    char value[VAR_VAL_MAX];
    int used;
} vars[MAX_VARS];

static const char *var_get(const char *name) {
    for (int i = 0; i < MAX_VARS; i++) {
        if (vars[i].used && strcmp(vars[i].name, name) == 0) {
            return vars[i].value;
        }
    }
    return "";
}

static void var_set(const char *name, const char *value) {
    int free_slot = -1;
    for (int i = 0; i < MAX_VARS; i++) {
        if (vars[i].used && strcmp(vars[i].name, name) == 0) {
            cpy(vars[i].value, value, VAR_VAL_MAX);
            return;
        }
        if (!vars[i].used && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot >= 0) {
        cpy(vars[free_slot].name, name, VAR_NAME_MAX);
        cpy(vars[free_slot].value, value, VAR_VAL_MAX);
        vars[free_slot].used = 1;
    }
}

static void var_unset(const char *name) {
    for (int i = 0; i < MAX_VARS; i++) {
        if (vars[i].used && strcmp(vars[i].name, name) == 0) {
            vars[i].used = 0;
        }
    }
}

/* ---- output helpers --------------------------------------------------- */

/* Where a builtin's output goes. 1 unless a redirect is in force.
 *
 * M72: the obvious implementation of `echo hello > file` is to dup2 the
 * file onto fd 1, run the builtin, and dup2 stdout back - which is what
 * this did first, and it worked exactly once per process. The FIRST
 * redirect in a shell landed in its file; every one after it silently
 * went to stdout, with the parse verifiably correct, the open succeeding
 * and no error anywhere. Whatever is wrong there is in the kernel's fd
 * table rather than here, and it is written up in the milestone notes as
 * an open finding.
 *
 * This sidesteps it, and is better anyway: a shell mutating its own fd 1
 * to redirect its own builtin is a strange thing to do. Spawned programs
 * still get the real dup2 treatment, because a child has to inherit the
 * redirect through its fd table and there is no other way to say so. */
static int out_fd = 1;

static void out(const char *s) {
    sys_write(out_fd, s, strlen(s));
}

/* ---- the current directory -------------------------------------------- */

static char cwd[PATH_MAX_LEN] = PATH_HOME;

/* Joins `name` onto the working directory unless it is already absolute.
 * No "." or ".." - leanfs deliberately does not store them (see
 * next_component in kernel/fs/leanfs.c), so resolving them here would be
 * this shell inventing a namespace the filesystem does not have. ".."
 * is handled textually because a shell can honestly do that much. */
static void resolve(const char *name, char *out_path) {
    if (name[0] == '/') {
        cpy(out_path, name, PATH_MAX_LEN);
        return;
    }
    if (strcmp(name, "..") == 0) {
        cpy(out_path, cwd, PATH_MAX_LEN);
        int n = (int)strlen(out_path);
        while (n > 1 && out_path[n - 1] != '/') {
            n--;
        }
        if (n > 1) {
            n--; /* drop the trailing slash, unless we are at the root */
        }
        out_path[n] = '\0';
        return;
    }
    cpy(out_path, cwd, PATH_MAX_LEN);
    if (strcmp(out_path, "/") != 0) {
        cat(out_path, "/", PATH_MAX_LEN);
    }
    cat(out_path, name, PATH_MAX_LEN);
}

/* ---- globbing ---------------------------------------------------------
 *
 * Expanded in the shell, before the spawn, which is the Unix rule and the
 * reason every program on this machine gets globbing without a line of
 * its own. A pattern that matches nothing is passed through unchanged -
 * the same choice sh makes, and the one that keeps `echo *.c` in an empty
 * directory printing something rather than nothing.
 */
static int glob_match(const char *pat, const char *name) {
    while (*pat && *name) {
        if (*pat == '*') {
            pat++;
            if (!*pat) {
                return 1;
            }
            for (const char *n = name; *n; n++) {
                if (glob_match(pat, n)) {
                    return 1;
                }
            }
            return 0;
        }
        if (*pat != '?' && *pat != *name) {
            return 0;
        }
        pat++;
        name++;
    }
    while (*pat == '*') {
        pat++;
    }
    return !*pat && !*name;
}

static int has_glob(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '*' || *p == '?') {
            return 1;
        }
    }
    return 0;
}

/* ---- parsing ----------------------------------------------------------
 *
 * One pass, in place: quotes are consumed, `$` is expanded, and the word
 * is written into the caller's buffer. Expansion happens during tokenising
 * rather than after it, so a variable holding a space does not silently
 * become two arguments - which is the behaviour that surprises people in
 * every shell that does it the other way.
 */
static int expand_var(const char **pp, char *dst, int dst_len, int cap) {
    const char *p = *pp;
    p++; /* past '$' */
    if (*p == '?') {
        char b[12];
        int n = 0, v = last_status;
        if (v == 0) {
            b[n++] = '0';
        }
        char tmp[12];
        int t = 0;
        while (v > 0) {
            tmp[t++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (t > 0) {
            b[n++] = tmp[--t];
        }
        for (int i = 0; i < n && dst_len < cap - 1; i++) {
            dst[dst_len++] = b[i];
        }
        *pp = p + 1;
        return dst_len;
    }
    char name[VAR_NAME_MAX];
    int n = 0;
    while (*p && (is_word_char(*p)) && n < VAR_NAME_MAX - 1) {
        name[n++] = *p++;
    }
    name[n] = '\0';
    const char *v = var_get(name);
    for (int i = 0; v[i] && dst_len < cap - 1; i++) {
        dst[dst_len++] = v[i];
    }
    *pp = p;
    return dst_len;
}



/* Splits one command into words, consuming quotes and expanding `$`.
 * Returns the word count, or -1 for a line this shell cannot parse.
 * `redir` comes back as the target of a `>`/`>>` (empty if none) and
 * `append` says which of the two it was. */
static int tokenise(const char *line, char words[MAX_WORDS][VAR_VAL_MAX],
                     char *redir, int *append) {
    int nwords = 0;
    redir[0] = '\0';
    *append = 0;
    const char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p || *p == '#') {
            break; /* a comment runs to the end of the line */
        }

        int want_redir = 0;
        if (*p == '>') {
            p++;
            if (*p == '>') {
                *append = 1;
                p++;
            }
            want_redir = 1;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (!*p) {
                return -1; /* "> " with nothing after it */
            }
        }

        char word[VAR_VAL_MAX];
        int wl = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '#' && *p != '>') {
            if (*p == '\'') {
                /* Single quotes are literal, including '$' - which is the
                 * one thing they are for. */
                p++;
                while (*p && *p != '\'' && wl < VAR_VAL_MAX - 1) {
                    word[wl++] = *p++;
                }
                if (*p == '\'') {
                    p++;
                }
                continue;
            }
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '$') {
                        wl = expand_var(&p, word, wl, VAR_VAL_MAX);
                    } else if (wl < VAR_VAL_MAX - 1) {
                        word[wl++] = *p++;
                    } else {
                        p++;
                    }
                }
                if (*p == '"') {
                    p++;
                }
                continue;
            }
            if (*p == '$') {
                wl = expand_var(&p, word, wl, VAR_VAL_MAX);
                continue;
            }
            if (wl < VAR_VAL_MAX - 1) {
                word[wl++] = *p++;
            } else {
                p++;
            }
        }
        word[wl] = '\0';

        if (want_redir) {
            cpy(redir, word, PATH_MAX_LEN);
            continue;
        }
        if (wl == 0) {
            continue;
        }

        /* Globbing, here rather than in the callee. A pattern that matches
         * nothing is passed through as itself. */
        if (has_glob(word)) {
            static char listing[4096];
            char dir[PATH_MAX_LEN];
            cpy(dir, cwd, PATH_MAX_LEN);
            long n = sys_listdir(dir, listing, sizeof(listing) - 1);
            int matched = 0;
            if (n > 0) {
                listing[n] = '\0';
                char name[PATH_MAX_LEN];
                int ni = 0;
                for (long i = 0; i <= n; i++) {
                    if (listing[i] == '\n' || listing[i] == '\0') {
                        name[ni] = '\0';
                        /* SYS_listdir appends '/' to directories - strip
                         * it before matching, or "*" never matches one. */
                        if (ni > 0 && name[ni - 1] == '/') {
                            name[ni - 1] = '\0';
                        }
                        if (name[0] && glob_match(word, name) && nwords < MAX_WORDS - 1) {
                            cpy(words[nwords++], name, VAR_VAL_MAX);
                            matched = 1;
                        }
                        ni = 0;
                        continue;
                    }
                    if (ni < PATH_MAX_LEN - 1) {
                        name[ni++] = listing[i];
                    }
                }
            }
            if (matched) {
                continue;
            }
        }

        if (nwords < MAX_WORDS - 1) {
            cpy(words[nwords++], word, VAR_VAL_MAX);
        }
    }
    return nwords;
}

/* ---- builtins ---------------------------------------------------------
 *
 * A builtin is a command this shell must run itself because running it in
 * a child would be pointless: `cd` in a child changes that child's
 * directory and then the child exits. Everything else is a program.
 */
static int run_builtin(int argc, char words[MAX_WORDS][VAR_VAL_MAX], int *handled) {
    *handled = 1;
    const char *cmd = words[0];

    if (strcmp(cmd, "exit") == 0) {
        should_exit = 1;
        exit_code = argc > 1 ? (int)(words[1][0] - '0') : last_status;
        return 0;
    }
    if (strcmp(cmd, "cd") == 0) {
        char target[PATH_MAX_LEN];
        resolve(argc > 1 ? words[1] : PATH_HOME, target);
        os_stat_t st;
        if (sys_stat(target, &st) != 0 || !st.is_dir) {
            out("cd: not a directory: ");
            out(target);
            out("\n");
            return 1;
        }
        cpy(cwd, target, PATH_MAX_LEN);
        return 0;
    }
    if (strcmp(cmd, "pwd") == 0) {
        out(cwd);
        out("\n");
        return 0;
    }
    if (strcmp(cmd, "echo") == 0) {
        /* A builtin rather than /bin/echo, so that `echo $VAR` prints a
         * shell variable. A spawned echo could not see one - this kernel
         * has no environment to pass. */
        for (int i = 1; i < argc; i++) {
            if (i > 1) {
                out(" ");
            }
            out(words[i]);
        }
        out("\n");
        return 0;
    }
    if (strcmp(cmd, "set") == 0) {
        for (int i = 0; i < MAX_VARS; i++) {
            if (vars[i].used) {
                out(vars[i].name);
                out("=");
                out(vars[i].value);
                out("\n");
            }
        }
        return 0;
    }
    if (strcmp(cmd, "unset") == 0) {
        for (int i = 1; i < argc; i++) {
            var_unset(words[i]);
        }
        return 0;
    }
    *handled = 0;
    return 0;
}

/* ---- running one command ---------------------------------------------- */

static int run_command(const char *cmd_text) {
    static char words[MAX_WORDS][VAR_VAL_MAX];
    char redir[PATH_MAX_LEN];
    int append = 0;

    int argc = tokenise(cmd_text, words, redir, &append);
    if (argc < 0) {
        out("sh: syntax error\n");
        return 2;
    }
    if (argc == 0) {
        return last_status; /* blank line or a pure comment changes nothing */
    }

    /* NAME=value, before anything else can mistake it for a command. Only
     * when it is the whole first word: `a=b c` assigning and then running
     * `c` is a real shell behaviour and a confusing one, and nothing here
     * needs it. */
    const char *eq = strchr(words[0], '=');
    if (eq && eq != words[0] && argc == 1) {
        char name[VAR_NAME_MAX];
        int n = (int)(eq - words[0]);
        if (n > VAR_NAME_MAX - 1) {
            n = VAR_NAME_MAX - 1;
        }
        memcpy(name, words[0], (size_t)n);
        name[n] = '\0';
        var_set(name, eq + 1);
        return 0;
    }

    /* The redirect is set up BEFORE the builtin dispatch, not after.
     *
     * `echo hello > file` is the most ordinary line a script contains and
     * `echo` is a builtin here, so a redirect applied only on the spawn
     * path would silently print to the terminal and create nothing - a
     * failure that looks like the redirect working right up until you
     * read the file. Builtins write to fd 1 like everything else, so
     * pointing fd 1 somewhere else covers them for free. */
    /* Open the redirect target once, whichever kind of command this turns
     * out to be - the file has to be created even if the command then
     * fails, which is what `> out` doing nothing but truncating means. */
    long rfd = -1;
    if (redir[0]) {
        char rpath[PATH_MAX_LEN];
        resolve(redir, rpath);
        uint32_t flags = OPEN_WRITE | OPEN_CREATE | (append ? 0u : OPEN_TRUNCATE);
        rfd = sys_open(rpath, flags);
        if (rfd < 0) {
            out("sh: cannot open ");
            out(rpath);
            out("\n");
            return 1;
        }
        if (append) {
            sys_lseek((int)rfd, 0, SEEK_END);
        }
    }

    int handled = 0;
    int rc;
    if (rfd >= 0) {
        out_fd = (int)rfd; /* builtins write here; see out_fd's comment */
    }
    rc = run_builtin(argc, words, &handled);
    out_fd = 1;
    if (handled) {
        if (rfd >= 0) {
            sys_close((int)rfd);
        }
        return rc;
    }

    /* Not a builtin: find it. A name with no '/' is looked up in /bin,
     * which is this machine's whole search path. */
    char path[PATH_MAX_LEN];
    if (strchr(words[0], '/')) {
        resolve(words[0], path);
    } else {
        cpy(path, PATH_BIN_DIR, PATH_MAX_LEN);
        cat(path, words[0], PATH_MAX_LEN);
    }

    int saved_out = -1;
    if (rfd >= 0) {
        /* A child inherits its parent's fd table, so pointing fd 1 at the
         * file here is the only way to tell it where its output goes. */
        saved_out = (int)sys_dup2(1, 9);
        sys_dup2((int)rfd, 1);
    }

    const char *argv[MAX_WORDS + 1];
    for (int i = 0; i < argc; i++) {
        argv[i] = words[i];
    }
    argv[argc] = 0;
    /* argv[1..] - sys_spawnv's vector is the arguments, not argv[0]; the
     * kernel supplies the path as argv[0] itself. Same convention
     * gui_terminal.c uses. */
    long pid = sys_spawnv(path, argc > 1 ? &argv[1] : &argv[argc]);

    if (rfd >= 0) {
        if (saved_out >= 0) {
            sys_dup2(saved_out, 1);
            sys_close(saved_out);
        }
        sys_close((int)rfd);
    }

    if (pid < 0) {
        out(words[0]);
        out(": command not found\n");
        return 127; /* the number every shell uses, so a script can test it */
    }
    long status = sys_wait((int)pid);
    return status < 0 ? 1 : (int)status;
}

/* ---- a whole line, with ; && || ---------------------------------------- */

static void run_line(char *line) {
    const char *p = line;
    char segment[LINE_MAX];
    int mode = 0; /* 0 = run, 1 = only if the last succeeded, 2 = only if it failed */

    while (*p && !should_exit) {
        int n = 0;
        int next_mode = 0;
        /* Scan to the next separator, respecting quotes so a `;` inside
         * them is data rather than syntax. */
        int in_single = 0, in_double = 0;
        while (*p) {
            if (*p == '\'' && !in_double) {
                in_single = !in_single;
            } else if (*p == '"' && !in_single) {
                in_double = !in_double;
            } else if (!in_single && !in_double) {
                if (*p == ';') {
                    p++;
                    break;
                }
                if (*p == '&' && *(p + 1) == '&') {
                    p += 2;
                    next_mode = 1;
                    break;
                }
                if (*p == '|' && *(p + 1) == '|') {
                    p += 2;
                    next_mode = 2;
                    break;
                }
            }
            if (n < LINE_MAX - 1) {
                segment[n++] = *p;
            }
            p++;
        }
        segment[n] = '\0';

        int skip = (mode == 1 && last_status != 0) || (mode == 2 && last_status == 0);
        if (!skip) {
            last_status = run_command(segment);
        }
        mode = next_mode;
    }
}

/* ---- input ------------------------------------------------------------- */

static int read_line_from_stdin(char *buf) {
    int len = 0;
    for (;;) {
        char c;
        long n = sys_read(0, &c, 1);
        if (n <= 0) {
            if (len > 0) {
                buf[len] = '\0';
                return len;
            }
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
                sys_write(1, "\b \b", 3);
            }
            continue;
        }
        if (len < LINE_MAX - 1) {
            buf[len++] = c;
            sys_write(1, &c, 1);
        }
    }
}

/* Runs every line of a file. THE reason this program exists: a shell that
 * can only read a terminal is not something another program can hand work
 * to, and `#!` in kernel/proc/proc.c resolves to exactly this path. */
static int run_script(const char *path) {
    static char text[8192];
    long n = sys_readfile(path, text, sizeof(text) - 1);
    if (n < 0) {
        out("sh: cannot read ");
        out(path);
        out("\n");
        return 127;
    }
    text[n] = '\0';

    char line[LINE_MAX];
    int li = 0;
    for (long i = 0; i <= n && !should_exit; i++) {
        if (text[i] == '\n' || text[i] == '\0') {
            line[li] = '\0';
            if (li > 0) {
                run_line(line);
            }
            li = 0;
            continue;
        }
        if (li < LINE_MAX - 1) {
            line[li++] = text[i];
        }
    }
    return should_exit ? exit_code : last_status;
}

int main(int argc, char **argv) {
    /* A path means "run this file"; nothing means "read stdin". That one
     * distinction is the whole difference between a prompt and a script,
     * and it is what makes this spawnable by something that is not a
     * person. */
    if (argc > 1 && argv[1][0]) {
        return run_script(argv[1]);
    }

    char line[LINE_MAX];
    while (!should_exit) {
        out(cwd);
        out(" $ ");
        read_line_from_stdin(line);
        run_line(line);
    }
    return exit_code;
}
