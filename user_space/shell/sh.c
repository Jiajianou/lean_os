/* user_space/shell/sh.c - /bin/sh
 *
 * M86 rewrote this. What was here was M72's shell: a line splitter with
 * expansion done during tokenising, a fixed array of 32 words of 128
 * bytes, and a `run_line` that scanned for `;`, `&&` and `||` by hand.
 * It was an honest shell for a person at a prompt and its own header
 * said what it was not: *"job control, `&`, subshells, functions, and
 * `|`"*. M86's bullet is that list plus `if`/`while`/`for`/`case`,
 * command substitution and parameter expansion - and none of those can
 * be bolted onto a line splitter, because every one of them is a
 * *nested* structure and a line is not.
 *
 * So this is a lexer, a recursive-descent parser producing a tree, and
 * an evaluator that walks it. That is a rewrite rather than a feature,
 * and the reason is worth stating in one line: **`if` spans lines, `|`
 * spans commands, `$(...)` contains a whole program, and a `case`
 * pattern must not be expanded until it is compared.** A shell that
 * expands while it tokenises - which is what M72 did, deliberately and
 * with a good reason at the time - cannot do the last of those at all.
 *
 * ---- what it does now ------------------------------------------------
 *
 *   - `if`/`elif`/`else`/`fi`, `while`, `until`, `for x in ...`, `case`,
 *     `{ ...; }` and `( ... )`, and functions: `name() { ...; }`
 *   - pipelines of any length, `&&`, `||`, `;`, `&`, and `!`
 *   - `>`, `>>`, `<`, `2>`, `2>&1`, `<<` and `<<-` heredocs
 *   - `$var`, `${var}`, `${var:-w}` `:=` `:+` `:?` and their colon-less
 *     forms, `${#var}`, `${var#pat}` `##` `%` `%%`, `$?` `$$` `$#` `$@`
 *     `$*` `$0`..`$9` `${10}`
 *   - `$(cmd)` and `` `cmd` ``, quoting, backslashes, IFS field
 *     splitting, and globbing that can finally look in another directory
 *   - two variable namespaces, with `export` moving a name between them
 *   - builtins: cd pwd exit echo export unset set shift read test [ true
 *     false : return break continue . source eval wait env
 *
 * ---- what it deliberately does not do --------------------------------
 *
 *   - **Job control** - `jobs`, `fg`, `bg`. `&` runs a command in the
 *     background and `wait` waits for it, but there is no foreground
 *     process group to hand around, because that needs M85's pty and
 *     this arc holds the pty until M98 (a build long enough to want
 *     `^C`). See M86's own entry for that decision and what it costs.
 *   - **Arithmetic expansion** (`$((...))`) and `[[`. The milestone says
 *     so: the measure is a script somebody else wrote running, not a
 *     feature list, and neither of these is what stops one.
 *   - Aliases, `trap`, `set -e`, `local`. None has been asked for by
 *     anything.
 *
 * ---- and one thing it gained by being written in POSIX ---------------
 *
 * Every system call this file makes is POSIX: open, read, write, close,
 * dup2, pipe, fork, execve, waitpid, chdir, getcwd, opendir, readdir.
 * There is not one `sys_` wrapper left in it, and that is not tidiness -
 * it is what lets `tools/sh-test.sh` compile THIS source for the host
 * and run the fixture scripts in `tests/sh/` in under a second. The
 * shell a person types at and the shell the tests grade are the same
 * program, which is the only arrangement where the tests mean anything.
 * M75-M85 are what made that possible; before them this file could not
 * have been written this way.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>   /* M99: trap */
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "paths.h"

extern char **environ;

/* One line of input, one prompt's worth. Not a limit on a *command*:
 * a construct spanning fifty lines is fifty of these, and a word is a
 * heap string with no ceiling at all. */
#define SH_LINE_MAX 4096

/* How deep `if` inside `while` inside a function may nest before this
 * shell refuses. A limit rather than a stack overflow: the parser and
 * the evaluator both recurse, and a script that nests a hundred deep is
 * a script doing something else wrong. */
#define SH_MAX_DEPTH 64

static int last_status;      /* $? */
static int interactive;      /* a prompt, not a script - see syntax() */
/* M99: set by the `exec` builtin when it is given no command, read by
 * exec_simple, which is the only place that could act on it - see both. */
static int exec_keep_redirs;

/* ---- M99: the `set -e` family -----------------------------------------
 *
 * `set` here took every argument as a positional parameter, so `set -e`
 * did not turn on errexit - it replaced the script's arguments with the
 * single word `-e`. CPython's Modules/makesetup is `#! /bin/sh` and then
 * `set -e` on line 2, so by line 58 its own argument parser was looking
 * at `-e`, did not recognise it, and printed its usage. configure had by
 * then run 753 checks and written pyconfig.h, and stopped on the last
 * line of the last step with "makesetup failed".
 *
 * Implemented rather than accepted-and-ignored, which is the M65 rule:
 * a `set -e` that returns 0 and changes nothing is worse than an error,
 * because the script it is in was written on the understanding that a
 * failed command stops it.
 *
 *   -e  errexit. Suspended inside a condition - see errexit_suspend.
 *   -u  nounset: expanding an unset parameter is an error.
 *   -f  noglob.
 *   -x  xtrace: write each command to stderr, `+ ` first.
 *   -o  by name, plus `posix`, which is a no-op here because POSIX
 *       behaviour is the only behaviour this shell has - there is no
 *       second mode for it to switch out of.
 *
 * An option this shell does not have is an ERROR rather than a no-op,
 * for the same reason as above. */
static int opt_errexit;
static int opt_nounset;
static int opt_noglob;
static int opt_xtrace;
/* Non-zero while a command's status is being *asked about* rather than
 * relied on: an `if`/`while`/`until` condition, the left of `&&`/`||`,
 * anything under `!`. POSIX exempts every one of them from errexit, and
 * a shell that did not would exit on the first `if test -f x`. */
static int errexit_suspend;
static int shell_pid;        /* $$ */

/* ---- flow control ------------------------------------------------------
 *
 * `break`, `continue`, `return` and `exit` all mean "stop walking the
 * tree and unwind to a particular place". A flag the evaluator checks
 * after every child, rather than longjmp: the evaluator has to close
 * descriptors and reap children on the way out, and a jump that skips
 * that leaks both. */
enum { FLOW_NONE = 0, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN, FLOW_EXIT };
static int flow;
static int flow_levels;      /* `break 2` */
static int exit_code;

/* ---- allocation --------------------------------------------------------
 *
 * A parsed command is a tree of small nodes and strings that all die
 * together, so they come from an arena and go back in one call. What
 * does NOT die together is a function body: `f() { ...; }` at a prompt
 * outlives the line that defined it. An arena holding one is retained
 * rather than freed, and that is the whole of this shell's memory
 * management. A script defining a bounded number of functions leaks a
 * bounded amount, once. */
typedef struct Chunk {
    struct Chunk *next;
    size_t used, cap;
    char *mem;
} Chunk;

typedef struct {
    Chunk *head;
    int retained;
} Arena;

static Arena *cur_arena;

static void die_oom(void) {
    write(2, "sh: out of memory\n", 18);
    _exit(1);
}

static void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15u) & ~(size_t)15u;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 8192 ? n : 8192;
        Chunk *c = (Chunk *)malloc(sizeof(Chunk));
        if (!c) {
            die_oom();
        }
        c->mem = (char *)malloc(cap);
        if (!c->mem) {
            die_oom();
        }
        c->used = 0;
        c->cap = cap;
        c->next = a->head;
        a->head = c;
    }
    void *p = a->head->mem + a->head->used;
    a->head->used += n;
    return p;
}

static void arena_free(Arena *a) {
    if (a->retained) {
        return; /* a function body lives here - see the note above */
    }
    Chunk *c = a->head;
    while (c) {
        Chunk *next = c->next;
        free(c->mem);
        free(c);
        c = next;
    }
    a->head = 0;
}

static char *astrndup(const char *s, size_t n) {
    char *p = (char *)arena_alloc(cur_arena, n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *astrdup(const char *s) {
    return astrndup(s, strlen(s));
}

/* ---- a string that grows ----------------------------------------------
 *
 * Malloc'd rather than from the arena: these are built during *execution*
 * and handed to execve, and the arena they would have come from belongs
 * to the parse. */
typedef struct {
    char *p;
    size_t len, cap;
} Sbuf;

static void sb_init(Sbuf *b) {
    b->p = 0;
    b->len = b->cap = 0;
}

static void sb_putn(Sbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        char *p = (char *)realloc(b->p, cap);
        if (!p) {
            die_oom();
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_putc(Sbuf *b, char c) {
    sb_putn(b, &c, 1);
}

static void sb_puts(Sbuf *b, const char *s) {
    sb_putn(b, s, strlen(s));
}

static void sb_free(Sbuf *b) {
    free(b->p);
    sb_init(b);
}

/* A vector of malloc'd strings: the words of one command after
 * expansion, or the fields one expansion produced. */
typedef struct {
    char **v;
    int n, cap;
} Vec;

static void vec_init(Vec *w) {
    w->v = 0;
    w->n = w->cap = 0;
}

static void vec_push(Vec *w, char *s) {
    if (w->n + 1 >= w->cap) {
        w->cap = w->cap ? w->cap * 2 : 8;
        char **v = (char **)realloc(w->v, sizeof(char *) * (size_t)w->cap);
        if (!v) {
            die_oom();
        }
        w->v = v;
    }
    w->v[w->n++] = s;
    w->v[w->n] = 0;
}

static void vec_free(Vec *w) {
    for (int i = 0; i < w->n; i++) {
        free(w->v[i]);
    }
    free(w->v);
    vec_init(w);
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (!p) {
        die_oom();
    }
    memcpy(p, s, n);
    return p;
}

static void out_fd_str(int fd, const char *s) {
    write(fd, s, strlen(s));
}

static void errmsg(const char *a, const char *b, const char *c) {
    out_fd_str(2, "sh: ");
    out_fd_str(2, a);
    if (b) {
        out_fd_str(2, b);
    }
    if (c) {
        out_fd_str(2, c);
    }
    out_fd_str(2, "\n");
}

static char *num_to_str(long v) {
    static char b[24];
    int i = (int)sizeof(b) - 1;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[i] = '\0';
    if (u == 0) {
        b[--i] = '0';
    }
    while (u) {
        b[--i] = (char)('0' + (int)(u % 10));
        u /= 10;
    }
    if (neg) {
        b[--i] = '-';
    }
    return b + i;
}

/* ---- two namespaces, and the export that moves a name between them ----
 *
 * M72's note said this plainly: *"`export` is accepted and does nothing
 * but assign, because this shell has one namespace - the reason POSIX
 * has two is subshells and functions, and it has neither."* It has both
 * now, so the sentence became a to-do and this is it.
 *
 * A shell variable lives here. An *exported* one lives here AND in the
 * process environment, which is what a child inherits - so the split is
 * not bookkeeping: `x=1; sh -c 'echo $x'` prints nothing and
 * `export x; sh -c 'echo $x'` prints 1, and that difference is the whole
 * point of the feature.
 *
 * The environment is kept as the authority for exported names rather
 * than duplicated, so setenv/getenv stay the one path to a child. */
typedef struct Var {
    struct Var *next;
    char *name;
    char *value;
    int exported;
    int readonly;
} Var;

static Var *vars;

static Var *var_find(const char *name) {
    for (Var *v = vars; v; v = v->next) {
        if (strcmp(v->name, name) == 0) {
            return v;
        }
    }
    return 0;
}

static const char *var_get(const char *name) {
    Var *v = var_find(name);
    if (v) {
        return v->value ? v->value : "";
    }
    const char *e = getenv(name);
    return e ? e : "";
}

static int var_is_set(const char *name) {
    Var *v = var_find(name);
    if (v) {
        return v->value != 0;
    }
    return getenv(name) != 0;
}

static void var_set(const char *name, const char *value) {
    Var *v = var_find(name);
    if (!v) {
        v = (Var *)malloc(sizeof(Var));
        if (!v) {
            die_oom();
        }
        v->name = xstrdup(name);
        v->value = 0;
        v->exported = getenv(name) != 0; /* it came from our own environment */
        v->readonly = 0;
        v->next = vars;
        vars = v;
    }
    if (v->readonly) {
        errmsg(name, ": is read only", 0);
        return;
    }
    free(v->value);
    v->value = xstrdup(value);
    if (v->exported) {
        setenv(name, value, 1);
    }
}

static void var_export(const char *name) {
    Var *v = var_find(name);
    if (!v) {
        var_set(name, var_get(name));
        v = var_find(name);
    }
    v->exported = 1;
    setenv(name, v->value ? v->value : "", 1);
}

static void var_unset(const char *name) {
    Var **pp = &vars;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            Var *dead = *pp;
            *pp = dead->next;
            free(dead->name);
            free(dead->value);
            free(dead);
            break;
        }
        pp = &(*pp)->next;
    }
    unsetenv(name);
}

/* ---- positional parameters -------------------------------------------- */

static char **pos_params;   /* $1 is pos_params[0] */
static int pos_count;
static char *script_name = (char *)"sh";   /* $0 */

static void set_positional(char **argv, int n) {
    for (int i = 0; i < pos_count; i++) {
        free(pos_params[i]);
    }
    free(pos_params);
    pos_params = 0;
    pos_count = 0;
    if (n > 0) {
        pos_params = (char **)malloc(sizeof(char *) * (size_t)n);
        if (!pos_params) {
            die_oom();
        }
        for (int i = 0; i < n; i++) {
            pos_params[i] = xstrdup(argv[i]);
        }
        pos_count = n;
    }
}

/* ---- globbing ----------------------------------------------------------
 *
 * M72's version matched names in the working directory only, because
 * SYS_listdir was what it had. This one splits the pattern at its last
 * `/` and reads whichever directory that names, so a pattern with a
 * directory in front of it finally means what it says - which matters
 * here rather than as polish, because a script somebody else wrote has a
 * pattern under `$srcdir` in its first ten lines.
 *
 * Matching is still the two-metacharacter kind: `*` and `?`, no
 * character classes. `[a-z]` is the next thing a real script wants and
 * nothing has asked for it yet.
 *
 * A pattern that matches nothing is left alone, which is what sh does
 * and what keeps `echo *.c` in an empty directory printing something.
 */
/* ---- M99: [...] and backslash --------------------------------------
 *
 * This matcher understood `*` and `?` and nothing else, which is enough
 * for `*.c` and is not a POSIX pattern. The two missing pieces are the
 * bracket expression and the backslash escape, and both are used by
 * `case` far more than by filename globbing - `case` is where a shell
 * script does its branching, and a generated script brackets everything.
 *
 * What found it: CPython's configure asking whether a directory name is
 * absolute -
 *
 *     case $ac_val in
 *       [\\/$]* | ?:[\\/]* ) continue;;
 *     esac
 *     as_fn_error $? "expected an absolute directory name for --$ac_var"
 *
 * - and the answer here was always "no", so configure stopped on
 * `--bindir: ${exec_prefix}/bin`, a value it had produced itself and
 * was about to expand.
 *
 * Returns a pointer past the closing `]`, or 0 if this is not a bracket
 * expression at all - an unmatched `[` is a literal `[`, which is what
 * POSIX says and what keeps `echo [` printing a bracket. */
static const char *bracket_end(const char *pat) {
    const char *p = pat + 1;
    if (*p == '!' || *p == '^') {
        p++;
    }
    if (*p == ']') {
        p++; /* a `]` first is the character, not the end */
    }
    while (*p && *p != ']') {
        if (*p == '\\' && p[1]) {
            p++;
        }
        p++;
    }
    return *p == ']' ? p + 1 : 0;
}

/* Does `c` belong to the bracket expression starting at `pat`? */
static int bracket_has(const char *pat, char c) {
    const char *p = pat + 1;
    int negate = 0;
    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }
    int found = 0;
    int first = 1;
    while (*p && (*p != ']' || first)) {
        first = 0;
        char lo = *p;
        if (lo == '\\' && p[1]) {
            p++;
            lo = *p;
        }
        p++;
        /* A `-` that is not last is a range. `[a-]` and `[-a]` are the
         * two places a literal hyphen is written, and both fall out of
         * checking that something follows it. */
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            char hi = *p;
            if (hi == '\\' && p[1]) {
                p++;
                hi = *p;
            }
            p++;
            if ((unsigned char)c >= (unsigned char)lo &&
                (unsigned char)c <= (unsigned char)hi) {
                found = 1;
            }
        } else if (c == lo) {
            found = 1;
        }
    }
    return negate ? !found : found;
}

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
        if (*pat == '[') {
            const char *end = bracket_end(pat);
            if (end) {
                if (!bracket_has(pat, *name)) {
                    return 0;
                }
                pat = end;
                name++;
                continue;
            }
            /* Unmatched `[` - a literal one, handled below. */
        }
        if (*pat == '\\' && pat[1]) {
            /* A backslash quotes the next character, so `\*` is a star
             * and not "anything". */
            pat++;
            if (*pat != *name) {
                return 0;
            }
            pat++;
            name++;
            continue;
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
        /* A `[` counts only when it closes - see bracket_end. Otherwise
         * `echo [` would become a pathname expansion that matches
         * nothing and prints itself, which is the same answer by a
         * longer road, and `find . -name x[` would not be. */
        if (*p == '[' && bracket_end(p)) {
            return 1;
        }
    }
    return 0;
}

static int str_cmp_qsort(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Appends every name matching `pat` to `out`, or `pat` itself if none
 * match. Sorted, because readdir order is the filesystem's business and
 * a script that lists a directory twice should see the same order. */
static void glob_expand(const char *pat, Vec *out) {
    char *slash = strrchr(pat, '/');
    char dirbuf[PATH_MAX_LEN];
    const char *dir;
    const char *base;

    if (slash) {
        size_t n = (size_t)(slash - pat);
        if (n == 0) {
            dirbuf[0] = '/';
            dirbuf[1] = '\0';
        } else {
            if (n >= sizeof(dirbuf)) {
                n = sizeof(dirbuf) - 1;
            }
            memcpy(dirbuf, pat, n);
            dirbuf[n] = '\0';
        }
        dir = dirbuf;
        base = slash + 1;
    } else {
        dir = ".";
        base = pat;
    }

    /* A glob in the DIRECTORY part is not expanded - matching every
     * directory and then every name under each would need a recursive
     * walk, and nothing has asked for one. Passed through unchanged,
     * which is the no-match rule applied to the same string. */
    if (has_glob(dir)) {
        vec_push(out, xstrdup(pat));
        return;
    }

    DIR *d = opendir(dir);
    if (!d) {
        vec_push(out, xstrdup(pat));
        return;
    }

    Vec hits;
    vec_init(&hits);
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        /* A leading dot is matched only by a pattern that has one, which
         * is the rule every shell follows and the reason `rm *` does not
         * delete your configuration. */
        if (e->d_name[0] == '.' && base[0] != '.') {
            continue;
        }
        if (!glob_match(base, e->d_name)) {
            continue;
        }
        Sbuf b;
        sb_init(&b);
        if (slash) {
            sb_putn(&b, pat, (size_t)(slash - pat) + 1);
        }
        sb_puts(&b, e->d_name);
        vec_push(&hits, b.p);
    }
    closedir(d);

    if (hits.n == 0) {
        vec_free(&hits);
        vec_push(out, xstrdup(pat));
        return;
    }
    qsort(hits.v, (size_t)hits.n, sizeof(char *), str_cmp_qsort);
    for (int i = 0; i < hits.n; i++) {
        vec_push(out, hits.v[i]);
    }
    free(hits.v);
}

/* ---- the lexer ---------------------------------------------------------
 *
 * Words come out of here RAW - quotes still in them, `$` unexpanded,
 * `$(...)` intact as one token however many spaces and semicolons it
 * contains. That is the change M86 turns on. M72 expanded during
 * tokenising, which is a defensible thing for a line splitter to do and
 * makes three of this milestone's features impossible:
 *
 *   - a `case` pattern must reach the matcher unexpanded, or `case $x in
 *     *.c)` compares against whatever `*.c` happened to glob to;
 *   - a heredoc body is expanded once, when it is written to the pipe,
 *     not when the line it sits on is read;
 *   - and a word must know which of its characters were quoted, because
 *     `"$x"` is one field and `$x` is however many the value splits into.
 *
 * So expansion moves to execution time, where POSIX puts it, and this
 * only has to find where one word ends.
 */
enum { T_EOF, T_WORD, T_IONUM, T_OP };

typedef struct {
    int type;
    char *text;
    int quoted;   /* any part of this word was inside quotes */
} Tok;

typedef struct Redir {
    struct Redir *next;       /* this command's list of redirects */
    /* And a second list, threaded through the same objects: the heredocs
     * whose bodies have not been read yet. Two links rather than one
     * reused, because a heredoc is on both lists at once - its command's,
     * and the lexer's pending queue - and a single `next` walked as both
     * is the sort of bug that produces a shell that works until a line
     * has two redirects on it. */
    struct Redir *here_next;
    int fd;         /* the descriptor being redirected */
    int kind;       /* RD_* below */
    char *word;     /* target, or the heredoc's delimiter then its body */
    int expand;     /* heredocs only: an unquoted delimiter means expand */
    int strip;      /* heredocs only: `<<-` strips leading tabs */
} Redir;

enum { RD_IN, RD_OUT, RD_APPEND, RD_DUP_OUT, RD_DUP_IN, RD_HEREDOC };

typedef struct {
    const char *src;
    size_t i;
    int incomplete;       /* input ended inside a quote or a construct */
    Redir *pending_here;  /* heredocs whose bodies the next newline collects */
} Lexer;

static Lexer lx;

static int is_op_char(char c) {
    return c == ';' || c == '&' || c == '|' || c == '<' || c == '>' ||
           c == '(' || c == ')' || c == '\n';
}

static int is_blank(char c) {
    return c == ' ' || c == '\t';
}

static int is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Everything between a `<<WORD` and a line that is exactly WORD.
 *
 * Collected when the newline after the redirect is consumed rather than
 * when the redirect is parsed, because that is where the body starts -
 * `cat <<A; cat <<B` is legal and the two bodies follow in order on the
 * lines after it. */
static void gather_heredocs(void) {
    Redir *queue = lx.pending_here;
    lx.pending_here = 0;
    while (queue) {
        {
            Redir *h = queue;
            queue = h->here_next;
            h->here_next = 0;
            const char *delim = h->word;
            int strip_tabs = h->strip;

            Sbuf body;
            sb_init(&body);
            for (;;) {
                if (!lx.src[lx.i]) {
                    /* At a prompt this means "keep typing"; in a script it
                     * means the heredoc was never closed, and the body so
                     * far is the honest thing to hand over. */
                    lx.incomplete = 1;
                    break;
                }
                size_t start = lx.i;
                while (lx.src[lx.i] && lx.src[lx.i] != '\n') {
                    lx.i++;
                }
                size_t len = lx.i - start;
                const char *line = lx.src + start;
                if (strip_tabs) {
                    while (len > 0 && *line == '\t') {
                        line++;
                        len--;
                    }
                }
                if (lx.src[lx.i] == '\n') {
                    lx.i++;
                }
                if (strlen(delim) == len && memcmp(line, delim, len) == 0) {
                    break;
                }
                sb_putn(&body, line, len);
                sb_putc(&body, '\n');
            }
            h->word = astrdup(body.p ? body.p : "");
            sb_free(&body);
        }
    }
}

/* One token. Words keep their quotes; operators are their own text. */
static Tok lex_next(void) {
    Tok t;
    t.type = T_EOF;
    t.text = 0;
    t.quoted = 0;

    for (;;) {
        while (is_blank(lx.src[lx.i])) {
            lx.i++;
        }
        /* A backslash-newline is not input at all - it is how a long
         * command is written on two lines. */
        if (lx.src[lx.i] == '\\' && lx.src[lx.i + 1] == '\n') {
            lx.i += 2;
            continue;
        }
        if (lx.src[lx.i] == '#') {
            while (lx.src[lx.i] && lx.src[lx.i] != '\n') {
                lx.i++;
            }
            continue;
        }
        break;
    }

    char c = lx.src[lx.i];
    if (!c) {
        return t;
    }

    if (c == '\n') {
        lx.i++;
        gather_heredocs();
        t.type = T_OP;
        t.text = astrdup("\n");
        return t;
    }

    if (is_op_char(c)) {
        const char *two[] = { "&&", "||", ";;", "<<", ">>", ">&", "<&", ">|", 0 };
        for (int i = 0; two[i]; i++) {
            if (c == two[i][0] && lx.src[lx.i + 1] == two[i][1]) {
                lx.i += 2;
                if (strcmp(two[i], "<<") == 0 && lx.src[lx.i] == '-') {
                    lx.i++;
                    t.type = T_OP;
                    t.text = astrdup("<<-");
                    return t;
                }
                t.type = T_OP;
                t.text = astrdup(two[i]);
                return t;
            }
        }
        lx.i++;
        char one[2] = { c, '\0' };
        t.type = T_OP;
        t.text = astrdup(one);
        return t;
    }

    /* A word. Everything up to an unquoted blank or operator, with the
     * quoting kept so that expansion can tell the parts apart. */
    Sbuf w;
    sb_init(&w);
    int quoted = 0;
    while ((c = lx.src[lx.i]) != '\0') {
        if (is_blank(c) || is_op_char(c)) {
            break;
        }
        if (c == '\\') {
            if (lx.src[lx.i + 1] == '\n') {
                lx.i += 2;
                continue;
            }
            if (!lx.src[lx.i + 1]) {
                lx.incomplete = 1;
                lx.i++;
                break;
            }
            sb_putc(&w, '\\');
            sb_putc(&w, lx.src[lx.i + 1]);
            lx.i += 2;
            quoted = 1;
            continue;
        }
        if (c == '\'') {
            quoted = 1;
            sb_putc(&w, c);
            lx.i++;
            while (lx.src[lx.i] && lx.src[lx.i] != '\'') {
                sb_putc(&w, lx.src[lx.i++]);
            }
            if (!lx.src[lx.i]) {
                lx.incomplete = 1;
                break;
            }
            sb_putc(&w, '\'');
            lx.i++;
            continue;
        }
        if (c == '"') {
            quoted = 1;
            sb_putc(&w, c);
            lx.i++;
            while (lx.src[lx.i] && lx.src[lx.i] != '"') {
                if (lx.src[lx.i] == '\\' && lx.src[lx.i + 1]) {
                    sb_putc(&w, lx.src[lx.i]);
                    sb_putc(&w, lx.src[lx.i + 1]);
                    lx.i += 2;
                    continue;
                }
                /* ---- M99: a substitution inside a quoted string -----
                 *
                 * `"..."` ends at the next `"` - except inside a
                 * `` `...` `` or a `$(...)`, where the quoting starts
                 * again from scratch. POSIX says so, and autoconf
                 * depends on it in the line that defines every
                 * HAVE_ macro a configure run produces:
                 *
                 *   printf "%s\n" "#define `printf "%s\n" \
                 *     "HAVE_$ac_header" | $as_tr_cpp` 1" >>confdefs.h
                 *
                 * Reading that as "quoted string, then bare text" ends
                 * the string at the `"` in `"%s\n"` and the rest of the
                 * line becomes several words. What it produced here was
                 * not an error: confdefs.h got a line reading
                 * `#define ` + the whole unexecuted pipeline, and the
                 * first compile after it failed with "macro name must
                 * be an identifier" - 6,357 lines into config.log,
                 * about a header check that had said `yes`.
                 *
                 * Copied across whole, with depth counting for `$(`,
                 * so it reaches expansion time intact and is parsed
                 * there by a lexer that starts fresh - which is exactly
                 * what "the quoting starts again" means. */
                if (lx.src[lx.i] == '`') {
                    sb_putc(&w, lx.src[lx.i++]);
                    while (lx.src[lx.i] && lx.src[lx.i] != '`') {
                        if (lx.src[lx.i] == '\\' && lx.src[lx.i + 1]) {
                            sb_putc(&w, lx.src[lx.i]);
                            lx.i++;
                        }
                        sb_putc(&w, lx.src[lx.i++]);
                    }
                    if (lx.src[lx.i]) {
                        sb_putc(&w, lx.src[lx.i++]);
                    }
                    continue;
                }
                if (lx.src[lx.i] == '$' && lx.src[lx.i + 1] == '(') {
                    int depth = 0;
                    sb_putc(&w, lx.src[lx.i++]); /* $ */
                    for (;;) {
                        char d = lx.src[lx.i];
                        if (!d) {
                            break;
                        }
                        if (d == '(') {
                            depth++;
                        } else if (d == ')') {
                            depth--;
                        }
                        sb_putc(&w, d);
                        lx.i++;
                        if (depth == 0) {
                            break;
                        }
                    }
                    continue;
                }
                sb_putc(&w, lx.src[lx.i++]);
            }
            if (!lx.src[lx.i]) {
                lx.incomplete = 1;
                break;
            }
            sb_putc(&w, '"');
            lx.i++;
            continue;
        }
        if (c == '`') {
            sb_putc(&w, c);
            lx.i++;
            while (lx.src[lx.i] && lx.src[lx.i] != '`') {
                sb_putc(&w, lx.src[lx.i++]);
            }
            if (!lx.src[lx.i]) {
                lx.incomplete = 1;
                break;
            }
            sb_putc(&w, '`');
            lx.i++;
            continue;
        }
        if (c == '$' && (lx.src[lx.i + 1] == '(' || lx.src[lx.i + 1] == '{')) {
            /* `$(...)` holds a whole command and `${...}` a whole
             * expansion, either of which may contain the characters this
             * loop would otherwise stop at. Copied across whole, counting
             * depth, so the word survives intact to expansion time. */
            char open = lx.src[lx.i + 1];
            char close = open == '(' ? ')' : '}';
            int depth = 0;
            sb_putc(&w, '$');
            lx.i++;
            for (;;) {
                char d = lx.src[lx.i];
                if (!d) {
                    lx.incomplete = 1;
                    break;
                }
                if (d == open) {
                    depth++;
                } else if (d == close) {
                    depth--;
                }
                sb_putc(&w, d);
                lx.i++;
                if (depth == 0) {
                    break;
                }
            }
            continue;
        }
        sb_putc(&w, c);
        lx.i++;
    }

    t.type = T_WORD;
    t.text = astrdup(w.p ? w.p : "");
    t.quoted = quoted;
    sb_free(&w);

    /* `2>file`: a bare number stuck to a redirect is which descriptor is
     * being redirected, not a word. */
    if (!quoted && t.text[0] >= '0' && t.text[0] <= '9') {
        int all_digits = 1;
        for (const char *p = t.text; *p; p++) {
            if (*p < '0' || *p > '9') {
                all_digits = 0;
                break;
            }
        }
        if (all_digits && (lx.src[lx.i] == '<' || lx.src[lx.i] == '>')) {
            t.type = T_IONUM;
        }
    }
    return t;
}

/* ---- the tree ----------------------------------------------------------
 *
 * One node kind per thing the grammar can produce. `if` inside `while`
 * inside a function is three nodes deep and evaluates by recursion, which
 * is the shape the old line splitter could not have however many
 * special cases were added to it.
 */
enum {
    N_SIMPLE, N_PIPE, N_AND, N_OR, N_SEQ, N_NOT, N_BG,
    N_IF, N_WHILE, N_UNTIL, N_FOR, N_CASE, N_SUBSHELL, N_GROUP, N_FUNC
};

typedef struct CaseItem {
    struct CaseItem *next;
    char **pats;
    int npats;
    struct Node *body;
} CaseItem;

typedef struct Node {
    int kind;
    struct Node *left, *right;   /* pipe/and/or/seq, and cond/body elsewhere */
    struct Node *third;          /* else branch */
    Redir *redirs;
    char **words;                /* simple: the command; for: the list */
    int nwords;
    char **assigns;              /* simple: NAME=value seen before the command */
    int nassigns;
    char *name;                  /* for-variable, function name */
    CaseItem *cases;
} Node;

static Node *node_new(int kind) {
    Node *n = (Node *)arena_alloc(cur_arena, sizeof(Node));
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    return n;
}

/* ---- the parser --------------------------------------------------------
 *
 * Recursive descent over one token of lookahead, in the shape POSIX's own
 * grammar has:
 *
 *   list     := and_or ( (';' | '&' | newline) and_or )*
 *   and_or   := pipeline ( ('&&' | '||') pipeline )*
 *   pipeline := ['!'] command ( '|' command )*
 *   command  := compound | simple
 *
 * `parse_error` is set once and checked everywhere rather than returned
 * through every frame: a shell reports the first syntax error and stops,
 * and threading a status through twenty functions to say so would be
 * more code than the parser.
 */
static Tok tok;          /* one token of lookahead */
static int parse_error;
static int parse_depth;

static void advance(void) {
    tok = lex_next();
}

static int tok_is_op(const char *s) {
    return tok.type == T_OP && strcmp(tok.text, s) == 0;
}

/* A reserved word is only reserved where a command could start, and only
 * if it was written unquoted - `"if"` is a command named if. */
static int tok_is_word(const char *s) {
    return tok.type == T_WORD && !tok.quoted && strcmp(tok.text, s) == 0;
}

static void syntax(const char *what) {
    if (parse_error) {
        return;
    }
    parse_error = 1;
    /* Running out of input in the middle of a construct is not the same
     * error at a prompt as it is in a script. At a prompt it means the
     * `fi` is on the next line and the loop should ask for it; in a file
     * there is no next line and it is exactly the error it looks like. */
    if (tok.type == T_EOF) {
        lx.incomplete = 1;
        if (interactive) {
            return;
        }
    }
    errmsg("syntax error near ", what, 0);
}

static void skip_newlines(void) {
    while (tok_is_op("\n")) {
        advance();
    }
}

static Node *parse_list(const char *const *terminators);
static Node *parse_and_or(void);
static Node *parse_command(void);

static int is_terminator(const char *const *terms) {
    if (!terms) {
        return 0;
    }
    for (int i = 0; terms[i]; i++) {
        if (tok.type == T_WORD && !tok.quoted && strcmp(tok.text, terms[i]) == 0) {
            return 1;
        }
        if (tok.type == T_OP && strcmp(tok.text, terms[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The quotes come off a heredoc's delimiter, and only there.
 *
 * Everywhere else a word keeps its quotes until expansion, which is the
 * whole design - but a delimiter is never expanded, it is compared, and
 * `<<'EOF'` has to match a line reading `EOF`. It did not: the quoted
 * form ran to the end of the file, swallowing the rest of the script as
 * its body, which is a failure that looks like the parser losing the
 * plot rather than like a string compare. */
static char *unquote(const char *raw) {
    Sbuf b;
    sb_init(&b);
    for (const char *p = raw; *p; p++) {
        if (*p == '\'' || *p == '"') {
            continue;
        }
        if (*p == '\\' && p[1]) {
            p++;
        }
        sb_putc(&b, *p);
    }
    char *out = astrdup(b.p ? b.p : "");
    sb_free(&b);
    return out;
}

/* [n]< file, [n]> file, >>, <<, >&n - collected wherever they appear in a
 * simple command, which is anywhere, because `> out echo hi` is legal and
 * scripts written by people who know that exist. */
static Redir *parse_redirect(void) {
    int fd = -1;
    if (tok.type == T_IONUM) {
        fd = atoi(tok.text);
        advance();
    }
    if (tok.type != T_OP) {
        return 0;
    }
    int kind;
    if (strcmp(tok.text, "<") == 0) {
        kind = RD_IN;
    } else if (strcmp(tok.text, ">") == 0 || strcmp(tok.text, ">|") == 0) {
        kind = RD_OUT;
    } else if (strcmp(tok.text, ">>") == 0) {
        kind = RD_APPEND;
    } else if (strcmp(tok.text, ">&") == 0) {
        kind = RD_DUP_OUT;
    } else if (strcmp(tok.text, "<&") == 0) {
        kind = RD_DUP_IN;
    } else if (strcmp(tok.text, "<<") == 0 || strcmp(tok.text, "<<-") == 0) {
        kind = RD_HEREDOC;
    } else {
        return 0;
    }
    int strip = tok.type == T_OP && strcmp(tok.text, "<<-") == 0;
    advance();
    if (tok.type != T_WORD) {
        syntax("a redirect with no target");
        return 0;
    }

    Redir *r = (Redir *)arena_alloc(cur_arena, sizeof(Redir));
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->word = tok.text;
    r->strip = strip;
    r->fd = fd >= 0 ? fd : (kind == RD_IN || kind == RD_HEREDOC || kind == RD_DUP_IN ? 0 : 1);
    if (kind == RD_HEREDOC) {
        /* An unquoted delimiter means the body gets expanded; a quoted
         * one means it does not, which is how a script writes a literal
         * `$` into a file. */
        r->expand = !tok.quoted;
        r->word = unquote(r->word);
        /* M99: appended, not prepended. Two here-documents on one line
         * are filled in the ORDER THEY APPEAR - `cat <<A; cat <<B`
         * reads A's body first - and a stack read them backwards, so
         * B's delimiter was hunted for in A's body and swallowed both.
         * Never noticed because nothing in this tree had ever written
         * two on one line; configure writes them constantly. */
        r->here_next = 0;
        Redir **tail = &lx.pending_here;
        while (*tail) {
            tail = &(*tail)->here_next;
        }
        *tail = r;
    }
    advance();
    return r;
}

static void append_redir(Node *n, Redir *r) {
    Redir **pp = &n->redirs;
    while (*pp) {
        pp = &(*pp)->next;
    }
    *pp = r;
}

static char **words_push(char **arr, int *n, char *w) {
    char **v = (char **)arena_alloc(cur_arena, sizeof(char *) * (size_t)(*n + 2));
    for (int i = 0; i < *n; i++) {
        v[i] = arr[i];
    }
    v[*n] = w;
    v[*n + 1] = 0;
    (*n)++;
    return v;
}

/* NAME=value, and only in the leading run of words - `a=b c=d cmd` is
 * two assignments and a command, `cmd a=b` is a command with an argument
 * that happens to have an `=` in it. */
static int is_assignment(const char *s) {
    if (!s[0] || (s[0] >= '0' && s[0] <= '9')) {
        return 0;
    }
    for (const char *p = s; *p; p++) {
        if (*p == '=') {
            return p != s;
        }
        if (!is_name_char(*p)) {
            return 0;
        }
    }
    return 0;
}

static Node *parse_simple(void) {
    Node *n = node_new(N_SIMPLE);
    int leading = 1;
    for (;;) {
        if (tok.type == T_IONUM ||
            (tok.type == T_OP && (strcmp(tok.text, "<") == 0 || strcmp(tok.text, ">") == 0 ||
                                  strcmp(tok.text, ">>") == 0 || strcmp(tok.text, "<<") == 0 ||
                                  strcmp(tok.text, "<<-") == 0 || strcmp(tok.text, ">&") == 0 ||
                                  strcmp(tok.text, "<&") == 0 || strcmp(tok.text, ">|") == 0))) {
            Redir *r = parse_redirect();
            if (!r) {
                return n;
            }
            append_redir(n, r);
            continue;
        }
        if (tok.type != T_WORD) {
            break;
        }
        /* `x='a b'` is an assignment even though the word is quoted -
         * what must be unquoted is the NAME, and is_assignment only
         * accepts name characters before the `=`, so a quote anywhere in
         * that part already disqualifies it. Requiring the whole word to
         * be unquoted made every assignment of a quoted value into a
         * command, which is the first thing the fixture script caught. */
        if (leading && is_assignment(tok.text)) {
            n->assigns = words_push(n->assigns, &n->nassigns, tok.text);
            advance();
            continue;
        }
        leading = 0;
        n->words = words_push(n->words, &n->nwords, tok.text);
        advance();
    }
    if (n->nwords == 0 && n->nassigns == 0 && !n->redirs) {
        return 0;
    }
    return n;
}

static Node *parse_if(void) {
    advance(); /* if */
    static const char *const then_term[] = { "then", 0 };
    Node *n = node_new(N_IF);
    n->left = parse_list(then_term);
    if (!tok_is_word("then")) {
        syntax("`if` with no `then`");
        return n;
    }
    advance();
    static const char *const body_term[] = { "elif", "else", "fi", 0 };
    n->right = parse_list(body_term);
    if (tok_is_word("elif")) {
        /* An `elif` chain is an `if` in the else branch, which is what it
         * means and saves the evaluator a case. */
        n->third = parse_if();
        return n;
    }
    if (tok_is_word("else")) {
        advance();
        static const char *const else_term[] = { "fi", 0 };
        n->third = parse_list(else_term);
    }
    if (!tok_is_word("fi")) {
        syntax("`if` with no `fi`");
        return n;
    }
    advance();
    return n;
}

static Node *parse_while(int until) {
    advance(); /* while | until */
    Node *n = node_new(until ? N_UNTIL : N_WHILE);
    static const char *const do_term[] = { "do", 0 };
    n->left = parse_list(do_term);
    if (!tok_is_word("do")) {
        syntax("a loop with no `do`");
        return n;
    }
    advance();
    static const char *const done_term[] = { "done", 0 };
    n->right = parse_list(done_term);
    if (!tok_is_word("done")) {
        syntax("a loop with no `done`");
        return n;
    }
    advance();
    return n;
}

static Node *parse_for(void) {
    advance(); /* for */
    if (tok.type != T_WORD) {
        syntax("`for` with no variable");
        return 0;
    }
    Node *n = node_new(N_FOR);
    n->name = tok.text;
    advance();
    skip_newlines();
    if (tok_is_word("in")) {
        advance();
        while (tok.type == T_WORD) {
            n->words = words_push(n->words, &n->nwords, tok.text);
            advance();
        }
    } else {
        /* `for x; do` iterates the positional parameters, which is what
         * the missing `in "$@"` means. Recorded here rather than in the
         * evaluator so the tree says what it will do. */
        n->words = words_push(n->words, &n->nwords, astrdup("\"$@\""));
    }
    while (tok_is_op(";") || tok_is_op("\n")) {
        advance();
    }
    if (!tok_is_word("do")) {
        syntax("`for` with no `do`");
        return n;
    }
    advance();
    static const char *const done_term[] = { "done", 0 };
    n->right = parse_list(done_term);
    if (!tok_is_word("done")) {
        syntax("`for` with no `done`");
        return n;
    }
    advance();
    return n;
}

static Node *parse_case(void) {
    advance(); /* case */
    if (tok.type != T_WORD) {
        syntax("`case` with nothing to match");
        return 0;
    }
    Node *n = node_new(N_CASE);
    n->name = tok.text;
    advance();
    skip_newlines();
    if (!tok_is_word("in")) {
        syntax("`case` with no `in`");
        return n;
    }
    advance();
    skip_newlines();

    CaseItem **tail = &n->cases;
    while (!tok_is_word("esac") && tok.type != T_EOF && !parse_error) {
        CaseItem *it = (CaseItem *)arena_alloc(cur_arena, sizeof(CaseItem));
        memset(it, 0, sizeof(*it));
        if (tok_is_op("(")) {
            advance();
        }
        for (;;) {
            if (tok.type != T_WORD) {
                syntax("a `case` branch with no pattern");
                return n;
            }
            it->pats = words_push(it->pats, &it->npats, tok.text);
            advance();
            if (tok_is_op("|")) {
                advance();
                continue;
            }
            break;
        }
        if (!tok_is_op(")")) {
            syntax("a `case` pattern with no `)`");
            return n;
        }
        advance();
        static const char *const item_term[] = { ";;", "esac", 0 };
        it->body = parse_list(item_term);
        if (tok_is_op(";;")) {
            advance();
        }
        skip_newlines();
        *tail = it;
        tail = &it->next;
    }
    if (!tok_is_word("esac")) {
        syntax("`case` with no `esac`");
        return n;
    }
    advance();
    return n;
}

static Node *parse_command(void) {
    if (++parse_depth > SH_MAX_DEPTH) {
        syntax("something nested too deeply");
        parse_depth--;
        return 0;
    }
    Node *n = 0;

    if (tok_is_word("if")) {
        n = parse_if();
    } else if (tok_is_word("while")) {
        n = parse_while(0);
    } else if (tok_is_word("until")) {
        n = parse_while(1);
    } else if (tok_is_word("for")) {
        n = parse_for();
    } else if (tok_is_word("case")) {
        n = parse_case();
    } else if (tok_is_op("(")) {
        advance();
        static const char *const close[] = { ")", 0 };
        n = node_new(N_SUBSHELL);
        n->left = parse_list(close);
        if (!tok_is_op(")")) {
            syntax("a subshell with no `)`");
        } else {
            advance();
        }
    } else if (tok_is_word("{")) {
        advance();
        static const char *const close[] = { "}", 0 };
        n = node_new(N_GROUP);
        n->left = parse_list(close);
        if (!tok_is_word("}")) {
            syntax("a group with no `}`");
        } else {
            advance();
        }
    } else if (tok.type == T_WORD) {
        /* `name()` is a function definition and nothing else starts that
         * way, so one token of lookahead decides it. The lexer hands `(`
         * back separately, so this is a peek at the next token rather
         * than a character. */
        char *maybe_name = tok.text;
        int was_quoted = tok.quoted;
        size_t save_i = lx.i;
        Tok save_tok = tok;
        /* ---- M99: peek at CHARACTERS, not at a token -----------------
         *
         * This used to `advance()` and roll the lexer back if the next
         * token was not `(`. Rolling back a position is not the same as
         * rolling back a lex, because lexing a newline has a side
         * effect: it collects the bodies of any here-documents pending
         * on that line (see gather_heredocs). So a peek that crossed a
         * newline consumed the here-document body, then put lx.i back
         * in front of it, and the body was parsed a second time - as
         * script.
         *
         *     cat <<EOF || fail=1
         *     body
         *     EOF
         *
         * printed `body` from cat, and then `sh: body: command not
         * found`. It needed all three parts: a here-document, an
         * operator, and a last command that is a bare assignment - a
         * command with a word after it (`|| echo x`) rolls back before
         * ever reaching the newline. That combination is 32,443 lines
         * into CPython's configure, which is where it was found.
         *
         * A function definition is `name ( ) compound-command`, and
         * POSIX allows only blanks between the name and the `(`. So the
         * question the peek is asking can be answered by looking at the
         * next non-blank character, which costs nothing and cannot
         * consume anything. `advance()` is only called once the answer
         * is yes, and then it cannot be crossing a newline. */
        size_t peek = lx.i;
        while (is_blank(lx.src[peek])) {
            peek++;
        }
        int looks_like_func = !was_quoted && lx.src[peek] == '(';
        if (looks_like_func) {
            advance();
        }
        if (looks_like_func && tok_is_op("(")) {
            advance();
            if (!tok_is_op(")")) {
                syntax("a function definition with no `)`");
                parse_depth--;
                return 0;
            }
            advance();
            skip_newlines();
            n = node_new(N_FUNC);
            n->name = maybe_name;
            n->left = parse_command();
            /* The body outlives the line that defined it, so the arena it
             * was parsed into is kept - see arena_free. */
            cur_arena->retained = 1;
            parse_depth--;
            return n;
        }
        /* Not a function. Nothing was consumed unless the peek said
         * yes, so the rollback is only needed in the one case where it
         * is provably safe: `(` is not a newline. */
        lx.i = save_i;
        tok = save_tok;
        n = parse_simple();
    } else {
        n = parse_simple();
    }

    /* A compound command can be redirected as a whole: `while ...; done
     * > log` writes every iteration to one file, which is not the same
     * as redirecting the last command in it. */
    if (n && n->kind != N_SIMPLE) {
        for (;;) {
            if (tok.type == T_IONUM ||
                (tok.type == T_OP && (strcmp(tok.text, "<") == 0 || strcmp(tok.text, ">") == 0 ||
                                      strcmp(tok.text, ">>") == 0 || strcmp(tok.text, "<<") == 0 ||
                                      strcmp(tok.text, "<<-") == 0 || strcmp(tok.text, ">&") == 0 ||
                                      strcmp(tok.text, "<&") == 0))) {
                Redir *r = parse_redirect();
                if (!r) {
                    break;
                }
                append_redir(n, r);
                continue;
            }
            break;
        }
    }
    parse_depth--;
    return n;
}

static Node *parse_pipeline(void) {
    int negate = 0;
    while (tok_is_word("!")) {
        negate = !negate;
        advance();
    }
    Node *n = parse_command();
    while (tok_is_op("|")) {
        advance();
        skip_newlines();
        Node *p = node_new(N_PIPE);
        p->left = n;
        p->right = parse_command();
        if (!p->right && !parse_error) {
            syntax("a pipe with nothing after it");
        }
        n = p;
    }
    if (negate) {
        Node *not_node = node_new(N_NOT);
        not_node->left = n;
        n = not_node;
    }
    return n;
}

static Node *parse_and_or(void) {
    Node *n = parse_pipeline();
    for (;;) {
        int kind;
        if (tok_is_op("&&")) {
            kind = N_AND;
        } else if (tok_is_op("||")) {
            kind = N_OR;
        } else {
            return n;
        }
        advance();
        skip_newlines();
        Node *p = node_new(kind);
        p->left = n;
        p->right = parse_pipeline();
        if (!p->right && !parse_error) {
            syntax("`&&` or `||` with nothing after it");
        }
        n = p;
    }
}

static Node *parse_list(const char *const *terminators) {
    Node *head = 0;
    for (;;) {
        skip_newlines();
        if (tok.type == T_EOF || is_terminator(terminators) || parse_error) {
            return head;
        }
        Node *n = parse_and_or();
        if (!n) {
            if (!parse_error && tok.type != T_EOF && !is_terminator(terminators)) {
                syntax(tok.text ? tok.text : "end of input");
            }
            return head;
        }
        if (tok_is_op("&")) {
            Node *bg = node_new(N_BG);
            bg->left = n;
            n = bg;
            advance();
        } else if (tok_is_op(";")) {
            advance();
        }
        if (!head) {
            head = n;
        } else {
            Node *seq = node_new(N_SEQ);
            seq->left = head;
            seq->right = n;
            head = seq;
        }
    }
}

/* ---- expansion ---------------------------------------------------------
 *
 * This runs at execution time, over the raw word the lexer kept, and it
 * is the half of the shell where the quoting rules actually live:
 *
 *   `$x`    expands, then splits into as many fields as its value has
 *           words, then globs each one
 *   `"$x"`  expands and is exactly one field, whatever is in it
 *   `'$x'`  is two characters and a letter
 *
 * The three differ only in what the *word* looked like, which is why the
 * lexer had to keep the quotes and why this could not be done there.
 */
static int exec_node(Node *n);
static int exec_text(const char *text);

typedef struct {
    Sbuf cur;
    int started;      /* a field exists even if it is empty: "" is a field */
    int quoted_here;  /* the current field had a quoted part */
    Vec *out;
    int split;        /* unquoted expansions become several fields */
    int glob;
    /* M99: set when `"$@"` expanded to no parameters at all, cleared the
     * moment any real text joins the field. POSIX: `"$@"` with nothing
     * in it is ZERO fields, not one empty one - so `for x in "$@"` with
     * no arguments runs its body zero times, and this shell ran it once
     * with x empty. Adjacent text still wins, because `a"$@"b` is the
     * one field `ab` everywhere. */
    int at_killed;
} Ex;

static void ex_flush(Ex *e) {
    if (e->at_killed && !e->cur.p) {
        /* An empty `"$@"` and nothing else - see at_killed. */
        e->started = 0;
        e->quoted_here = 0;
        e->at_killed = 0;
        return;
    }
    e->at_killed = 0;
    if (!e->started) {
        return;
    }
    char *field = e->cur.p ? e->cur.p : xstrdup("");
    if (!e->cur.p) {
        vec_push(e->out, field);
    } else if (e->glob && !opt_noglob && !e->quoted_here && has_glob(field)) {
        glob_expand(field, e->out);
        free(field);
    } else {
        vec_push(e->out, field);
    }
    sb_init(&e->cur);
    e->started = 0;
    e->quoted_here = 0;
}

static void ex_add(Ex *e, const char *s, size_t n) {
    if (n) {
        e->at_killed = 0; /* real text joined the field - see at_killed */
    }
    sb_putn(&e->cur, s, n);
    e->started = 1;
}

static const char *ifs_chars(void) {
    const char *v = var_get("IFS");
    return v[0] ? v : " \t\n";
}

/* Text that came out of an unquoted expansion: split it on IFS here,
 * which is the step that makes `for f in $list` iterate. */
static void ex_add_split(Ex *e, const char *s) {
    if (!e->split) {
        ex_add(e, s, strlen(s));
        return;
    }
    const char *ifs = ifs_chars();
    for (const char *p = s; *p; p++) {
        if (strchr(ifs, *p)) {
            ex_flush(e);
            continue;
        }
        ex_add(e, p, 1);
    }
}

/* ---- $(...) and `...` --------------------------------------------------
 *
 * A whole shell in a pipe: fork, point the child's stdout at the write
 * end, let it run the text, and read what comes back. Trailing newlines
 * come off, which is the rule that makes `x=$(pwd)` useful.
 */
static char *capture_command(const char *text) {
    int fds[2];
    if (pipe(fds) != 0) {
        errmsg("cannot create a pipe for $( )", 0, 0);
        return xstrdup("");
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        errmsg("cannot fork for $( )", 0, 0);
        return xstrdup("");
    }
    if (pid == 0) {
        close(fds[0]);
        if (fds[1] != 1) {
            dup2(fds[1], 1);
            close(fds[1]);
        }
        int st = exec_text(text);
        _exit(st);
    }
    close(fds[1]);

    Sbuf b;
    sb_init(&b);
    char buf[512];
    long n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
        sb_putn(&b, buf, (size_t)n);
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    while (b.len > 0 && b.p[b.len - 1] == '\n') {
        b.p[--b.len] = '\0';
    }
    return b.p ? b.p : xstrdup("");
}

/* The value of one parameter name, with the specials in the same place a
 * script expects to find them. */
static char *param_value(const char *name) {
    if (strcmp(name, "?") == 0) {
        return xstrdup(num_to_str(last_status));
    }
    if (strcmp(name, "$") == 0) {
        return xstrdup(num_to_str(shell_pid));
    }
    if (strcmp(name, "#") == 0) {
        return xstrdup(num_to_str(pos_count));
    }
    if (strcmp(name, "0") == 0) {
        return xstrdup(script_name);
    }
    if (name[0] >= '1' && name[0] <= '9') {
        int idx = atoi(name);
        if (idx >= 1 && idx <= pos_count) {
            return xstrdup(pos_params[idx - 1]);
        }
        return xstrdup("");
    }
    if (strcmp(name, "*") == 0 || strcmp(name, "@") == 0) {
        Sbuf b;
        sb_init(&b);
        for (int i = 0; i < pos_count; i++) {
            if (i) {
                sb_putc(&b, ' ');
            }
            sb_puts(&b, pos_params[i]);
        }
        return b.p ? b.p : xstrdup("");
    }
    return xstrdup(var_get(name));
}

/* `${name#pat}` and friends: the shortest or longest prefix or suffix
 * matching a glob pattern, removed. Not in M86's bullet list and in
 * every real script's first twenty lines: `${f%.c}` is how a script
 * drops a suffix, and the same operator with a slash in the pattern is
 * how it says dirname without spawning one. */
static char *strip_affix(const char *value, const char *pat, int from_end, int longest) {
    size_t n = strlen(value);
    Sbuf b;
    sb_init(&b);
    /* Candidate lengths in the order that finds the wanted match first. */
    for (size_t step = 0; step <= n; step++) {
        size_t len = longest ? n - step : step;
        char *piece;
        if (from_end) {
            piece = xstrdup(value + (n - len));
        } else {
            piece = (char *)malloc(len + 1);
            if (!piece) {
                die_oom();
            }
            memcpy(piece, value, len);
            piece[len] = '\0';
        }
        int hit = glob_match(pat, piece);
        free(piece);
        if (hit) {
            if (from_end) {
                sb_putn(&b, value, n - len);
            } else {
                sb_puts(&b, value + len);
            }
            return b.p ? b.p : xstrdup("");
        }
    }
    return xstrdup(value);
}

static void expand_dollar(Ex *e, const char **pp, int in_quotes);
static void expand_word(const char *raw, Vec *out, int split, int do_glob);

/* ---- M99: expanding the WORD inside ${name+word} -----------------------
 *
 * Every `${name-word}`, `${name+word}`, `${name#pattern}` has a word in
 * it that is itself expanded, and this file used to do that with a small
 * private loop that understood `$` and nothing else. Two things that
 * scripts write constantly went wrong:
 *
 *   ${x+"$y"}   the quotes were literal characters and the `$y` inside
 *               them came out as the two characters `$y`.
 *   ${1+"$@"}   which is how every autoconf-generated script forwards
 *               its arguments, and which has to produce N FIELDS.
 *
 * A private expander that has to grow quotes, and then fields, is the
 * word expander with a different name. This calls the real one. `split`
 * is 0 because the word's own IFS splitting is not the ${} operator's
 * business - but "$@" still produces one field per parameter, because
 * that is done by the $@ expansion itself and not by splitting. */
static void expand_braced_word(Ex *e, const char *word, int in_quotes) {
    Vec wv;
    vec_init(&wv);
    /* Splitting is decided HERE and once. An unquoted `${x-$y}` splits
     * its default on IFS; a quoted one does not; and either way the
     * fields that come back are final - re-splitting them afterwards is
     * what turned `${1+"$@"}` with `b c` in it into two arguments. */
    expand_word(word, &wv, !in_quotes, 0);
    for (int i = 0; i < wv.n; i++) {
        if (i) {
            ex_flush(e);
        }
        ex_add(e, wv.v[i], strlen(wv.v[i]));
        if (in_quotes) {
            e->quoted_here = 1;
        }
    }
    if (wv.n == 0) {
        e->at_killed = 1;
    }
    vec_free(&wv);
}

/* ${...} - the brace form, where the whole `:-` family lives. */
static void expand_braced(Ex *e, const char *body, int in_quotes) {
    /* `${#name}` is a length and `${#-word}` is `$#` with a default,
     * and the character after the `#` is the only thing that tells them
     * apart: an operator there means `#` was the parameter. */
    if (body[0] == '#' && body[1] && !strchr(":-+=?", body[1])) {
        char *v = param_value(body + 1);
        char *len = xstrdup(num_to_str((long)strlen(v)));
        free(v);
        ex_add(e, len, strlen(len));
        free(len);
        return;
    }

    /* Split at the operator, which is the first of these that is not
     * part of the name. */
    size_t i = 0;
    while (body[i] && (is_name_char(body[i]) || (i == 0 && strchr("?$#*@", body[i])))) {
        if (i == 0 && strchr("?$#*@", body[i])) {
            i++;
            break;
        }
        i++;
    }
    char *name = xstrdup(body);
    name[i] = '\0';
    const char *op = body + i;

    if (!*op) {
        char *v = param_value(name);
        if (strcmp(name, "@") == 0 && in_quotes) {
            /* "$@" is N fields, one per parameter, and that is the whole
             * reason it exists next to "$*". */
            free(v);
            free(name);
            for (int p = 0; p < pos_count; p++) {
                if (p) {
                    ex_flush(e);
                }
                ex_add(e, pos_params[p], strlen(pos_params[p]));
                e->quoted_here = 1;
            }
            if (pos_count == 0) {
                e->at_killed = 1;
            }
            return;
        }
        if (in_quotes) {
            ex_add(e, v, strlen(v));
        } else {
            ex_add_split(e, v);
        }
        free(v);
        free(name);
        return;
    }

    int colon = 0;
    if (*op == ':') {
        colon = 1;
        op++;
    }
    char kind = *op;
    const char *word = op + 1;
    if (kind == '#' || kind == '%') {
        int longest = (word[0] == kind);
        if (longest) {
            word++;
        }
        char *v = param_value(name);
        /* The pattern is itself expanded first: `${x#$prefix}` is a
         * thing scripts write. Through the real word expander since
         * M99 - see expand_braced_word for what the private one could
         * not do. */
        Vec pv;
        vec_init(&pv);
        expand_word(word, &pv, 0, 0);
        char *result = strip_affix(v, pv.n ? pv.v[0] : "", kind == '%', longest);
        vec_free(&pv);
        free(v);
        if (in_quotes) {
            ex_add(e, result, strlen(result));
        } else {
            ex_add_split(e, result);
        }
        free(result);
        free(name);
        return;
    }

    char *v = param_value(name);
    int unset_or_empty = colon ? (v[0] == '\0') : !var_is_set(name);
    if (name[0] >= '1' && name[0] <= '9') {
        unset_or_empty = colon ? (v[0] == '\0') : (atoi(name) > pos_count);
    } else if (name[1] == '\0' && strchr("*@#?$0", name[0])) {
        /* ---- M99: the special parameters are not variables ----------
         *
         * `var_is_set` asks the variable table, and there is no variable
         * called `*`, so `${*-Setup}` said "unset" and produced the
         * default even with eight arguments in hand. CPython's
         * Modules/makesetup opens its main loop with exactly that -
         * `for i in ${*-Setup}` - so it processed one file named
         * `Setup`, which does not exist, and configure produced a
         * Makefile with no modules in it and did not fail.
         *
         * `$*` and `$@` are set when there is at least one positional
         * parameter. `$#`, `$?`, `$$` and `$0` always are - there is
         * always a count, always a last status, always a pid and always
         * a name, even when they are zero or empty. */
        if (name[0] == '*' || name[0] == '@') {
            unset_or_empty = colon ? (v[0] == '\0') : (pos_count == 0);
        } else {
            unset_or_empty = colon ? (v[0] == '\0') : 0;
        }
    }

    if (kind == '-' || kind == '=' || kind == '+' || kind == '?') {
        int use_word = (kind == '+') ? !unset_or_empty : unset_or_empty;
        if (kind == '?' && unset_or_empty) {
            errmsg(name, ": ", word[0] ? word : "parameter null or not set");
            free(v);
            free(name);
            flow = FLOW_EXIT;
            exit_code = 1;
            return;
        }
        if (use_word) {
            /* The default is a word in its own right and gets expanded,
             * which is what makes `${x:-$HOME}` work. M99 replaced a
             * private loop here with the real word expander: the old one
             * stripped quotes and copied their contents literally, so
             * `${x+"$y"}` produced the two characters `$y`, and it could
             * only ever produce one field, so `${1+"$@"}` - how every
             * autoconf script forwards its arguments - collapsed to the
             * first one. */
            if (kind == '=') {
                /* `:=` assigns, and an assignment is one string. Taking
                 * the first field is what the operator means: the value
                 * of a variable is not a list. */
                Vec wv;
                vec_init(&wv);
                expand_word(word, &wv, 0, 0);
                char *val = xstrdup(wv.n ? wv.v[0] : "");
                vec_free(&wv);
                var_set(name, val);
                if (in_quotes) {
                    ex_add(e, val, strlen(val));
                } else {
                    ex_add_split(e, val);
                }
                free(val);
            } else {
                expand_braced_word(e, word, in_quotes);
            }
        } else if (kind != '+') {
            if (in_quotes) {
                ex_add(e, v, strlen(v));
            } else {
                ex_add_split(e, v);
            }
        }
        free(v);
        free(name);
        return;
    }

    /* An operator this shell does not know: treated as a plain name,
     * which is what a shell without arithmetic does with `${x:1}`. */
    if (in_quotes) {
        ex_add(e, v, strlen(v));
    } else {
        ex_add_split(e, v);
    }
    free(v);
    free(name);
}


/* ---- M99: $(( )) -------------------------------------------------------
 *
 * Added because somebody else's configure asked for it by name. CPython's
 * `configure` decides whether the shell running it is fit for the job by
 * evaluating a block it calls `as_required`, and the last line of that
 * block is
 *
 *     test $(( 1 + 1 )) = 2 || exit 1
 *
 * so a shell without arithmetic expansion is rejected before configure
 * has printed a single line - which is what this one was, and why M99's
 * last box turned out not to be about wall clock at all.
 *
 * A recursive-descent evaluator over `long`, with C's precedence, which
 * is what POSIX specifies: the operators and their meanings are "the
 * same as in the ISO C standard", so the reference implementation is a
 * language everyone already agrees about. What is deliberately NOT here:
 *
 *   - assignment, `++` and `--`. POSIX makes them optional and autoconf
 *     does not use them. A wrong answer is worse than a refusal, and a
 *     refusal here is a syntax error naming the operator.
 *   - unsigned, floating point, and integer overflow behaviour beyond
 *     what C gives. Division by zero is an error rather than a trap.
 *
 * A bare name inside the parentheses is a shell variable read as a
 * number, and an unset or empty one is zero - POSIX says so, and it is
 * why `$(( i + 1 ))` works on the first pass of a loop.
 */
typedef struct {
    const char *p;
    int error;
} Arith;

static long arith_expr(Arith *a);
static char *expand_one(const char *raw);

static void arith_skip(Arith *a) {
    while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n') {
        a->p++;
    }
}

/* True when the next characters are `op` AND are not the prefix of a
 * longer operator that starts the same way - so `<` does not match
 * inside `<<` and `&` does not match inside `&&`. */
static int arith_eat(Arith *a, const char *op, const char *longer) {
    arith_skip(a);
    size_t n = strlen(op);
    if (strncmp(a->p, op, n) != 0) {
        return 0;
    }
    if (longer && strncmp(a->p, longer, strlen(longer)) == 0) {
        return 0;
    }
    a->p += n;
    return 1;
}

static long arith_primary(Arith *a) {
    arith_skip(a);
    if (*a->p == '(') {
        a->p++;
        long v = arith_expr(a);
        arith_skip(a);
        if (*a->p == ')') {
            a->p++;
        } else {
            a->error = 1;
        }
        return v;
    }
    if (*a->p == '-') {
        a->p++;
        return -arith_primary(a);
    }
    if (*a->p == '+') {
        a->p++;
        return arith_primary(a);
    }
    if (*a->p == '!') {
        a->p++;
        return !arith_primary(a);
    }
    if (*a->p == '~') {
        a->p++;
        return ~arith_primary(a);
    }
    if (*a->p >= '0' && *a->p <= '9') {
        /* 0x for hex and a leading 0 for octal, which is what C says and
         * therefore what POSIX says. strtol with base 0 is exactly this
         * rule and is already in this libc. */
        char *end = 0;
        long v = strtol(a->p, &end, 0);
        if (end == a->p) {
            a->error = 1;
            return 0;
        }
        a->p = end;
        return v;
    }
    if (is_name_char(*a->p) && !(*a->p >= '0' && *a->p <= '9')) {
        const char *start = a->p;
        while (is_name_char(*a->p)) {
            a->p++;
        }
        char *name = astrndup(start, (size_t)(a->p - start));
        const char *val = var_get(name);
        if (!val || !*val) {
            return 0; /* unset or empty is zero - POSIX */
        }
        Arith inner;
        inner.p = val;
        inner.error = 0;
        long v = arith_expr(&inner);
        if (inner.error) {
            a->error = 1;
        }
        return v;
    }
    a->error = 1;
    return 0;
}

static long arith_mul(Arith *a) {
    long v = arith_primary(a);
    for (;;) {
        if (arith_eat(a, "*", 0)) {
            v = v * arith_primary(a);
        } else if (arith_eat(a, "/", 0)) {
            long d = arith_primary(a);
            if (d == 0) {
                a->error = 1;
                return 0;
            }
            v = v / d;
        } else if (arith_eat(a, "%", 0)) {
            long d = arith_primary(a);
            if (d == 0) {
                a->error = 1;
                return 0;
            }
            v = v % d;
        } else {
            return v;
        }
    }
}

static long arith_add(Arith *a) {
    long v = arith_mul(a);
    for (;;) {
        if (arith_eat(a, "+", 0)) {
            v = v + arith_mul(a);
        } else if (arith_eat(a, "-", 0)) {
            v = v - arith_mul(a);
        } else {
            return v;
        }
    }
}

static long arith_shift(Arith *a) {
    long v = arith_add(a);
    for (;;) {
        if (arith_eat(a, "<<", 0)) {
            v = v << arith_add(a);
        } else if (arith_eat(a, ">>", 0)) {
            v = v >> arith_add(a);
        } else {
            return v;
        }
    }
}

static long arith_rel(Arith *a) {
    long v = arith_shift(a);
    for (;;) {
        /* `<=` before `<`, and `<` refused when it is really `<<`. */
        if (arith_eat(a, "<=", 0)) {
            v = (v <= arith_shift(a));
        } else if (arith_eat(a, ">=", 0)) {
            v = (v >= arith_shift(a));
        } else if (arith_eat(a, "<", "<<")) {
            v = (v < arith_shift(a));
        } else if (arith_eat(a, ">", ">>")) {
            v = (v > arith_shift(a));
        } else {
            return v;
        }
    }
}

static long arith_eq(Arith *a) {
    long v = arith_rel(a);
    for (;;) {
        if (arith_eat(a, "==", 0)) {
            v = (v == arith_rel(a));
        } else if (arith_eat(a, "!=", 0)) {
            v = (v != arith_rel(a));
        } else {
            return v;
        }
    }
}

static long arith_band(Arith *a) {
    long v = arith_eq(a);
    while (arith_eat(a, "&", "&&")) {
        v = v & arith_eq(a);
    }
    return v;
}

static long arith_bxor(Arith *a) {
    long v = arith_band(a);
    while (arith_eat(a, "^", 0)) {
        v = v ^ arith_band(a);
    }
    return v;
}

static long arith_bor(Arith *a) {
    long v = arith_bxor(a);
    while (arith_eat(a, "|", "||")) {
        v = v | arith_bxor(a);
    }
    return v;
}

static long arith_land(Arith *a) {
    long v = arith_bor(a);
    /* Both sides are evaluated. C short-circuits and so does every
     * shell, and the difference is only observable through a side
     * effect - which needs assignment, `++` or a command substitution
     * inside the parentheses, and none of those is here. Written down
     * because it is the thing to fix first if assignment ever is. */
    while (arith_eat(a, "&&", 0)) {
        long r = arith_bor(a);
        v = (v && r);
    }
    return v;
}

static long arith_lor(Arith *a) {
    long v = arith_land(a);
    while (arith_eat(a, "||", 0)) {
        long r = arith_land(a);
        v = (v || r);
    }
    return v;
}

static long arith_expr(Arith *a) {
    return arith_lor(a);
}

/* Evaluates `text` (already expanded, so `$x` has become its value) and
 * returns the answer as a freshly allocated decimal string. A malformed
 * expression is a diagnostic and a zero, which is what dash does; bash
 * exits, and the difference is not worth a divergence in a shell whose
 * whole job is to agree with the others about the ordinary cases. */
static char *arith_eval(const char *text) {
    Arith a;
    a.p = text;
    a.error = 0;
    long v = arith_expr(&a);
    arith_skip(&a);
    if (*a.p) {
        a.error = 1;
    }
    if (a.error) {
        errmsg("arithmetic syntax error: ", text, 0);
        v = 0;
    }
    return xstrdup(num_to_str(v));
}

static void expand_dollar(Ex *e, const char **pp, int in_quotes) {
    const char *p = *pp + 1; /* past '$' */
    if (p[0] == '(' && p[1] == '(') {
        /* M99: arithmetic, and it has to be tested before command
         * substitution because `$((` is a prefix of `$(`. The closing
         * `))` is found by counting parentheses from the inner one, so
         * that `$(( (1+2)*3 ))` closes in the right place - a scan for
         * the first `))` would stop inside it. */
        const char *start = p + 2;
        const char *q = start;
        int depth = 1;
        while (*q) {
            if (*q == '(') {
                depth++;
            } else if (*q == ')') {
                if (--depth == 0) {
                    break;
                }
            }
            q++;
        }
        if (depth != 0 || q[0] != ')' || q[1] != ')') {
            /* Not a closed `$(( ... ))`. POSIX says this is then a
             * command substitution of a subshell - `$( (list) )` - and
             * falling through says so without a special case. */
        } else {
            size_t len = (size_t)(q - start);
            char *raw = (char *)malloc(len + 1);
            if (!raw) {
                die_oom();
            }
            memcpy(raw, start, len);
            raw[len] = '\0';
            /* Expanded first, then evaluated: `$(( $x + 1 ))` and
             * `$(( x + 1 ))` must mean the same thing, and only the
             * second is the evaluator's business. */
            char *expanded = expand_one(raw);
            free(raw);
            char *result = arith_eval(expanded ? expanded : "");
            free(expanded);
            /* Never split and never globbed: the result is one field, a
             * number, in quotes or out of them. */
            ex_add(e, result, strlen(result));
            free(result);
            *pp = q + 2;
            return;
        }
    }
    if (*p == '(') {
        int depth = 0;
        const char *start = p + 1;
        const char *q = p;
        for (; *q; q++) {
            if (*q == '(') {
                depth++;
            } else if (*q == ')') {
                if (--depth == 0) {
                    break;
                }
            }
        }
        size_t len = (size_t)(q - start);
        char *text = (char *)malloc(len + 1);
        if (!text) {
            die_oom();
        }
        memcpy(text, start, len);
        text[len] = '\0';
        char *result = capture_command(text);
        free(text);
        if (in_quotes) {
            ex_add(e, result, strlen(result));
        } else {
            ex_add_split(e, result);
        }
        free(result);
        *pp = *q ? q + 1 : q;
        return;
    }
    if (*p == '{') {
        int depth = 0;
        const char *start = p + 1;
        const char *q = p;
        for (; *q; q++) {
            if (*q == '{') {
                depth++;
            } else if (*q == '}') {
                if (--depth == 0) {
                    break;
                }
            }
        }
        size_t len = (size_t)(q - start);
        char *body = (char *)malloc(len + 1);
        if (!body) {
            die_oom();
        }
        memcpy(body, start, len);
        body[len] = '\0';
        expand_braced(e, body, in_quotes);
        free(body);
        *pp = *q ? q + 1 : q;
        return;
    }
    if (is_name_char(*p) || strchr("?$#*@", *p)) {
        char name[64];
        size_t n = 0;
        if (strchr("?$#*@", *p) || (*p >= '0' && *p <= '9')) {
            name[n++] = *p++;
        } else {
            while (*p && is_name_char(*p) && n < sizeof(name) - 1) {
                name[n++] = *p++;
            }
        }
        name[n] = '\0';
        if (strcmp(name, "@") == 0 && in_quotes) {
            for (int i = 0; i < pos_count; i++) {
                if (i) {
                    ex_flush(e);
                }
                ex_add(e, pos_params[i], strlen(pos_params[i]));
                e->quoted_here = 1;
            }
            if (pos_count == 0) {
                /* M99: zero fields, not one empty one - see at_killed.
                 * This is the BARE `$@` site; the `${@}` one above had
                 * the same line and this one did not, which is why
                 * `for x in "$@"` with no arguments ran its body once. */
                e->at_killed = 1;
            }
            *pp = p;
            return;
        }
        char *v = param_value(name);
        if (in_quotes) {
            ex_add(e, v, strlen(v));
        } else {
            ex_add_split(e, v);
        }
        free(v);
        *pp = p;
        return;
    }
    /* A `$` that begins nothing is a dollar sign. */
    ex_add(e, "$", 1);
    *pp = p;
}

/* One raw word into however many fields it produces. */
static void expand_word(const char *raw, Vec *out, int split, int do_glob) {
    Ex e;
    memset(&e, 0, sizeof(e));
    sb_init(&e.cur);
    e.out = out;
    e.split = split;
    e.glob = do_glob;

    const char *p = raw;
    while (*p) {
        if (*p == '\\') {
            if (p[1]) {
                ex_add(&e, p + 1, 1);
                e.quoted_here = 1;
                p += 2;
                continue;
            }
            p++;
            continue;
        }
        if (*p == '\'') {
            p++;
            e.started = 1;
            e.quoted_here = 1;
            while (*p && *p != '\'') {
                ex_add(&e, p, 1);
                p++;
            }
            if (*p) {
                p++;
            }
            continue;
        }
        if (*p == '"') {
            p++;
            e.started = 1;
            e.quoted_here = 1;
            while (*p && *p != '"') {
                /* M99: a backslash-newline inside double quotes is a
                 * LINE CONTINUATION - both characters disappear. POSIX
                 * says so, and CPython's configure writes a
                 * twenty-one-line `SRCDIRS="\` that way. Keeping them
                 * put a backslash and a newline into a Makefile
                 * variable, which is not an error anywhere and is a
                 * different Makefile. */
                if (*p == '\\' && p[1] == '\n') {
                    p += 2;
                    continue;
                }
                if (*p == '\\' && p[1] && strchr("$`\"\\", p[1])) {
                    ex_add(&e, p + 1, 1);
                    p += 2;
                    continue;
                }
                if (*p == '$') {
                    expand_dollar(&e, &p, 1);
                    continue;
                }
                if (*p == '`') {
                    const char *q = ++p;
                    while (*q && *q != '`') {
                        q++;
                    }
                    char *text = (char *)malloc((size_t)(q - p) + 1);
                    if (!text) {
                        die_oom();
                    }
                    memcpy(text, p, (size_t)(q - p));
                    text[q - p] = '\0';
                    char *result = capture_command(text);
                    free(text);
                    ex_add(&e, result, strlen(result));
                    free(result);
                    p = *q ? q + 1 : q;
                    continue;
                }
                ex_add(&e, p, 1);
                p++;
            }
            if (*p) {
                p++;
            }
            continue;
        }
        if (*p == '$') {
            expand_dollar(&e, &p, 0);
            continue;
        }
        if (*p == '`') {
            const char *q = ++p;
            while (*q && *q != '`') {
                q++;
            }
            char *text = (char *)malloc((size_t)(q - p) + 1);
            if (!text) {
                die_oom();
            }
            memcpy(text, p, (size_t)(q - p));
            text[q - p] = '\0';
            char *result = capture_command(text);
            free(text);
            ex_add_split(&e, result);
            free(result);
            p = *q ? q + 1 : q;
            continue;
        }
        if (split && strchr(ifs_chars(), *p)) {
            ex_flush(&e);
            p++;
            continue;
        }
        ex_add(&e, p, 1);
        p++;
    }
    ex_flush(&e);
}

/* A word that must come out as exactly one string: a redirect target, an
 * assignment's value, the thing a `case` matches on. */
static char *expand_one(const char *raw) {
    Vec v;
    vec_init(&v);
    expand_word(raw, &v, 0, 0);
    char *s = v.n ? xstrdup(v.v[0]) : xstrdup("");
    vec_free(&v);
    return s;
}

/* A `case` pattern: `$x` is expanded, `*` is not - the metacharacters
 * are the whole point of the construct and expanding them here would
 * match against whatever happened to be in the directory. */
static char *expand_pattern(const char *raw) {
    Vec v;
    vec_init(&v);
    Ex e;
    memset(&e, 0, sizeof(e));
    sb_init(&e.cur);
    e.out = &v;
    e.split = 0;
    e.glob = 0;
    const char *p = raw;
    while (*p) {
        if (*p == '\'' || *p == '"') {
            char q = *p++;
            e.started = 1;
            while (*p && *p != q) {
                ex_add(&e, p, 1);
                p++;
            }
            if (*p) {
                p++;
            }
            continue;
        }
        if (*p == '$') {
            expand_dollar(&e, &p, 1);
            continue;
        }
        ex_add(&e, p, 1);
        p++;
    }
    ex_flush(&e);
    char *s = v.n ? xstrdup(v.v[0]) : xstrdup("");
    vec_free(&v);
    return s;
}

/* ---- redirection -------------------------------------------------------
 *
 * M72's note here described a day lost to the order in which stdout was
 * parked and the target opened, and the fix was to park first. That
 * whole problem belongs to a shell that redirects *itself* and then puts
 * itself back, which this one only does for builtins - an external
 * command is forked now, and a child that redirects its own descriptors
 * and then execs has nothing to restore.
 *
 * So there are two paths and the difference is `save`: with one, every
 * descriptor touched is copied out of the way first and put back after;
 * without one, this is a child that is about to be replaced.
 */
#define SH_SAVE_BASE 20
#define SH_MAX_REDIR 16

typedef struct {
    int n;
    int fd[SH_MAX_REDIR];
    int saved[SH_MAX_REDIR];
    pid_t here[SH_MAX_REDIR];
    int nhere;
} RedirSave;

/* A heredoc is fed by a process, not by a temp file and not by writing
 * into the pipe from here. Writing it here deadlocks the moment the body
 * is bigger than the pipe's buffer - nothing is reading yet - and a temp
 * file needs a name, a cleanup path, and a filesystem that keeps an open
 * file alive after it is unlinked, which leanfs does not. A writer
 * process costs a fork and has none of those problems. */
static int heredoc_fd(Redir *r, RedirSave *save) {
    Sbuf text;
    sb_init(&text);
    if (r->expand) {
        /* Expanded as if inside double quotes: `$x` yes, splitting no. */
        Ex e;
        Vec v;
        vec_init(&v);
        memset(&e, 0, sizeof(e));
        sb_init(&e.cur);
        e.out = &v;
        e.split = 0;
        e.glob = 0;
        const char *p = r->word;
        while (*p) {
            if (*p == '\\' && p[1] == '\n') {
                p += 2; /* M99: a line continuation - see expand_word */
                continue;
            }
            if (*p == '\\' && p[1] && strchr("$`\\", p[1])) {
                ex_add(&e, p + 1, 1);
                p += 2;
                continue;
            }
            if (*p == '$') {
                expand_dollar(&e, &p, 1);
                continue;
            }
            /* ---- M99: and a backtick, which is half of "as if inside
             * double quotes" and was the missing half.
             *
             * An unquoted here-document delimiter means the body gets
             * parameter expansion, command substitution and arithmetic
             * expansion - the same three a double-quoted string gets.
             * This did the first and not the second, and what that
             * looked like is the reason it is worth a comment: every
             * `#define HAVE_...` in a configure run is written by
             *
             *     cat >>confdefs.h <<_ACEOF
             *     #define `printf "%s\n" "HAVE_$ac_hdr" | $as_tr_cpp` 1
             *     _ACEOF
             *
             * so confdefs.h received the *text of the pipeline*, and
             * every compile after it failed with "macro name must be an
             * identifier" - while configure carried on reporting `yes`
             * for the headers it had just failed to record, and then
             * reported every `sizeof` as 0. Which is the lesson this
             * project already had written down from binutils: a
             * configure probe that fails to compile writes a NUMBER,
             * not an error. */
            if (*p == '`') {
                const char *q = ++p;
                while (*q && *q != '`') {
                    if (*q == '\\' && q[1]) {
                        q++;
                    }
                    q++;
                }
                char *body = astrndup(p, (size_t)(q - p));
                char *result = capture_command(body);
                ex_add(&e, result, strlen(result));
                free(result);
                p = *q ? q + 1 : q;
                continue;
            }
            ex_add(&e, p, 1);
            p++;
        }
        e.started = 1;
        ex_flush(&e);
        sb_puts(&text, v.n ? v.v[0] : "");
        vec_free(&v);
    } else {
        sb_puts(&text, r->word);
    }
    int fds[2];
    if (pipe(fds) != 0) {
        sb_free(&text);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        sb_free(&text);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        size_t off = 0;
        while (off < text.len) {
            long n = write(fds[1], text.p + off, text.len - off);
            if (n <= 0) {
                break;
            }
            off += (size_t)n;
        }
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    sb_free(&text);
    if (save && save->nhere < SH_MAX_REDIR) {
        save->here[save->nhere++] = pid;
    }
    return fds[0];
}

/* Somewhere to put a descriptor while its number is borrowed.
 *
 * Two rules, and this project has now paid for both of them.
 *
 * The FIRST is M72's, and its note is worth re-reading: park before you
 * open. A shell that opens the target and then parks stdout hands the
 * open the chance to land on the parking slot, at which point
 * `dup2(1, park)` overwrites the file with stdout and `dup2(park, 1)`
 * copies stdout onto stdout - a redirect that silently becomes a no-op.
 * M72 fixed that and this file re-broke it by opening first; the symptom
 * was that a script's FIRST redirect worked and every one after it went
 * to the console, which is exactly the symptom M72 recorded.
 *
 * The SECOND is new here and is why the slot is searched for rather than
 * fixed at a constant. A shell on this machine does not start with two
 * descriptors: it is spawned by init and inherits a table with something
 * like forty entries in it, so the first `open` in a script returned fd
 * 42. A fixed parking number would sooner or later be one of those
 * inherited descriptors, and dup2 does not ask - it releases whatever is
 * there. So the slot is probed with fcntl, which is the one call here
 * that can tell an open descriptor from a free one.
 */
static int park_slot(int fd) {
    for (int slot = SH_SAVE_BASE; slot < SH_SAVE_BASE + 64; slot++) {
        if (fcntl(slot, F_GETFD) >= 0) {
            continue; /* somebody's - possibly inherited, certainly not ours */
        }
        if (dup2(fd, slot) == slot) {
            return slot;
        }
        return -1; /* fd itself is not open: nothing to put back later */
    }
    return -1;
}

static int apply_redirs(Redir *list, RedirSave *save) {
    if (save) {
        save->n = 0;
        save->nhere = 0;
    }
    for (Redir *r = list; r; r = r->next) {
        int target = -1;
        /* Before anything is opened - see park_slot. */
        if (save && save->n < SH_MAX_REDIR) {
            save->fd[save->n] = r->fd;
            save->saved[save->n] = park_slot(r->fd);
            save->n++;
        }
        if (r->kind == RD_HEREDOC) {
            target = heredoc_fd(r, save);
            if (target < 0) {
                errmsg("cannot set up a here-document", 0, 0);
                return -1;
            }
        } else if (r->kind == RD_DUP_OUT || r->kind == RD_DUP_IN) {
            char *w = expand_one(r->word);
            if (strcmp(w, "-") == 0) {
                free(w);
                close(r->fd);
                continue;
            }
            target = atoi(w);
            free(w);
            /* `2>&1` is a copy of whatever fd 1 is NOW, which is why the
             * order of redirects on a line matters and why this is not
             * resolved at parse time. */
            int dupd = dup2(target, r->fd);
            if (dupd < 0) {
                errmsg("cannot duplicate descriptor ", num_to_str(target), 0);
                return -1;
            }
            continue;
        } else {
            char *path = expand_one(r->word);
            int flags = 0;
            if (r->kind == RD_IN) {
                flags = O_RDONLY;
            } else if (r->kind == RD_OUT) {
                flags = O_WRONLY | O_CREAT | O_TRUNC;
            } else {
                flags = O_WRONLY | O_CREAT | O_APPEND;
            }
            target = open(path, flags, 0644);
            if (target < 0) {
                /* M98: with the reason, now that open() infers one -
                 * "cannot open /dev/null" with no why cost a boot of
                 * guessing. */
                char why[48];
                why[0] = '\0';
                if (errno) {
                    strcpy(why, ": ");
                    strncat(why, strerror(errno), sizeof(why) - 3);
                }
                errmsg("cannot open ", path, why[0] ? why : 0);
                free(path);
                return -1;
            }
            free(path);
        }

        if (target != r->fd) {
            dup2(target, r->fd);
            close(target);
        }
    }
    return 0;
}

static void undo_redirs(RedirSave *save) {
    for (int i = save->n - 1; i >= 0; i--) {
        if (save->saved[i] >= 0) {
            dup2(save->saved[i], save->fd[i]);
            close(save->saved[i]);
        } else {
            close(save->fd[i]);
        }
    }
    save->n = 0;
    for (int i = 0; i < save->nhere; i++) {
        int st;
        waitpid(save->here[i], &st, 0);
    }
    save->nhere = 0;
}

/* ---- functions --------------------------------------------------------- */

typedef struct Func {
    struct Func *next;
    char *name;
    Node *body;
} Func;

static Func *funcs;

static Func *func_find(const char *name) {
    for (Func *f = funcs; f; f = f->next) {
        if (strcmp(f->name, name) == 0) {
            return f;
        }
    }
    return 0;
}

static void func_define(const char *name, Node *body) {
    Func *f = func_find(name);
    if (!f) {
        f = (Func *)malloc(sizeof(Func));
        if (!f) {
            die_oom();
        }
        f->name = xstrdup(name);
        f->next = funcs;
        funcs = f;
    }
    f->body = body;
}

/* ---- builtins ----------------------------------------------------------
 *
 * A builtin is a command that must run in *this* process because running
 * it in a child would be pointless - `cd` in a child changes that child's
 * directory and then the child exits - or because there is no program to
 * run: `test` and `true` are builtins here for the same reason M72's own
 * self-test noted, that this machine has no /bin/true and a script that
 * needs one should not have to care.
 */
static int background_pids[64];
static int nbackground;

static int bi_test(int argc, char **argv);

static int is_builtin(const char *name) {
    static const char *const names[] = {
        "cd", "pwd", "exit", "echo", "export", "unset", "set", "shift",
        "read", "test", "[", "true", "false", ":", "return", "break",
        "continue", ".", "source", "eval", "wait", "env", "unalias",
        /* M99: both asked for by name by somebody else's configure -
         * see run_builtin. */
        "exec", "trap", 0
    };
    for (int i = 0; names[i]; i++) {
        if (strcmp(name, names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int read_line_fd(int fd, Sbuf *out) {
    char c;
    int got = 0;
    for (;;) {
        long n = read(fd, &c, 1);
        if (n <= 0) {
            return got ? 1 : 0;
        }
        got = 1;
        if (c == '\n') {
            return 1;
        }
        sb_putc(out, c);
    }
}


/* ---- M99: trap ---------------------------------------------------------
 *
 * The second thing CPython's configure asks of a shell that this one did
 * not have. It uses two forms and both matter:
 *
 *   trap 'exit_status=$?; ...; rm -f ...; exit $exit_status' 0
 *   trap 'as_fn_exit 1' 1 2 13 15
 *
 * The first is how configure cleans up its temporary files, and without
 * it a configure run leaves conftest.* behind and - worse - the `exit
 * $exit_status` never happens, so the exit status of the whole script is
 * whatever the last command did.
 *
 * Signal 0 is EXIT and is not a signal at all: it means "when this shell
 * exits, for any reason". It is kept separately below for that reason
 * rather than as slot zero of an array of handlers, so that nothing can
 * accidentally kill(0) it.
 *
 * ---- what a trapped signal does here, and what it does not ------------
 *
 * The action is recorded and the signal is caught with a handler that
 * sets a flag; the flag is drained between commands, which is where
 * every shell runs a trap and is the only place a shell CAN run one -
 * running arbitrary script inside a signal handler is not something a
 * shell can do safely on any system. The consequence, stated because it
 * is a real divergence: a trap on a signal that arrives while a
 * foreground child is running is run when that child finishes, not the
 * instant it arrives.
 *
 * `trap - SIG` restores the default, `trap '' SIG` ignores it, and
 * `trap` with no arguments lists what is set - all three POSIX, all
 * three used by scripts in the wild.
 */
#define TRAP_MAX_SIG 32

static char *trap_action[TRAP_MAX_SIG]; /* by signal number */
static char *trap_exit_action;          /* signal 0 - see above */
static volatile sig_atomic_t trap_pending[TRAP_MAX_SIG];
static volatile sig_atomic_t trap_any_pending;

static void trap_handler(int sig) {
    if (sig > 0 && sig < TRAP_MAX_SIG) {
        trap_pending[sig] = 1;
        trap_any_pending = 1;
    }
}

/* The names POSIX requires, without the SIG prefix, so that `trap x INT`
 * and `trap x 2` and `trap x SIGINT` are all the same thing. */
typedef struct { const char *name; int sig; } TrapName;

static const TrapName TRAP_NAMES[] = {
    {"EXIT", 0},   {"HUP", SIGHUP},   {"INT", SIGINT},
    {"QUIT", SIGQUIT}, {"ILL", SIGILL}, {"ABRT", SIGABRT},
    {"FPE", SIGFPE}, {"KILL", SIGKILL}, {"SEGV", SIGSEGV},
    {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM},
    {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"CHLD", SIGCHLD},
    {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP},
    {0, 0}
};

/* The NAME for a number, for the listing form. POSIX says `trap` with no
 * arguments writes something that can be read back as input, and every
 * shell writes the name - so a number here would be a listing that is
 * correct and does not match, which is the only kind of difference a
 * differential test can see and the only kind worth having it see. */
static const char *trap_signame(int sig) {
    for (int i = 0; TRAP_NAMES[i].name; i++) {
        if (TRAP_NAMES[i].sig == sig) {
            return TRAP_NAMES[i].name;
        }
    }
    return 0;
}

static int trap_signo(const char *name) {
    const TrapName *NAMES = TRAP_NAMES;
    if (name[0] >= '0' && name[0] <= '9') {
        int n = atoi(name);
        return (n >= 0 && n < TRAP_MAX_SIG) ? n : -1;
    }
    if (strncmp(name, "SIG", 3) == 0) {
        name += 3;
    }
    for (int i = 0; NAMES[i].name; i++) {
        if (strcmp(name, NAMES[i].name) == 0) {
            return NAMES[i].sig;
        }
    }
    return -1;
}

static void trap_set(int sig, const char *action) {
    char **slot = (sig == 0) ? &trap_exit_action : &trap_action[sig];
    free(*slot);
    *slot = 0;
    if (action) {
        *slot = xstrdup(action);
    }
    if (sig == 0) {
        return;
    }
    /* SIGKILL and SIGSTOP cannot be caught anywhere, and saying so by
     * refusing to install rather than by pretending is the same choice
     * `chmod` made in M65. */
    if (sig == SIGKILL || sig == SIGSTOP) {
        return;
    }
    if (!action) {
        signal(sig, SIG_DFL);
    } else if (action[0] == '\0') {
        signal(sig, SIG_IGN);
    } else {
        signal(sig, trap_handler);
    }
}

/* Run between commands - see the note above on why not in the handler. */
static void trap_run_pending(void) {
    if (!trap_any_pending) {
        return;
    }
    trap_any_pending = 0;
    for (int sig = 1; sig < TRAP_MAX_SIG; sig++) {
        if (!trap_pending[sig]) {
            continue;
        }
        trap_pending[sig] = 0;
        if (trap_action[sig] && trap_action[sig][0]) {
            int saved = last_status;
            exec_text(trap_action[sig]);
            last_status = saved;
        }
    }
}

/* Run on the way out, from every path that leaves the shell. `$?` inside
 * the action is the status the shell is exiting with, which is what
 * configure's own trap reads on its first line. */
static void trap_run_exit(void) {
    char *action = trap_exit_action;
    if (!action || !action[0]) {
        return;
    }
    /* Cleared first: an EXIT trap that itself exits must not re-enter. */
    trap_exit_action = 0;
    exec_text(action);
    free(action);
}

static int run_builtin(int argc, char **argv) {
    const char *cmd = argv[0];

    if (strcmp(cmd, ":") == 0 || strcmp(cmd, "true") == 0) {
        return 0;
    }
    if (strcmp(cmd, "false") == 0) {
        return 1;
    }
    if (strcmp(cmd, "exit") == 0) {
        flow = FLOW_EXIT;
        exit_code = argc > 1 ? atoi(argv[1]) : last_status;
        return exit_code;
    }
    /* ---- M99: exec --------------------------------------------------
     *
     * Two jobs in one word, and configure uses both:
     *
     *   exec 5>>config.log     redirections that OUTLIVE the command,
     *                          because there is no command. Every
     *                          `>&5` in the rest of the script depends
     *                          on this one line having worked.
     *   exec sh "$0" "$@"      replace this shell with a program.
     *                          autoconf's preamble re-executes itself
     *                          under a better shell this way.
     *
     * The first falls out for free and is the reason `exec` has to be a
     * builtin rather than a program: apply_redirs has already run by
     * the time this is reached, and returning without asking for them
     * to be undone is exactly "make them permanent". exec_simple is
     * what honours the flag.
     */
    if (strcmp(cmd, "exec") == 0) {
        if (argc == 1) {
            exec_keep_redirs = 1;
            return 0;
        }
        char **args = argv + 1;
        /* PATH lookup, and then the diagnostic. POSIX says a
         * non-interactive shell EXITS when exec cannot find the
         * program, and that is not pedantry here: autoconf's re-exec is
         * followed by `printf ... "could not re-execute"; exit 255`,
         * which is dead code on every working shell and must stay dead
         * on this one. */
        execvp(args[0], args);
        errmsg("exec: ", args[0], ": not found");
        if (!interactive) {
            trap_run_exit();
            _exit(127);
        }
        return 127;
    }
    if (strcmp(cmd, "trap") == 0) {
        if (argc == 1) {
            /* POSIX: list the traps in a form that can be re-read as
             * input. The quoting is single quotes, which is what dash
             * and bash both emit. */
            /* Written with this shell's own out_fd_str rather than
             * with printf, and that is not style. Everything else here
             * reaches fd 1 through write(); stdio buffers. Mixing the
             * two reorders the output - the listing came out at exit,
             * after lines that were written later - which the fixture
             * caught as a diff in position rather than in content. */
            for (int sig = 0; sig < TRAP_MAX_SIG; sig++) {
                const char *action = (sig == 0) ? trap_exit_action
                                                : trap_action[sig];
                if (!action) {
                    continue;
                }
                const char *name = trap_signame(sig);
                out_fd_str(1, "trap -- '");
                out_fd_str(1, action);
                out_fd_str(1, "' ");
                out_fd_str(1, name ? name : num_to_str(sig));
                out_fd_str(1, "\n");
            }
            return 0;
        }
        /* `trap ACTION SIG...`, and the one ambiguity POSIX resolves by
         * looking at the first argument: `trap - INT` and `trap 2` mean
         * different things, because a first argument that is a valid
         * signal name with no signals after it is a RESET of that
         * signal rather than an action. */
        int first = 1;
        const char *action = argv[1];
        int reset = 0;
        if (strcmp(action, "-") == 0) {
            reset = 1;
            first = 2;
        } else if (argc == 2 && trap_signo(action) >= 0) {
            reset = 1;
            first = 1;
        } else {
            first = 2;
        }
        if (first >= argc) {
            errmsg("trap: ", "no signal given", 0);
            return 1;
        }
        for (int i = first; i < argc; i++) {
            int sig = trap_signo(argv[i]);
            if (sig < 0) {
                errmsg("trap: ", argv[i], ": bad signal");
                return 1;
            }
            trap_set(sig, reset ? 0 : action);
        }
        return 0;
    }
    if (strcmp(cmd, "return") == 0) {
        flow = FLOW_RETURN;
        return argc > 1 ? atoi(argv[1]) : last_status;
    }
    if (strcmp(cmd, "break") == 0 || strcmp(cmd, "continue") == 0) {
        flow = cmd[0] == 'b' ? FLOW_BREAK : FLOW_CONTINUE;
        flow_levels = argc > 1 ? atoi(argv[1]) : 1;
        if (flow_levels < 1) {
            flow_levels = 1;
        }
        return 0;
    }
    if (strcmp(cmd, "cd") == 0) {
        const char *where = argc > 1 ? argv[1] : var_get("HOME");
        if (!where[0]) {
            where = PATH_HOME;
        }
        if (chdir(where) != 0) {
            errmsg("cd: not a directory: ", where, 0);
            return 1;
        }
        char buf[PATH_MAX_LEN];
        if (getcwd(buf, sizeof(buf))) {
            var_set("PWD", buf);
        }
        return 0;
    }
    if (strcmp(cmd, "pwd") == 0) {
        char buf[PATH_MAX_LEN];
        if (!getcwd(buf, sizeof(buf))) {
            return 1;
        }
        out_fd_str(1, buf);
        out_fd_str(1, "\n");
        return 0;
    }
    if (strcmp(cmd, "echo") == 0) {
        int start = 1;
        int newline = 1;
        if (argc > 1 && strcmp(argv[1], "-n") == 0) {
            newline = 0;
            start = 2;
        }
        for (int i = start; i < argc; i++) {
            if (i > start) {
                out_fd_str(1, " ");
            }
            out_fd_str(1, argv[i]);
        }
        if (newline) {
            out_fd_str(1, "\n");
        }
        return 0;
    }
    if (strcmp(cmd, "export") == 0) {
        if (argc == 1) {
            for (Var *v = vars; v; v = v->next) {
                if (v->exported) {
                    out_fd_str(1, "export ");
                    out_fd_str(1, v->name);
                    out_fd_str(1, "=");
                    out_fd_str(1, v->value ? v->value : "");
                    out_fd_str(1, "\n");
                }
            }
            return 0;
        }
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq) {
                *eq = '\0';
                var_set(argv[i], eq + 1);
                var_export(argv[i]);
                *eq = '=';
            } else {
                var_export(argv[i]);
            }
        }
        return 0;
    }
    if (strcmp(cmd, "unset") == 0) {
        for (int i = 1; i < argc; i++) {
            var_unset(argv[i]);
        }
        return 0;
    }
    if (strcmp(cmd, "env") == 0) {
        for (int i = 0; environ && environ[i]; i++) {
            out_fd_str(1, environ[i]);
            out_fd_str(1, "\n");
        }
        return 0;
    }
    if (strcmp(cmd, "set") == 0) {
        if (argc == 1) {
            for (Var *v = vars; v; v = v->next) {
                out_fd_str(1, v->name);
                out_fd_str(1, "=");
                out_fd_str(1, v->value ? v->value : "");
                out_fd_str(1, "\n");
            }
            return 0;
        }
        int i = 1;
        int saw_dashdash = 0;
        for (; i < argc; i++) {
            const char *a = argv[i];
            if (strcmp(a, "--") == 0) {
                saw_dashdash = 1;
                i++;
                break;
            }
            if (a[0] != '-' && a[0] != '+') {
                break;
            }
            if (!a[1]) {
                break; /* a bare `-` is an argument, not an option */
            }
            int on = (a[0] == '-');
            if (a[1] == 'o') {
                /* `set -o` with no name lists; with one, sets it. */
                if (i + 1 >= argc) {
                    static const struct { const char *name; int *flag; } OPTS[] = {
                        {"errexit", &opt_errexit}, {"nounset", &opt_nounset},
                        {"noglob", &opt_noglob},   {"xtrace", &opt_xtrace},
                        {0, 0}
                    };
                    for (int k = 0; OPTS[k].name; k++) {
                        out_fd_str(1, OPTS[k].name);
                        out_fd_str(1, *OPTS[k].flag ? "\ton\n" : "\toff\n");
                    }
                    /* Named because configure asks for it by name, and
                     * the answer is true: this shell has one mode. */
                    out_fd_str(1, "posix\ton\n");
                    continue;
                }
                const char *name = argv[++i];
                if (strcmp(name, "errexit") == 0) { opt_errexit = on; }
                else if (strcmp(name, "nounset") == 0) { opt_nounset = on; }
                else if (strcmp(name, "noglob") == 0) { opt_noglob = on; }
                else if (strcmp(name, "xtrace") == 0) { opt_xtrace = on; }
                else if (strcmp(name, "posix") == 0) { /* the only mode */ }
                else {
                    errmsg("set: ", name, ": no such option");
                    return 2;
                }
                continue;
            }
            for (const char *f = a + 1; *f; f++) {
                switch (*f) {
                    case 'e': opt_errexit = on; break;
                    case 'u': opt_nounset = on; break;
                    case 'f': opt_noglob = on; break;
                    case 'x': opt_xtrace = on; break;
                    default: {
                        char one[2] = {*f, 0};
                        errmsg("set: -", one, ": no such option");
                        return 2;
                    }
                }
            }
        }
        /* POSIX: options alone leave the positional parameters ALONE.
         * Only `--` or a first non-option argument replaces them - and
         * `set --` with nothing after it clears them. */
        if (saw_dashdash || i < argc) {
            set_positional(argv + i, argc - i);
        }
        return 0;
    }
    if (strcmp(cmd, "shift") == 0) {
        int n = argc > 1 ? atoi(argv[1]) : 1;
        if (n > pos_count || n < 0) {
            return 1;
        }
        /* Moved down in place rather than through set_positional, which
         * frees the old array before it reads the new one - and the new
         * one WAS the old one, one element in. A use-after-free that
         * `shift` alone reaches, found by the fixture script the first
         * time it shifted. */
        for (int i = 0; i < n; i++) {
            free(pos_params[i]);
        }
        for (int i = n; i < pos_count; i++) {
            pos_params[i - n] = pos_params[i];
        }
        pos_count -= n;
        return 0;
    }
    if (strcmp(cmd, "read") == 0) {
        Sbuf line;
        sb_init(&line);
        int got = read_line_fd(0, &line);
        const char *text = line.p ? line.p : "";
        if (argc <= 1) {
            var_set("REPLY", text);
        } else {
            /* Every name but the last gets one field; the last gets what
             * is left, which is the rule that makes `read name rest`
             * useful. */
            const char *p = text;
            const char *ifs = ifs_chars();
            for (int i = 1; i < argc; i++) {
                while (*p && strchr(ifs, *p)) {
                    p++;
                }
                if (i == argc - 1) {
                    var_set(argv[i], p);
                    break;
                }
                const char *start = p;
                while (*p && !strchr(ifs, *p)) {
                    p++;
                }
                char *word = (char *)malloc((size_t)(p - start) + 1);
                if (!word) {
                    die_oom();
                }
                memcpy(word, start, (size_t)(p - start));
                word[p - start] = '\0';
                var_set(argv[i], word);
                free(word);
            }
        }
        sb_free(&line);
        return got ? 0 : 1;
    }
    if (strcmp(cmd, "test") == 0 || strcmp(cmd, "[") == 0) {
        return bi_test(argc, argv);
    }
    if (strcmp(cmd, "eval") == 0) {
        Sbuf b;
        sb_init(&b);
        for (int i = 1; i < argc; i++) {
            if (i > 1) {
                sb_putc(&b, ' ');
            }
            sb_puts(&b, argv[i]);
        }
        int st = b.p ? exec_text(b.p) : 0;
        sb_free(&b);
        return st;
    }
    if (strcmp(cmd, ".") == 0 || strcmp(cmd, "source") == 0) {
        if (argc < 2) {
            errmsg(".: needs a file", 0, 0);
            return 2;
        }
        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            errmsg(".: cannot read ", argv[1], 0);
            return 1;
        }
        Sbuf text;
        sb_init(&text);
        char buf[512];
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            sb_putn(&text, buf, (size_t)n);
        }
        close(fd);
        int st = text.p ? exec_text(text.p) : 0;
        sb_free(&text);
        /* A sourced file's `return` ends the file, not the shell. */
        if (flow == FLOW_RETURN) {
            flow = FLOW_NONE;
        }
        return st;
    }
    if (strcmp(cmd, "wait") == 0) {
        int st = 0;
        if (argc > 1) {
            int status = 0;
            waitpid(atoi(argv[1]), &status, 0);
            st = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        } else {
            for (int i = 0; i < nbackground; i++) {
                int status = 0;
                waitpid(background_pids[i], &status, 0);
                st = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            }
            nbackground = 0;
        }
        return st;
    }
    if (strcmp(cmd, "unalias") == 0) {
        return 0; /* there are no aliases; a script that clears them is fine */
    }
    return 127;
}

/* `test` - the one builtin with a grammar of its own.
 *
 * Enough of it to be useful and not one operator more: the file tests a
 * script actually writes, string comparison, and integer comparison,
 * with `!`, `-a` and `-o`. No parentheses: they need a real parser and
 * `test a -a b -o c` is already further than most scripts go. */
static int test_one(int argc, char **argv, int *i);

static int file_is(const char *path, int want_dir) {
    DIR *d = opendir(path);
    if (d) {
        closedir(d);
        return want_dir;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    return !want_dir;
}

static int file_exists(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        closedir(d);
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

static long file_size(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    long end = lseek(fd, 0, SEEK_END);
    close(fd);
    return end;
}

static int test_one(int argc, char **argv, int *i) {
    if (*i >= argc) {
        return 0;
    }
    const char *a = argv[*i];

    if (strcmp(a, "!") == 0) {
        (*i)++;
        return !test_one(argc, argv, i);
    }
    if (a[0] == '-' && a[1] && !a[2] && *i + 1 < argc) {
        const char *arg = argv[*i + 1];
        *i += 2;
        switch (a[1]) {
            case 'e': return file_exists(arg);
            case 'f': return file_is(arg, 0);
            case 'd': return file_is(arg, 1);
            case 'r': return file_exists(arg);
            case 'w': return file_exists(arg);
            case 'x': return file_exists(arg);
            case 's': return file_size(arg) > 0;
            case 'z': return arg[0] == '\0';
            case 'n': return arg[0] != '\0';
            default:  return arg[0] != '\0';
        }
    }
    /* A binary operator, if the next word is one. */
    if (*i + 2 < argc) {
        const char *op = argv[*i + 1];
        const char *b = argv[*i + 2];
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            *i += 3;
            return strcmp(a, b) == 0;
        }
        if (strcmp(op, "!=") == 0) {
            *i += 3;
            return strcmp(a, b) != 0;
        }
        if (op[0] == '-' && strlen(op) == 3) {
            long x = atol(a), y = atol(b);
            *i += 3;
            if (strcmp(op, "-eq") == 0) { return x == y; }
            if (strcmp(op, "-ne") == 0) { return x != y; }
            if (strcmp(op, "-lt") == 0) { return x < y; }
            if (strcmp(op, "-le") == 0) { return x <= y; }
            if (strcmp(op, "-gt") == 0) { return x > y; }
            if (strcmp(op, "-ge") == 0) { return x >= y; }
            *i -= 3;
        }
    }
    (*i)++;
    return a[0] != '\0';
}

static int bi_test(int argc, char **argv) {
    /* `[ ... ]` must end with its bracket, and `test` must not have one. */
    if (strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            errmsg("[: missing `]'", 0, 0);
            return 2;
        }
        argc--;
    }
    if (argc <= 1) {
        return 1;
    }
    int i = 1;
    int result = test_one(argc, argv, &i);
    while (i < argc) {
        if (strcmp(argv[i], "-a") == 0) {
            i++;
            result = test_one(argc, argv, &i) && result;
        } else if (strcmp(argv[i], "-o") == 0) {
            i++;
            int rhs = test_one(argc, argv, &i);
            result = result || rhs;
        } else {
            break;
        }
    }
    return result ? 0 : 1;
}

/* ---- the evaluator -----------------------------------------------------
 *
 * One function per node kind, recursion for the nesting, and a check of
 * `flow` after anything that could have run a `break`. The three places
 * that fork - a pipeline stage, a subshell, and `&` - are the only
 * places a command's effect on this shell's own state is deliberately
 * thrown away.
 */
static int exec_list_node(Node *n) {
    return n ? exec_node(n) : last_status;
}

/* Looks a command up the way a shell does: a name with a slash is a
 * path, and a name without one is tried in each directory of $PATH.
 * $PATH is one directory on this machine and the environment is where
 * that is written down, which is M75's whole point. */
static int exec_external(char **argv, Node *n) {
    pid_t pid = fork();
    if (pid < 0) {
        errmsg("cannot fork", 0, 0);
        return 1;
    }
    if (pid == 0) {
        RedirSave dummy;
        memset(&dummy, 0, sizeof(dummy));
        if (apply_redirs(n->redirs, 0) != 0) {
            _exit(1);
        }
        /* `FOO=bar cmd` - the child's environment, and only the child's. */
        for (int i = 0; i < n->nassigns; i++) {
            char *copy = xstrdup(n->assigns[i]);
            char *eq = strchr(copy, '=');
            *eq = '\0';
            char *value = expand_one(eq + 1);
            setenv(copy, value, 1);
            free(value);
            free(copy);
        }

        if (strchr(argv[0], '/')) {
            execve(argv[0], argv, environ);
        } else {
            const char *path = var_get("PATH");
            if (!path[0]) {
                path = PATH_DEFAULT; /* M98: "/bin:/usr/bin" - see paths.h */
            }
            const char *p = path;
            while (*p) {
                const char *end = strchr(p, ':');
                size_t len = end ? (size_t)(end - p) : strlen(p);
                Sbuf full;
                sb_init(&full);
                sb_putn(&full, p, len);
                if (len == 0 || p[len - 1] != '/') {
                    sb_putc(&full, '/');
                }
                sb_puts(&full, argv[0]);
                execve(full.p, argv, environ);
                sb_free(&full);
                if (!end) {
                    break;
                }
                p = end + 1;
            }
        }
        out_fd_str(2, "sh: ");
        out_fd_str(2, argv[0]);
        out_fd_str(2, ": command not found\n");
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int call_function(Func *f, char **argv, int argc) {
    char **saved = pos_params;
    int saved_n = pos_count;
    char *saved_name = script_name;
    pos_params = 0;
    pos_count = 0;
    set_positional(argv + 1, argc - 1);

    int st = exec_list_node(f->body);
    if (flow == FLOW_RETURN) {
        flow = FLOW_NONE;
    }

    for (int i = 0; i < pos_count; i++) {
        free(pos_params[i]);
    }
    free(pos_params);
    pos_params = saved;
    pos_count = saved_n;
    script_name = saved_name;
    return st;
}

static int exec_simple(Node *n) {
    Vec argv;
    vec_init(&argv);
    for (int i = 0; i < n->nwords; i++) {
        expand_word(n->words[i], &argv, 1, 1);
    }

    /* M99: `set -x`. After expansion, because what a person debugging a
     * script needs to see is the command that is about to run and not
     * the one that was written. `+ ` and a space between words is the
     * format every shell uses, and PS4 is not honoured here because
     * nothing sets it and a variable nobody writes is a feature that
     * cannot be wrong. */
    if (opt_xtrace && argv.n) {
        out_fd_str(2, "+");
        for (int i = 0; i < argv.n; i++) {
            out_fd_str(2, " ");
            out_fd_str(2, argv.v[i]);
        }
        out_fd_str(2, "\n");
    }

    if (argv.n == 0) {
        /* Assignments alone, or a bare redirect: `x=1` and `> file` are
         * both commands that do something and run nothing. */
        RedirSave save;
        memset(&save, 0, sizeof(save));
        int rc = 0;
        if (n->redirs) {
            rc = apply_redirs(n->redirs, &save);
        }
        for (int i = 0; i < n->nassigns; i++) {
            char *copy = xstrdup(n->assigns[i]);
            char *eq = strchr(copy, '=');
            *eq = '\0';
            char *value = expand_one(eq + 1);
            var_set(copy, value);
            free(value);
            free(copy);
        }
        if (n->redirs) {
            undo_redirs(&save);
        }
        vec_free(&argv);
        return rc == 0 ? 0 : 1;
    }

    Func *f = func_find(argv.v[0]);
    if (f || is_builtin(argv.v[0])) {
        /* Run here, so that `cd` and `read` and a function's assignments
         * mean something after the command finishes. */
        RedirSave save;
        memset(&save, 0, sizeof(save));
        if (n->redirs && apply_redirs(n->redirs, &save) != 0) {
            vec_free(&argv);
            return 1;
        }
        for (int i = 0; i < n->nassigns; i++) {
            char *copy = xstrdup(n->assigns[i]);
            char *eq = strchr(copy, '=');
            *eq = '\0';
            char *value = expand_one(eq + 1);
            var_set(copy, value);
            free(value);
            free(copy);
        }
        /* M99: `exec` with no command sets this, and what it means is
         * "do not put the descriptors back". That is the whole of the
         * first half of exec: the redirections have already been
         * applied above, so making them permanent is not doing
         * something extra - it is skipping the undo. */
        exec_keep_redirs = 0;
        int st = f ? call_function(f, argv.v, argv.n) : run_builtin(argv.n, argv.v);
        if (n->redirs && !exec_keep_redirs) {
            undo_redirs(&save);
        }
        exec_keep_redirs = 0;
        vec_free(&argv);
        return st;
    }

    int st = exec_external(argv.v, n);
    vec_free(&argv);
    return st;
}

/* A pipeline is N processes and N-1 pipes, and every stage - not just a
 * program - runs in one of them: `echo a | while read x; do ...; done` is
 * a loop in a forked shell, which is why this runs exec_node in the
 * child rather than looking for a command to exec. */
static int exec_pipeline(Node *n) {
    Node *stages[32];
    int count = 0;
    Node *cur = n;
    while (cur->kind == N_PIPE && count < 30) {
        stages[count++] = cur->right;
        cur = cur->left;
    }
    stages[count++] = cur;
    /* Collected right to left; run them left to right. */
    for (int i = 0; i < count / 2; i++) {
        Node *t = stages[i];
        stages[i] = stages[count - 1 - i];
        stages[count - 1 - i] = t;
    }

    int in_fd = -1;
    pid_t pids[32];
    for (int i = 0; i < count; i++) {
        int fds[2] = { -1, -1 };
        if (i + 1 < count && pipe(fds) != 0) {
            errmsg("cannot create a pipe", 0, 0);
            return 1;
        }
        pid_t pid = fork();
        if (pid < 0) {
            errmsg("cannot fork", 0, 0);
            return 1;
        }
        if (pid == 0) {
            if (in_fd >= 0) {
                dup2(in_fd, 0);
                close(in_fd);
            }
            if (fds[1] >= 0) {
                close(fds[0]);
                dup2(fds[1], 1);
                close(fds[1]);
            }
            int st = exec_node(stages[i]);
            _exit(st);
        }
        pids[i] = pid;
        if (in_fd >= 0) {
            close(in_fd);
        }
        if (fds[1] >= 0) {
            close(fds[1]);
            in_fd = fds[0];
        }
    }

    int st = 0;
    for (int i = 0; i < count; i++) {
        int status = 0;
        waitpid(pids[i], &status, 0);
        /* The pipeline's status is the LAST stage's, which is what
         * `grep x file | head` returning 0 depends on. */
        if (i == count - 1) {
            st = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
    }
    return st;
}

static int exec_case(Node *n) {
    char *subject = expand_one(n->name);
    int st = 0;
    for (CaseItem *it = n->cases; it; it = it->next) {
        for (int i = 0; i < it->npats; i++) {
            char *pat = expand_pattern(it->pats[i]);
            int hit = glob_match(pat, subject);
            free(pat);
            if (hit) {
                st = exec_list_node(it->body);
                free(subject);
                return st;
            }
        }
    }
    free(subject);
    return st;
}

static int exec_for(Node *n) {
    Vec items;
    vec_init(&items);
    for (int i = 0; i < n->nwords; i++) {
        expand_word(n->words[i], &items, 1, 1);
    }
    int st = 0;
    for (int i = 0; i < items.n; i++) {
        var_set(n->name, items.v[i]);
        st = exec_list_node(n->right);
        if (flow == FLOW_BREAK) {
            if (--flow_levels <= 0) {
                flow = FLOW_NONE;
            }
            break;
        }
        if (flow == FLOW_CONTINUE) {
            if (--flow_levels <= 0) {
                flow = FLOW_NONE;
                continue;
            }
            break;
        }
        if (flow != FLOW_NONE) {
            break;
        }
    }
    vec_free(&items);
    return st;
}

static int exec_while(Node *n, int until) {
    int st = 0;
    for (;;) {
        /* The condition is asked about, not relied on - see
         * errexit_suspend. */
        errexit_suspend++;
        int cond = exec_list_node(n->left);
        errexit_suspend--;
        if (flow != FLOW_NONE) {
            break;
        }
        int go = until ? (cond != 0) : (cond == 0);
        if (!go) {
            break;
        }
        st = exec_list_node(n->right);
        if (flow == FLOW_BREAK) {
            if (--flow_levels <= 0) {
                flow = FLOW_NONE;
            }
            break;
        }
        if (flow == FLOW_CONTINUE) {
            if (--flow_levels <= 0) {
                flow = FLOW_NONE;
                continue;
            }
            break;
        }
        if (flow != FLOW_NONE) {
            break;
        }
    }
    return st;
}

static int exec_forked(Node *body) {
    pid_t pid = fork();
    if (pid < 0) {
        errmsg("cannot fork", 0, 0);
        return 1;
    }
    if (pid == 0) {
        int st = exec_list_node(body);
        _exit(st);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int exec_node_inner(Node *n);

static int exec_node(Node *n) {
    if (!n || flow != FLOW_NONE) {
        return last_status;
    }
    /* M99: a trapped signal runs HERE - between commands - and not in
     * the handler that caught it. That is where every shell runs one and
     * it is the only place a shell can: the action is arbitrary script,
     * and arbitrary script inside a signal handler is not something any
     * of them attempts. See trap_run_pending. */
    trap_run_pending();
    int st_ee = exec_node_inner(n);
    /* M99: `set -e`. Checked here, once, where every command comes back,
     * and only for the kinds whose status is RELIED ON rather than asked
     * about - a compound command runs its own children through this same
     * function, so checking it again would exit on an `if` whose
     * condition was simply false. */
    if (opt_errexit && st_ee != 0 && !errexit_suspend && flow == FLOW_NONE &&
        (n->kind == N_SIMPLE || n->kind == N_PIPE)) {
        flow = FLOW_EXIT;
        exit_code = st_ee;
    }
    return st_ee;
}

static int exec_node_inner(Node *n) {
    switch (n->kind) {
        case N_SIMPLE:
            return exec_simple(n);
        case N_SEQ: {
            int st = exec_node(n->left);
            last_status = st;
            if (flow != FLOW_NONE) {
                return st;
            }
            return exec_node(n->right);
        }
        case N_AND: {
            /* The LEFT of `&&` is a condition and is exempt from
             * errexit; the right is not, because its status is the
             * status of the whole thing. POSIX spells this out, and it
             * is the difference between `set -e` being usable and every
             * `grep -q x file && something` ending the script. */
            errexit_suspend++;
            int st = exec_node(n->left);
            errexit_suspend--;
            last_status = st;
            if (st != 0 || flow != FLOW_NONE) {
                return st;
            }
            return exec_node(n->right);
        }
        case N_OR: {
            errexit_suspend++;
            int st = exec_node(n->left);
            errexit_suspend--;
            last_status = st;
            if (st == 0 || flow != FLOW_NONE) {
                return st;
            }
            return exec_node(n->right);
        }
        case N_NOT: {
            errexit_suspend++;
            int st = exec_node(n->left);
            errexit_suspend--;
            return st == 0 ? 1 : 0;
        }
        case N_PIPE:
            return exec_pipeline(n);
        case N_BG: {
            pid_t pid = fork();
            if (pid < 0) {
                errmsg("cannot fork", 0, 0);
                return 1;
            }
            if (pid == 0) {
                int st = exec_node(n->left);
                _exit(st);
            }
            if (nbackground < (int)(sizeof(background_pids) / sizeof(background_pids[0]))) {
                background_pids[nbackground++] = (int)pid;
            }
            /* No job number and no `jobs` to print it in - see this
             * file's header for why job control is M98's and not this
             * milestone's. The pid is announced because `wait` takes one
             * and a person has no other way to learn it. */
            out_fd_str(1, "[");
            out_fd_str(1, num_to_str(pid));
            out_fd_str(1, "]\n");
            return 0;
        }
        case N_IF: {
            RedirSave save;
            memset(&save, 0, sizeof(save));
            if (n->redirs && apply_redirs(n->redirs, &save) != 0) {
                return 1;
            }
            /* The condition is asked about, not relied on - see
             * errexit_suspend. Without this, `set -e` plus any
             * `if test -f x` ends the script the first time the file is
             * not there, which is the opposite of what the `if` is for. */
            errexit_suspend++;
            int cond = exec_list_node(n->left);
            errexit_suspend--;
            int st;
            if (flow != FLOW_NONE) {
                st = cond;
            } else if (cond == 0) {
                st = exec_list_node(n->right);
            } else {
                st = n->third ? exec_list_node(n->third) : 0;
            }
            if (n->redirs) {
                undo_redirs(&save);
            }
            return st;
        }
        case N_WHILE:
        case N_UNTIL:
        case N_FOR:
        case N_CASE:
        case N_GROUP: {
            RedirSave save;
            memset(&save, 0, sizeof(save));
            if (n->redirs && apply_redirs(n->redirs, &save) != 0) {
                return 1;
            }
            int st;
            if (n->kind == N_WHILE) {
                st = exec_while(n, 0);
            } else if (n->kind == N_UNTIL) {
                st = exec_while(n, 1);
            } else if (n->kind == N_FOR) {
                st = exec_for(n);
            } else if (n->kind == N_CASE) {
                st = exec_case(n);
            } else {
                st = exec_list_node(n->left);
            }
            if (n->redirs) {
                undo_redirs(&save);
            }
            return st;
        }
        case N_SUBSHELL: {
            RedirSave save;
            memset(&save, 0, sizeof(save));
            if (n->redirs && apply_redirs(n->redirs, &save) != 0) {
                return 1;
            }
            int st = exec_forked(n->left);
            if (n->redirs) {
                undo_redirs(&save);
            }
            return st;
        }
        case N_FUNC:
            func_define(n->name, n->left);
            return 0;
        default:
            return 0;
    }
}

/* ---- running text ------------------------------------------------------
 *
 * Parse and evaluate a string, with the lexer's whole state saved around
 * it. `eval`, `.`, and every `$( )` come through here, and every one of
 * them can happen in the middle of parsing something else. */
static int exec_text(const char *text) {
    Lexer save_lx = lx;
    Tok save_tok = tok;
    Arena *save_arena = cur_arena;
    int save_err = parse_error;
    int save_depth = parse_depth;
    int save_interactive = interactive;

    Arena a;
    memset(&a, 0, sizeof(a));
    cur_arena = &a;
    memset(&lx, 0, sizeof(lx));
    lx.src = text;
    parse_error = 0;
    parse_depth = 0;
    interactive = 0;

    advance();
    Node *prog = parse_list(0);
    int st = last_status;
    if (parse_error) {
        st = 2;
    } else if (prog) {
        st = exec_node(prog);
        last_status = st;
    }

    arena_free(&a);
    cur_arena = save_arena;
    lx = save_lx;
    tok = save_tok;
    parse_error = save_err;
    parse_depth = save_depth;
    interactive = save_interactive;
    return st;
}

/* ---- input -------------------------------------------------------------- */

static int read_line_interactive(Sbuf *buf) {
    for (;;) {
        char c;
        long n = read(0, &c, 1);
        if (n <= 0) {
            return buf->len > 0;
        }
        if (c == '\n' || c == '\r') {
            write(1, "\n", 1);
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            if (buf->len > 0) {
                buf->p[--buf->len] = '\0';
                write(1, "\b \b", 3);
            }
            continue;
        }
        sb_putc(buf, c);
        write(1, &c, 1);
    }
}

/* A construct can span lines, so an incomplete parse is not an error at
 * a prompt - it is a request for the next line. The accumulated text is
 * re-parsed from the start each time, which costs nothing at this size
 * and means there is exactly one parser rather than one for whole
 * commands and another for continuations. */
static void interactive_loop(void) {
    Sbuf pending;
    sb_init(&pending);
    interactive = 1;

    for (;;) {
        char dir[PATH_MAX_LEN];
        if (pending.len == 0) {
            if (getcwd(dir, sizeof(dir))) {
                out_fd_str(1, dir);
            }
            out_fd_str(1, " $ ");
        } else {
            out_fd_str(1, "> ");
        }

        Sbuf line;
        sb_init(&line);
        if (!read_line_interactive(&line)) {
            sb_free(&line);
            break;
        }
        sb_puts(&pending, line.p ? line.p : "");
        sb_putc(&pending, '\n');
        sb_free(&line);

        Arena a;
        memset(&a, 0, sizeof(a));
        cur_arena = &a;
        memset(&lx, 0, sizeof(lx));
        lx.src = pending.p;
        parse_error = 0;
        parse_depth = 0;
        advance();
        Node *prog = parse_list(0);

        if (lx.incomplete) {
            arena_free(&a);
            continue; /* keep reading - the quote or the `fi` is on the next line */
        }
        if (!parse_error && prog) {
            last_status = exec_node(prog);
        }
        arena_free(&a);
        sb_free(&pending);
        sb_init(&pending);

        if (flow == FLOW_EXIT) {
            break;
        }
        flow = FLOW_NONE;
    }
    sb_free(&pending);
}

static int run_script_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        errmsg("cannot read ", path, 0);
        return 127;
    }
    Sbuf text;
    sb_init(&text);
    char buf[1024];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        sb_putn(&text, buf, (size_t)n);
    }
    close(fd);
    int st = text.p ? exec_text(text.p) : 0;
    sb_free(&text);
    return flow == FLOW_EXIT ? exit_code : st;
}

/* M99: the last thing the top-level shell does.
 *
 * Only the top-level shell: every other exit in this file is a forked
 * child - a pipeline stage, a subshell, a command substitution - and
 * POSIX says a subshell starts with the parent's caught traps reset to
 * their defaults. Running the parent's EXIT action once per subshell
 * would delete configure's temporary files in the middle of the run
 * that is using them.
 *
 * `$?` inside the action is the status the shell is exiting with, which
 * is what the first line of configure's own EXIT trap reads. */
static int shell_leaving(int status) {
    last_status = status;
    flow = FLOW_NONE; /* an `exit` already unwound; the action gets to run */
    trap_run_exit();
    return status;
}

int main(int argc, char **argv) {
    shell_pid = getpid();

    /* M98: PATH exists in the environment from here on down, not just
     * in two fallbacks. execvp and this shell's own lookup both default
     * to PATH_DEFAULT when PATH is unset, but a default a child cannot
     * *read* is half a truth: gcc's driver locates cc1 by finding
     * itself through getenv("PATH"), and with no PATH it degraded to
     * prefixes relative to the literal string "gcc" - "../libexec/..."
     * from whatever the working directory was. On every other Unix,
     * login infrastructure exports PATH before anything runs; on this
     * machine the shell IS the login infrastructure, so it does. The 0
     * means an inherited PATH is left exactly as it came. */
    setenv("PATH", PATH_DEFAULT, 0);

    /* A descriptor to complain on.
     *
     * A task on this machine starts with fd 0 and fd 1 and nothing else -
     * `sched.h` says so - so every error this shell has written to fd 2
     * has gone nowhere at all, including "command not found". That is not
     * a thing to work around silently: a shell whose diagnostics vanish
     * is worse than one with none, because the script looks like it
     * worked. If fd 2 is not open, it becomes a copy of fd 1, which is
     * what a login shell inherits everywhere else. `2>file` still
     * redirects it afterwards, because it is now a real descriptor to
     * redirect. */
    if (write(2, "", 0) < 0) {
        dup2(1, 2);
    }
    if (!var_is_set("IFS")) {
        var_set("IFS", " \t\n");
    }

    /* `sh -c 'text'` - how every program that runs a command line runs
     * one, and the form `./configure` uses on itself. */
    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        script_name = argv[0];
        if (argc > 3) {
            set_positional(argv + 3, argc - 3);
        }
        int st = exec_text(argv[2]);
        return shell_leaving(flow == FLOW_EXIT ? exit_code : st);
    }

    if (argc > 1 && argv[1][0]) {
        script_name = argv[1];
        if (argc > 2) {
            set_positional(argv + 2, argc - 2);
        }
        return shell_leaving(run_script_file(argv[1]));
    }

    interactive_loop();
    return shell_leaving(flow == FLOW_EXIT ? exit_code : last_status);
}
