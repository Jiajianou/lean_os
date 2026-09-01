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
#include <fcntl.h>
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
        r->here_next = lx.pending_here;
        lx.pending_here = r;
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
        advance();
        if (!was_quoted && tok_is_op("(")) {
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
        /* Not a function: put the word back by re-lexing from where it
         * started. Cheaper than a token queue, and this is the only place
         * that needs two tokens of lookahead. */
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
} Ex;

static void ex_flush(Ex *e) {
    if (!e->started) {
        return;
    }
    char *field = e->cur.p ? e->cur.p : xstrdup("");
    if (!e->cur.p) {
        vec_push(e->out, field);
    } else if (e->glob && !e->quoted_here && has_glob(field)) {
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

/* ${...} - the brace form, where the whole `:-` family lives. */
static void expand_braced(Ex *e, const char *body, int in_quotes) {
    if (body[0] == '#' && body[1]) {
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
                e->started = 0;
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
         * thing scripts write. */
        Vec pv;
        vec_init(&pv);
        Ex pe;
        memset(&pe, 0, sizeof(pe));
        sb_init(&pe.cur);
        pe.out = &pv;
        pe.split = 0;
        pe.glob = 0;
        const char *p = word;
        while (*p) {
            if (*p == '$') {
                expand_dollar(&pe, &p, 1);
                continue;
            }
            ex_add(&pe, p, 1);
            p++;
        }
        ex_flush(&pe);
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
             * which is what makes `${x:-$HOME}` work. */
            Vec wv;
            vec_init(&wv);
            Ex we;
            memset(&we, 0, sizeof(we));
            sb_init(&we.cur);
            we.out = &wv;
            we.split = 0;
            we.glob = 0;
            const char *p = word;
            while (*p) {
                if (*p == '$') {
                    expand_dollar(&we, &p, 1);
                    continue;
                }
                if (*p == '\'' || *p == '"') {
                    char q = *p++;
                    while (*p && *p != q) {
                        ex_add(&we, p, 1);
                        p++;
                    }
                    if (*p) {
                        p++;
                    }
                    we.started = 1;
                    continue;
                }
                ex_add(&we, p, 1);
                p++;
            }
            ex_flush(&we);
            char *val = wv.n ? xstrdup(wv.v[0]) : xstrdup("");
            vec_free(&wv);
            if (kind == '=') {
                var_set(name, val);
            }
            if (in_quotes) {
                ex_add(e, val, strlen(val));
            } else {
                ex_add_split(e, val);
            }
            free(val);
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

static void expand_dollar(Ex *e, const char **pp, int in_quotes) {
    const char *p = *pp + 1; /* past '$' */
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
            if (*p == '\\' && p[1] && strchr("$`\\", p[1])) {
                ex_add(&e, p + 1, 1);
                p += 2;
                continue;
            }
            if (*p == '$') {
                expand_dollar(&e, &p, 1);
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
                errmsg("cannot open ", path, 0);
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
        "continue", ".", "source", "eval", "wait", "env", "unalias", 0
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
        if (strcmp(argv[1], "--") == 0) {
            i = 2;
        }
        set_positional(argv + i, argc - i);
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
                path = PATH_BIN;
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
        int st = f ? call_function(f, argv.v, argv.n) : run_builtin(argv.n, argv.v);
        if (n->redirs) {
            undo_redirs(&save);
        }
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
        int cond = exec_list_node(n->left);
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

static int exec_node(Node *n) {
    if (!n || flow != FLOW_NONE) {
        return last_status;
    }
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
            int st = exec_node(n->left);
            last_status = st;
            if (st != 0 || flow != FLOW_NONE) {
                return st;
            }
            return exec_node(n->right);
        }
        case N_OR: {
            int st = exec_node(n->left);
            last_status = st;
            if (st == 0 || flow != FLOW_NONE) {
                return st;
            }
            return exec_node(n->right);
        }
        case N_NOT: {
            int st = exec_node(n->left);
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
            int cond = exec_list_node(n->left);
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

int main(int argc, char **argv) {
    shell_pid = getpid();

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
        return flow == FLOW_EXIT ? exit_code : st;
    }

    if (argc > 1 && argv[1][0]) {
        script_name = argv[1];
        if (argc > 2) {
            set_positional(argv + 2, argc - 2);
        }
        return run_script_file(argv[1]);
    }

    interactive_loop();
    return flow == FLOW_EXIT ? exit_code : last_status;
}
